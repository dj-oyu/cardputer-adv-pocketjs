# アプリの常駐中断（設計、2026-09-27、`vm/app-suspend`）

`vm/main` の目標は「やりかけのことを置いて別のことをし、戻ってきたら続きから」（backlog の冒頭、
editor の下書きと同じ約束）。L2c まででターンの途中の中断・再開はできた。本書はその一段上、**Back で
アプリを壊さずに止めて残し、ホームや設定から戻ったらそのまま続ける**「常駐中断」を設計する。
flash へ書き出すハイバネート（別アプリの起動や電源断を越える）は本書の後で判断する（§9）。

事実は現行コードから取った（行番号は `78be9a9` 時点）。「推論」と書いたもの以外はコードで確認した。

## 1. いまの仕組み

- **Back は必ず終了**: `tick_run()` が `KEY_BACK` で `app_tick(0x2000)`（終了フックのターン、`VM_LEAVE_BUDGET_US`
  50 ms、yield なし）→ `app_request_stop()` → `end_run()` → `app_stop()`（main.c:674-677、:559-580）。
- **`app_stop()` は 16 の面を逆順に壊してからゲストを消す**（app_session.c:715-807）。ゲストのコールバックを
  持つ面はゲストより先に reset する規則（:338-352）。
- **`stop_requested` は一度立つと戻らない**: `interrupt()` が `stop_requested || now>deadline` を返すので、
  以後の JS への入りはすべて例外になる（:436-439）。消すのは次の `app_start_test` だけ（:853）。
- **ゲストの枠は 1 つ**: `static pocketjs_guest_t *guest`（:120）。オーバーレイ（deskclock・music）も同じ枠を
  使い（`app_start_overlay` → `app_start_test`、:1274-1284）、ホームに戻ると `overlay_tick` が起動する
  （main.c:936）。前景のアプリとオーバーレイは同時に存在しない。
- **ヒープ**: ゲストは内部 DRAM から確保し（guest.c:293 ほか）、上限 160 KiB。出荷アプリは実行中 38〜107 KB。
  ホームの空きは約 234 KiB（`memlog`、QIO・R4 後の実測）。
- **先例**: `workspace.run` の `returnState`（1 KB の JSON、1 回だけ `launchContext()` に渡る）、editor の下書き
  （`srcstore_draft_save`）、終了フックでの保存。どれも「壊して作り直す」前提。

## 2. 目標と範囲

**やること**: 中断に参加したアプリは、Back でゲストを保ったまま止まる。ホーム・設定・Playground など
ネイティブの画面を行き来しても残り、同じアプリをメニューから開くとその時点から続く。

**やらないこと（この段）**:
- 2 つの JS アプリを同時に残すこと。枠は 1 つのまま。別の JS アプリ（オーバーレイを含む）を起動するときは、
  残っているアプリを**退去**させる（§6）。
- 電源断・リセットを越えること（ハイバネート、§9）。
- 止めている間に JS を走らせること（バックグラウンド実行）。止めている間、ゲストには一切入らない。

## 3. 状態遷移

```
             start                  Back（参加アプリ）
   (none) ─────────▶ Running ───────────────────────▶ Suspending ──▶ Suspended
                        ▲      Back（不参加）/exit()          │ 失敗・予算超過   │
                        │            ▼                        ▼                  │ 同じアプリを開く
                        │         Stopping ◀──────────────────┘                  ▼
                        │            │           退去（§6）◀──── Suspended ── Resuming
                        │            ▼                                            │
                        │         (none)                                          │
                        └─────────────────────────────────────────────────────────┘
```

- **Suspending**: 1 ターン。`suspend` フック（§5）を今の終了フックと同じ予算（50 ms、yield なし）で走らせ、
  ジョブを消化する。L2c で中断中の鎖が残っていれば、ここで終わらせる。予算内に終わらなければ中断を諦めて
  Stopping へ（理由 `"back"`）。つまり**中断は失敗しても今の終了と同じ結果になる**。
- **Suspended**: ゲストは生きているが、`app_tick` も pump も呼ばない。`interrupt()` が常に真を返す状態にして、
  誤ってゲストに入ったら即例外にする（今の `stop_requested` とは別の旗。再開で下ろせる必要がある）。
- **Resuming**: 1 ターン。各面を元に戻し（§4）、`resume` フックを走らせ、Kasane の表示を全面で描き直す。
  失敗したら Stopping（理由 `"evict"`）。
- **Stopping**: 今の `app_stop()` そのもの。退去もここを通る（理由 `"evict"`）。

## 4. 各面の扱い（止めている間）

原則: **ハードウェアと共有資源は中断のときに手放し、再開のときに「手放された」ことをアプリに知らせる。**
JS の値（購読、Promise の枠、フック）はゲストと一緒に残るので、そのまま持ち越す。

| 面 | 中断のとき | 再開のとき | 根拠 |
| --- | --- | --- | --- |
| `pocket.app` | `suspend` フック。onFrame は止まる | `resume({suspendedMs})`。最初の onFrame の `deltaMs` は 1 フレーム分に丸め、実際の経過は `suspendedMs` で渡す | 今は `now - frame_last_us` をそのまま渡すので、再開直後に数分の delta が来る（pocket_app.c:531） |
| `pocket.time.sleep` | 期限はそのまま進む | 期限切れのものは OK で解決（今の pump の挙動） | pocket_app.c:711-735 |
| Promise の枠（`pocket_api`） | 走っているドライバを `CANCELLED` で止めて解決する | — | 今の pump は期限切れを最初の pump で `TIMEOUT` にする（pocket_api.c:524-525）。中断で何分も止めると、無関係な TIMEOUT が一斉に出る |
| `pocket.imu` | 購読は残し、ジャイロは止める | ジャイロを戻す。`dropped` は中断の分を足さない | ジャイロは購読がある間ずっと入っている（pocket_imu.c:28-30） |
| `pocket.av`（再生） | 一時停止（§8-2） | 再開 | ストリーム処理自体は main.c でゲストと無関係に回る（main.c:820-830）。music オーバーレイの再生は例外で、鳴り続ける（S5） |
| `pocket.capture`（マイク） | 録音を止め、レコーダを壊す | `closed` を知らせる | リングは約 64 ms。止めたままでは必ず溢れる（pocket_capture.c:14、:445-453） |
| `pocket.net` | リースを返して無線を下ろす | リンク変化（down）を知らせる。取り直しはアプリ | リースがある限り無線は上がったまま（pocket_net.c:1307-1328）。無線を上げたままではホームの空きが約 37 KiB 減る |
| `pocket.fs` | 開いているハンドルを破棄で閉じ、SD を外して許可も落とす | ハンドルは無効、`sd:` は選び直し | 今の reset と同じ（pocket_fs.c:4069-4111）。SD の媒体処理は fs の pump からしか動かない（:1861） |
| `pocket.io`（UART/I2C/RMT） | 閉じる | 無効を知らせる | UART は pump が動かないと受信バッファが溢れる（推論） |
| `pocket.bridge` | 受信箱を空にし、以後の着信は落として数える | 落とした数を知らせる | 受信箱は 2 枠（pocket_bridge.c:66） |
| Kasane | APP のリースを外す（ref・schema は残す） | リースを取り直し、全面を描き直す | リースは 1 つ（ksn_runtime.c:70）。ホームのオーバーレイ描画と取り合う |
| `pocket.text`・picker | 閉じる（取り消し） | — | 開いたままの入力欄は 3 つの JSValue を持つ（pocket_text.c:186） |
| `pocket.memory` | 圧迫の購読は残す | 再開の最初の pump で最新の状態を届ける | pocket_memory.c:226 |
| 身元（storage/fs の owner） | 触らない | `app_registry_select`・owner を**付け直す** | どちらも大域の変数で、途中で他のセッションが起動すると書き換わる（app_registry.c:137-143） |

「知らせる」の経路は、各面がすでに持っているイベント（リンク変化、`onVolumeChange`、購読の `closed`）を
使い、新しい種類は作らない。

## 5. API

```ts
pocket.app.start(hooks: {
  start?: () => void | Promise<void>;
  stop?: (reason: "back" | "replace" | "shutdown" | "evict") => void | Promise<void>;
  suspend?: () => void | Promise<void>;                        // 新規
  resume?: (info: { suspendedMs: number }) => void | Promise<void>;  // 新規
}): void;
```

- **参加の宣言は `resume` フックの登録**（§8-1）。登録していないアプリは今と同じく Back で終了する。
  既存のアプリは「Back で終わる」前提で終了フックに保存を書いているので、既定で中断にすると保存の時期が
  変わる。
- `stop` の理由に `"evict"` を足す。退去は中断中のアプリが**保存する最後の機会**で、今の Back と同じ予算で走る。
- `capabilities` に `app.suspend` を足す（実装したときだけ `supported: true`）。

## 6. 退去（中断中のアプリを終わらせる）

中断中のアプリは、次のどれかで `stop("evict")` を経て終了する。

1. 別の JS アプリを起動する（メニュー、`workspace.run`、USB の診断）。今の `begin_run` の前。
2. （退去させない）中断中のアプリがある間、ホームはオーバーレイを起動しない（§8-3）。
3. 空きが足りない: ホームの背景（最大 30,671 B、scene_mem.h:10）・Wi-Fi（`NET_RADIO_MIN_FREE` 56 KiB）・
   `!running` を前提にした診断（効果音表 6,480 B など、main.c:959-994）が、空きを必要とするとき。
4. 電源を切る（理由は `"shutdown"`、今と同じ）。

`!running && screen==SCREEN_HOME` を条件にしている場所（main.c の 7 箇所）は、「ゲストが**走っていない**」と
「ゲストが**存在しない**」を区別する必要がある。前者だけで足りるもの（音量キーなど）と、後者が要るもの
（診断、オーバーレイ）を仕分ける。

## 7. メモリの予算

- 中断のときに GC を 1 回走らせ、R3 の小ブロックのキャッシュを返す。ゴミの分はシステムへ戻る
  （ゲストの確保は共有の内部ヒープから 1 個ずつなので、解放はそのまま空きになる）。
- **中断してよい条件**: 中断後のシステムの空きが、ホームの背景（30.7 KB）＋無線（56 KiB）＋余裕を満たす
  こと（仮に 96 KiB）。満たさなければ中断せずに終了する。数字は実装時に `memlog` で決める。
- 見込み（推定）: 出荷アプリの実行中の使用は 38〜107 KB なので、ホームの空きは約 234 KiB から 130〜200 KiB
  になる。hello・STRESS 程度なら条件を満たし、大きいアプリは退去の判断が要る。

## 8. 決めたこと（2026-09-27、ユーザー）

1. **参加は `resume` フックを登録したアプリだけ**。登録していないアプリは今と同じく Back で終了する。
2. **中断したアプリの音声は一時停止**、再開で戻す。**ただしバックグラウンドアプリの music の再生は鳴り
   続ける**（前景のアプリを開いている間も、アプリを中断してホームにいる間も）。
   - 今のコードでは鳴り続けない。前景のアプリを開くとオーバーレイが解放され（main.c:599-605）、その
     `app_stop()` の `pocket_av_reset()` が再生を壊す（pocket_av.c:1648-1662）。3 と合わせると、music の
     JS が動いていない間も再生が続く必要があるので、**再生をオーバーレイのセッションから切り離す**
     （S5）。再生の間はデコーダの状態（Opus で約 18 KB＋復号のスタック約 10 KB、common-api.md）を前景の
     ゲストと同時に持つので、メモリの予算（§7）を再生中にも満たすかを実機で確かめる。
3. **中断中のアプリがある間、オーバーレイは起動しない**。
4. **再開の入口はメニューの同じ行**。その行に**一時停止中のアイコン**を出す。行は増やさない
   （`test_settings.py`・`capture_home.py` の押下回数を変えない）。

## 9. ハイバネート（本書の後）

別アプリの起動や電源断を越えるには、ゲストのヒープを固定番地の専用領域へ移し（今は共有ヒープから 1 個
ずつ確保）、各面の C 側の状態も保存できる形にする必要がある。常駐中断の状態遷移（§3）と各面の中断・再開
（§4）はそのまま土台になる。常駐中断を実装して、各面の中断処理が揃ってから判断する。

## 10. 実装の段と検証

| 段 | 内容 | 検証 |
| --- | --- | --- |
| S0 | `app_session` の状態（Suspended の旗、interrupt、`app_suspend()`/`app_resume()`/`app_evict()`）。面は今の reset のまま（中断＝ほぼ全部手放す） | ホスト: ゲストの寿命の検査（`tools/vmtest/guest_lifecycle.c`）に中断・再開の周回を足し、漏れ 0・ASan |
| S1 | `pocket.app` のフック、`deltaMs` の丸め、Promise の取り消し、Kasane の付け外し | ホスト: STRESS のハーネスで中断・再開を挟んで 900 フレーム |
| S2 | main.c の経路（Back→中断、メニュー→再開、退去の条件、`!running` の仕分け、オーバーレイの抑止、メニューの行の一時停止アイコン） | 実機: smoke に中断・再開・退去の周回、`memlog --port --check` を中断中にも取る、`test_settings.py`・`capture_home.py` |
| S3 | 各面の中断処理（§4 の表を 1 行ずつ） | 面ごとの実機の筋書き（SD・無線・IMU・音声） |
| S4 | 出荷アプリの参加（pet・imucal など） | アプリごとに「中断→設定を変える→再開」で状態が残ること |
| S5 | music の再生をオーバーレイのセッションから切り離す（前景のアプリの起動・中断を越えて鳴り続ける） | 実機: music 再生中にアプリを開く→中断→ホーム→再開で音声障害 0、再生中の空きの予算 |

## 11. 実装の状態（2026-09-27、`vm/app-suspend`）

S0〜S3 を実装した。§4 の表からの変更と、決めたことの実装:

- **Kasane のリースは外さない**。リースの末尾の領域に provider・schema・ref があり、外すと眠っているアプリの
  描画の状態が消える（`ksn_runtime_app_detach` が末尾を解放する）。中断中はオーバーレイを起動しない（§8-3）
  のでリースを取り合う相手がいない。代わりに `pocket_kasane_set_dormant()` で、ホストの画面から APP が
  見えないようにした（通知の合成・ソースの待ち・system の帯）。再開で全面を描き直す。
- **操作はすべて取り消す**（`pocket_api_cancel_all`）。締め切りが絶対時刻なので、残すと再開直後に一斉に
  TIMEOUT になる。取り消しは JS が起きている間に決着させる（`settle_all`、200 ms まで）。
- **中断はホームのメニューから起動したアプリだけ**。Playground・チュートリアルの実行は編集画面にバッファを
  返す経路があり、`workspace.run` の作品はどれも同じ ID なので取り違える。
- 休眠の旗はゲストの層（`pocketjs_guest_set_dormant`）。L2c の「ターンの途中で止まっている」
  （`pocketjs_guest_suspended`）と区別して dormant と呼ぶ。
- 中断が失敗したら通常の Back（終了フック `"back"`）。中断後の空きが 96 KiB 未満でも終了する。

**検証（実機、2026-09-27）**:

| 検査 | 結果 |
| --- | --- |
| セルフテスト `L`（ゲストの層） | 休眠 20 周・途中の鎖で拒否・直接の `JS_Eval` を止める・休眠中の破棄、空き前後一致。既存 900 件も通過 |
| セルフテスト `S`（`tools/vmtest/device_suspend.py --cycles 5`） | 5/5: sleep は `CANCELLED`、`APP_SUSPENDED`（1.4〜2.0 ms、空き約 191.4 KB で周回しても一定）、同じ行で再開すると中断時の frame 数から続き、ファイルは `CLOSED`、別の起動で `stop("evict")` |
| セルフテスト `M`（Back の順序） | 通過 |
| 出荷構成 | smoke 20 周・故障回復 6 種、`test_settings.py`、`capture_home.py`（30 fps）、`test_editor_draft.py`、STRESS PASS、`memlog --check` 予算内（静的 DIRAM +80 B） |

**残り**: S4（出荷アプリの参加。どのメニューのアプリも `globalThis.frame` を直接定義していて
`pocket.app.start` を使っていないので、書き換えが要る）、S5（music の再生をオーバーレイのセッションから
切り離す）。無線・SD・マイク・I/O を使っている最中の中断は、診断アプリでは扱っていない（面ごとの実機の
筋書きは S3 の残り）。

### 11.1 S4: 出荷アプリの参加（2026-09-27）

IMU CALIBRATION・POCKET PET・PET COMPANION を `pocket.app.start` に書き換え、`resume` を登録した。
HELLO WORLD（smoke が Back での終了を前提にしている）と STRESS TEST（負荷試験）は今のまま終了する。

- **pet**: `frame(buttons)` の押下の立ち上がりを `pocket.input.onAction` の press に、Back の `0x2000` での保存を
  `suspend`／`stop` フックに移した。`resume` で経過時間の基準を取り直すので、眠っていた間は育成が進まない
  （起動し直したときと同じ）。
- **companion**: `frame` を `onFrame` に。`resume` で描き直す。タイマーとアラームはネイティブ（pet hub）なので
  眠っていても鳴る。
- **imucal**: `frame` を `onFrame` に。`resume` で眠る前の姿勢のサンプルを捨てる。IMU が無いときの経路は
  従来の `frame` のまま（参加しない）。
- 再開のときも最初の提示で `KASANE_FRAME_PRESENTED` を出す（アプリを開いた後にスクリプトが待つ目印）。
- 診断（`K`、`1`〜`6`、効果音の検査）の前の退去が `CONFIG_POCKET_VM_SELFTEST` の中にしか無く、出荷構成では
  眠っているアプリがあると `K` が `START_FAILED`（pocket API が別の realm に入ったまま）になった。外へ出した。

**検証**: `tools/test_app_resume.py`（3 アプリそれぞれ 中断→同じ行で再開（`*_READY` が 2 度出ない）→中断→
HELLO で退去）、`node tools/test_pet.cjs`（新たに「10 分眠っても空腹が進まない」）、`node
tools/test_companion.cjs`（再開で描き直す）。実機の回帰: `app_mount_device_test.py`（IMU CALIBRATION の 2 度目は
再開として、再開後もセンサーの表示が動く）、`kasane_input_device_test.py`、smoke 20 周、`test_settings.py`、
`capture_home.py`、`test_editor_draft.py`、STRESS、`memlog --check`。検査の間に別のセッションが COM3 へ別の
ファームを書いたので、1 回目の結果は捨て、`test_app_resume.py` を最初と最後に置いた回で確かめた（私の
ファームでしか通らない）。

`tools/test_kasane_imucal.c` は `vm/main` の時点で既に落ちている（模擬の Kasane が `BUSY`、IMU が無い経路から）。
今回の書き換えに合わせて模擬に `pocket.app` と途中の `resume` を足したが、落ちる理由は別件（backlog）。

### 11.2 実機で一通り触って直したもの（2026-09-27）

画面の取り込みとログで、中断・再開・退去・COMPANION のタイマー・オーバーレイを確かめた（`.cache/vm-archive/s4_walk2`）。

- **一時停止の印が `??` になっていた**。フォントに `|` の字形が無い。0x7F に 2 本の縦棒の字形を割り当て、行と同じ
  倍率（10×14 px）で左の余白に描く。
- **再開したアプリは起動し直したアプリより 16 KiB 少ないメモリで動いていた**。起動の経路はホームの背景の作業
  メモリ（`scene_mem`）を返すが、再開の経路は返していなかった。同じアプリの 2 回目の中断の空きがきっかり
  16,384 B 少ないことで見つかった。修正後は 1 回目と 2 回目が一致（pet 164,032 B、companion 171,736 B）。
- **IMU CALIBRATION の再開で、次の姿勢を待っていた段が勝手に進んだ**。`resume` で直近のサンプルを捨てたため、
  空の窓が「動いた」と読まれた。静止の数え上げだけをやり直すようにした。
- 確かめたこと: pet は中断の前後で画面が同じで、17 秒眠っても状態が進まない。COMPANION のタイマーは眠っている
  間に鳴り、ホームに「TIMER FINISHED」が出る。眠っている間に設定でオーバーレイ（時計）を選んでも起動せず、
  HELLO で退去させてホームに戻るとすぐ起動する（設定は元の値に戻した）。
