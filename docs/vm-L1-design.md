# VM L1 設計: ジョブ境界の実行制御と起床（`vm/l1-host-sched`）

対象: [quickjs-freertos-vm-spec.md](quickjs-freertos-vm-spec.md) §6（L1）。根拠は [vm-L0-report.md](vm-L0-report.md) §2.1（実機の基準値）・§2.2（予算案）と [vm-ledger/03-jobs-interrupts.md](vm-ledger/03-jobs-interrupts.md)（1 ティックの実際の順序）。2026-09-12 時点。

本書は実装者が従う文書で、調査ではなく**決定**を書く。各決定には理由と、それを検査する項目（§7）を付ける。数値は **実測**（L0 §2.1、device / build / host を区別）か **推定** かを毎回明記する。

行番号の基準: `main/` と `components/` は本ブランチの HEAD `edc21ed`。`quickjs.c` は台帳 03 の基準版（`015fc62`）の行番号を台帳の事実番号とともに引く。`ui_qjs.c` は本作業ツリーにはまだ無く、読み取り専用の `.cache/pocketjs/hosts/esp-idf/components/pocketjs_ui_qjs/src/ui_qjs.c` を読んだ（§2.4 で取り込む）。

## 0. 要約（決めたこと）

| # | 決定 | 値 | 根拠（実測）|
| --- | --- | --- | --- |
| 1 | 予算はジョブ境界でだけ見る。時間が主、件数は時計を読む間隔と底と天井 | `VM_TURN_BUDGET_US=8000` / `VM_JOB_STRIDE=4` / `VM_JOB_FLOOR=8` / `VM_JOB_BACKSTOP=64` | F は 62 件 30.27 ms（0.49 ms/件）、D は 41 件 2.95 ms（0.07 ms/件）。件数だけでは 7 倍違う二つを同じに扱う |
| 2 | 残ったジョブは次ターンの**冒頭**、どの pump より前に片付ける。空になるまで新しい JS 呼び出し（pump の配送・`frame()`・キー）は保留 | `app_tick()` の `deadline` 再武装（app_session.c:492）の直後 | 台帳 03 事実 62 の順序 intake → resolve → `frame()` → drain を「1 本の drain」として保つ |
| 3 | 未処理 rejection は**キューが空になった境界**でだけ報告する。予算切れの境界では報告しない | `drain` が `JS_IsJobPending()==false` で終わったときのみ | rejections.js の「同じ drain 内で後から catch」が誤報になる |
| 4 | 起床は FreeRTOS の直接タスク通知（カウント型）。ui タスクは待てないので、L1 が置き換えるのはフレーム待ちの `vTaskDelay` だけ | main.c:698 の `vTaskDelay` → `ulTaskNotifyTake(pdTRUE, 残り)` | 完了→resolve の遅延の下限が「ターン長 + フレーム周期の残り」（audio 条件で A 98.58 ms、B 15.40 ms）|
| 5 | 暴走は「1 本の論理 drain が drain の中で使った時間」で検出し、ジョブ境界でセッションを終える。例外は投げない | `VM_RUNAWAY_US=250000` / `VM_RUNAWAY_JOBS=100000`（時計が死んだときの受け皿）| §5.2 の改訂。ターン数で数えた初版は正直な長い連鎖を殺した（§11.1）|
| 6 | 不変条件は `tools/vmtest/` で件数モード（決定的）の予算を掛けて既存コーパスとバイト一致で検査する | §7 の点検表 | 期待値の書き換えは新規ファイルだけ（README の規則）|

VM 本体（`quickjs.c`）は変更しない（§6 完了条件）。変更するのは `components/pocketjs_guest/src/guest.c`、新設の `components/pocketjs_guest/src/vm_sched.c` / `vm_clock.c`、取り込む `pocketjs_ui_qjs`、`main/app_session.c`、`main/main.c`、`main/pocket/pocket_app.c` の 1 行、`tools/vmtest/vmrun.c`。

## 1. 予算: どこで・何を・尽きたらどうするか

### 1.1 どこで見るか — ジョブ境界だけ

予算の判定は `drain_jobs()`（guest.c:179-215）のループ、`JS_ExecutePendingJob()`（quickjs.c:2176-2205、台帳 03 事実 7）の**呼び出しと呼び出しの間**にだけ置く。ジョブの内側では見ない。理由は三つ。

- `JS_ExecutePendingJob` は 1 件を取り出して `job_func` を呼び切って戻る（事実 7）。境界で止めれば、ジョブは常に完走しており、途中状態が無い。§3-6「中断は例外ではない」を VM を変えずに満たせる唯一の地点。
- ジョブの内側で止める手段は今の VM には割り込みハンドラ（捕捉不能な `InternalError: interrupted`、事実 27・37）しか無く、それは `finally` を飛ばし（事実 37）、`await` の Promise を永久に pending にする（事実 39-41）。予算に使えば、予算切れのたびにその状態を作る。
- 割り込みハンドラは §5 の暴走ガードのままにする。予算と暴走を同じ機構に載せると、旧 50 ms ガードの失敗（app_session.c:441-458 のコメント: 他タスクにプリエンプトされただけで殺した）を繰り返す。

判定はループの先頭で行う。1 件目の前にも見るが、`floor` があるので実際に止まるのは `floor` 件以降（§1.2）。

```c
/* vm_sched.c -- host-compilable: no esp headers, the clock is injected. */
vm_drain_status_t vm_sched_drain(JSRuntime *rt, vm_budget_t *b, unsigned *ran, JSContext **failed_ctx) {
    unsigned n = 0;
    for (;;) {
        if (!JS_IsJobPending(rt)) { *ran = n; return VM_DRAIN_EMPTY; }
        if (n >= b->backstop)     { *ran = n; return VM_DRAIN_YIELDED; }
        /* The clock is read once per stride and never before the floor:
         * the floor is what guarantees progress when frame() alone has
         * already spent the turn (sec.1.2), and the stride is what bounds
         * the overrun past the limit to stride * cost-per-job. */
        if (n >= b->floor && (n % b->stride) == 0 &&
            b->clock() - b->start_us >= b->limit_us) { *ran = n; return VM_DRAIN_YIELDED; }
        int r = JS_ExecutePendingJob(rt, failed_ctx);
        if (r < 0) { *ran = n; return VM_DRAIN_THREW; }   /* guest.c:185-190 unchanged */
        n++;
    }
}
```

`VM_DRAIN_THREW` の扱いは今のまま（`js_std_dump_error` して `ESP_FAIL`、残りは次の drain へ）。job_throw.js がその挙動を固定している。

### 1.2 予算の値

| 定数 | 値 | 意味 | 根拠 |
| --- | --- | --- | --- |
| `VM_TURN_BUDGET_US` | 8,000 µs | 1 ターンの JS（`frame()` + drain）の目標上限。起点はターン開始（`app_tick()` 冒頭の時計読み） | L0 §2.2 の提案値。30 fps の 33.3 ms から転送 7.7 ms（`PERF send`、実測(device)）と UI tick+draw 0.9〜1.5 ms（実測(device)）を引いた残りの約 1/3。完了→resolve の遅延はターン長がそのまま出る（実測: audio 条件で lat 中央値 = ターン長）ので、この値が遅延の上限になる |
| `VM_JOB_STRIDE` | 4 件 | 時計を読む間隔 | 超過の粒度 = stride × 単価。F の単価 0.49 ms（実測）で 2.0 ms、D の 0.07 ms で 0.28 ms。時計 1 回の費用が 1 µs 級なら毎件読んでも 62 µs/62 件で誤差だが（**推定**、clock-bench の結果で決める）、stride は時計の費用に依らず超過を有界にする側の数字なので残す |
| `VM_JOB_FLOOR` | 8 件 | キューが空でない限り、時計に関係なく必ず走らせる件数 | `frame()` だけで予算を使い切るターン（A 117.56 ms、C 49.75 ms、D 8.66 ms、いずれも実測 base 中央値）でも drain が 0 件にならないための前進保証。F の単価で 3.9 ms、これが floor が予算超過に足しうる最大 |
| `VM_JOB_BACKSTOP` | 64 件 | 時計が壊れていても止まる天井 | 実測の最大 drain 件数 68（F、all 条件）とほぼ同じ。時計が働くなら 8 ms で先に止まる（F なら 16 件前後）ので、測った全ワークロードで backstop が先に効くことはない。時計抽象が定数を返す（clock-bench 未着手のホストなど）ときの安全網 |

**ターン開始を起点にする理由**: 目標は「ターン長」であり、それは完了遅延の測定値そのもの（L0 §2.1「遅延の下限はターン長」）。drain 開始を起点にすると `frame()` の分だけ目標から外れる。`frame()` は L1 では分割できない（それは L2）。だから `frame()` が長いアプリでは予算は守れず、drain が floor まで縮むだけになる。**これは設計の限界であって欠陥ではない**: A / C の `frame()` は L2 の対象と L0 §2.2 が既に述べている。

各ワークロードで何が起きるか（実測の単価からの**推定**、時計の費用 0）:

| ワークロード | 今のターン（実測 base） | L1 後の 1 ターン | 変わること |
| --- | --- | --- | --- |
| D promise_chain | 8.66 + 2.95 = 11.6 ms、41 件 | `frame()` 8.66 → 予算超過 → floor 8 件（0.56 ms）→ 次ターン冒頭に 33 件（2.3 ms）→ 空 → pump → `frame()` … | 総量同じ。連鎖の完了が 1 ターン後ろへ。ターン長は約 11.6 ms のまま |
| F async_generator | 1.55 + 30.27 = 31.8 ms、62 件 | `frame()` 1.55 → 約 14〜16 件で 8 ms → 継続ターン 16 件 × 2 → 4 ターン目で空 → pump → `frame()` | ターン長 32 ms → 約 8〜10 ms。`frame()` は 4 ターン（約 130 ms）に 1 回。**完了の resolve も 4 ターンに 1 回**（§2.3）|
| E io_wait | 0.04 + 1.14 ms、2 件 | 変化なし | — |
| A / B / C | drain 0.01 ms | 変化なし（drain は空か 1 件） | — |

F で改善するのは画面・音声・OS の応答性（ターン長）であって、JS の完了遅延ではない。§6 の「通知取り込みの遅延改善と JS ハンドラ実行の遅延改善を別々に報告する」はこのため。

### 1.3 時計の抽象

時計の選択は clock-bench の結果を待つ。設計は時計を 1 箇所に閉じ込める。

```c
/* vm_clock.h */
typedef int64_t (*vm_clock_fn)(void);          /* monotonic, microseconds */
int64_t vm_clock_now_us(void);                 /* the one call sites use */
void    vm_clock_install(vm_clock_fn fn);      /* tests and clock-bench swap it here */
```

- 既定は `esp_timer_get_time()`（L0 プローブが使ったもの。`app_session.c:492` の deadline とも同じ時計なので、ターン起点を 1 回の読みで共有できる）。
- clock-bench が `esp_cpu_get_cycle_count()` を選ぶ場合、32 bit で 240 MHz なら約 17.9 s で巻く（**計算値**）。予算はターン内の差分にしか使わないので巻きは無害だが、`start_us` を保持する `vm_budget_t` はその時計の単位で持つ。差分だけを扱う契約を `vm_budget_t` のコメントに書く。
- ホスト（vmtest）は `clock_gettime(CLOCK_MONOTONIC)` か、件数モード（§7）では「常に 0 を返す時計」を入れる。`limit_us=0` なら時計を読まずに stride/floor/backstop だけで動く。**期待値と比較するテストは全部この件数モードで走らせる**。時間モードはホストでは決定的でない。

`vm_budget_t` は 1 ターンに 1 個、`app_tick()` / `app_overlay_tick()` の冒頭で `vm_budget_begin(&b, VM_TURN_BUDGET_US)` が時計を 1 回読んで作る。guest.c はそれを `pocketjs_guest_budget(guest, &b)` で受け取り、同じターンの `frame()` 後の drain と、次ターンの継続 drain（§2）が同じ構造体を見る。

### 1.4 尽きたら何が起きるか

- `vm_sched_drain` が `VM_DRAIN_YIELDED` を返す。`drain_jobs()` は **`ESP_OK`** を返す（失敗ではない）。guest に `jobs_pending=true`、`yields++`。
- 例外を投げない。ジョブを捨てない。rejection の報告は行わない（§3）。
- ターンはそのまま UI コアの tick と draw（ui_qjs.c:861-862）へ進み、描画と転送（app_session.c:539 以降）が走る。§6「未完了の drain の間にネイティブな描画・I/O 処理を許す」。
- 統計は `pocketjs_guest_stats_t` に `yields` / `continuations` / `jobs_pending` / `jobs_dropped` を足す（`struct_size` 付きなので ABI は保てる）。**既存のログ標識（`PERF …` など）の書式は 1 バイトも変えない。** 追加の観測は `CONFIG_POCKET_VM_PROBE` 時の `VMSCHED` 行だけ。

## 2. 継続の保証と互換モード

### 2.1 保証

「キューを残して戻ったターン」の次のターンは、**どの pump よりも前に**、そのキューを（予算の範囲で）走らせる。空になるまで、ホストから JS への新しい呼び出しは一切行わない。空になったターンだけが pump → `frame()` → drain の通常経路に進む。

これで「旧来の 1 本の drain」= 予算で切られた drain + 継続ターンの drain の連結、が成り立つ。その間に走るのは UI コアの tick/draw、レンダラ、転送、他タスクだけで、どれも JS を呼ばない。**ジョブの実行順は旧来と同一**（FIFO は `job_list` が保つ、事実 4・7）。変わるのは境界の間に壁時計が進むことだけ。

### 2.2 どこで強制するか — `app_tick()` の冒頭

```c
esp_err_t app_tick(uint32_t buttons) {
    deadline = esp_timer_get_time() + 250000;          /* app_session.c:492, kept */
    vm_budget_begin(&budget, VM_TURN_BUDGET_US);       /* one clock read; see sec.1.3 */
    pocketjs_guest_budget(guest, &budget);
    /* NOTE (2026-09-12): the first implementation read pocket_app_exit_requested()
     * HERE, and that was wrong -- see sec.11.2. The stop it asks for reaches the
     * guest as an uncatchable interrupt, so honouring it at the top of a
     * continuation turn kills job k+1 of a drain that pre-L1 ran to its end.
     * The flag is read below, on a turn that begins with an empty queue, which
     * is where pocket_app_pump() read it pre-L1. */
    if (pocketjs_guest_jobs_pending(guest)) {
        esp_err_t e = pocketjs_ui_turn_continue(binding, &frame);   /* sec.2.4 */
        if (e) return e;
        if (pocketjs_guest_jobs_pending(guest)) {
            /* Still not empty: nothing new reaches JS this turn. Keys are not
             * lost -- they are held for the first turn that runs the pumps. */
            deferred_buttons |= buttons;
            if (++continuation_turns >= VM_RUNAWAY_TURNS) return ESP_ERR_TIMEOUT;   /* sec.5 */
            goto present;
        }
    }
    continuation_turns = 0;
    if (pocket_app_exit_requested()) app_request_stop();   /* sec.11.2 */
    buttons |= deferred_buttons; deferred_buttons = 0;
    pocket_app_pump();          /* app_session.c:497-523, unchanged from here on */
    pocket_text_pump();         /* sec.11.3: the key path's JS half, deferred to here */
    ...
    e = pocketjs_ui_turn(binding, &input, &frame);
present:
    /* app_session.c:539-: damage plan, strips, board_present */
}
```

`app_overlay_tick()`（app_session.c:439-475）も同じ骨格。オーバーレイには UI コアが無いので継続は `pocketjs_guest_continue(guest)` を直接呼ぶ。オーバーレイ側の費用制御（`budget_us` 8,000 / 12,000 µs と `over_limit` 60 ターン、ui/overlay.c:50,63,323）は別機構のまま。L1 の予算をそれ以下にしてあるので、JS の drain が原因で費用制御が発火することはない（**推定**: floor の 3.9 ms を足しても 12 ms を超えない。ただし `frame()` の長さは制御外）。

### 2.3 「配送」の定義 — pump ごと

L1 での「JS への配送」= ホストが `JS_Call` でゲストのコードを呼ぶこと。`app_tick()` が呼ぶ 10 の pump（app_session.c:497-523）を読んで分類した。

| pump | ネイティブな取り込み | JS への配送 | 継続ターンで止まると |
| --- | --- | --- | --- |
| `pocket_app_pump` (pocket_app.c:729-757) | sleep の期限を `pocket_api_complete` に転記（734-742）。**exit の転記（733）は §2.2 のとおり冒頭へ持ち出す** | start hook の `JS_Call`（749 経由 `run_hook`）| sleep の完了記録が 1 ターン遅れる。resolve 自体が保留なので観測差なし。`lat` の値にはこの遅れが乗る（正直な数字）|
| `pocket_imu_pump` (pocket_imu.c:140) | — | `watch` 購読へ `pocket_api_sub_deliver`（pocket_api.c:294-320 の `JS_Call`）| 次の通常ターンでまとめて 1 回。IMU は状態通知なので取りこぼしではない |
| `pocket_io_pump` (pocket_io.c:1445) | UART 受信を `pocket_api_complete` | — | ドライバのリングバッファが保持。完了記録が遅れるだけ |
| `pocket_bridge_pump` (pocket_bridge.c:351) | — | イベント購読へ `sub_deliver` | 遅れる。ブリッジ側は ACK 待ちで保持 |
| `pocket_net_pump` (pocket_net.c:1279) | リンク／スキャンの完了記録 | lease 購読へ `sub_deliver` | 遅れる |
| `pocket_capture_pump` (pocket_capture.c:435) | 録音の完了記録 | — | 遅れる |
| `pocket_api_pump` (pocket_api.c:490-516) | — | **resolve / reject の `JS_Call`（475）**。これ自体は reaction を末尾に積むだけ（事実 53）| §6 が名指しで保留を求めるもの。resolve を先にしても FIFO は壊れない（reaction は残りの後ろに積まれる）が、「旧 drain の完了前に resolve が起きた」順序になる。**互換モードは保留する。** 保留しない「公平モード」は L1 の範囲外（§8）|
| `pocket_fs_pump` (pocket_fs.c:1880) | — | volume 購読へ `sub_deliver` | 遅れる |
| `pocket_av_pump` (pocket_av.c:1629) | — | player / power 購読へ `sub_deliver` | 遅れる |
| `pocket_ui_pump(buttons)` (pocket_ui.c:1130-1155) | `held_mask` の更新 | action 購読（press / release / repeat）へ `sub_deliver` | **押下は落とさない**: `deferred_buttons` に OR して、最初の通常ターンに `buttons | deferred_buttons` を渡す。エッジ検出は `held_mask`（pump 内でしか更新されない）との差なので、押下→通常ターンで press、次の `app_tick(0)`（main.c:500 の release フレーム、または次周の無キー）で release が出る。`input.held()` はジョブから読むと継続中は古い値を返す（文書化のみ）|
| `pocket_text_pump`（pocket_text.c、§11.3 で新設）| `pocket_text_key()` が main.c:499 で（= `app_tick()` の**外**で）行う IME・バッファ・キャレット・再描画 | onEdit / onSubmit / onCancel の `JS_Call` | 遅れる。打鍵は落とさない（イベントはキューに積まれ、空になったターンの pump で順番どおり配送される）|
| `frame()`（`pocketjs_ui_turn` → `pocketjs_guest_frame`、ui_qjs.c:858）| — | `JS_Call`（guest.c:420）と、その中の `onFrame` 配送（pocket_app.c:567）| 呼ばない。UI コアの tick/draw は呼ぶ（§2.4）|

**離脱ターンの例外**: `tick_run()` は Back で `app_tick(0x2000)` を 1 回呼んでから `app_request_stop()` する（main.c:490。ゲストに最後の保存機会を与える）。このターンが継続だけで終わるとアプリは保存フレームを失う。決定: `buttons` に `0x2000` を含むターンは継続の予算を `VM_LEAVE_BUDGET_US=50000` / backstop 256 に広げ、それでも残れば残したまま `frame(0x2000)` を呼ぶ。セッションはこの直後に終わる（§3.2）ので、順序の乱れが観測される機会は無い。50 ms は旧ガード 250 ms の 1/5 で、F の単価なら約 100 件（**推定**）。

### 2.4 `pocketjs_ui_qjs` の取り込みと継続の入口

`frame()` → UI tick → UI draw の順序を決めているのは `pocketjs_ui_turn`（ui_qjs.c:818-863）で、本作業ツリーには無い（L0 §6-4）。L1 はこれを `components/pocketjs_ui_qjs/` に**取り込む**（quickjs-ng と同じ規則: 最初のコミットは無改変のバイト列、改変は別コミット）。加える関数は 1 つ。

```c
/* Runs the continuation drain in place of frame(), then the core's tick and
 * draw exactly as pocketjs_ui_turn() does -- the display keeps animating while
 * the queue is worked off, which sec.6 allows and a frozen frame would not. */
esp_err_t pocketjs_ui_turn_continue(pocketjs_ui_qjs_t *binding, pocketjs_ui_frame_view_t *out_frame);
```

`pocketjs_guest_continue(guest)` は guest.c 側の新 API で、`vm_sched_drain` を回し、`VM_DRAIN_EMPTY` のときだけ rejection を報告する（§3.1）。`pocketjs_guest_frame` の drain（guest.c:439）も同じ関数を使う。

## 3. 未処理 rejection の報告時点

### 3.1 「旧 drain が終わった地点」の代わり

決定: **`vm_sched_drain` が `VM_DRAIN_EMPTY` を返した直後**（= `JS_IsJobPending()` が偽になったジョブ境界）だけで、`guest->rejections`（guest.c:150-177 のトラッカーが積む連結リスト）を消費して報告する。`VM_DRAIN_YIELDED` では触らない。

これは旧 `drain_jobs()` の報告地点（guest.c:191-214、ループがキューを空にした後）と**同値**である。理由:

- 旧来の 1 本の drain は L1 では「予算で切られた drain + 継続 drain 群」の連結（§2.1）で、その間にゲストの JS は 1 行も走らない。トラッカーは `perform_promise_then` でハンドラが付いたとき `handled=true` で呼ばれ（事実 19b）、guest.c:157-165 はそのとき該当エントリをリストから外す。予算切れの境界で報告を保留しておけば、継続ターン内の catch が旧来どおりエントリを消す。
- 逆に予算切れの境界で報告すると、rejections.js のケース 2（`late.catch` が 2 ジョブ後に付く）が、その 2 ジョブの間に境界が落ちたとき誤報になる。これが §6 の「同じ連鎖内の catch を誤って未処理と報告しない」の具体。

`VM_DRAIN_THREW`（ジョブが例外）は今のまま: 報告せず `ESP_FAIL`、残りと報告は次の drain（job_throw.js が固定）。ただし実機ではこの `ESP_FAIL` は `tick_run` → `end_run(e)`（main.c:517）でセッションを終える。L1 はそれも変えない。

### 3.2 ジョブを残したままセッションが終わるとき

経路: `end_run` → `app_stop()`（app_session.c:200-241）→ 各 `*_reset()` → `pocket_api_reset()`（pocket_api.c:534-546、armed の Promise を settle せず解放）→ `pocketjs_guest_destroy` → `JS_FreeRuntime` がジョブを**実行せずに**解放（quickjs.c:2294-2309、事実 9）。

決定: **この「実行せずに捨てる」を維持する。** 理由は §3-7（解放は所有規則に従う）と、既に `pocket_api_reset` が同じ選択をしている整合性。加えて:

- 捨てた件数を数えて `jobs_dropped` に入れ、`app_report()`（app_session.c:192）が `ESP_LOGW("app","jobs dropped at stop: %u")` を出す。新しい行であり、契約済み標識の書式には触れない。
- 残っていた `rejections` エントリは**報告しない**で解放する。捨てたジョブの中に catch があったかもしれず、報告は推測になる。これは今日の `pocketjs_guest_destroy` が既にしていること（guest.c の destroy 内、`while (guest->rejections)` で `JS_FreeValue` して `free` するだけ）で、L1 はそれを規則として書き留める。今日の `VMPROBE` 条件スクリプトが踏んだ「未処理 rejection 1 件でセッション終了」（L0 §2.1.1-2）とは別の話で、こちらは終了が先に決まっている場合。
- stop hook（pocket_app.c:772-797）は独自に `JS_ExecutePendingJob` を 200 ms 回す（793-796）。これは残ったジョブも hook のジョブも FIFO で走らせる。予算は掛けない: セッション終了中で、`APP_STOP_MS` と `stop_interrupt`（767-770）が既に上限。ただし §5.3 のとおりハンドラの登録経路は 1 本化する。

## 4. 起床と待機

### 4.1 正直な答え: ui タスクは今日、待てない

`ui_task`（main.c:537-700）は 30 fps のフレームループで、キー待ちはタイムアウト 0 の `xQueueReceive`（544）、フレーム末尾は `vTaskDelay(cap-held)`（698）。ホームの pet / FLOWER シーン、オーバーレイ、各画面の再描画がこのループに乗っており、JS が無くても止められない。ランタイムの所有者もこのタスク（`app_session.c` の static 群と全 pump がここから呼ばれる）。

「未処理ジョブもイベントも無いときだけ待つ」を文字どおり実現するには、JS の所有を専用タスクへ移し、`pocketjs_ui_turn` が返すフレームビューと `pocketjs_rgb565_prepare/render_strip`（app_session.c:540-558）が読む UI コアの状態を受け渡す構造が要る（§6 第 1 項）。その費用は専用タスクのスタック: ui タスクは 32 KiB（main.c:739）で、深い再帰時の残りが 8,252 B（実測(device)、`stack_hw`）= 約 24 KiB を JS が使う。同等の専用タスクは **静的 DIRAM +24〜32 KiB（推定）** で、L0 §2.2 の上限 +8 KiB を超える。無線を上げた状態の空きヒープ最小 14,768 B（実測）に対しても無理。**L1 では専用タスクを作らない。**

### 4.2 L1 が代わりに届けるもの

遅延の内訳（実測、L0 §2.1）: 完了→resolve は「進行中のターンの残り + フレーム周期の残り（最大 33 ms）」。前者は §1 の予算が縮め、後者をここで縮める。

決定: 通知は **FreeRTOS の直接タスク通知（カウント型、index 0）**。

```c
/* vm_wake.h -- the owner task's wake source. */
void vm_wake_bind(void);                 /* owner task, once at start: remembers its handle */
void vm_wake_post(void);                 /* any task: xTaskNotifyGive(owner) */
void vm_wake_post_from_isr(BaseType_t *hpw);   /* ISR: vTaskNotifyGiveFromISR */
uint32_t vm_wake_wait(TickType_t max);   /* owner only: ulTaskNotifyTake(pdTRUE, max) */
```

- `pocket_api_complete()`（pocket_api.c:455-467）が `done` を書いた**後**に `vm_wake_post()` を呼ぶ。完了記録の公開が先、起床が後（§7 の「通知側は完了記録を公開した後に起床要求を送る」）。
- **所有タスク自身が投稿した完了では起床しない**（`vm_wake_post()` が `xTaskGetCurrentTaskHandle()==owner` を捨てる、§11.4）。完了の大半は ui タスク自身が記録するもの（`pocket_app_pump` の sleep 期限、`pocket_io_pump` の UART、`pocket_net_pump` のリンク／スキャン、`tick_run` のピッカー結果）で、それらは**同じターンの** `pocket_api_pump()` が settle する。カウント型通知はそのまま残るので、除外しないとフレームキャップが毎回すぐ返り、周期が 33 ms ではなく下限の 8 ms に落ちる。縮めるべき待ちは「このタスクが予期できない完了」=別タスクと ISR のものだけ。
- `main.c:698` の `vTaskDelay(pdMS_TO_TICKS(rest))` を、ゲスト実行中（`running`）に限り `vm_wake_wait(pdMS_TO_TICKS(rest))` に置き換える。非実行時は今までどおり `vTaskDelay`（通知は誰も送らないので、送られても次周で消費されるだけ）。
- 早く起きたターンは `app_tick()` を普通に回す（pump → `frame()` → drain）。**`frame()` を飛ばした「完了専用ターン」は作らない。** 作ると「resolve → drain → `frame()`」の順になり、`.then` が次の `frame()` より前に走るという観測可能な変化がアプリに入る。互換モードを既定とする §6 の趣旨に反するので、L1 では入れない（§8）。
- 副作用: 完了が連続して届くアプリ（UART、10 ms の sleep ループ）は `frame()` が 30 fps より速く呼ばれる。`onFrame` の `dt`（pocket_app.c:557、実測時刻から計算）は正しい値を運ぶが、`frame()` の回数を時計代わりにするアプリは速くなる。**フレーム周期の下限 `VM_MIN_PERIOD_MS=8`** を置く: 早起きしても前回のターン開始から 8 ms 経つまでは `vTaskDelay` で埋める。8 ms は予算と同じで、これより短い周期で回しても転送（7.7 ms、実測）が追いつかない。

### 4.3 待機直前の競合で起床を失わない理由

タスク通知は**カウンタ**であり、条件変数ではない。`xTaskNotifyGive` は TCB の 32 bit 値を +1 し、`ulTaskNotifyTake(pdTRUE, …)` は値が 0 のときだけブロックし、戻るときに値を 0 に戻して読んだ値を返す（FreeRTOS の仕様。IDF v6.0.1 の `tasks.c` で確認すること。ここでは仕様として引く）。

```text
owner:    check(no job pending && no completion)  ....  ulTaskNotifyTake()
poster:                                  complete(); Give()
```

check と Take の間に Give が入っても、Take は値 1 を見て**ブロックせずに戻る**。Give が check より前なら、check が完了記録（`done`、pocket_api.c:466）を見る。`done` の公開が Give より前なので（順序の契約）、「記録は無いのに通知だけある」は起きず、「記録はあるのに通知が無い」は check が拾う。したがってどの交錯でも、次の Take は起きるか、check が仕事を見つけるかのどちらか。**失う経路が無い。**

なぜキューやセマフォでないか: 完了記録の置き場は既に `promises[]`（pocket_api.c:362、確保無し、ISR 安全）にあり、キューは同じものの複製になる。バイナリセマフォは通知と同じ意味を持つが、ヒープ上のオブジェクトを 1 つ増やす。FreeRTOS の文書は通知の方が軽いと述べているが、**本書はそれを測っていない**。選ぶ理由は「複製が無く、カウンタなので競合の証明が上のとおり短い」こと。

`configTASK_NOTIFICATION_ARRAY_ENTRIES` は IDF 既定 1。ui タスクで index 0 を他に使う箇所が無いことを `grep -rn "xTaskNotify\|ulTaskNotify\|vTaskNotify" main/` で確認してから使う（本書の時点で 0 件、grep で確認。IDF のコンポーネント側が ui タスクの通知を使うことは無いが、`components/` も同じ grep で見ること）。

### 4.4 優先度

ui タスクは優先度 5、input 6、デコーダ 6、音声 7（main.c:738-739、app_session.c:446-447）。§6 の「taskYIELD は低優先度タスクへの実行保証にならない」は、通知で ready にしてもより高い優先度が走っている間は待たされる、という意味で L1 にも当てはまる。L1 は優先度を**変えない**。変えるならフレーム時間の分布（L0 §2.1、ui / audio / all 条件）を同一バイナリで取り直してから。

## 5. 暴走ガード

### 5.1 今のガードと、予算が変える失敗の形

今: `app_session.c:84-87` の `interrupt()` が `stop_requested || now > deadline` を返し、`deadline` はターン冒頭で +250 ms（492、459）。発火すると捕捉不能な `InternalError: interrupted`（事実 27）。壁時計なので他タスクにプリエンプトされた時間も数える（コメント 445-450）。

予算導入後、単一ジョブの無限ループはこれまでどおりこのガードが止める（§6「既存の暴走停止ガードを維持する」）。**新しい失敗の形**は「ジョブがジョブを積み続ける」もの:

- `function f(){ Promise.resolve().then(f) } f()` — キュー長は 1 のまま**空にならない**。各ジョブは µs 単位で終わるので deadline は来ない。継続ターンが永遠に続き、pump も `frame()` も二度と走らない。画面は UI コアの tick で動き続けるのでアプリが死んだように見えない。
- ファンアウト（1 ジョブが 2 件積む）— キューが指数で伸び、`JS_EnqueueJob` の `js_malloc` が 160 KiB の上限で失敗して例外（事実 4）→ `VM_DRAIN_THREW` → セッション終了。**メモリ上限が先に効く**ので L1 の検出は不要。

### 5.2 検出と処置（2026-09-12 改訂）

決定: **1 本の論理 drain（予算で切られた drain + その継続群、§2.1）が `vm_sched_drain` の中で使った時間**で検出する。`VM_RUNAWAY_US = 250,000 µs`。時計が働かない環境（ホストの件数モード、壊れたタイマ）のための受け皿として、同じ論理 drain のジョブ総数 `VM_RUNAWAY_JOBS = 100,000` も見る。どちらも `pocketjs_guest_drain_total()` が持ち、キューが空になった時点で 0 に戻る。

**初版（`VM_RUNAWAY_TURNS = 30`）を捨てた理由。** ターンは「ゲストがどれだけ仕事を頼んだか」の単位ではない。ターンが終わるのは (a) 壁時計が 8 ms を告げたとき — 混んだ機械ではその大半は他タスクの時間である — か、(b) 64 件の backstop が告げたときで、後者は**ジョブがどれだけ安くても**効く。したがって「30 ターン」は安いジョブなら「1,920 件」を意味し、実測 0.07 ms/件（L0 のワークロード D）では JS 時間 134 ms、置き換えたはずの旧 250 ms ガードのおよそ 2 倍厳しい。実際に 2,500 段の正直な連鎖が終了コード 5 になった（§10.2 の欠陥 4、再現は `tools/vmtest/corpus/budget_honest_long_chain.js`）。**予算が壁時計であること自体はここでの欠陥ではない**が、「プリエンプトされた時間はゲストに課金されない」という `vm_sched.h` の旧コメントは誤りだった（壁時計の予算を通してターン数に変換され、課金されていた）。修正済み。

新しい数え方の性質:

- 合計するのは `vm_sched_drain` の中の時間だけで、`frame()`・pump・レンダラ・転送・他タスクの時間は入らない。旧ガードの 250 ms はそれら全部を含む壁時計だったので、**新ガードは旧ガードより必ず緩い**: 旧ファームが最後まで走らせた drain を、これが殺すことはない。
- drain の中でプリエンプトされた時間は依然として課金される。ホスト側の機構でそれを JS 時間と区別する手段は無い（`esp_timer` も CCOUNT もタスクの実行時間ではない）。答えは単位を細かくすることではなく、許容量を旧ガードと同じ桁に置くことである。
- キュー長は使わない（初版と同じ理由。`JS_EnqueueJob` へのフックは VM の変更で、L1 の完了条件に反する）。
- 処置は初版と同じ: `app_tick()` が `ESP_ERR_TIMEOUT` を返し、`end_run` → `app_stop()`。`jsconsole_set_error("JOB QUEUE RUNAWAY")`、ログは `ESP_LOGE("app","RUNAWAY one drain spent %lld us over %llu jobs in %u turns")`。**例外は投げない**ので、事実 37（finally の飛ばし）も事実 39-41（await の永久 pending）もこのガードは作らない。
- 離脱ターン（§2.3）は継続を待たず `frame(0x2000)` へ進むので、この合計の対象外。

ホストでの検査（件数モードでは時計を読まないので、判定できるのはジョブ総数の側）: `tools/vmtest/corpus/runaway_jobs.js`（`--runaway-jobs 2000`、無限に自分を積むジョブ → 終了コード 5）と `tools/vmtest/corpus/budget_honest_long_chain.js`（出荷時の `--runaway-jobs 100000`、2,500 件の正直な連鎖 → すべての予算で完走）。後者が、初版が殺していたプログラムそのものである。

### 5.3 割り込みハンドラの 1 本化

`JS_SetInterruptHandler` は単一スロットで（事実 22）、今は 3 箇所が上書き合戦をしている: guest.c:253（epoch 方式、常に負けて死んでいる、事実 46-47）、app_session.c:90（deadline）、pocket_app.c:781（stop hook 用）。決定: 登録は guest.c:253 の 1 箇所だけにし、ホストは述語を差し替える。

```c
/* guest.c keeps the one JS_SetInterruptHandler call; the host supplies the
 * predicate. NULL restores the guest's own epoch-based one. */
void pocketjs_guest_set_watchdog(pocketjs_guest_t *g, int (*fn)(void *), void *opaque);
```

`install_limits`（app_session.c:88-92）と `pocket_app_reset`（781）はこれを呼ぶ。挙動は同じ、登録の経路が 1 本になるだけ。`pocketjs_guest_interrupt()`（guest.c:449-454）は `pocketjs_ui_qjs_interrupt` から呼べるが呼び出し元が無い（事実 47）。L1 では削らず、述語が NULL のときだけ生きる形にする。

## 6. 変更点の一覧（実装順）

1. `components/pocketjs_guest/src/vm_clock.[ch]` — §1.3。ホストで `-DVM_HOST` ならライブラリ非依存。
2. `components/pocketjs_guest/src/vm_sched.[ch]` — §1.1 の `vm_sched_drain` と `vm_budget_t`。**esp ヘッダを含めない**。vmrun はこれを写さずリンクする（README の「写したもの」が L1 からは「同じもの」になる）。
3. `guest.c` — `drain_jobs` を `vm_sched_drain` 経由に、`pocketjs_guest_continue` / `pocketjs_guest_jobs_pending` / `pocketjs_guest_budget` / `pocketjs_guest_set_watchdog` を追加、stats のフィールド追加、destroy 時の `jobs_dropped` 集計（`JS_IsJobPending` を見てから `JS_FreeRuntime`。件数は `JS_ExecutePendingJob` を呼ばずに数える手段が無いので **pending だったか否かの 1 bit**。件数が要るなら L2 のフック）。
4. `pocketjs_ui_qjs` の取り込みと `pocketjs_ui_turn_continue` — §2.4。
5. `main/app_session.c` — §2.2 の骨格を `app_tick` と `app_overlay_tick` に。`deferred_buttons`、`continuation_turns`、離脱ターンの予算。
6. `main/pocket/pocket_app.c` — `exit_requested` の読み出しを関数に出す 1 行。
7. `main/main.c` — §4.2 の `vm_wake_wait` と `VM_MIN_PERIOD_MS`。`vm_wake_bind()` を `ui_task` 冒頭に。
8. `main/pocket/pocket_api.c` — `pocket_api_complete` 末尾に `vm_wake_post()`。ISR から呼ばれる面（IR 送信など）があれば `_from_isr` 版。呼び出し元を `grep -rn pocket_api_complete main/` で洗い、ISR 文脈のものを列挙してから。
9. `tools/vmtest/vmrun.c` — §7 のフラグ。
10. 計測: `tools/memlog.py --map … --port COM3 --check`（DIRAM 増分、**推定 +200 B 未満**、上限 +8 KiB）、L0 の行列を `vm-L0` から復元して同一バイナリで F / E / D の turn と lat を取り直す。ターン中央値 +5% 以内（ばらつき 5.6% の実測があるので、それ未満の差は主張しない）。

## 7. 不変条件と検査（実装者とレビュアーの共通点検表）

前提: `vmrun` は `vm_sched.c` をリンクし、次のフラグを持つ。`--budget-jobs N`（件数モード: 時計は常に 0、`floor=stride=N`、`backstop=N`。N 件ごとに `VM_DRAIN_YIELDED` で戻り、次の「ターン」= 継続 drain が続く。空になるまで `frame()` は呼ばない）、`--force-yield`（= `--budget-jobs 1`。README の弱シンボル `vmtest_vm_set_force_yield` はこれを指すように定義する。L1 のチェックポイント = ジョブ境界）、`--runaway-turns N`、`--host-events`（§7-6 用の完了シム）。終了コードに `5 = runaway` を足す。**期待値ファイルは既存のものを書き換えない**（README の規則）。新規ファイルだけ `--bless`。

| # | 不変条件 | 検査 | 合格の定義 |
| --- | --- | --- | --- |
| 1 | FIFO 順序 | `promise_chain.js`、`microtask_order.js` を `--budget-jobs 1 / 3 / 7 / 16` で | `expected/*.txt` とバイト一致（`run.sh --force-yield` と `run.sh --budget-jobs N` の両方）|
| 2 | then / catch / finally の観測順序 | `microtask_order.js`（38 行の順序）、`try_finally.js`、`generators.js` | 同上。特に finally の上書き（`override-fin-override`）と thenable の 2 段（`thenable-then-called` × 2）の位置 |
| 3 | マイクロタスクとフレームの境界 | 新規 `budget_frame_boundary.js`: eval で 100 段の連鎖を作り、`frame()` は番号を出力。`--frames 3 --budget-jobs 8` | 出力が `--budget-jobs 0`（無制限）と**同一**。`frame` が連鎖の途中に現れない。ターン数は `#info` に出し diff から外す |
| 4 | ジョブを半分に割らない | `generators.js` / `try_finally.js` / `closures.js`（async の中断中フレームを捕捉したクロージャ）を `--force-yield` で、ASan 版 | バイト一致、ASan / UBSan / LSan 報告 0。構造的にも: `vm_sched_drain` が `JS_ExecutePendingJob` の外でしか時計を読まないことをレビューで確認 |
| 5 | rejection の報告時点 | `rejections.js` を `--budget-jobs 1 / 2 / 3` で | バイト一致。**ケース 2（`handled-same-drain`）が報告されたら不合格** — 予算境界で報告している証拠 |
| 6 | 完了を失わない | 新規 `budget_completions.js` + `--host-events`: vmrun の `host.request(k)` が「k 番目のターン境界」で完了を投稿する Promise を返す。継続ターンの最中に 8 件投稿し、`frame()` を 1 回挟む | 8 つのハンドラが**投稿順に 1 回ずつ**走り、すべて継続 drain が空になった後の同じターンで走る。`job_throw.js` は不変 |
| 7 | 永久に飢えるキューが無い | 新規 `budget_starve.js`: `frame()` がジョブを 40 件積み、`--frames 10 --budget-jobs 8`（件数モードでは `frame()` の費用を予算超過に見立て、floor だけが走る条件になる）| 各ターンで少なくとも floor 件走り、10 ターン目までに連鎖が完了した回数を出力。`ceil(40/8)=5` ターンごとに 1 回 |
| 8 | 暴走の検出 | 新規 `runaway_jobs.js`: `function f(){Promise.resolve().then(f)} f()`。`--runaway-turns 30 --budget-jobs 8` | 終了コード 5、継続ターン数 30 を `#info` に、LSan 報告 0（捨てたジョブの argv が `JS_FreeRuntime` で解放される、事実 9）|
| 9 | ジョブを残した終了 | 新規 `stop_with_queue.js`: 1000 段の連鎖 + `--frames 1 --budget-jobs 4` | 終了コード 0、LSan 0、`E pocketjs_guest: Unhandled Promise rejection` が **1 行も出ない**（残った rejection は報告しない、§3.2）。`#info jobs_dropped=1` |
| 10 | Test262 の合格集合 | `test262.py --force-yield -j 8` と `--variant o2 --force-yield` | `test262-baseline.txt` から減らない（7,501 pass） |
| 11 | 予算無効時の費用 | `timing.py` を予算無効（`limit_us=0, backstop=UINT_MAX`）で | `bench_*` の中央値が `timing-baseline.txt` の p95 以内（README: p95 と中央値の差より小さい差は結果と呼ばない）|
| 12 | 実機（ホストでは不可） | `smoke_device.py --cycles 20`、`test_settings.py`、`capture_home.py`、`benchmark_app.py` | 全標識バイト一致。`memlog.py --port --check` の予算内。L0 行列の F / E / D を同一バイナリで再取得し、F の turn 中央値 ≤ 12 ms（**推定**の目標）、E の lat 中央値が 33 ms のフレーム周期成分を失っていること |

レビュアーは 1〜5・10 を「予算あり／なしで出力が同一」の観点で、6〜9 を「新規の期待値が §2・§3・§5 の文と一致するか」の観点で読む。

## 8. L1 に含めないもの

L2（ジョブの内側での中断が要る）:

- 長い同期 `frame()`（A 117 ms、C 50 ms、実測）の分割。opcode チェックポイント、`VM_YIELDED`、`--force-yield` の opcode 粒度（台帳 04）。
- 割り込みを例外でなくする改造（§3-6）と、割り込まれた `await` が永久 pending になる不具合（事実 39-41）の修正。
- キュー長の計測とそれによる暴走検出（`JS_EnqueueJob` へのフック）。捨てたジョブの**件数**も同じ理由で 1 bit 止まり。

L1 の範囲外だが L2 ではない（別途判断）:

- 専用の所有タスク（§4.1、DIRAM +24〜32 KiB 推定）。
- 公平モード: 継続 drain より前に `pocket_api_pump` の resolve だけを許す（FIFO は壊れないが旧 drain 完了前の resolve になる）。F 型のアプリの完了遅延を縮めるのはこれだが、互換モードを既定とする §6 の下では build 時選択の候補として記録に留める。
- `frame()` を飛ばす完了専用ターン（§4.2）。`.then` が次の `frame()` より前に走る順序変更を伴う。
- pump をネイティブ取り込みと JS 配送に二分する改修（§2.3 の表の「取り込み」列を継続ターンでも走らせる）。10 ファイルに触る割に、resolve が保留される以上、観測差は `lat` の数字だけ。
- GC 閾値（256 KiB > 160 KiB、L0 §2）と OOM 時の use-after-free。`main` にも効く不具合で、L1 と独立。

---

## 9. 実装の記録（2026-09-12、`vm/l1-host-sched`）

本節は設計ではなく**実装したものの記録**で、設計との差分をすべて名指しする。数値は 実測 / 推定 を毎回書く。

### 9.1 どこに何が入ったか

| 場所 | 中身 |
| --- | --- |
| `components/pocketjs_guest/include/pocketjs/vm_clock.h` / `src/vm_clock.c` | §1.3 の時計抽象。既定は device が `esp_timer_get_time()`、host が `clock_gettime(CLOCK_MONOTONIC)`。`vm_clock_install()` で差し替え |
| `.../vm_sched.h` / `src/vm_sched.c` | §1.1 の `vm_sched_drain` と `vm_budget_t`、定数 `VM_TURN_BUDGET_US` 8000 / `VM_JOB_STRIDE` 4 / `VM_JOB_FLOOR` 8 / `VM_JOB_BACKSTOP` 64 / `VM_LEAVE_BUDGET_US` 50000 / `VM_LEAVE_BACKSTOP` 256 / `VM_RUNAWAY_TURNS` 30。esp ヘッダを一切含まない |
| `guest.c` / `guest.h` | `drain_jobs` が `vm_sched_drain` 経由に。`pocketjs_guest_budget` / `_jobs_pending` / `_continue` / `_set_watchdog` を追加。stats に `yields` / `continuations` / `jobs_pending` / `jobs_dropped` |
| `components/pocketjs_ui_qjs` | `pocketjs_ui_turn_continue()`（§2.4） |
| `main/app_session.c` / `.h` | §2.2 の骨格、`arm_turn()`、`deferred_buttons`、`continuation_turns`、離脱ターンの予算、`present_frame()` の切り出し、`app_vm_watchdog()` |
| `main/pocket/pocket_app.c` / `.h` | `pocket_app_exit_requested()`、stop hook の割り込み登録を `app_vm_watchdog()` 経由に |
| `main/vm/vm_wake.[ch]` | §4.2 の起床。ISR 文脈は `xPortInIsrContext()` で内部判定 |
| `main/main.c` | `vm_wake_bind()`、フレームキャップの `vTaskDelay` → `vm_wake_wait`、`VM_MIN_PERIOD_MS` 8 |
| `main/pocket/pocket_api.c` | `pocket_api_complete()` の末尾（`done` 公開の**後**）に `vm_wake_post()` |
| `main/Kconfig.projbuild` | `CONFIG_POCKET_VM_SCHED`（既定 y）。off で予算が無制限になり、L1 前の挙動に完全に戻る |
| `sdkconfig.vmsched_off.defaults` | その off ビルドの重ね方 |
| `tools/vmtest/` | §7 のフラグと 5 本の新規コーパス |

### 9.2 設計どおりに実装できなかった / 足したもの

1. **`--stop-turns N` は設計に無いフラグ。** §7 の 9（キューを残した終了）は `--frames 1 --budget-jobs 4` で終了コード 0 と `jobs_dropped=1` を求めているが、継続ターンに上限が無ければ 1000 段の連鎖は 250 ターンで**完走してしまう**し、`--runaway-turns` を使えば終了コード 5 になって 0 にならない。「セッションが先に終わる」側の機構が要る。`--stop-turns N` は N 回の継続ターンでセッションを終わらせ、キューを実行せず捨て、終了コード 0 を返す。`app_stop()` の代役。
2. **vmrun の `--runaway-turns` は既定で無効**（ファームの既定は 30 のまま）。`--budget-jobs 1` は正当な 200 件の drain を 200 回の継続ターンにするので、既定 30 では §7 の 1・2（`promise_chain.js` などを `--force-yield` で）が全部 runaway で落ちた。暴走検査は `--runaway-turns 30` を明示する。
3. **`present_frame()` の切り出しは設計に無い。** §2.2 の骨格は `goto present` で書かれており、実装では `app_tick()` の後半（damage plan・ストリップ・`board_present`・PAINT 集計）を `present_frame()` に切り出して継続ターンからも呼ぶ形にした。挙動は同じで、`goto` がラベルを跨いで初期化を飛ばす問題を避けるための形の違い。
4. **`jobs_dropped` は `pocketjs_guest_stats()` が毎回 `JS_IsJobPending()` を読む。** `app_report()` は `pocketjs_guest_destroy()` より**前**に走るので、destroy 時に latch するだけでは常に 0 になる。設計の「1 bit」はそのまま。
5. **継続ターンも `turn_sum` / `ticks` に数える。** `PERF` / `PAINT` の書式は 1 バイトも変えていないが、`turn_ms` の意味を「JS が走ったターンの平均」に保つために継続ターンも母数に入れた。入れないと安いターンだけの平均になる。
6. **時計は `arm_turn()` で `deadline` とは別に読む。** §1.3 は「1 回の読みで共有できる」と書いているが、`vm_clock` がどの時計を使うかは `vm_clock` の決定（clock-bench の 実測 では CCOUNT が systimer の 1/33 の費用）であり、`esp_timer` 単位の値を渡すとその決定が変わった日に黙って壊れる。1 ターンあたり時計 1 回の追加で、費用は 実測 25 ns（CCOUNT）〜833 ns（systimer）。
7. **未実装: §7 の 12（実機）。** ホストのみで完結させる作業だったので、`smoke_device.py` / `test_settings.py` / `capture_home.py` / `benchmark_app.py` と L0 行列の再取得、`memlog.py --port --check` の実測ヒープは**まだ走らせていない**。焼く前にこれが要る。
8. **未実装: `pocketjs_guest_interrupt()` の epoch 経路。** §5.3 のとおり削らず残し、述語が NULL のときだけ生きる形にした。呼び出し元は今も無い（事実 47）。

### 9.3 測ったもの

- **コーパス（実測(host)）**: 28 件（既存 23 + 新規 5）が、予算なし・`--budget-jobs 1 / 3 / 7 / 16`・`--force-yield` のすべてで、asan と o2 の両方でバイト一致。既存の `expected/*.txt` は 1 バイトも書き換えていない。
- **Test262（実測(host)）**: `--force-yield` で asan / o2 とも **7,501 pass / 194 fail / 0 skip**。`test262-baseline.txt` から減っていない。
- **静的 DIRAM（実測(build)）**: 115,372 B（`cd5117d`、L1 前）→ **115,420 B**（`ab2eb11`）= **+48 B**。内訳は `app_session.c.obj +40` / `vm_clock.c.obj +4` / `vm_wake.c.obj +4` で、合計が全差分と一致する（`vm_sched.c` と `guest.c` の DIRAM は 0）。上限 +8 KiB に対して 0.6%。同じ `sdkconfig.defaults` から生成した別々の `sdkconfig` で、probe off の 2 ビルドを比較したもの。
- **Flash Code（実測(build)）**: 1,550,840 → 1,552,016 = **+1,176 B**。`CONFIG_POCKET_VM_SCHED=n` のビルドは 1,551,812（DIRAM は同じ 115,420）。
- **実機の数値は 1 つも無い。** ターン長・完了遅延・空きヒープはすべて未測定。

---

## 10. 独立レビューの記録（2026-09-12、ホストのみ）

実装報告を疑って読み直し、ホストの全スイートを自分で走らせた結果。**実機には一切触れていない**ので、§7 の 12 は依然として未実施のままである。

### 10.1 走らせて確認したもの（実測(host)）

| スイート | 結果 |
| --- | --- |
| コーパス（新規 3 件込み 31 件）× {asan, o2} × {予算なし, `--force-yield`, `--budget-jobs 1/3/7/16`} | 12 通りすべてで 31/31 バイト一致。既存 `expected/*.txt` は 1 バイトも書き換えていない |
| Test262 `--force-yield` asan / o2 | 7,501 pass / 194 fail / 0 skip、`regressions: 0`（基準と同一） |
| `timing.py`（-O2、予算オフ） | 8 本中 6 本の中央値が基準の p95 以内。`bench_alloc` 26.93（p95 26.78）と `bench_calls` 56.48（p95 56.03）は p95 を 0.6〜0.8% 超えたが、基準の p95−中央値（それぞれ 1.77 / 2.25 ms）より小さく、README の規則で「結果」と呼べる差ではない。加えて基準記録時と `quickjs.c` の sha1 が違う（`271d718782c1` → `30877d7c8a45`）ので、そもそも同一バイナリの比較ではない |
| `tools/build_pocket_text_test.sh` | all passed |
| `tools/build_pocket_random_test.sh` | **L1 が壊していた**（10.2 の欠陥 3）。直してから `POCKET_RANDOM_OK` |
| `tools/build_pocket_capture_test.sh` | built |
| ファーム両ビルド（probe on / `CONFIG_POCKET_VM_SCHED=n`） | どちらも警告なしでビルド成立。sched off の DIRAM 115,452 B は実装報告の 115,420 B + 本レビューの修正 32 B と一致する |

### 10.2 見つけた欠陥

**欠陥 1（修正した）— `jobs dropped at stop` は決して出力されない。** `app_stop()` は `pocketjs_guest_destroy(guest)` を呼び `guest=NULL` を代入した**後**に `app_report()` を呼ぶ（app_session.c）。`app_report()` は `if(guest) pocketjs_guest_stats(...)` なので、そこでの stats は常に全ゼロ = `jobs_dropped` は常に偽。§3.2 がホストに言わせたかった 1 行は、書かれてから一度も出力可能になっていない。実装報告の逸脱 4 は「`app_report()` が `pocketjs_guest_destroy()` より**前**に走るので destroy 時の latch は常に 0 になる」と書いているが、順序は逆である。修正: guest がまだ生きている destroy 直前（= stop hook が 200 ms を使い切った後、捨てられるものが確定した地点）で `final_stats` に latch し、`app_report()` は guest が無いときそれを読む。`MEM` 行は**触っていない** — `tools/memlog.py` が `js=` を正規表現で拾い、start / stop の対を予算検査に使っているため。

**欠陥 2（修正した）— 離脱ターンの `frame(0x2000)` が黙って落ちる。** `main.c:509` は `if(leave) { e=app_tick(0x2000); app_request_stop(); }` で、この 1 回がゲストの最後の保存機会であり、直後にセッションが終わるので「次のターン」は存在しない。しかし `app_tick()` は離脱ターンも他と同じ継続分岐に入れ、キューが空にならなければ `deferred_buttons|=0x2000` して戻っていた = 保存の合図は誰にも配られない。しかも忙しくてキューが残っているアプリ、つまり保存が最も要る側でだけ起きる。設計 §5.2 は明文で逆を書いている（「離脱ターンは継続を待たず `frame(0x2000)` へ進むので、このカウンタの対象外」）。修正: `leaving` のときは継続 drain を（`VM_LEAVE_BUDGET_US` / `VM_LEAVE_BACKSTOP` の広い予算で）走らせた上で、空にならなくても pump → `frame(0x2000)` へ抜ける。継続カウンタにも数えない。

**欠陥 3（修正した）— `tools/build_pocket_random_test.sh` がコンパイルできない。** `main/pocket/pocket_api.c` が新設の `main/vm/vm_wake.h` を include し、そのホストビルドの `-I` に `main/vm` が無いため `fatal error: vm_wake.h: No such file or directory`。CLAUDE.md が名指しで警告している失敗の形そのもの（`main/` にディレクトリを足したら `grep -rn "main/" tools/` で参照元を洗う）で、実装者はこのスイートを走らせていない。修正: `tools/hostshim/vm_wake.h` にスタブを置いた（`vm_wake_post()` の呼び出しは `CONFIG_POCKET_VM_SCHED` の内側で、ホストはそれを定義しないので、必要なのは名前が解決することだけ）。

**欠陥 4（未修正 / 設計の判断）— 出荷時の定数で、正直な長い drain が暴走として殺される。** `VM_JOB_BACKSTOP=64` は時計と無関係の硬い天井なので、1 継続ターンは**どれだけ安くても** 64 件で終わる。すると `VM_RUNAWAY_TURNS=30` は「30 × 64 = 1,920 件を超える 1 本の drain はセッション終了」を意味する。§5.2 は 30 を「30 × 8 ms = 240 ms の JS 時間 ≒ 旧 250 ms ガード」と正当化しているが、その等式は**各ターンを時計が終わらせる場合にだけ**成り立つ。実測の Promise 連鎖（L0 のワークロード D、0.07 ms/件）では 64 件は 4.5 ms で、時計は一度も効かない — つまり新ガードが許すのは 134 ms 相当で、置き換えた旧ガードの 250 ms よりおよそ 2 倍厳しい。

証拠はコーパスの中にある: `promise_chain.js`（3,000 段の then）は `--runaway-turns 30` を明示すると `--budget-jobs 8 / 16 / 64` のいずれでも**終了コード 5**（`#info turns=30 max_run_turns=30 jobs_dropped=1`）になる。コーパスがこれに気づかないのは、vmrun の `--runaway-turns` が既定で無効だからである（実装報告の逸脱 2）。実装者はこの衝突をハーネス側で観測しておきながら（「既定 30 では §7 の 1・2 が全部 runaway で落ちた」）、同じ衝突が実機の 8 ms 予算でも起きることを追っていない。再現は `tools/vmtest/known/runaway_vs_honest_chain.js`。

これを直すには「1 本の論理 drain が消費してよい JS 時間の総量」を決め直す必要がある（ターン数ではなく時間で数える、あるいは backstop が終わらせたターンを数えない、など）。どれも §5 の決定そのものなので、コード側で黙って数字を変えず、実装者・設計者へ差し戻す。

**欠陥 5（未修正 / 設計の判断）— `deferred_buttons` は 2 つの打鍵を 1 フレームに融合する。** `main.c:519` は `app_tick(buttons)` の直後に `app_tick(0)` を呼び、「連続した打鍵が別物として届く」ことを離鍵フレームで保証している。継続ターンが続く間に別々の打鍵が 2 回届くと、`deferred_buttons |= buttons` はそれを 1 つのマスク（例: UP|RIGHT）にまとめ、ゲストは同時押しを 1 フレームで見る — 予算導入前には起こり得ない入力である。設計 §2.2 が `|=` をそのまま指定しているので、これも設計側の判断として差し戻す（キューにするなら離鍵フレームの対も作り直す必要がある）。

**所見（修正不要）— `vm_budget_restart()` は呼び出し元が無い。** 継続ターンは `arm_turn()` が毎ターン新しい予算を張るので、この関数は現状どこからも使われていない。

### 10.3 足したホスト検査（`tools/vmtest/corpus/`）

いずれも「予算をどこで切っても出力が同じ」を要求する形で、上の 12 通りすべてで一致することを確認した。

- `budget_reject_far_catch.js` — 不変条件 5 を `rejections.js` より遠くまで押す。catch が 70 件先（= `VM_JOB_BACKSTOP` の外）、rejection が drain の 30 件目で**生まれて** 40 件先で捕まる、`frame()` が積んだ連鎖の中だけで完結する、の 3 形。どれも報告されてはならない。対照として誰も捕まえない 1 件を置き、これは必ず報告される（報告そのものを止めた実装が通らないようにするため）。
- `budget_boundary_exact.js` — 不変条件 6 を境界そのもので。ジョブの中から `k=0` で要求した完了（= その drain が到達する境界でちょうど準備できる）、完了ハンドラの中から要求した完了（再入）、その完了が積んだ連鎖の最中に記録された完了。配送は必ず「キューが空になったターン」で、記録順に 1 回ずつ。
- `budget_teardown_live.js` — §3.2 を `stop_with_queue.js` より重い形で。await で中断した async 関数 4 本（到達しない `finally` 付き）、try/finally の中で止まった generator、要求が残った async generator、切断の向こう側にある thenable、catch が捨てられるジョブの中にある rejection。終了コード 0、報告 0 行、`#info jobs_dropped=1`、LSan 0。
- `known/runaway_vs_honest_chain.js` — 欠陥 4 の再現。予算に依存する結果なのでコーパスには置けない（`run.sh` は全コーパスを複数の予算で回してバイト一致を要求する）。

---

## 11. 第二次レビューへの対応（2026-09-12、ホストのみ）

外部レビューが L1 に対して 4 件を挙げた。3 件は再現し、1 件は根拠が一部誤っていたので、正しい部分だけを直した。**実機には触れていない**（§7 の 12 は依然として未実施）。

### 11.1 暴走ガードが正直なアプリを殺す（再現・設計変更で修正）

指摘: 8 ms 予算は壁時計なので、プリエンプトされた時間もターンに課金され、ターン数で数える暴走ガードは正当なアプリを殺す。

再現（実測(host)）: 出荷時の定数に相当する `--runaway-turns 30 --budget-jobs 64` で、2,500 段の正直な連鎖が**終了コード 5**。これは §10.2 の欠陥 4 が「設計へ差し戻す」と書いて放置していたもので、今回設計を決め直した。

修正: §5.2 を改訂し、ガードを「1 本の論理 drain が `vm_sched_drain` の中で使った時間」（`VM_RUNAWAY_US=250000`、時計が死んだときの受け皿として `VM_RUNAWAY_JOBS=100000`）に置き換えた。実装は `vm_budget_t.elapsed`（drain 1 回の所要時間、返り道 1 本で必ず書く）→ `guest.c` が論理 drain ごとに合算 → `pocketjs_guest_drain_total()` → `app_session.c` の `drain_runaway()`。合計するのは drain の中の時間だけなので、`frame()`・pump・描画・転送を全部含んでいた旧 250 ms 壁時計ガードより必ず緩い。

レビューの主張のうち**誤っていた点**: 「`vm_sched.h` のコメントが言う『プリエンプトはゲストに課金されない』は偽」は正しい（コメントを直した）。一方「予算切れのターンは常に floor 8 件になる」は一般には成り立たない — floor を超えた件数は `n >= floor && n % stride == 0` の刻みでしか判定されないので、時計が既に超過していれば 8 件で止まるのは事実だが、それが「暴走判定の原因」ではない。原因は backstop とターン計数の組み合わせ（§5.2）で、時計が一度も効かない安いジョブでも起きる。

検査: `tools/vmtest/corpus/budget_honest_long_chain.js`（新規、旧 `known/runaway_vs_honest_chain.js` を格上げ。出荷時の `--runaway-jobs 100000` で全予算で完走）、`corpus/runaway_jobs.js`（`--runaway-jobs 2000` に変更。期待値ファイルは判定文の変更に合わせて書き換えた。件数は予算で変わるので `#info runaway_jobs=` へ移した）。

### 11.2 `exit()` の冒頭読みは順序を変える（再現・修正）

指摘: `app_tick()` の冒頭に持ち出した `pocket_app_exit_requested()` は、継続ターンで `app_request_stop()` を呼び、次の JS 呼び出し = 同じ論理 drain のジョブ k+1 を捕捉不能な `InternalError` で殺す。予算の落ちた位置で `.finally` が走ったり走らなかったりする。

確認: `stop_requested` は `interrupt()`（app_session.c）が読み、発火は捕捉不能（事実 27）。旧実装では `exit_requested` を読むのは `pocket_app_pump()`（cd5117d の pocket_app.c:733）だけで、それはキューが空になったターンでしか走らない。つまり冒頭読みは**保存ではなく意味変更**で、レビューの指摘どおり。

修正: 読み出しを継続分岐の**後ろ**（pump の直前）に戻した。`app_tick()` と `app_overlay_tick()` の両方。「長い連鎖から exit したアプリが連鎖の終わりまで生き続ける」という冒頭読みの理由づけは旧来の挙動そのものであり、その連鎖は §5.2 の暴走ガードが有界にする。

検査: `tools/vmtest/corpus/budget_exit_midchain.js`（新規）。vmrun に `host.exit()`（`--host-events`）と、停止要求後に 1 を返す割り込みハンドラを入れて実機の経路を写した。8 段の連鎖の 3 件目で exit し、4〜8 件目・`finally`・その後の `then` が**どの予算でも**同じ順序で走ることを要求する。冒頭読みに戻すと出力が exit の行で切れる（実測(host): 実際に戻して確認した）。

### 11.3 `pocket.input.text` の打鍵は `app_tick()` の外から JS を呼んでいた（再現・修正）

指摘: `main.c:499` の `pocket_text_key()` は `app_tick()` より前に走り、`fire()` が `JS_Call` でゲストの onEdit / onSubmit / onCancel を呼ぶ。ジョブが残っているターンでは、1 本の論理 drain の途中に JS が入る。§2.3 の表は 10 の pump と `frame()` しか数えていなかった。

確認: そのとおり。ただしレビューが併記した「`pocket_text_reset()` の on_cancel も同じ経路」は**誤り** — `pocket_text_reset()` は onCancel を**発火しない**（pocket_text.h の契約「Closes any open session WITHOUT firing onCancel」、実装は `detach(); destroy(s);`）。

修正: 打鍵はホスト側の半分（IME・バッファ・キャレット・再描画・submit/cancel の自動 close）をその場で行い、ゲストのコールバックは**キューに積む**。`pocket_text_pump()` が `app_tick()` の pump 段（= キューが空のターン）で、積まれた順に配送する。イベントはヒープ（セッションと同じく 1 件 1 確保）で、静的 DRAM は 8 B しか増えない — 静的配列にすると 1,108 B 増えることを実測(build)したので捨てた。遅延は通常時変わらない（同じフレームの後半で配送される）。

検査: `tools/test_pocket_text.c` に case 8（打鍵では誰も呼ばれず、pump で、打った文字のまま届く。1 打鍵が生む 2 イベントの順序も）と case 9（配送前に `pocket_text_reset()` が来たら発火せずに解放する）を追加。既存 7 ケースのヘルパは「1 打鍵 = key + pump」= 1 フレームに直した。ASan/UBSan/LSan つきで 9 ケース全通過（実測(host)）。

### 11.4 所有タスク自身の完了で起床していた（再現・修正）

指摘: 完了の大半は ui タスク自身が投稿する。`vm_wake_post()` は `xTaskNotifyGive(owner)` を無条件に呼ぶので、投稿した本人が待つときカウントが 1 のまま残り、`vm_wake_wait()` が即座に返る。フレームキャップが下限 8 ms に潰れる。

確認: `main/vm/vm_wake.c` に自タスク判定は無く、`pocket_api_complete()` は ui タスクの pump 群（sleep 期限・UART・net・ピッカー）から呼ばれる。同じターンの `pocket_api_pump()` が settle するので、その通知は「もう配ったものを取りに行く」ための起床になる。

修正: ISR でない、かつ現在のタスクが所有タスクなら投稿しない。縮めるべきなのは「このタスクが予期できない完了」（別タスク・ISR）の待ちだけ。

**未測定**: この 4 件の効果はすべてホストとコードの上での確認で、フレーム周期・完了遅延・空きヒープの実機値は取っていない。§7 の 12 は焼く前に必要。

### 11.5 走らせたスイート（実測(host)）

| スイート | 結果 |
| --- | --- |
| コーパス 33 件 × {asan, o2} × {予算なし, `--force-yield`, `--budget-jobs 1/3/7/16`} | 12 通りすべて 33/33 バイト一致。既存の `expected/*.txt` は `runaway_jobs.txt` を除き 1 バイトも変えていない（その 1 件は §5.2 の判定文の変更に伴うもの）|
| Test262 `--force-yield` asan / o2 | どちらも 7,501 pass / 194 fail / 0 skip、`regressions: 0` |
| `tools/build_pocket_text_test.sh` | 9/9 all passed（ASan/UBSan/LSan）|
| `tools/build_pocket_random_test.sh` / `_capture_test.sh` | `POCKET_RANDOM_OK` / built |
| ファーム両ビルド（既定 / `CONFIG_POCKET_VM_SCHED=n`）| 警告なしで成立。DIRAM 115,468 B（前回記録 115,452 B から **+16 B**）、Flash 1,552,612 B |

`--budget-jobs 64` は元から文書化された行列の外で、`stop_with_queue.js`（自前で `--budget-jobs 4 --stop-turns 20` を指定している）が 1,000 段の連鎖を走り切ってしまい不一致になる。予算を上書きされたときのそのファイルの性質で、本修正とは無関係。
