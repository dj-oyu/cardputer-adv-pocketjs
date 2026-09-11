# 03: ジョブキューと割り込み

対象: `components/quickjs-ng/quickjs-ng/quickjs.c`（vendored quickjs-ng 0.14.0 + immutable-buffer patch）、
`components/pocketjs_guest/src/guest.c`、`main/pocket/pocket_api.c`、`main/app_session.c`。
各項目は `quickjs.c:<行>` などファイル:行で該当箇所を指す事実の記述。判断・提言は末尾の「L2/L3/L5 に効く点」のみ。

## ジョブキューの構造と所有者

1. `job_list` は `JSContext` ではなく `JSRuntime` のフィールド。`struct JSRuntime` は quickjs.c:268 で始まり、`interrupt_handler` / `interrupt_opaque`（quickjs.c:312-313）、`host_promise_rejection_tracker`（quickjs.c:321-322）、`job_list`（quickjs.c:324、コメント `/* list of JSJobEntry.link */`）は全て同じ `JSRuntime` 内にある。1つのランタイムに複数の `JSContext` をぶら下げた場合、ジョブキュー・割り込みハンドラ・rejection tracker は**全コンテキストで共有**される（quickjs.c:268-334）。現行ファームはギルドごとに1ランタイム1コンテキストなので今は無関係だが、L2/L3 でランタイムを共有する設計にするとこの3つが衝突点になる。
2. `JSJobEntry` は可変長構造体で `argv[]` を末尾に持つ（quickjs.c:979-985）。`ctx` フィールドを持つので、ジョブは「どのコンテキストで実行するか」を自分で運ぶ。
3. `rt->job_list` は `JS_NewRuntime` 相当の初期化で `init_list_head(&rt->job_list)`（quickjs.c:1985）。

## JS_EnqueueJob / 実行順序

4. `JS_EnqueueJob`（quickjs.c:2138-2159）は `js_malloc` で `JSJobEntry` を確保し、`argv` の各要素を `js_dup` で参照カウントを増やしてコピーし、`list_add_tail(&e->link, &rt->job_list)` でキューの**末尾**に追加する。失敗時（`js_malloc` が NULL）は -1 を返すのみで、呼び出し元がそれぞれ例外を立てる。
5. `JS_IsJobPending`（quickjs.c:2161-2164）は `!list_empty(&rt->job_list)`。
6. `JS_GetPendingJobContext`（quickjs.c:2166-2172）は先頭ジョブ（`job_list.next`）の `ctx` を返す。
7. `JS_ExecutePendingJob`（quickjs.c:2176-2205）は `job_list.next`（＝先頭＝一番古いジョブ）を1件取り出して `list_del` し、`job_func` を呼び、`argv` を `JS_FreeValue` してから `js_free` でエントリ自体を解放する。**追加は末尾・取り出しは先頭**なので job_list は厳密な FIFO。同時に複数実行されることはなく、1回の呼び出しで1件だけ実行して戻る（呼び出し側がループで回す設計）。
8. 戻り値は「例外なら -1、ジョブなしなら 0、成功なら 1」（quickjs.c:2174-2175 のコメント通り）。実行結果が例外（`JS_IsException(res)`）でも、そのジョブは既にキューから外れており、以降のジョブは影響を受けずに残る——ただし呼び出し元が `< 0` を見てループを止めるかどうかは呼び出し元の自由（後述、guest.c の `drain_jobs` は止める）。
9. `JS_FreeRuntime`（quickjs.c:2294-2309）はランタイム破棄時に `rt->in_free = true` を立てた上で `job_list` を走査し、各エントリの `argv` を `JS_FreeValueRT` で解放して `js_free_rt` するだけで、**`job_func` は一切呼ばない**。つまり未実行のジョブ（保留中の Promise reaction や dynamic import 等）はランタイム破棄時に黙って破棄され、対応する Promise は解決も拒否もされないまま消える。

## ジョブを作る4種

10. Dynamic import: `JS_EnqueueJob(ctx, js_dynamic_import_job, 5, vc(args))`（quickjs.c:31846）。
11. `queueMicrotask`: `js_global_queueMicrotask`（quickjs.c:41293-41303）が `JS_EnqueueJob(ctx, js_microtask_job, 1, &argv[0])`（quickjs.c:41299）。`js_microtask_job`（quickjs.c:41287-41291）は渡された関数を `ctx->global_obj` を this にして引数なしで呼ぶだけ。
12. Promise reaction job: `promise_reaction_job`（quickjs.c:55982-56025）が実体。エンキュー元は2箇所——`fulfill_or_reject_promise`（quickjs.c:56079、Promise が pending から確定した瞬間、登録済みの reaction 全部について）と `perform_promise_then`（quickjs.c:56939、`.then()` を呼んだ時点で Promise が既に確定済みだった場合の1件）。
13. Thenable resolution job: `js_promise_resolve_thenable_job`（quickjs.c:56091-56125）。エンキュー元は `JS_EnqueueJob(ctx, js_promise_resolve_thenable_job, 3, args)`（quickjs.c:56252、resolve 関数が「then を持つ値」で呼ばれたとき）。
14. FinalizationRegistry callback job: `js_finrec_job`（quickjs.c:64099-64102、`JS_Call(ctx, argv[0], JS_UNDEFINED, 1, &argv[1])`）。エンキュー元は `reset_weak_ref`（quickjs.c:64147-64230）内、`JS_WEAK_REF_KIND_FINALIZATION_REGISTRY_ENTRY` のケース（quickjs.c:64196-64226）。`JS_EnqueueJob(fre->ctx, js_finrec_job, 2, args)` は quickjs.c:64222。

## FinalizationRegistry ジョブがいつ積まれるか

15. `reset_weak_ref` は対象オブジェクトが弱参照から切り離される（＝GC で回収される／解放される）瞬間に呼ばれる汎用関数で、WeakMap/WeakSet エントリ・WeakRef・FinalizationRegistry エントリの3種を一括処理する（quickjs.c:64147-64230）。
16. FinalizationRegistry 用のエンキューは無条件ではない。`bool enqueue = !rt->in_free;`（quickjs.c:64205）——ランタイム破棄中は積まない。さらに `held_val` や `cb`（コールバック本体）が `p->free_mark`（すでに解放マーク済み）または `p->header.mark`（サイクル回収の tmp_obj_list に乗っている）なら `enqueue = false`（quickjs.c:64206-64217）——コールバックや保持値自身が同じ GC サイクルで一緒に回収される場合はジョブを積まない。
17. この関数はオブジェクトの解放経路（参照カウント0での即時解放、または循環参照 GC のサイクル回収）から呼ばれる汎用処理であり、割り込みハンドラのポーリング箇所とは独立。つまり FinalizationRegistry ジョブは「バイトコードのどの命令を実行中でも」GC が動けば積まれ得る（推定—GC の起動条件そのものは本ファイルで追っていない。未確認セクション参照）。

## HostPromiseRejectionTracker が発火するタイミング

18. `JS_SetHostPromiseRejectionTracker`（quickjs.c:56033-56039）はコールバックを `rt->host_promise_rejection_tracker` に単純代入するだけ（単一スロット、チェーン不可）。
19. 発火箇所は2つ、どちらも**同期**（ジョブとしてキューされるのではなく、その場で `rt->host_promise_rejection_tracker(...)` を直接呼ぶ）。
    - (a) `fulfill_or_reject_promise`（quickjs.c:56065-56070）: Promise が **reject で確定した瞬間**、かつ `!s->is_handled` なら `handled=false` で呼ぶ。この時点では reaction の登録有無は問わない——「今まさに reject された」タイミングでの通知。
    - (b) `perform_promise_then`（quickjs.c:56926-56931）: `.then()`/`.catch()` を呼んだ時点で Promise が**既に** `JS_PROMISE_REJECTED` かつ `!s->is_handled` なら `handled=true` で呼ぶ。つまり「後から見つかった unhandled rejection にハンドラが付いた」ことの通知。
20. `s->is_handled` は `perform_promise_then` の最後で無条件に `true` にセットされる（quickjs.c:56944）——ペンディング状態で `.then()` を呼んだ場合も、確定済みで呼んだ場合も、`.then()` を一度でも呼べば以後 `is_handled=true` になる。`JSPromiseData.is_handled` のコメントは「デバッグ用にしか使わない」（quickjs.c:55909）だが、実際には tracker の (a)/(b) 分岐の判定に使われている。
21. トラッカー自体は Promise の状態や reaction リストを一切変更しない。あくまで「ホストに通知するだけ」のフック。

## 割り込みハンドラの土台

22. `JSInterruptHandler *interrupt_handler` と `void *interrupt_opaque` は `JSRuntime` のフィールド（quickjs.c:312-313）。`JS_SetInterruptHandler(rt, cb, opaque)`（quickjs.c:2120-2124）は単純代入——**単一スロットで、複数登録はできない**。後から呼んだ方が前の登録を完全に上書きする。
23. `ctx->interrupt_counter` は `JSContext` 側のフィールド（コメント quickjs.c:526「when the counter reaches zero, JSRuntime.interrupt_handler is called」）。カウンタ自体はコンテキストごと、呼ばれるハンドラはランタイム共有という非対称な設計。
24. `JS_INTERRUPT_COUNTER_INIT` は 10000（quickjs.c:476、コメント「大きすぎるとコストが無視できず、小さすぎると呼び出し頻度が低くなりすぎる」の意図で選ばれた定数）。
25. `js_poll_interrupts`（quickjs.c:8469-8476）は `--ctx->interrupt_counter <= 0` のときだけ `__js_poll_interrupts` を呼ぶインライン関数。それ以外は何もしない（デクリメントのみ）。
26. `__js_poll_interrupts`（quickjs.c:8456-8467、`no_inline`）は呼ばれるたびに `ctx->interrupt_counter = JS_INTERRUPT_COUNTER_INIT` でカウンタを即リセットしてから `rt->interrupt_handler` があれば呼び、真を返せば `JS_ThrowInterrupted(ctx)` して -1 を返す。ハンドラが NULL のときは何もせず0を返す（＝カウンタが尽きても何も起きない。ハンドラ未登録時は割り込み機構全体が無効）。
27. `JS_ThrowInterrupted`（quickjs.c:8450-8454）は `JS_ThrowInternalError(ctx, "interrupted")` してから `JS_SetUncatchableError(ctx, ctx->rt->current_exception)` を呼ぶ。つまり**`InternalError: interrupted` を uncatchable フラグ付きで投げる**。
28. `JS_IsUncatchableError` / `JS_SetUncatchableError` / `JS_ClearUncatchableError`（quickjs.c:12006-12037）は `JSObject` の `is_uncatchable_error` ビットで実装。`JS_CLASS_ERROR` のオブジェクトにしか意味を持たない。

## js_poll_interrupts の全呼び出し箇所

29. `JS_OrdinaryIsInstanceOf` のプロトタイプチェーン走査ループ（quickjs.c:8706、コメント「must check for timeout to avoid infinite loop」——Proxy を挟むと無限ループになり得るための保険）。
30. `build_for_in_iterator`（for-in の列挙対象を作る関数）内、2箇所——影private判定でプロトタイプを辿る `slow_path` 手前のループ（quickjs.c:16913）と、本体の shadow チェックループ（quickjs.c:16972）。
31. `JS_CallInternal`（インタプリタ本体）の**関数呼び出しのたびに1回、命令ディスパッチの前**（quickjs.c:18164）。バイトコードループに入る前の関門なので、関数呼び出し自体がインタプリタループとは独立に「1呼び出し=カウンタ1減算」を発生させる。
32. `JS_CallInternal` のバイトコードディスパッチ内、分岐命令の直後7箇所——`OP_goto`（quickjs.c:19241）、`OP_goto16`（quickjs.c:19247）、`OP_goto8`（quickjs.c:19253）、`OP_if_true`（quickjs.c:19272）、`OP_if_false`（quickjs.c:19292）、`OP_if_true8`（quickjs.c:19312）、`OP_if_false8`（quickjs.c:19332）。前方分岐・後方分岐を問わず、この7命令が実行されるたびに毎回ポーリングされる（＝ループの backward-edge だけでなく forward の if/goto でも減算される）。他の演算命令（算術・プロパティアクセス等）ではポーリングされない。
33. `JS_CallConstructorInternal`（`new` 呼び出し）内、`JS_CallInternal` 同様に呼び出しの手前で1回（quickjs.c:20970）。
34. `js_object_isPrototypeOf`（`Object.prototype.isPrototypeOf`）のプロトタイプ走査ループ（quickjs.c:42572、同じく Proxy 対策のコメント）。
35. `js_object___lookupGetter__`（`__lookupGetter__`/`__lookupSetter__` 共用）のプロトタイプ走査ループ（quickjs.c:42659、同コメント）。
36. `lre_check_timeout`（quickjs.c:50400-50406）は libregexp（`libregexp.c`、本ledgerのスコープ外）からコールバックとして呼ばれる関数で、`js_poll_interrupts`/`interrupt_counter` の仕組みを一切使わない。`rt->interrupt_handler` を**毎回無条件に直接呼び**、真偽値をそのまま libregexp 側へ返す。**訂正（レビュー指摘）: 旧稿は「`JS_ThrowInterrupted` は呼ばれない」「libregexp側の挙動は不明」としていたが誤り。** `lre_check_timeout` 自身は投げないが、これを呼んだ `lre_exec` が `LRE_RET_TIMEOUT` を返した場合、呼び出し元の `js_regexp_exec`（quickjs.c内、libregexpの外）が明示的に `JS_ThrowInterrupted(ctx)` を呼ぶ——`quickjs.c:50531-50532`（1箇所目の `lre_exec` 呼び出し）と `quickjs.c:50745-50746`（2箇所目）の2箇所。つまり**正規表現マッチはCビルトインの内側にある中断発生地点であり、`interrupt_counter`のデクリメントを経由せず、opcodeベースの中断確認（04-opcode-checkpoints.md）の対象範囲の外で、同じ捕捉不能な`InternalError: interrupted`を投げうる**。長い正規表現マッチ中は04・01両台帳が列挙する「中断確認ポイント」がどれも実行されないまま、`lre_check_timeout`のコールバック粒度（libregexpの内部実装依存、本ファイルのスコープ外）でのみ中断確認が起きる。

## 割り込みが実際に起きたときの挙動

37. インタプリタの `exception:` ラベル（quickjs.c:20816-20844）は、`JS_IsUncatchableError(rt->current_exception)` が真のとき、**オペランドスタック上の `JS_TAG_CATCH_OFFSET` を探して catch/finally へジャンプする分岐そのものを丸ごとスキップする**（`if (!JS_IsUncatchableError(...)) { while (...) {...} }`、quickjs.c:20823-20844）。つまり **ユーザーコードの `try/catch` も `try/finally` も実行されずに素通りする**——`finally` の後始末（リソース解放など）が走らない。
38. ただしローカル変数・オペランドスタックの JSValue 自体は正しく解放される。`done:` ラベル（quickjs.c:20853-20863）で `local_buf` から `sp` まで無条件に `JS_FreeValue` するので、catch/finally をスキップしても**メモリリークにはならない**（catch-offset 処理を飛ばすのは「JS レベルの catch/finally コードを実行しない」ことであって、C レベルの参照カウント解放を飛ばすことではない）。
39. `js_async_function_resume`（quickjs.c:21406-21445 付近）: `await` から再開した async 関数本体が例外を投げたとき（quickjs.c:21412 `JS_IsException(func_ret)`）、それが uncatchable（＝割り込み）なら `is_success = false` にするだけで、**`s->resolving_funcs[1]`（reject 関数）を一切呼ばない**（quickjs.c:21414-21416 の分岐、else 節 quickjs.c:21417-21430 が reject を呼ぶ側）。その後 `js_async_function_terminate(ctx->rt, s)`（quickjs.c:21431）が呼ばれるが、これは `async_func_free` で内部のジェネレータ実行状態を解放するだけ（quickjs.c:21338-21344）で、**Promise の resolve/reject はしない**。
40. `js_async_function_free0`（quickjs.c:21346-21350）で初めて `s->resolving_funcs[0]`/`[1]`（resolve/reject クロージャ自体）が `JS_FreeValueRT` される。この関数は `JSAsyncFunctionData` の参照カウントが0になったとき（＝async 関数が返した Promise オブジェクト自身が GC された等）に呼ばれる経路にあると読める（本ファイル中の呼び出し元は未追跡、推定）。
41. 39+40 を合わせると：**実行中の await 再開処理の途中で割り込みが発生すると、その async 関数が返した Promise は resolve も reject もされないまま pending で固まる**。`.then()`/`.catch()` を付けたコードは二度と呼ばれず、Promise オブジェクトと resolve/reject クロージャは（GC が回収するまで）生き続ける。ここは L2/L3 の「実行を強制終了する」設計と正面から衝突しうる事実。

## ガード対象ゲスト側（components/pocketjs_guest/src/guest.c）

42. `pocketjs_guest` 構造体は `atomic_uint interrupt_epoch` と `unsigned int handled_interrupt_epoch`（非atomic）を持つ（guest.c:37-38）。
43. `guest_interrupt(rt, opaque)`（guest.c:117-128）は edge-triggered な一発方式：`interrupt_epoch` を読み、前回処理済みの値と同じなら 0（割り込みなし）を返す。異なれば `handled_interrupt_epoch` をその値に更新してから 1 を返す。つまり `pocketjs_guest_interrupt()` を1回呼ぶごとに、次のポーリングで**ちょうど1回だけ**真を返し、以後は再度呼ばれるまでずっと 0（自動では再武装しない）。
44. `pocketjs_guest_interrupt(guest)`（guest.c:414-418）は `atomic_fetch_add_explicit(&guest->interrupt_epoch, 1, memory_order_relaxed)` するだけ。
45. `pocketjs_guest_create`（guest.c:209-244）は `JS_SetInterruptHandler(guest->runtime, guest_interrupt, guest)`（guest.c:233）と `JS_SetHostPromiseRejectionTracker(guest->runtime, promise_rejection, guest)`（guest.c:234）を**この順で**、ランタイム/コンテキスト生成直後に登録する。
46. `main/app_session.c` の `install_limits`（app_session.c:87-91）は `JS_SetInterruptHandler(JS_GetRuntime(ctx), interrupt, NULL)`（app_session.c:89）を呼ぶ。これは `pocketjs_guest_quickjs_install(guest, install_limits, NULL)`（app_session.c:270）として `app_start_test()`（app_session.c:238 以降）の中で `pocketjs_guest_create` 直後・他のどの `pocketjs_guest_quickjs_install_once` よりも前に呼ばれる（app_session.c:269-270）。`JS_SetInterruptHandler` は単一スロットの単純代入（quickjs.c:2120-2124、事実22）なので、**この呼び出しで guest.c の `guest_interrupt` は即座に上書きされ、以後そのランタイムでは二度と呼ばれない**。`host_promise_rejection_tracker` は別フィールドなので上書きされず、guest.c の `promise_rejection` は生き続ける。
47. リポジトリ全体を検索した限り、`pocketjs_guest_interrupt()` の呼び出し元は `components/pocketjs_guest/src/guest.c` 自身の定義（guest.c:414）と README 中の言及（`components/pocketjs_guest/README.md:9`）以外に存在しない（`main/` にも `components/` の他ファイルにも呼び出しなし、grep 確認済み）。事実46と合わせ、**guest.c の epoch ベース割り込み機構は現行ファームでは実質デッドコード**——常に `app_session.c` の `interrupt()` が代わりに使われている。
48. `main/app_session.c` の `interrupt(rt, opaque)`（app_session.c:83-86）は `atomic_load(&stop_requested) || esp_timer_get_time() > deadline` を返す。`stop_requested` は `atomic_bool` で `app_request_stop()`（app_session.c:92）から `atomic_store(&stop_requested, true)` される。`deadline` はスレッド間で共有される `static int64_t`（非atomic）。
49. `deadline` への代入箇所は4箇所——`app_start_test()` 冒頭で `esp_timer_get_time()+2000000`（2秒、app_session.c:241）、`app_overlay_tick()` 冒頭で `+250000`（250ms、app_session.c:447）、`app_tick()` 冒頭で `+250000`（250ms、app_session.c:480）。いずれも各呼び出しの**最初**、ポンプ類やゲスト呼び出しより前に再武装される。
50. app_session.c:429-446 のコメント（`app_overlay_tick` 直前）は開発時の実測を伴う根拠付きの記述——この 250ms ウォッチドッグは以前 50ms だったが、FLOWER シーンが 71ms/フレーム（うちカーネル 53ms）かかる状況で、UI タスクが優先度6のデコーダや優先度7のオーディオタスクにプリエンプトされただけで実際の割り込みが起きた（実測: 「board run under the FLOWER scene threw "InternalError: interrupted" inside a five-line loop that counts characters」）。この `deadline` は**壁時計**（wall clock）であり、ゲストの実行時間ではなくタスクがプリエンプトされていた時間も含めてカウントする、とコメントが明記している。50ms→250msへの変更で今のファームになっている。この watchdog は `ui/overlay.c` の `budget_us`/`over_limit`（コスト制御、別機構）とは意図的に別物として書かれている（コメントより）。

## pocket_api.c の resolve/reject 経路

51. `pocket_api_promise_arm`（pocket_api.c:411-439）は `JS_NewPromiseCapability` で Promise と resolve/reject 関数を作り、`pocket_promise_t` に `resolve`/`reject`/`cancel`/`deadline_us`/`ops`/`user` を保存して `armed=true` にする。`JS_NewPromiseCapability` 失敗時は `cancel` を解放し `promise_release` してスロットを戻す（対応する `ops` は未設定のままなので、後続の完了通知は誰にもマッチしない——コメントに明記、pocket_api.c:422-425）。
52. `pocket_api_complete(request, status)`（pocket_api.c:441-448）はドライバ（IMU/AV/IO 等の各面）側から呼ばれる完了通知。`atomic_store(&p->status, status)` の後に `atomic_store(&p->done, request)`——**status を先に書いてから done を書く**順序をコメントが明記（「pump がまず番号を読み、その後にしか status を信用しない」）。
53. `promise_settle`（pocket_api.c:450-461）は `JS_Call(ctx, rejected ? p->reject : p->resolve, JS_UNDEFINED, 1, &value)` を**同期的に直接呼ぶ**。`JS_EnqueueJob` は経由しない。ここで呼ばれた resolve/reject 関数の中身は quickjs.c の `js_promise_resolve_function_call`（quickjs.c:56200 以降）で、内部で `fulfill_or_reject_promise` を呼び、それが登録済みの `.then()`/`.catch()` ハンドラを `promise_reaction_job` として `rt->job_list` に**積むだけ**（quickjs.c:56079、事実12）——ここではまだ実行されない。
54. `pocket_api_pump()`（pocket_api.c:471-491）は全 `POCKET_MAX_PROMISES` スロットを毎回走査し、`atomic_load(&p->done)==request` なら `p->ops->settle(...)` でアプリ向けの値を作って `promise_settle` を呼ぶ。未完了なら cancel トークンの要求か `now > p->deadline_us` を見て `promise_stop` を呼ぶ（Promise 自体はまだ確定させない——`stop_code` を刻んでドライバに `stop` を要求するだけ、コメント pocket_api.c:463-464 に明記）。
55. `pocket_api_reset()`（pocket_api.c:493-504）はゲスト破棄時に呼ばれる想定の関数で、armed 中のリクエストは resolve/reject を一切呼ばずに `ops->stop` を呼んでスロットを解放するだけ（コメント：「この realm は消えるので、settle 先の誰もいない」）。**Promise は pending のまま解体される**——事実9（`JS_FreeRuntime` がジョブを実行せず捨てる）と対になる、pocket_api 側の対応する挙動。

## drain_jobs（guest.c）と1ティックの実行順序

56. `drain_jobs`（guest.c:159-195）は `while ((result = JS_ExecutePendingJob(...)) > 0) guest->jobs++;` で job_list を**空になるまで**実行する。`result < 0`（＝いずれかのジョブが例外を投げた）で即座にループを抜け、`js_std_dump_error` してから `ESP_FAIL` を返す——**この時点で job_list に残っている未実行ジョブがあれば、そのまま次回の drain_jobs 呼び出しまで残る**（quickjs.c の `JS_ExecutePendingJob` は1件ずつ取り出すだけなので、これは guest.c 側のポリシー）。
57. ジョブ実行後、`guest->rejections`（未処理 rejection の連結リスト、guest.c:130-157 の `promise_rejection` トラッカーが積む）を全件消費し、`ESP_LOGE` でログしてから解放する（guest.c:173-193）。`rejection_tracking_failed`（トラッカーの `calloc` 失敗フラグ、guest.c:150-152）が立っていれば、rejection が0件でも `failed=true` になり `ESP_FAIL` を返す。
58. `pocketjs_guest_eval`（guest.c:294-317）は最初の `JS_Eval` が成功した後、`globalThis.frame` を読み直してから `drain_jobs(guest)` を呼ぶ（guest.c:316）——起動時の1回。
59. `pocketjs_guest_frame`（guest.c:351-412）は `JS_Call(guest->context, guest->frame, ...)` で `frame()` を呼ぶ。**例外（`JS_IsException(result)`）の場合はそこで即 `return ESP_FAIL`（guest.c:400-405）——`drain_jobs` は呼ばれない**。例外がなければ `drain_jobs(guest)` を呼ぶ（guest.c:406-411）。つまり `frame()` が割り込み等で失敗したティックでは、その直前の `pocket_api_pump()` が積んだ reaction ジョブも、そのティックでは一切実行されない。
60. `main/app_session.c` の `app_tick(buttons)`（app_session.c:479 以降）の実測できる呼び出し順は：① `deadline` 再武装（250ms） → ② `pocket_app_pump()`・`pocket_imu_pump()`・`pocket_io_pump()` → ③ `pocket_bridge_pump()` → ④ `pocket_net_pump()` → ⑤ `pocket_capture_pump()` → ⑥ `pocket_api_pump()`（ここで完了済みリクエストの Promise を resolve/reject——同期呼び出しで reaction ジョブがキューされるだけ、事実53・59） → ⑦ `pocket_fs_pump()`・`pocket_av_pump()`（コメントにより「新規の完了は生成しない」） → ⑧ `pocket_ui_pump(buttons)` → ⑨ `pocketjs_ui_turn(binding, &input, &frame)`。
61. `pocketjs_ui_turn` はこのワークツリー内にソースも宣言ヘッダも見当たらない（grep で `main/app_session.c:518` の呼び出し以外どこにも出てこない）。呼び出し直前のコメント（app_session.c:513-516）は「frame() in QuickJS plus the UI core's tick and draw」と述べており、これは**`pocketjs_guest_frame()`（＝`frame()` 呼び出し＋事実59の drain）を内包していると読める**が、本ledgerのスコープ（quickjs.c 中心）では実装を直接確認できていない（推定、未確認セクション参照）。
62. 事実53・59・60・61を合わせると、実際の確定順序は **「完了取り込み（intake）→ pocket_api_pump() が resolve/reject をジョブとして積む（まだ実行しない）→ frame() の JS 本体が走る（ここでも .then/async/queueMicrotask が新たにジョブを積み得る）→ frame() が例外なく戻った場合に限り drain_jobs() が job_list を FIFO で空にする」** であり、「resolve → drain → frame()」ではなく **「resolve（キューに積むだけ）→ frame()（実行）→ drain（frame() 内外双方のジョブをまとめて実行）」** の順。つまり pocket_api_pump() がそのティックで解決した Promise の `.then()` 継続は、**同じティックの `frame()` 本体の実行より後**（frame() の戻り値を待ってから）に走る。

## L2/L3/L5 に効く点（判断ではなく着目点の列挙）

- 事実37・39-41: 割り込み（interrupted / stop_requested）は「例外」ではなく「その場で JS の後始末を全部すっ飛ばして戻ってくる」ものに近い。`try/finally` も async 関数の Promise の確定も保証されない。L2/L3 でタスクを強制終了させる設計をするなら、ゲスト側のクリーンアップをこの仕組みに頼れない。
- 事実22・46-47: `JS_SetInterruptHandler` は単一スロット。guest.c と app_session.c が同じランタイムに対して2つの独立した割り込み機構を用意しているが、後勝ちで片方（guest.c の epoch 方式）が常に死んでいる。L2/L3 で割り込み要因を増やす（複数タスクからの停止要求、ウォッチドッグ等）なら、ここを1本化しないと同じ罠を踏む。
- 事実49-50: 現行の 250ms ウォッチドッグは壁時計で、実行時間ではなくスケジューリング遅延も食う。VM/協調スケジューリング設計でタイムスライスの根拠にこの値をそのまま使うのは危険（実測ベースで50ms→250msに緩めた経緯がある）。
- 事実56・59: `JS_ExecutePendingJob` が失敗するとそのジョブより後ろの未実行ジョブは次回呼び出しまで残る。`pocketjs_guest_frame` は `frame()` が例外を投げたティックでは `drain_jobs` 自体を呼ばない。ジョブが「いつまでも実行されない」経路が複数ある。
- 事実9・55: ランタイム破棄・セッションリセットのどちらも「未確定 Promise を確定させずに捨てる」設計で揃っている（`JS_FreeRuntime` はジョブを実行せず解放、`pocket_api_reset` は resolve/reject を呼ばずスロット解放）。L2/L3 でセッションをまたいで状態を持ち越す設計にするなら、この「捨てて終わり」が前提になっている箇所の洗い出しが要る。

## 未確認

- `lre_check_timeout`（quickjs.c:50400-50406）が libregexp からどの頻度（何バイトマッチごと等）で呼ばれるかは `libregexp.c` 側の実装であり、本ledgerのスコープ（quickjs.c）外のため未読。**quickjs.c側の受け口（`LRE_RET_TIMEOUT`→`JS_ThrowInterrupted`）は判明済み（fact 36 訂正済み、`quickjs.c:50531-50532`・`50745-50746`）。**
- **本台帳群の行番号の基準:** 上記の訂正・追記は `git rev-parse HEAD`（本ワークツリーでは `d9ef1f9`）に、`apps/vmprobe/` 計測フック用の未コミット差分（`quickjs.c` に+59行、6箇所のhunk）が当たった状態の `quickjs.c` を直接読んで確認した（本追記時点）。旧稿の行番号（例: fact 36 の `50341-50347`）はこの差分適用前の版を指しており、以後 `quickjs.c` が変わるたびに全台帳の行番号は再びずれる。どの版に対する行番号かを台帳自身が記録していない構造的な問題は残っている——次に読む側は周辺コードの一致を確認すること。
- `pocketjs_ui_turn` の実装（`pocketjs_guest_frame` を内包しているかどうかの直接確認、UI core の tick/draw と `frame()` 呼び出し・`drain_jobs` の前後関係）はこのワークツリーにソースが見当たらず未確認（事実61）。app_session.c のコメントからの推定に留まる。
- GC（`JS_RunGC` や参照カウント0での即時解放）がバイトコード実行中のどの箇所からトリガーされ得るか（アロケータの `malloc_gc_threshold` 到達がどのタイミングで発生しうるか）は本ファイル内で個別に追っていない。事実15-17（FinalizationRegistry ジョブがいつ積まれるか）の「バイトコードのどの命令中でも起こりうる」は、GC トリガー自体の網羅的な呼び出し箇所リストを作った上での結論ではない。
- `js_async_function_free0`（quickjs.c:21346-21350）の呼び出し元（`JSAsyncFunctionData` の参照カウントがどの経路で0になるか）は本ファイル中で個別に追跡していない。事実41の「pending のまま Promise が生き続ける」期間がどれくらいかは未確認。
- Async generator（`js_async_generator_resume_next` 等、通常の async function とは別の再開ロジックを持つはず）が同種の「uncatchable を reject せず飲み込む」挙動を持つかどうかは未読・未確認。事実39-41 と同型の問題が起きる可能性があるが未検証。
- `promise_reaction_job` 自身が uncatchable な例外を返した場合（quickjs.c:56006-56010、`return JS_EXCEPTION` するだけで `JS_GetException` を呼ばない分岐）、`JS_ExecutePendingJob` 側がその戻り値をどう扱うか（`res = e->job_func(...)`; `JS_IsException(res)` で ret=-1 とするだけに見えるが、current_exception が既に「消費済み」状態でこの分岐に来た場合の整合性）は、具体的な発生シナリオを組んで確認していない。
