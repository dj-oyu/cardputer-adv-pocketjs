# VM L1 報告: ジョブ境界の実行制御と起床（`vm/l1-host-sched`）

対象: [quickjs-freertos-vm-spec.md](quickjs-freertos-vm-spec.md) §6。設計は [vm-L1-design.md](vm-L1-design.md)、基準値は [vm-L0-report.md](vm-L0-report.md) §2.1/§2.2、1 ティックの実際の順序は [vm-ledger/03-jobs-interrupts.md](vm-ledger/03-jobs-interrupts.md)。2026-09-12 時点。

本書は**ユーザーが「L1 を完了と認めるか」「L2 に進むか」を判断するための報告**で、設計判断の理由は設計書側にある。数値は **実測(device) / 実測(build) / 実測(host) / 推定** を毎回書く。

**先に結論を 3 行で。**

1. ホスト側の検査は L1 の意味論をすべて満たしている（コーパス 33 件 × 12 通りでバイト一致、Test262 回帰 0、実測(host)）。
2. **実機の数値は 1 つも無い。** §6 の完了条件 4（RAM 増分と応答遅延の実測）は**未達**で、L1 の目的（ターン長の短縮）が達成されたかは**測られていない**。
3. したがって **L1 は「ホスト側で完了、実機で未検証」**。実機で測るまで完了と呼べない、という判断が仕様に沿う。

---

## 1. L1 で何が変わったか

### 1.1 利用者（JS アプリ）から見た挙動

| 変わるもの | 変わる内容 | どのアプリで見えるか |
| --- | --- | --- |
| 1 ターンの長さ | Promise ジョブの drain が **8 ms（`VM_TURN_BUDGET_US`）でジョブとジョブの間で中断**し、残りは次ターンの冒頭で続く。ターンが長いアプリほど画面・音・OS の応答が戻る | drain が長いアプリ（L0 のワークロード F: drain 30.27 ms、実測(device)）。drain が 0.01 ms のアプリ（A/B/C）は**何も変わらない** |
| `frame()` の呼ばれ方 | ジョブが残っているターンでは `frame()` を**呼ばない**。キューが空になったターンだけが pump → `frame()` → drain の通常経路に進む | F 型のアプリは `frame()` が約 4 ターンに 1 回になる（推定、実測の単価から） |
| 完了（`await` の解決）の届き方 | resolve / reject の配送も継続ターン中は**保留**される（§6 が求める互換モード）。したがって **F 型アプリの JS 完了遅延は縮まない。むしろ `frame()` と同じく約 4 ターンに 1 回にまとまる**（推定） | 設計 §1.2 が明記している L1 の限界。縮むのは「ターン長」であって「JS ハンドラ到達時刻」ではない |
| フレーム周期 | 他タスク・ISR からの完了で `vTaskDelay` を早く抜ける（`vm_wake_*`、FreeRTOS 直接タスク通知）。ただし下限 `VM_MIN_PERIOD_MS=8` ms | UART / ネット / 音声の完了を待つアプリ。`frame()` の回数を時計代わりにするアプリは速くなりうる |
| キー入力 | 継続ターン中の押下は落とさず `deferred_buttons` に保持し、最初の通常ターンで配る。**ただし 2 つの打鍵が 1 マスクに融合しうる**（§5 の残存危険 2） | 継続ターンが続く忙しいアプリのみ |
| `pocket.input.text` の打鍵 | ホスト側（IME・バッファ・キャレット・再描画）は従来どおり即座。**ゲストの onEdit/onSubmit/onCancel は `app_tick()` の pump 段に遅延**（同じフレーム内） | 観測差は通常無し。1 本の論理 drain の途中に JS が割り込まなくなる |
| 暴走 | 「1 本の論理 drain が `vm_sched_drain` の中で使った時間」が 250 ms（`VM_RUNAWAY_US`）を超えるとセッション終了。例外は投げない（`finally` を飛ばさない、`await` を永久 pending にしない）。時計が死んだときの受け皿が 10 万件（`VM_RUNAWAY_JOBS`） | `function f(){Promise.resolve().then(f)} f()` のような「空にならないキュー」。**旧 250 ms 壁時計ガードより必ず緩い**（`frame()`・pump・描画・転送を数えないため） |
| 離脱（Back） | 離脱ターンは広い予算（`VM_LEAVE_BUDGET_US=50000` / backstop 256）で継続を走らせ、**空にならなくても** `frame(0x2000)` へ抜ける。ゲストの最後の保存機会を保つ | 忙しいアプリ（= 保存が最も要る側） |

**変えていないもの**: ログ標識（`PERF` / `PAINT` / `MEM` / `HOME_READY` / `APP %u` …）の書式は 1 バイトも変えていない。`MEM` は `tools/memlog.py` が `js=` を正規表現で拾うので特に触っていない。追加した行は 2 本だけ — `W app: jobs dropped at stop: queue was not empty (yields=%u continuations=%u)` と `E app: RUNAWAY one drain spent %lld us over %llu jobs in %u turns`（後者は `jsconsole` に `JOB QUEUE RUNAWAY`）。

### 1.2 ビルド時スイッチ

| 名前 | 既定 | 意味 |
| --- | --- | --- |
| `CONFIG_POCKET_VM_SCHED` | **y** | L1 の全体。`n` にすると予算が無制限になり、drain は中断できず、継続ターンが存在せず、未処理 rejection の報告地点は L1 前と同じ、フレームキャップは旧来の無条件 `vTaskDelay` に戻る |
| `sdkconfig.vmsched_off.defaults` | — | 上記を `n` にするための重ね方（`-D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.vmsched_off.defaults"`） |

仕様 §12（各段階は revert 可能、`main` の既定挙動を変えない）を 1 つの Kconfig で満たしている。off ビルドは実測(build)で成立を確認済み（§2.2）。

### 1.3 コード上の場所

`quickjs.c` は **1 バイトも変更していない**（§6 完了条件 1）。新設・改変は次のとおり。

- `components/pocketjs_guest/…/vm_clock.[ch]` — 時計抽象（既定は device で `esp_timer_get_time()`、host で `clock_gettime`）
- `components/pocketjs_guest/…/vm_sched.[ch]` — `vm_sched_drain()` と `vm_budget_t`、全定数。**esp ヘッダを含まない**ので `tools/vmtest/vmrun.c` が写さずに**リンク**する（= ハーネスは出荷するスケジューラそのものを検査する）
- `guest.c` — `drain_jobs` を `vm_sched_drain` 経由に、`pocketjs_guest_continue/_jobs_pending/_budget/_set_watchdog/_drain_total`、stats に `yields`/`continuations`/`jobs_pending`/`jobs_dropped`
- `components/pocketjs_ui_qjs/` — **新規に取り込み**（`77095b0` が無改変のバイト列、以後が改変）。`pocketjs_ui_turn_continue()` を追加
- `main/app_session.c` — 継続の骨格、`arm_turn()` / `present_frame()` / `drain_runaway()` / `final_stats`
- `main/vm/vm_wake.[ch]`、`main/main.c`、`main/pocket/pocket_api.c`、`main/pocket/pocket_app.c`、`main/pocket/pocket_text.c`（`pocket_text_pump()`）
- `tools/vmtest/` — `--budget-jobs` / `--force-yield` / `--runaway-jobs` / `--stop-turns` / `--host-events`、新規コーパス 10 件

---

## 2. 実機の結果

### 2.1 実機の数値は 1 つも無い（実測(device) 0 件）

**L0 比の表は作れない。** ターン長・完了遅延・スループット・空きヒープ・最大空きブロック・スタックの、実機の値は**すべて未測定**である。

理由は 2 つで、どちらもソフトウェアの問題ではない。

1. **ボードが物理的に繋がっていない。** `USB\VID_303A&PID_1001`（Cardputer のネイティブ USB-JTAG/serial）が Windows の PnP で `CM_PROB_PHANTOM`（"this hardware device is not connected"）。同時に見えていた COM4（CH340）・COM5（VID `291A:8355`）はどちらも `esptool chip-id` が "No serial data received" で、このボードではない。
2. **実機には別作業のベンチビルドが載ったままである。** [vm-l1-clock.md](vm-l1-clock.md)（`vm/l1-clockbench`）§5 の記録によれば、最後に書き込めたのは `CONFIG_POCKET_VM_L1_CLOCKBENCH=y` の `build_bench_clock` で、その後 USB リセットに応答しなくなり**素のビルドへ戻せていない**。したがって実機は L1 の測定に使う前に、まず素のビルドへ戻す必要がある。

### 2.2 実機を使わずに測れたもの（実測(build)）

| 項目 | L1 前 (`cd5117d`) | L1 後（現 HEAD `6482692`） | 差 | L0 §2.2 の上限 |
| --- | --- | --- | --- | --- |
| 静的 DIRAM | 115,372 B | **115,468 B** | **+96 B** | +8 KiB（1.2% を使用）|
| Flash Code | 1,550,840 B | **1,552,612 B** | +1,772 B | — |
| DIRAM（`CONFIG_POCKET_VM_SCHED=n`）| — | 115,452 B（レビュー時点の記録）| — | off でも L1 のコードは大半がリンクされる |
| DIRAM（`CONFIG_POCKET_VM_PROBE=y`）| 119,692 B | 119,772 B | +80 B | プローブ自体の +4,320 B は L0 の実測どおり |

**L1-new と L1-legacy（`CONFIG_POCKET_VM_SCHED=n`）の DIRAM がバイト一致**していることは、スケジューラの費用が静的フットプリントではなく挙動にあることを意味する。RAM の上限（+8 KiB）は**満たしている**。ただし L0 §2.2 が「RAM の上限は無線を上げた状態の**実行中の空きヒープ最小値**で判定する」と書いた側（現状 14,768 B、実測(device)）は**未測定**である。

### 2.3 実機で何を測るべきか（手順は確定済み）

実機が戻ったらこの順で走らせる。ビルドは既に手元にある（`build_l1_dev_probe_on` / `build_l1_dev_probe_legacy`）。

```powershell
. 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1'
cd C:\devs\m5stack\cardputer-adv-pocketjs-vm

idf.py -B build_l1_dev_probe_on -p COM3 flash
python tools\vm_l0_capture.py --port COM3 --workloads ABCDEF --conditions base,all --reps 3 --out .cache\vm\l1-new.jsonl
python tools\vm_l0_capture.py --port COM3 --workloads D,F --conditions ui,audio,wifi --reps 3 --out .cache\vm\l1-new.jsonl --append

idf.py -B build_l1_dev_probe_legacy -p COM3 flash     # CONFIG_POCKET_VM_SCHED=n
python tools\vm_l0_capture.py --port COM3 --workloads ABCDEF --conditions base,all --reps 3 --out .cache\vm\l1-legacy.jsonl
python tools\vm_l0_capture.py --port COM3 --workloads D,F --conditions ui,audio,wifi --reps 3 --out .cache\vm\l1-legacy.jsonl --append

python tools\vm_l0_capture.py --summarize .cache\vm\l1-new.jsonl --markdown
python tools\vm_l0_capture.py --summarize .cache\vm\l1-legacy.jsonl --markdown

idf.py -B build_l1_dev -p COM3 flash                  # 非プローブの素の L1
python tools\test_settings.py --port COM3
python tools\capture_home.py --port COM3
python tools\smoke_device.py --port COM3 --cycles 20
python tools\memlog.py --map build_l1_dev\cardputer_pocketjs.map --port COM3 --check
```

**同一バイナリ比較の注意**: L0 の基準値（`vm-L0-report.md` §2.1）は L1 前のバイナリで採ったものなので、L1 の効果を主張するには **L1-legacy（同じツリー・同じプローブ・`SCHED=n`）との比較**を使う。L0 の表との直接比較は命令キャッシュのアラインメント（CLAUDE.md: 同じカーネルがビルド間で 15% 動く）を含むので、L0 との差は「桁の確認」にしか使えない。

判定の目安（設計 §7-12、いずれも**推定**の目標であって測定結果ではない）: F の turn 中央値 ≤ 12 ms、E の lat 中央値からフレーム周期成分（33 ms）が消えていること、全ワークロードの turn 中央値が legacy 比 +5% 以内（L0 §2.2。反復間ばらつきが実測 5.6% あるので、それ未満の差は主張しない）。

---

## 3. 仕様 §6 の完了条件チェックリスト

| # | 完了条件 | 判定 | 根拠 |
| --- | --- | --- | --- |
| 1 | **VM 本体の変更なしで動作する** | **満たした** | `quickjs.c` は 1 バイトも変更していない。判定は `JS_ExecutePendingJob()` の呼び出しと呼び出しの**間**だけ（`vm_sched.c`）。ジョブを割らない・二重に入らない・飛ばさないことは構造（`list_del` してから実行、`jobs_pending` が唯一の再開ゲート）とコーパス（`generators.js` / `try_finally.js` / `closures.js` を `--force-yield` で、ASan/UBSan/LSan 報告 0、実測(host)）の両方で確認 |
| 2 | **FIFO、then/catch/finally の観測順序が基準と一致** | **ホストで満たした / 実機で未検証** | コーパス 33 件 × {asan, o2} × {予算なし, `--force-yield`, `--budget-jobs 1/3/7/16`} = 12 通りで**全件バイト一致**。既存 `expected/*.txt` は `runaway_jobs.txt` 1 件（暴走の判定文の変更に伴う）を除き書き換えていない。Test262 `--force-yield` は asan/o2 とも 7,501 pass / 194 fail / 0 skip、`regressions: 0`（実測(host)）。**実機経路のみで通る順序**（`app_tick` の pump 順、overlay 経路、離脱ターン）は実機で未確認 |
| 3 | **完了通知の集中・満杯・停止直後・通知と待機の競合で取りこぼし / 永久待機がない** | **ホストで満たした / 実機で未検証** | 記録は確保無しの `promises[]`、`done` の公開が `vm_wake_post()` より**前**なので「記録が無いのに通知だけ」は起きず、「記録はあるのに通知が無い」は待機直前の check が拾う（設計 §4.3 の証明）。カウント型通知なので check と Take の間の Give も失われない。ISR 文脈は `xPortInIsrContext()` で分岐。停止直後は `pocket_api_reset()` が armed を settle せず解放（従来どおり）。ホスト検査は `budget_completions.js` / `budget_boundary_exact.js`（記録順に 1 回ずつ、キューが空になったターンでのみ配送） |
| 4 | **追加タスクのスタックを含む RAM 増分と応答遅延の変化を実測する** | **未達** | RAM の**静的**側は実測(build) +96 B（上限 +8 KiB の 1.2%）。追加タスクは**作っていない**（§4.1 の判断: 専用タスクは DIRAM +24〜32 KiB 推定で上限超え）。**応答遅延は 1 つも測っていない。**実行中の空きヒープ最小値も未測定 |

**総合判定: §6 の 4 条件のうち 3 つがホスト側で満たされ、4 番目（実測）が未達。** したがって仕様の文面上、L1 はまだ完了していない。

なお §6 の機能項目のうち **「runtime の所有タスクを明示する」は「`ui_task` のまま」という明示**で満たし、**「未処理ジョブもイベントも無い場合だけ待機する」は文字どおりには満たしていない** — `ui_task` は 30 fps のフレームループで、JS が無くても止められないため（設計 §4.1）。L1 が実現したのは「フレーム待ちを完了通知で早く抜ける」までで、専用タスク化は L1 の範囲外と設計が明記している。この解釈をユーザーが承認するかは §6 の決定事項 5。

---

## 4. 意味論の保証と、それを守るテスト

「どのテストが何を守っているか」。すべて実測(host)、`tools/vmtest/`。

| 守る意味論 | テスト | 不合格の形 |
| --- | --- | --- |
| FIFO 順序が予算に依らない | `promise_chain.js` / `microtask_order.js` を 12 通りで | どれか 1 通りで期待値と 1 バイトでも違う |
| then/catch/finally の観測順序 | `microtask_order.js`（finally の上書き、thenable の 2 段）/ `try_finally.js` / `generators.js` | 同上 |
| マイクロタスクとフレームの境界を跨がない | `budget_frame_boundary.js` | 100 段の連鎖の**途中**に `frame` の出力が現れる |
| ジョブを半分に割らない | `closures.js`（async の中断中フレームを捕まえたクロージャ）/ `generators.js` を `--force-yield` + ASan | LSan/ASan/UBSan の報告、または出力差 |
| 未処理 rejection を**予算境界では**報告しない | `rejections.js`（`--budget-jobs 1/2/3`）、**`budget_reject_far_catch.js`** | catch が 70 件先（backstop の外）／ drain の 30 件目で生まれ 40 件先で捕まる rejection が「未処理」と報告される。逆に**誰も捕まえない対照 1 件は必ず報告される**ので、報告を止めただけの実装も落ちる |
| 完了を失わない・重複配送しない・順序を保つ | **`budget_boundary_exact.js`**（ジョブの中から `k=0` で要求した完了 = 境界ちょうど、完了ハンドラからの再入要求、その連鎖の最中の記録）、`budget_completions.js` | 2 回配送、順序違い、継続ターン中の配送 |
| 飢えるキューが無い（前進保証） | `budget_starve.js`（`frame()` が 40 件積む条件で floor だけが走る） | floor 件すら走らないターンが出る |
| 暴走を検出する | `runaway_jobs.js`（`--runaway-jobs 2000`） | 終了コード 5 にならない、または LSan 報告 |
| **正直な長い連鎖を暴走と誤認しない** | **`budget_honest_long_chain.js`**（2,500 段、出荷時の `--runaway-jobs 100000`） | どれかの予算で終了コード 5。**初版の設計（ターン数で数える）はここで落ちた** |
| ジョブを残した終了で報告せず漏らさない | `stop_with_queue.js`、**`budget_teardown_live.js`**（await で止まった async 4 本 + 到達しない `finally`、try/finally の中の generator、要求が残った async generator、切断の向こうの thenable、捨てられるジョブの中の catch） | rejection 報告が 1 行でも出る、LSan 報告、`jobs_dropped` が立たない |
| **exit() が同じ drain の続きを殺さない** | **`budget_exit_midchain.js`**（vmrun に `host.exit()` と停止要求後に 1 を返す割り込みハンドラを入れ、実機経路を写した） | exit の行で出力が切れる（= 冒頭読みに戻した状態。実際に戻して落ちることを確認済み） |
| Test262 の合格集合が減らない | `test262.py --force-yield`（asan / o2） | `regressions` が 0 でない |
| 予算無効時の費用 | `timing.py`（-O2、予算オフ） | 中央値が基準の p95 を、基準自身の p95−中央値より大きく超える |
| `pocket.input.text` が `app_tick()` の外から JS を呼ばない | `tools/test_pocket_text.c` case 8 / 9（9/9 通過、ASan/UBSan/LSan） | 打鍵時点でコールバックが走る、teardown で発火する、リーク |

**検査の設計上の要**: 期待値ファイルは既存のものを書き換えない（`tools/vmtest/README` の規則）。新規は `--bless`。予算を変えても**出力がバイト一致**という形にしているので、「予算に依存する挙動」を導入した瞬間に落ちる。

**検査が届いていない場所**（§5 でも再掲）:
- vmrun の件数モードは `limit_us=0` なので、**時計側の分岐（`n >= floor && n % stride == 0 && clock() - start >= limit`）はホストでは一度も実行されていない。** floor / stride / 「`frame()` が既にターンを使い切った」場合 / 離脱予算は、**実機でしか動かない**。
- `--budget-jobs 64` は文書化された行列の外。`stop_with_queue.js` が自前の `--stop-turns 20` と衝突して不一致になるが、これはそのファイルの性質で L1 とは無関係（既知）。
- `timing.py` の基準は `quickjs.c` の sha1 が違う（`271d718782c1` → `30877d7c8a45`）ので**同一バイナリの比較ではない**。`bench_alloc` 26.93（p95 26.78）と `bench_calls` 56.48（p95 56.03）が p95 を 0.6〜0.8% 超えているが、基準自身の p95−中央値（1.77 / 2.25 ms）より小さく、規則上「結果」と呼べる差ではない。

---

## 5. 残った危険と未検証

### 5.1 実機で未検証（最大の空白）

設計 §7 の不変条件 12 が**丸ごと未実施**。`smoke_device.py` / `test_settings.py` / `capture_home.py` / `benchmark_app.py`、L0 行列の F/E/D 再取得、`memlog.py --port --check` の実測ヒープ — すべて未実行。**焼いた後に初めて分かることが残っている**:

- ターン長が実際に縮むか（L1 の唯一の目的）。
- 完了遅延がどう動くか。互換モードなので **F 型では悪化しうる**（`frame()` と resolve が約 4 ターンに 1 回にまとまる、推定）。改善と悪化を分けて報告する必要がある。
- `vm_wake` の起床が実際にフレーム周期を縮めるか。自タスク投稿を落とす修正を入れたので、**残るのは音声・ネット・ISR からの完了だけ**。効果が測定に出ない可能性もある。
- 空きヒープ最小値（無線を上げた状態で現在 14,768 B、実測(device)）が 4 KiB 以上減っていないか。
- 継続ターン中も `board_present()` が走る（`present_frame()`）ので、**転送回数が増える**可能性。F 型で `frame()` が 1/4 になっても転送は毎ターン走る。`PERF send` で確認すること。

### 5.2 設計として残した危険（修正していない）

1. **`deferred_buttons` は 2 つの打鍵を 1 フレームに融合する。** `main.c` は `app_tick(buttons)` の直後に `app_tick(0)` を呼び「連続した打鍵が別物として届く」ことを離鍵フレームで保証しているが、継続ターンが続く間に別々のキーが届くと `deferred_buttons |= buttons` が 1 マスク（例: UP|RIGHT）にまとめる。**予算導入前には起こり得ない入力**。設計 §2.2 が `|=` を指定しているので設計側の判断として残してある。キューにするなら離鍵フレームの対も作り直しが要る。

2. **予算は壁時計なので、プリエンプトされた時間も drain に課金される。** 暴走ガードの許容量を旧ガードと同じ桁（250 ms）に置き、かつ合算対象を drain の中だけに絞ることで「旧ファームが完走させた drain をこれが殺すことはない」を構造的に保証した。しかし**混んだ機械では 8 ms 予算が他タスクの時間で尽きる**ので、1 ターンあたりのジョブ数は負荷に依存する。ホスト側の機構でこの時間を JS 時間と区別する手段は無い（`esp_timer` も CCOUNT もタスクの実行時間ではない）。

3. **互換モード（継続 drain より前に resolve を配らない）は F 型アプリの完了遅延を縮めない。** §6 が既定に求めたものなので L1 の欠陥ではないが、L1 の投資に対して「JS の応答性」は改善しない。改善したいなら「公平モード」（§8 のビルド時選択候補）が必要で、それは観測順序を変える。

4. **`frame()` は分割できない。** A（117.56 ms）・C（49.75 ms）の `frame()` はそのまま（いずれも実測(device) base 中央値）。これらのアプリでは 8 ms 予算は守れず、drain が floor（8 件）まで縮むだけ。**L1 の限界であって欠陥ではない**が、「応答性が改善する」とユーザーに説明するときこの 2 型は例外である。

5. **`VM_MIN_PERIOD_MS=8` により `frame()` が 30 fps より速く呼ばれうる。** `onFrame` の `dt` は実測時刻から計算されるので正しいが、**`frame()` の回数を時計代わりにするアプリは速くなる**。

### 5.3 clockbench の結論が取り込まれていない

**2026-09-12 に §8 で決着した**（pin は採用 = core 1、毎ジョブ読み（`VM_JOB_STRIDE=1`）は採用、CCOUNT は不採用。いずれも実測(device)）。以下は決着前の記述として残す。

`vm/l1-clockbench`（[vm-l1-clock.md](vm-l1-clock.md)）は実測(device)で 3 つの結論を出しているが、**`vm/l1-host-sched` には 1 つも入っていない**（ブランチは未マージ、`git merge-base --is-ancestor` で確認）。

| clockbench の結論（実測(device)） | L1 の実装 | 差 |
| --- | --- | --- |
| 予算判定は CCOUNT（25 ns）を読む。systimer は 833 ns で 33 倍高い | `vm_clock` の既定は `esp_timer_get_time()`（= systimer、833 ns） | **未採用** |
| 毎ジョブ読む（N=1）。粗くすると async generator の塊で最大 (N−1)×0.49 ms の検出遅れ | `VM_JOB_STRIDE=4`（4 件に 1 回）→ F の単価で最大 1.5 ms の超過（推定） | **未採用** |
| `ui_task` を core 1 に pin する。実測(build) で DIRAM +0 B、CCOUNT のコア移動問題が構造的に消える | `xTaskCreate(ui_task, …)`（pin 無し） | **未採用** |

`vm_clock_install()` があるので**差し替えは 1 箇所**で済む設計になっている。ただし CCOUNT を採るなら `vm_budget_t` の単位がサイクルになり、`limit_us` の名前と `VM_RUNAWAY_US` の換算を見直す必要がある（ヘッダは「差分だけを扱う」と契約済み）。また clockbench 側も **pin したビルドの実行時コストが未測定**（機材が USB リセットに応答しなくなってブロック）で、その節自体が未完成である。

### 5.4 実機の状態そのもの

**解消済み（2026-09-12）。** 実機は COM3 に戻り、§8 の測定を経て現在は `build_l1_ship`
（`sdkconfig.defaults` のみ、プローブ無し、`CONFIG_POCKET_UI_TASK_CORE=1`）が載っている。
以下は解消前の記述。

実機には `build_bench_clock`（`CONFIG_POCKET_VM_L1_CLOCKBENCH=y`、pin 無し）が載ったまま素のビルドへ戻せていない（[vm-l1-clock.md](vm-l1-clock.md) §5）。**L1 の測定を始める前に、まず素のビルドへ戻す必要がある。**

---

## 6. L2 に進む前に決めること

（形式は [vm-L0-report.md](vm-L0-report.md) §6 に合わせた。番号ごとに「決めること」と「決めないとどうなるか」を書く。）

1. **L1 の完了の承認。** §3 のとおり §6 の完了条件 4（実測）だけが未達。選択肢は (a) 実機が戻ってから §2.3 を走らせ、その数値をもって完了とする、(b) 「実機検査は `vm/main` へのマージ前の関所で行う」とし、ホスト完了をもって L1 のコード作業を閉じる、(c) 実機の数値が出るまで L1 を開いたままにする。**仕様の文面に忠実なのは (a)。** どれを選ぶかで L2 の着手時期が決まる。

2. **出荷時定数の承認。** `VM_TURN_BUDGET_US=8000` / `VM_JOB_STRIDE=4` / `VM_JOB_FLOOR=8` / `VM_JOB_BACKSTOP=64` / `VM_LEAVE_BUDGET_US=50000` / `VM_LEAVE_BACKSTOP=256` / `VM_RUNAWAY_US=250000` / `VM_RUNAWAY_JOBS=100000` / `VM_MIN_PERIOD_MS=8`。L0 §2.2 の提案（件数 16、時間 8 ms、遅延 p95 8 ms / 最大 20 ms、DIRAM +8 KiB、低下 +5% 以内）を L1 がどう解釈したかは設計 §1.2 にある。**遅延の上限（p95 8 ms / 最大 20 ms）は互換モードでは達成できない**（resolve が継続ターン中は保留されるため）ので、この提案値を L1 に対して適用するのか、L2 以降の目標に送るのかを決める必要がある。

3. **時計の決定（§5.3）。決定済み — §8 を参照（pin は採用 = core 1、毎ジョブ読みは採用、CCOUNT は不採用。いずれも実測(device)による）。** 以下は決定前の記述。clockbench の 3 結論（CCOUNT・毎ジョブ読み・ui タスク pin）を L1 に取り込むか。取り込むなら `vm/l1-clockbench` のマージ順と、`vm_budget_t` の単位変更を含めた作業になる。取り込まないなら、その判断（systimer 833 ns × 1/4 件を許容する）を設計書に書き留める。**どちらにせよ、`vm/l1-clockbench` の pin ビルド実行時比較が未完成であることは実機復旧後に片付ける必要がある。**

4. **`deferred_buttons` の融合（§5.2-1）をどうするか。** (a) 受け入れて文書化する、(b) キュー化する（離鍵フレームの対を作り直す）、(c) 継続ターン中は最初の 1 つだけ保持し残りを捨てる。影響を受けるのは「継続ターンが続くほど忙しいアプリ」だけで、現状そのようなアプリは F 型のプローブしかない。

5. **§6 の「未処理ジョブもイベントも無い場合だけ待機する」の解釈。** L1 は `ui_task` を所有タスクのままとし（専用タスクは DIRAM +24〜32 KiB 推定、L0 §2.2 の上限 +8 KiB を超える）、フレーム待ちを完了通知で早く抜ける形に留めた。これを §6 の充足と認めるか、専用タスクを別段階（L1b など）として立てるかを決める。**認めない場合、L2 の前に RAM の作り直しが要る。**

6. **「公平モード」をビルド時選択にするか。** 継続 drain より前に `pocket_api_pump` の resolve だけを許す形（FIFO は壊れないが、旧 drain 完了前に resolve が起きる順序になる）。F 型アプリの完了遅延を縮めるのはこれだけである。L1 は互換モードを既定とする §6 に従って**入れていない**。§8 の候補として記録済み。

7. **L0 で見つかった既存不具合の扱い**（L0 §6-6 から持ち越し。いずれも `main` にも効き、L1 とは独立）:
   - GC 閾値が上限を超えている件（`JS_SetGCThreshold` 1 行）。
   - OOM 時の use-after-free。
   - 中断された `await` の Promise が pending のまま残る件（台帳の事実 39-41）。L1 は**予算切れではこれを作らない**（例外を投げないため）が、暴走ガードと `stop_interrupt` の経路では依然として起きる。

8. **マージとタグ。** `vm/l1-host-sched` は 10 コミットで push していない。`vm-branching.md` の規則は「関所の検査を通したら `--no-ff` で `vm/main` へ」「完了した段階に `vm-L1` のタグ」。**関所の検査 = 実機検査（§7-12）**なので、1 の決定に従属する。`vm/main` → `main` は段階の関所でユーザーが判断する。`CONFIG_POCKET_VM_SCHED` があるので `main` に入れても既定を `n` にすれば挙動は変わらない（仕様 §12）が、**現在の既定は `y`** である。これも決定事項。

9. **作業ツリーの未コミット変更をどうするか。** §7 の一覧のとおり、実機計測のために L0 のワークロードを一時的に復元した 8 パスが未コミットで残っている。実機計測を行うなら必要、行わないなら破棄でよい。**いずれにせよ `vm/main` へマージする前に消すこと**（`main/CMakeLists.txt` に `TEMPORARY (uncommitted)` のコメント付きで印がある）。

10. **実機の復旧。** USB の抜き差し（または電源再投入）の後、まず素のビルドを焼いて clockbench ビルドを落とす。これはソフトウェアからは直せない（§2.1）。

---

## 7. 作業ツリーの状態（2026-09-12 時点）

```
$ git -C C:\devs\m5stack\cardputer-adv-pocketjs-vm status --short
 M main/CMakeLists.txt
 M main/Kconfig.projbuild
 M main/app_session.c
 M main/main.c
 M main/pocket/vmprobe.c
 M main/pocket/vmprobe.h
?? apps/vmprobe/
?? tools/vm_l0_capture.py
```

この 8 パスは**すべて L0 のワークロード（6 種 + `condition.js`）と採取スクリプトを実機計測のために一時復元したもの**で、retire コミット `9723327` を逆適用し、`git checkout vm-L0 -- apps/vmprobe tools/vm_l0_capture.py` で戻したもの。L1 の実装とは無関係。**実機計測を行うなら必要、行わないなら破棄。マージ前には必ず消すこと。** `main/CMakeLists.txt` の該当ブロックには `TEMPORARY (uncommitted)` のコメントが付いている。

`dependencies.lock` は `skip-worktree` のままで、この作業では 1 度も staging していない。

コミット（`vm/main..HEAD`、10 件、未 push）:

| SHA | 内容 |
| --- | --- |
| `77095b0` | `pocketjs_ui_qjs` の無改変取り込み |
| `cd5117d` | 取り込んだ `pocketjs_ui_qjs` をリンク（`.cache` の上書きをやめる）。**L1 前の基準バイナリ** |
| `ab54946` | L1 設計（ジョブ境界の予算・継続・起床・暴走） |
| `cdebd23` | `vm_clock` / `vm_sched` — 2 ジョブの間で止まれる drain |
| `1767824` | vmtest がスケジューラをリンク + 新規 5 件 |
| `97d8275` | `pocketjs_ui_turn_continue()` |
| `ab2eb11` | `app_session` / `main.c` / `vm_wake` — 継続・起床・暴走ガード |
| `45610c8` | 設計書 §9（実装の記録と設計との差分） |
| `dd016a0` | 独立レビュー — ホスト側の欠陥 2 件 + ビルド破壊 1 件の修正、新規コーパス 3 件 |
| `6482692` | 意味論レビューへの対応 — 暴走ガードの設計変更、`exit()` の位置、`pocket_text` の pump 化、自タスク起床の抑止 |

---

## 8. 時計とコアの決定（2026-09-12、実機で決着）

§5.3 と §6-3 が残した「clockbench の 3 結論を取り込むか」を実機で測って決めた。
[vm-l1-clock.md](vm-l1-clock.md) §2 の「pin したビルドの実行時コストは未測定」も
ここで埋まっている。測定はすべて `CONFIG_POCKET_VM_PROBE=y` + `tools/vm_l0_capture.py`
（配置の比較はワークロード A/D/F × 条件 base/audio/all、時計と stride の比較は
ジョブの重い D/F × base/all、いずれも反復 2 × 15 秒）と、ホーム画面の
`PERF` 行を 12 秒のウォームアップ後 60 秒プールしたもの。**決定の結果は
`CONFIG_POCKET_UI_TASK_CORE=1` / `CONFIG_POCKET_VM_CCOUNT=n` / `VM_JOB_STRIDE=1`** で、
実機検査（§8.5）はその組み合わせの素のビルドで走らせている。

**3 つの配置は同じ 1 本のバイナリ族**で、`xTaskCreatePinnedToCore()` の即値だけが違う
（`main.c` の呼び出し口は 1 つ）。静的 DIRAM は 3 つとも **119,788 B でバイト一致**。

### 8.1 pin の実測(device)

| 指標 | 無 pin | core 1 | core 0 |
| --- | --- | --- | --- |
| sync_loop / base turn 中央値 | 117.37 ms | 117.36 ms | **123.91 ms (+5.6%)** |
| sync_loop / all turn 中央値 | 121.31 ms | 121.39 ms | **131.92 ms (+8.7%)** |
| promise_chain / all turn 中央値 | 13.04 ms | 13.08 ms | **14.03 ms (+7.6%)** |
| async_generator / all turn 中央値 | 8.16 ms | 8.00 ms | **9.19 ms (+12.6%)** |
| LCD 転送 `PERF send` 中央値 | 7.66–7.70 ms (5 boot) | 7.67–7.70 ms (5 boot) | **8.25–8.34 ms (3 boot、範囲が重ならない)** |
| 他コアの仕事: LAN gateway への HTTP GET（`pocket_http` タスク、JS 実行中） | 422 ms (n=14) | 428 ms | **488 ms (+15.6%)** |
| 音の再武装回数（tone 完了から再発行、15 秒） | 10 | 10 | 10 |
| 空きヒープ最小（promise_chain/base） | 83,760 B | 83,760 B | 83,760 B |
| 静的 DIRAM | 119,788 B | 119,788 B | 119,788 B |

**ホーム画面の fps では pin の有無を分離できない。** 同じバイナリの boot 間で
無 pin が 23.20 / 22.60 / 21.70 / 18.25 / 18.85 fps と **27% ばらつく**（背景が
毎 boot 違う庭を生成するため）。この幅は無 pin と core 1 の差を丸ごと飲み込むので、
fps でどちらが速いとは言わない。**分離できたのは `send` と JS 行列の方**で、
そちらは反復間 0.5% 以内に収まる。

**core 0 が高いのは構造的**: ESP-IDF は Wi-Fi ドライバタスクを core 0 に固定して作る
（起動ログ `wifi driver task: ..., core=0`、[vm-l1-clock.md](vm-l1-clock.md) §2）。
ファーム自身のワーカー（`sfx` 優先度 7、`pocket_http`、`wifi_*`）はすべて `xTaskCreate`
= 無指定なので、ui を core 1 に置くと OS が空いている側へ流せる。core 0 に置くと
描画・JS・無線が 1 つのコアに積まれ、**JS のターンだけでなく他コアの仕事（HTTP +15.6%）
まで悪くなる**。

**Wi-Fi は測れた。** [vm-l1-clock.md](vm-l1-clock.md) §2 は「資格情報が無いので
スキャンしか出来ない」と書いているが、現在は保存されており、`all` 条件の全ランで
`VMCOND wifi connected 192.168.1.42` が出て実際にリンク・HTTP まで通っている。
上の HTTP の行がその実測。

### 8.2 時計の実測(device)

`ui_task` を core 1 に固定した 4 ビルド（プローブ付き、他は同一）。drain の µs。

| ビルド | promise_chain/base 中央値 / p95 / 最大 | async_generator/base 中央値 / p95 / 最大 |
| --- | --- | --- |
| esp_timer, stride 4（現行の出荷値） | 730 / 897 / 1,053 | 4,138 / 6,720 / 7,144 |
| CCOUNT, stride 4 | 739 / 865 / 1,046 | 4,056 / 6,551 / 6,844 |
| esp_timer, stride 1 | 731 / 858 / 1,035 | 3,862 / 4,184 / 4,877 |
| CCOUNT, stride 1 | 738 / 873 / 1,056 | 3,806 / 4,432 / 4,818 |
| esp_timer, stride 1, **floor 0**（検査専用、出荷しない） | 729 / 900 / 1,060 | 4,728 / 6,736 / 7,041 |

**時計を替えても drain は動かない。** stride 4 でも stride 1 でも、CCOUNT と
esp_timer の差は promise_chain で **+8 µs（CCOUNT が遅い側）**、async_generator で
**−56 µs（CCOUNT が速い側）** と**符号が揃わず**、どちらも同じ器械が示す
ビルド間ばらつき（同じソース・違うビルドで turn 中央値が 6.50→5.91 ms と 9% 動く。
CLAUDE.md の「命令キャッシュのアラインメントで 15%」そのもの）より小さい。
**33 倍安い読み取りが、drain のどこにも現れない。**

読む回数の側から見ても同じ結論になる（推定、上の実測値から）: promise_chain は
1 ターン 41 ジョブ・drain 730 µs で、stride 4 なら時計は 9 回、stride 1 でも 33 回。
esp_timer の 833 ns（実測、vm-l1-clock.md §1）を掛けて 7.5 µs / 27.5 µs、
つまり最悪でも drain の 3.8%。**その 3.8% すら測定には出ていない。**

### 8.3 決定

1. **`ui_task` を core 1 に pin する（採用）。** `CONFIG_POCKET_UI_TASK_CORE` の既定を
   `1` にした。実測で core 1 は無 pin と区別できない（sync_loop と promise_chain の
   ターン中央値は 0.1% 以内、async_generator は 1.5〜2% で反復間ばらつきと同程度、
   転送 0.04% 以内、DIRAM 0 B）、core 0 は 5.6〜15.6% 高い。無 pin は「OS が選ぶ」
   ＝将来の負荷次第で core 0 に載りうる側なので、測って安い方を固定する。
   これは**時計とは独立に成り立つ判断**で、CCOUNT を採らなくても残る。

2. **CCOUNT は採用しない。** 測定が利益を示さないため（§8.2）。代わりに払う代償は
   小さくない — CCOUNT はコアごとのレジスタなので `ui_task` の pin と**永久に結び付く**し、
   `CONFIG_PM_ENABLE`（DFS）を入れた日に**黙って**間違える（CCOUNT は CPU サイクルを
   数えるので、周波数が動けばレートが動く）。**測って差が出ないものに、静かに壊れる
   結合を足さない。** clockbench の「無料だからやる」は読み取りコストの比（33 倍）を
   根拠にしていたが、その比が drain に現れないことがここでの実測。

3. **`VM_JOB_STRIDE` を 1 にする（採用）。** clockbench の 3 結論のうち
   「毎ジョブ読む」だけは実測が支持した — ただし clockbench の理由（読み取りが安いから）
   ではなく、**予算の超過が実際に縮むから**である。

   | ワークロード / 条件 | stride 4（drain 中央値 / p95 / 最大 µs） | stride 1 | 差 |
   | --- | --- | --- | --- |
   | promise_chain / base | 730 / 897 / 1,053 | 731 / 858 / 1,035 | 変化なし |
   | promise_chain / all | 730 / 1,104 / 13,210 | 728 / 1,170 / 13,115 | 変化なし |
   | async_generator / base | 4,138 / 6,720 / 7,144 | 3,862 / **4,184** / **4,877** | **p95 −38%、最大 −32%** |
   | async_generator / all | 4,424 / 10,964 / 32,982 | 4,429 / 12,377 / 34,610 | 差なし（後述） |

   async_generator の p95 が 2.5 ms 縮むのは、このワークロードの高いジョブが
   中央値のジョブではないからで、stride 4 の 1 区間に高いジョブが 3 つ入りうるという
   clockbench の (N−1)×0.49 ms がそのまま出ている。**同じ差が CCOUNT 側の 2 本でも
   再現する**（6,551 → 4,432 µs、−32%）ので、ビルド間ばらつきではない。

   **`all` 条件では効果が消える。** 音・UI・Wi-Fi が載ると drain の尾を決めるのは
   予算検査の粒度ではなく**プリエンプション**で（p95 11〜12 ms、最大 33 ms はどちらの
   stride でも同じ）、細かく測っても縮まない。**改善するのは「他に誰も走っていない
   ときの超過」だけ**であって、混んだ機械の応答性ではない。

   費用は測定に出ない: promise_chain は 1 ターン 41 ジョブで、stride 4→1 の drain は
   730 → 731 µs（同じ器械が CCOUNT との 8 µs 差は分離している）。

4. **実装は残す。** `CONFIG_POCKET_VM_CCOUNT`（既定 `n`）として入っており、
   `CONFIG_POCKET_UI_TASK_CORE < 0` では**コンパイルを拒否する**（`vm_clock.c` の
   `#error`）。採らない判断を、次に誰かが測り直せる形で残すため。

### 8.4 単位と折り返し（採否に関わらず正しくした）

CCOUNT は 32 bit で、240 MHz では 2^32/240e6 ≒ **17.9 秒で一周する**。したがって:

- **時計の型を `vm_tick_t = uint32_t` にした。** 差分は `(vm_tick_t)(now - start)` と
  **符号なし 32 bit** で取る。C の mod-2^32 規則により、真の経過が周期未満である限り
  折り返しをまたいでも差は正しい。8 ms のターンに対して周期は 3 桁上なので、
  **折り返しは分岐ではなく型で処理されている**。esp_timer 経路も同じ型に落とし込んで
  あり（µs を 32 bit に切る）、こちらの周期は 4,295 秒。
- **µs ↔ tick の変換場所は 2 箇所だけで、どちらもジョブごとではない。**
  (a) 予算を張るとき（`vm_budget_begin_full`）に `limit_us × vm_clock_ticks_per_us()`
  で tick へ — **1 ターンに 1 回の掛け算**。(b) drain が返るときに
  `(vm_tick_t)(now - start) / ticks_per_us` で µs へ — **1 drain に 1 回の割り算**
  （小さな定数で割る 32 bit 除算、数十サイクル。drain は ms 単位）。
  **ジョブごとの検査は tick のままの引き算と比較**で、掛け算も割り算も入らない。
- `limit_ticks` は 2^31 tick で飽和させてある。符号なし差分は周期未満の間隔しか
  区別できないので、周期以上の予算は「期限切れになったことが観測できない」予算に
  なるため。240 MHz で 8.9 秒であり、このファームが張る最長の予算（離脱ターンの
  50 ms = 12e6 tick）から 2 桁離れている — 到達しないことを保つための飽和。
- 予算の**外向きの単位は µs のまま**（`VM_TURN_BUDGET_US` / `VM_RUNAWAY_US` /
  `RUNAWAY` のログ行 / `budget.elapsed` を足し込む暴走ガード）。単位が変わったのは
  `vm_sched.c` の内側だけで、`guest.c` も `app_session.c` も 1 行も変わっていない。

### 8.5 この変更の費用（実測(build) / 実測(device)）

| 項目 | 値 |
| --- | --- |
| 静的 DIRAM（素の L1 ビルド） | **115,468 B = §2.2 の L1 と 1 バイトも変わらず（+0 B）** |
| Flash Code | 1,552,644 B（+32 B） |
| ホーム画面の空きヒープ（`memlog.py --port --check`） | idle_free 277,052 / app_free 120,856 / app_largest 69,632 / js 86,356 → `MEMLOG_OK within budget` |
| ホスト検査（corpus） | 33 件 × {asan, o2} × {既定, `--force-yield`, `--budget-jobs 1/3/7/16`} = **12 通りすべて 33 passed, 0 failed** |
| ホスト検査（Test262） | `test262.py --variant o2 --force-yield`: **7,501 pass / 194 fail / 0 skip、regressions: 0**（§3 の基準と一致） |
| 実機検査 | `SETTINGS_OK sound=ON categories, toggles, mute, app-return` / `capture_home` 4 モード全取得（mode 0/1/2 は 29.6–30.3 fps、mode 3 は 19.1–19.7 fps）/ `SMOKE_OK 20`（`MEM (277052, 159744)` が 10 周目と 20 周目でバイト一致 = リーク無し、`FAULT_RECOVERY_OK` 6 件） |

### 8.6 開いたまま残る問い

- **`VM_JOB_FLOOR=8` と、現れない読み取りコスト。** promise_chain は 1 ターン 41 ジョブで、
  stride 1 なら時計は 33 回読まれるはずで、esp_timer の 833 ns（実測、vm-l1-clock.md §1）
  なら +27 µs 出るはずである。**出ない**（730 → 731 µs、同じ器械が 8 µs 差は分離している）。
  floor が効いて「1 回の drain 呼び出しが 8 件に届いていない」のではないかと疑い、
  **floor を 0 にしたビルドを焼いて確かめた**（§8.2 の最終行）。結果は promise_chain
  729 µs で、やはり変わらない — floor は理由ではなかった。残る可能性は 2 つで、
  どちらも未確認（推定）: (a) この文脈での `esp_timer_get_time()` は clockbench が
  孤立して測った 833 ns よりずっと安い、(b) 41 ジョブが drain の外でも数えられている。
  **決定には影響しない** — 高い方の時計で測っても費用が出ない、という向きの疑問だから。
- **floor 0 は async_generator の尾を広げた**（p95 4,184 → 6,736 µs、stride 1 同士の比較）。
  予算を早く効かせるはずの変更が逆に働いており、floor は「前進保証」以上の役割を
  持っている可能性がある。スケジューラ定数の問題で、時計とは別に測るべき。
- **ホーム画面 mode 3 の fps は boot ごとに 27% 動く。** 背景が毎 boot 違う庭を生成する
  ため。ホームの描画に関する主張をするなら、この器械では**同一 boot 内の比較**か、
  庭を固定する手段が要る。
