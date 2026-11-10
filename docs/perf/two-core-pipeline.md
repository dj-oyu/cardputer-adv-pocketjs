# 2 コアのフレームパイプライン（設計、未実装）

[性能と描画の文書](README.md) / [1 フレームの内訳](frame-breakdown.md)

状態: **設計のみ。** 2026-11-10 にコードを読んで作った（HEAD 881887f、ビルド・測定なし）。行番号は ±数行の目安。数値は「実測（出典）」か「推定（式）」を付けてある。未確認の前提は §6 にまとめた。

フレーム N の描画・転送を別のタスク（CPU 0）で回している間に、UI タスク（CPU 1）がフレーム N+1 の JS を進める。JS が描画の読む状態に触れる所でだけ待ち合わせる（join）。

## 0. 結論

- **新しい画素バッファは要らない。** 追加の DRAM は表示タスクの分だけで、静的に約 4.7 KB（推定: stack 4,096 B ＋ TCB 約 0.4 KB ＋ セマフォ約 90 B ＋ job 約 120 B）。UI の stack を 32,768 → 28,672 B に縮めれば、差し引き約 +0.6 KB。
- **同期は「所有権のトークン 1 つ ＋ `disp_join()`」。** 表示中のフレームの入力（Shiki の前面、Kasane の 2 つの bank、strip、SPI）は、表示タスクが読むだけで、UI は書かない。制御状態（`submitted`/`active`/`pending`/`has_committed`）は、UI だけが join の中で書く。ロックで守る共有の可変データは作らない。
- **以前の読みの訂正**:
  - `V.patch/replace` の BUSY は `presenting` ではなく、`core_begin` の `building||submitted||repairing`（`ksn_core.c:271`）から来る。`V.stats` は BUSY を返さない。
  - レンダラは core を `const` でしか読まない。core に書くのは `prepare_frame`/`presented`/`failed`/`defer_repair` だけなので、この 4 つを UI 側に移せば、core は表示中に変わらない。
  - 表示は遅れない。フレーム N の表示が始まるのは、今と同じく JS N の直後。frame-breakdown.md §4 の「1 フレーム遅れる」は、この設計では起きない。
- **重ねるフレームは許可リストで絞る**（§1.3）。DERBY・STRESS・BIG WAVE・MEGADEMO の通常のフレームだけ join を遅らせ、それ以外はすぐ join する（今と同じ意味）。
- 1 フレームの見込み（推定）: T = max(D, loop_other + tick_pre + js_pre) + js_post + 後処理（D は描画＋転送）。STRESS 約 11.4〜11.7 ms、DERBY のレース約 8.5〜9.9 ms。命令キャッシュの取り合いと、CPU 0 の他のタスクの影響は入っていない。

## 1. 所有権のモデル

### 1.1 相とトークン

`disp_state`（`atomic_uchar`）は次の順に移る。

- `IDLE`: UI がすべてを持つ。
- `RUNNING`: 表示タスクが job を実行している。
- `DONE`: 表示タスクは終わったが、UI がまだ join していない。

その後 `IDLE` に戻る。UI から見ると、`IDLE` 以外はすべて「表示相」。

### 1.2 状態ごとの持ち主

| 状態 | 表示相で表示タスクがすること | 表示相で UI がしてよいこと | 守る仕組み |
| --- | --- | --- | --- |
| Shiki のフレームの前面 `[0,count)` とヘッダ | 読む | 読むだけ。書く `merge()`（`pocket_shiki_frame.c:743`）は join の後 | `commit_impl:756` の先頭で join |
| 後ろ `[tail_base, …)` と `tail_*`、`building` | 触らない | 読み書き | `tail_base ≥ count`（§5 P1a の 5b） |
| `candidate_color` / `candidate_sig` / `sig_ready` | 読む | 書かない | 5a・5c |
| `committed_*` / `pending` / `has_committed` / `repair_required` / `committed_lost` | 読む | join の中（`present_result:1186`）と、join 後の commit でだけ書く | join |
| `half_carry` / `image_band_buf` / `image_band_*` | 読み書き | 触らない | 5c・5d |
| Shiki のヒープ（ロックなし）、`slots[]`、`shiki_vm`、JIT のコードと登録 | 触らない | 専有 | 5d、ヒープに `NOT_DISPLAY` の表明 |
| Kasane の 2 つの bank、image の登録表 | 読む | 読むだけ（`core_begin` は `submitted\|\|repairing` で BUSY） | `submitted/repairing/presenting` を join まで立てたままにする |
| core の制御状態（`active/submitted/repairing/full_redraw/repair_*/transaction/outcome`） | 読む | 書くのは handoff の前の `prepare_frame` と、join の中だけ | レンダラから変更の呼び出しを抜く（P1a） |
| core の `invalidated*` | 触らない | 書いてよい | `prepare_frame`（`ksn_core.c:961-988`）が `repair_*` へ移してから handoff する |
| `ksn_view_host`、`kasane_state`、ksn_cache | 触らない | UI の専有。JS から見える読み出しは join の後 | §2.3 |
| `ksn_render.c` の static と prof の累計 | 専有 | join の後に読む | A/B のスイッチは join 済みの場所で切り替える |
| strip `shared[]`、`tx_*`、`half_*`、SPI | 専有 | 呼ばない | `board_strip_next/board_present*/tx_reap` に持ち主の表明を置く |
| host の重ね描き（text、pet、capture、`paint.c`、notice） | 読まない（`job.host_overlays=false`） | 自由 | 適格条件で「handoff の時点ですべて無効」を求める |
| `board_capture` | 遅延モードでは無効 | `board_capture(true)` の前に join | capture 中は UI の上でその場で実行する |
| pump の類 | 触らない | 自由 | Kasane の入口が join する |
| ゲストのヒープ、JS の値 | 触らない | 専有 | 表示の経路は JS の参照を持たない |
| システムのヒープ | 確保しない | 自由 | デバッグ: alloc hook で、表示タスクから呼ばれたら abort |
| ログ・統計 | `job.*` に書くだけ。`ESP_LOG` を呼ばない | join の中で合算し、ログは join の外で書く | 30 フレームのログを `present_frame` の冒頭（join の直後）へ移す |

### 1.3 重ねてよいフレーム（handoff のときに UI が判定する）

次のすべてを満たすフレームだけ、join を遅らせる。

- フォアグラウンドのゲストである。
- 続きのターンでも、Back のターンでもなく、停止の要求も出ていない。
- `app_tick_body` の末尾の、通常の present である。
- schema、provider、pixel・video・grid の resource、animation block、modal、notice がどれも無い。
- text の編集、pet の alert、録音、`board_capture` のどれも動いていない。
- 前の join で IO のエラーが出ていない。

DERBY の `SK.resource({maxSegments:768, spare:512})` と STRESS の `P.resource()` は、どちらも image モードの proc surface なので、この条件を満たす。

## 2. 同期の仕組み

### 2.1 選んだ形

```c
bool       disp_start(disp_job *job, bool deferred); // UI。IDLE→RUNNING。!deferred は直後に join
bool       disp_inflight(void);                      // UI。state != IDLE
ksn_result disp_join(void);  // UI だけ。IDLE なら atomic load 1 回で戻る。そうでなければ done を待ち、
                             // IDLE にして present_finish(job) を UI で実行する
```

- **UI → 表示タスク**: 表示タスクの task notification（index 0）で起こす。
- **表示タスク → UI**: static の 2 値セマフォ `done` で知らせる。UI の notification の index 0 は `vm_wake`（`main/vm/vm_wake.c:37-49`）が使っているので使えない。index を増やすと、全タスクの TCB が太る。
- job は static に 1 つ置く。今 `present_frame` の stack にある構造体（`app_session.c:2535-2556`）を job へ移す。

### 2.2 退けた案

| 案 | 退けた理由 |
| --- | --- |
| mutex（優先度継承） | 表示タスクが取る前に UI が取れてしまう。両タスクが同じ優先度なので、継承の利点もない |
| event group | 遅く、足りる機能はセマフォと同じ |
| spinlock / portMUX | 割り込みを止めたまま、数 ms の描画をまたげない |
| atomic / seqlock で共有の値を守る | 共有の可変の値を job への写しで無くしたので、要らない |
| UI が spin して join を待つ | CPU 1 を空回しで焼く。まず block で作り、測ってから考える |

### 2.3 join する場所

**A. JS から見える入口**（先頭で `disp_join()`）
- Shiki: `commit`（`:756`）、`resource`、`createSurface`。
- `kasane` 名前空間のメソッド（`pocket_kasane.c:2717-2723, 2803`）、scene と schema のメソッド、`animation.*`。
- `pocket_kasane.c` が他のモジュールに出している関数のうち、状態を読むもの。
- **join しないもの**: `beginFrame`、`draw*`、`pocket_proc_native_*`、`P.heap()`、`pocket_kasane_invalidate(_bands)`。

**B. `app_tick_body`**
- 表示相なら `prior_submission` は false とみなす。
- `needs_present` の門は、handoff の後に新しい無効化があるときだけ join する。
- 続きのターンで `has_submission()` / `proc_pending()` を読む前に join する。
- **`present_frame()` の先頭では必ず join する。** 表示が JS より長い場面では、ここで待つのが律速になる。

**C. 遷移と解体**
- `app_stop`、`app_suspend`、`app_resume`、`app_start*`、`app_overlay_tick`、`app_vm_prepare_stop` の先頭で join する。
- `pocket_kasane_reset` は `presenting` の間 BUSY を返すので、必ず先に join する。
- 表示タスクは起動時に 1 回だけ作り、壊さない。

**D. UI ループの直接描画**
- `tick_run` のモーダル 3 種の前と、`board_capture(true)` の前で join する。

**E. 転送の失敗（`KSN_IO`）**
- join の中で `present_finish` を今どおり走らせ、同じ内容を同期で最大 3 回やり直す。
- それでも失敗したら、そのセッションの残りは遅延モードを止める。
- **意味が変わるのはここだけ**で、LCD の転送が続けて失敗したときに限る。

### 2.4 不変条件

- I1: `disp_state != IDLE` の間、UI は §1.2 で「表示タスクが読む」としたものへ書かない。
- I2: 表示タスクは、ヒープ・ログ・JS・`paint.c`・host の重ね描き・`invalidated*` に触れない。待つのは SPI の結果キューだけ。
- I3: core の制御状態、`pending/has_committed`、`host->presenting` を書くのは UI だけで、handoff の前か join の中に限る。
- I4: JS から見える値は、join 済みの状態か、join の前後で変わらないものだけ。
- I5: `board_*` の present 系を呼ぶのは、表示相なら表示タスク、そうでなければ UI。

### 2.5 費用・優先度の逆転・WDT・ISR（推定）

- **1 フレームの費用**: CPU 時間は 6〜10 µs、遅れは最大で約 50 µs（16.67 ms の 0.3%）。
- **デッドロック**: 待つ向きは「UI が表示タスクを待つ」の一方向だけ（I2）なので起きない。
- **優先度**: 表示タスクは優先度 5 から始め、音（sfx 7、mp3dec 6）を遅らせない。
- **WDT**: 表示タスクは notification か SPI の待ちで block する。UI の「idle が 500 ms 走れていなければ 1 tick 譲る」（6d04053）はそのまま要る。join は 1 s で timeout のエラーを出し、そのあとも待ち続ける。
- **ISR**: SPI2 の ISR は `app_main`（CPU 0）が登録するので、CPU 0 にあるはず（未確認）。

### 2.6 違反を安く見つける（`CONFIG_POCKET_DISP_ASSERT`、既定 n）

1. **持ち主のタグ**: `xPortGetCoreID()` を比べ、`ASSERT_UI_JOINED`、`ASSERT_DISPLAY_OR_JOINED`、`ASSERT_NOT_DISPLAY` を要所に置く。
2. **入力のチェックサム**: handoff のときに前面と bank の FNV を取り、join で比べる（推定 50〜100 µs）。
3. **影の再描画**: join の直後に同じフレームをハッシュだけ取る port で描き直し、送った画素のハッシュと比べる。実機の本当のタイミングで画素が一致するかを確かめる唯一の手段。
4. **host の「遅い join」**: `disp_start` は job を覚えるだけにして、`disp_join` で実行する。最も意地悪な順序が決まった形で再現できる。加えて、pthread の表示スレッドと `-fsanitize=thread` で試す。

## 3. 表示タスクのコア・優先度・stack

- **CPU 0 に固定、優先度 5、stack 4,096 B、`xTaskCreateStaticPinnedToCore` で .bss に置く。** 描画は PIE を使い、コプロセッサを使うタスクはコアに固定されるので、固定は必須。
- **stack の見積もり**: 最も深い呼び出しの鎖で約 1.3〜1.8 KB、割り込みとコプロセッサの退避域を足して約 2.5 KB（推定）。主な関数のフレームは `render_rects_body` 912 B、`render_group` 640 B（いずれも ELF の `entry` から）。`uxTaskGetStackHighWaterMark` で空きが 1 KB 以上あることを確かめる。
- **UI の stack**: 未使用の最小は実測 23,788 B（vm-L2-results.md:818）。28,672 B に縮めても約 19.7 KB の余裕がある（推定）。全アプリで最高水位を取り直してから、別のコミットで縮める。
- **CPU 0 の他のタスク**: Wi-Fi のリンク中は表示が 6〜8% 延びる見込み（類推）。sfx が CPU 0 を占める割合は未測定。
- **CPU 1 に置く変種**（`CONFIG_POCKET_DISP_CORE=1`）: 重なるのは転送の待ち（実測 2.8〜5.2 ms）だけ。STRESS は約 15.7 ms、DERBY のレースは 11.6〜14.3 ms の見込み（推定）。CPU 0 だと表示が 10% 以上延びるときや、命令キャッシュの取り合いが大きいときの退避先。

## 4. 命令キャッシュ

- **事実**: 描画の主なコードはすべて flash にある（`render_rects_body` 7,021 B、Shiki の `render_band` 3,162 B、`render_group` 2,248 B など）。IRAM に置くと DRAM を 1:1 で食う。
- **見立て（推定）**: 描画が触るのは 6〜10 KiB。JS から見ると、使える命令キャッシュが 32 KiB から 22〜26 KiB に縮む。ミスが 15% 増えると、JS は約 8.5% 遅くなる（0.57 × 0.15。STRESS で約 0.6 ms）。
- **決めるための測定**:
  1. 実機なしで、`tools/r5sim/icache.py` を 24 KiB 相当（6-way × 128 set）で回す。
  2. P1b の後、同じバイナリで `sync` と `defer` を窓ごとに巡回させ、JS の CPU 時間（JS − join の待ち）と描画時間を比べる。
  3. CPU 0 で描画の真似をする妨害タスクを窓ごとに入れ切りし、JS の伸びを見る。
- **判断**: JS の CPU 時間が +10% 以下、描画が +15% 以下なら進める。+25% を超えたら、(a) `render_band` だけを IRAM に置く（DRAM −3.2 KB）、(b) CPU 1 の変種、の順に試す。

## 5. 段階

共通の確認:
- host: STRESS の digest `de4a66c5`（`tools/test_stress_app.c:330`）、DERBY の先頭 `f2d667e3`（`tools/games/test_derby_host.c:1261`）、`tools/kasane_contract/run.sh`。
- 実機: `FRAME_BD` に `disp=inline|sync|defer` を足し、60 フレームの窓ごとに巡回させる。

| 段 | 中身 | DRAM | 合格 | 中止 |
| --- | --- | --- | --- | --- |
| **P0** 探針 | join の場所に印だけ置き、`js_pre`・`js_post`、場所ごとの回数、適格条件の項目ごとの回数、UI stack の最高水位を測る | 0 | レースと STRESS で `js_pre` が JS の 70% 以上、適格が 95% 以上、T の予測が 13 ms 以下 | `js_pre` が 50% 未満のアプリは、CPU 1 の変種が本命 |
| **P1a** 整理（タスクなし） | 下の 1〜7 | 数 B | digest が変わらない、契約試験が通る | digest が動く |
| **P1b** 表示タスク、すぐ join | `disp.c` を足し、`inline` と `sync` を巡回 | +4.7 KB（UI stack を縮めれば +0.6 KB） | `sync` の描画＋転送が `inline` +0.15 ms 以下。表示 stack の空き ≥ 1 KB。音の途切れ 0。WDT の警告なし（10 分）。影の再描画が一致 | 描画＋転送が +10% 超 → CPU 1 の変種 |
| **P2a** join を遅らせる | `beginFrame` の BUSY を緩める、join の場所を有効に、重ね描きを切る、IO の規則、3 モードを巡回 | 0 | host の遅い join で digest が変わらず TSan 0 件。実機で表明・チェックサム・影の再描画が 0 件。1 フレームの仕事が STRESS ≤ 12.5 ms、DERBY レース ≤ 11 ms、WIDE 2 ≤ 15.5 ms | §4 の判断 |
| **P2b** 適格を広げる | notice・animation・text の重ね描きを 1 つずつ | 0〜数十 B | 同上 | — |
| **P3** commit の前倒し | join の前に `band_signature` と `frame_damage` を済ませる | 0 | STRESS で 1 フレームの仕事 −0.5 ms 以上 | digest が動く |
| **P4** | §4 の (a) | −3.2 KB | 配置を変えた A/B で比べる | 逼迫判定の回数が増える |

P1a の中身:
1. `render_rects_body`（`ksn_render.c:1946`〜）から `ksn_core_presented/failed/defer_repair` の呼び出し（約 20 か所）を抜き、結果を返り値にする。
2. `ksn_view_host_present_complete`（`ksn_view.c:190`）を begin / run / end に分ける。
3. `pocket_kasane_present_complete`（`pocket_kasane.c:3042`）を prepare / finish に分ける。
4. `present_frame`（`app_session.c:2514`）を `present_prepare`、`present_run(job)`、`present_finish(job)` に分け、ログの塊を外に出す。
5. Shiki の直し:
   - (a) `tail_color` を足す。
   - (b) `:663` の base を `(has_committed || pending) ? candidate->count : 0` から作る。
   - (c) `sig_ready=false` と、draw ごとの `image_band_valid=false; carry_drop()` を `merge` に寄せる。
   - (d) `half_carry` の確保（`:570`）を `present_prepare` に移す。
6. `kasane_send`（`:604-635`）の重ね描きを `job.host_overlays` で切る。
7. host に「遅い join」のモードを足す。

## 6. コードだけでは決められないこと

- **R1** 命令キャッシュと SPI0 の取り合い、キャッシュの置換方式（§4 の測定 1〜3）。
- **R2** CPU 0 の負荷で表示が延びるか（音あり・なし、音楽、Wi-Fi、BLE で比べる）。
- **R3** SPI の ISR がどのコアにあるか（`board_lcd_isr_core()` で見る）。
- **R4** 表示タスクの stack の深さ、コプロセッサの退避域、`StaticTask_t` の大きさ。
- **R5** DRAM の余裕: レースの空き約 42 KB から 4.7 KB を引くと、逼迫を抜ける条件（36,864 B）までの余裕は約 0.4 KB（推定）。UI stack を縮める交換が前提になる。
- **R6** まだ数えていない入口（`pocket_shape.c`、`pocket_derby.c`、`pocket_overlay.c`、`pocket_grid/pixel/video` ほか）。
- **R7** 表示経路の副作用（`ksn_font.c`、静的アセットの image port など）。
- **R8** PIE を 2 つ目のタスクで使えるか（コードには「owner task only」とある）。
- **R9** 続きのターン・park・Back の保存ターン・強制停止と表示相の組み合わせ。
- **R10** 待ち方との噛み合わせ（`last_frame_us` に join の待ちが入る）。
- **R11** 見込みの値（キャッシュと CPU 0 の影響を含まない）。P0 の実測から引き直す。
