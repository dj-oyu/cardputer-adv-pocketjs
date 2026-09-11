# L3 台帳: フレームメモリへの生ポインタ (02-frame-pointers)

対象: `components/quickjs-ng/quickjs-ng/quickjs.c`（quickjs-ng 0.14.0 + immutable-buffer patch）。全エントリは `ファイル:行` で実コードの事実を指す。推定と明記した箇所以外は読んだコードの記述。行番号は本セッションで読んだ時点のもの。

## 1. `JSStackFrame` 構造体そのもの

`quickjs.c:367-381`

```c
typedef struct JSStackFrame {
    struct JSStackFrame *prev_frame;
    JSValue cur_func;
    JSValue *arg_buf;
    JSValue *var_buf;
    struct JSVarRef **var_refs;
    uint8_t *cur_pc;
    uint16_t var_ref_count;
    uint16_t arg_count;
    bool is_strict_mode;
    JSValue *cur_sp;
} JSStackFrame;
```

- `arg_buf` / `var_buf` / `var_refs` / `cur_sp` はすべて生ポインタ。所有権情報を型では持たない。
- `JSStackFrame` 自体は値型で、呼び出し規約ごとに置かれる場所が違う（下記2節）。`prev_frame` でリンクリストを作るのは `rt->current_stack_frame` を根とする単方向リスト。

## 2. `JSStackFrame` の確保場所は呼び出し種別で3通り

### 2a. バイトコード関数の通常呼び出し（C再帰） — C スタック（`alloca`）

`JS_CallInternal` (`quickjs.c:18124` 以降)。

- `quickjs.c:18132`: `JSStackFrame sf_s, *sf = &sf_s;` — **`JS_CallInternal` のC関数フレーム上のローカル変数**。C再帰で1段深くなるたびに新しい `sf_s` が生成される。
- `quickjs.c:18213-18226`: `alloca_size = sizeof(JSValue) * (arg_allocated_size + b->var_count + b->stack_size) + sizeof(JSVarRef *) * b->var_ref_count;` を計算し `js_check_stack_overflow` でチェック後 `local_buf = alloca(alloca_size);`（`quickjs.c:18226`）。**引数・変数・オペランドスタック・var_refs配列が1つの `alloca` ブロックに同居する。**
- レイアウト: `local_buf`（= 場合により `arg_buf`）→ `var_buf = local_buf + arg_allocated_size`（`quickjs.c:18238`）→ `stack_buf = var_buf + b->var_count`（`quickjs.c:18246`）→ `sf->var_refs = (JSVarRef **)(stack_buf + b->stack_size)`（`quickjs.c:18247`）。
- `sf->prev_frame = rt->current_stack_frame; rt->current_stack_frame = sf;`（`quickjs.c:18256-18257`）でリンクに接続。
- **このブロックは呼び出し元の `JS_CallInternal` フレームが `return` するまでしか存在しない。** C スタックの巻き戻し（`longjmp` は使わない。例外は `goto exception` の後 `done:`/`return` で通常のC関数returnとして戻る）で自動的に無効化される。
- `argc < b->arg_count` かつ `JS_CALL_FLAG_COPY_ARGV` が立っていない場合は `arg_allocated_size = 0` となり `arg_buf = (JSValue *)argv;`（`quickjs.c:18221`）——**呼び出し元が持つ `argv` バッファへの生ポインタをそのまま使う**（コピーしない）。呼び出し元の argv 寿命に依存する追加の生ポインタ経路。

### 2b. ジェネレータ／async関数の呼び出し — ヒープ（`js_malloc`、`JSAsyncFunctionState` に埋め込み）

`async_func_init` (`quickjs.c:21051-21094`)。

- `sf = &s->frame;`（`quickjs.c:21062`）—— `JSStackFrame` は `JSAsyncFunctionState.frame` として**値埋め込み**（`quickjs.c:874`: `JSStackFrame frame;`）。
- `quickjs.c:21069-21071`: `alloc_size = sizeof(JSValue) * max_int(local_count, 1) + sizeof(JSVarRef *) * b->var_ref_count; sf->arg_buf = js_malloc(ctx, alloc_size);` — 引数・変数・stack・var_refsの全部が**1つのヒープブロック**（`js_malloc`、GCの対象ではなく単純ヒープ）。
- レイアウト: `sf->var_buf = sf->arg_buf + arg_buf_len`（`quickjs.c:21079`）→ `sf->cur_sp = sf->var_buf + b->var_count`（`quickjs.c:21080`、初期値。実行中は稼働ポインタとしても使われる）→ `sf->var_refs = (JSVarRef **)(sf->cur_sp + b->stack_size)`（`quickjs.c:21081`）。
- `JSAsyncFunctionState` 自体（＝ `sf` を含む構造体）は3種の外側構造体のどれかに埋め込まれ、いずれも `js_mallocz` でヒープ確保される：
  - `JSGeneratorData`（`quickjs.c:21165-21168`）— `js_call_generator_function`（`quickjs.c:21299-21334`）の `s = js_mallocz(ctx, sizeof(*s));`（`quickjs.c:21307`）。**GCオブジェクトではない**（`add_gc_object` を呼ばない）。所有者は生成した `JSObject`（`JS_CLASS_GENERATOR`）の `p->u.generator_data`（`quickjs.c:21181` 等参照経路）。
  - `JSAsyncFunctionData`（`quickjs.c:879-884`）— `js_async_function_call`（`quickjs.c:21508-21545`）の `s = js_mallocz(ctx, sizeof(*s));`（`quickjs.c:21515`）＋ `add_gc_object(ctx->rt, &s->header, JS_GC_OBJ_TYPE_ASYNC_FUNCTION);`（`quickjs.c:21520`）。**GCオブジェクトである**（参照カウント循環コレクタの対象）。
  - `JSAsyncGeneratorData`（`quickjs.c:21568-21573`）— `js_async_generator_function_call`（`quickjs.c:21964-22002`）の `s = js_mallocz(ctx, sizeof(*s));`（`quickjs.c:21973`）。**GCオブジェクトではない**（`js_async_generator_mark` は所有 `JSObject` 経由で呼ばれる、`quickjs.c:21605-21624`）。
- **結論:** ジェネレータ／async関数の `JSStackFrame` とその引数・変数・スタック領域は、対応するオブジェクトが生きている限り**安定したヒープアドレス**を持つ。C スタック上には存在しない。L2/L3 の「移動しない／移動可能スタック」は、まずこの3系統のヒープブロックのレイアウトを踏襲するのが自然な出発点になりうる（判断は本ファイルの範囲外）。
- 再開時: `JS_CallInternal` は `func_obj` のタグがオブジェクトでなく `JS_CALL_FLAG_GENERATOR` が立つ場合、`func_obj` を **`JSAsyncFunctionState*` への生ポインタとして再解釈**する（`quickjs.c:18167-18189`）。コメント原文: `/* the tag does not matter provided it is not an object */`（`quickjs.c:21148` 相当のコメントも同旨）。`sf = &s->frame;`（`quickjs.c:18172`）とし、`rt->current_stack_frame` に接続してから `goto restart` または `goto exception`。**`sf->cur_sp` が非NULLならそれを実行中スタックポインタとして復元し（`quickjs.c:18180`）、直後に `sf->cur_sp = NULL;`（`quickjs.c:18181`、「実行中はcur_spはNULL」という不変条件を保つコメントが直前にある）。**

### 2c. ネイティブ（C）関数呼び出し — C スタック、フィールドは部分初期化のみ

3箇所で同型のパターンが繰り返される。いずれも `JSStackFrame sf_s, *sf = &sf_s;` をC関数のローカルとして確保し、`cur_func` / `arg_buf` / `arg_count` / `is_strict_mode` だけを設定して `prev_frame`/`current_stack_frame` をつなぐ。**`var_buf` / `var_refs` / `var_ref_count` / `cur_pc` / `cur_sp` は一切書き込まれない（未初期化のまま）。**

- `js_call_c_function`（`quickjs.c:17922-18049`）: `sf->arg_buf = (JSValue *)arg_buf;`（`quickjs.c:17965`）のみ。`arg_buf` は呼び出し元の `argv`（`argc >= arg_count` の場合）か、この関数がローカルに `alloca` した一時配列（`quickjs.c:17956`）。
- `js_call_c_function_data`（`quickjs.c:6415-6457`、行番号は読んだ範囲からの相対値。関数名は本文中の `s->func(ctx, this_val, argc, arg_buf, s->magic, vc(s->data));`（`quickjs.c:6454`）から特定）: 同様のパターン。
- `js_call_c_closure`（`quickjs.c:6549-6589`）: 同様のパターン。
- **これが安全な理由（現状のコードパスに限る）:** `build_backtrace`（`quickjs.c:8081-8088`）と `JS_GetScriptOrModuleName`（`quickjs.c:31558-31562`）は `js_class_has_bytecode(p->class_id)` を通ったフレームだけ `b`（`JSFunctionBytecode*`）を取り、それ以外は `var_ref_count` 等を触らずに `(native)` 表示や `JS_ATOM_NULL` 返却で終わる。`close_var_refs` はそのフレームの `JS_CallInternal` 自身の `done:` パス（`quickjs.c:20854-20858`）でしか呼ばれず、ネイティブ呼び出しのフレームに対しては呼ばれない。**未確認:** 今後フレームを汎用的に歩く仕組み（GC相当のフレームウォーカーなど）を足す場合、`class_id` によるガードなしにこれらのフィールドを読むと未初期化のC스택メモリを読むバグになる。現状そのようなコードは無い。

## 2d. `sf->cur_func` の所有権は呼び出し規約で非対称（借用 vs dup）——レビュー指摘により追記

- 通常呼び出し（2a節）: `sf->cur_func = unsafe_unconst(func_obj);`（`quickjs.c:18282`）——**`func_obj` を dup せずそのまま生代入する借用。** `func_obj` の生存はこの `JS_CallInternal` 呼び出しの引数として渡されている間（＝呼び出し元がその値を保持している間）にのみ保証される。
- 同じパターンはネイティブ関数呼び出し（2c節）にも共通: `js_call_c_function`（`quickjs.c:18009`）・`js_call_c_function_data`（`quickjs.c:6511`）・`js_call_c_closure`（`quickjs.c:6642`）のいずれも `sf->cur_func = unsafe_unconst(func_obj);` で借用のみ。
- ジェネレータ/async（2b節）: `async_func_init` の `sf->cur_func = js_dup(func_obj);`（`quickjs.c:21134`）——**明示的に参照カウントを増やして自分で所有する。** 対応する解放は `async_func_free`（`quickjs.c:21116-21138`台、5節）内の `JS_FreeValueRT(rt, sf->cur_func)`。
- **L2への含意（事実の列挙）:** 通常呼び出しの `sf->cur_func` は「呼び出し元が生きている前提」の借用ポインタである。呼び出し元がC側の一時値（例: ホストが `JS_Call` に渡した後すぐ `JS_FreeValue` する `func_obj`）だった場合、通常呼び出しの `JSStackFrame` を（yield/awaitの無いまま）L2的に中断・保存しようとすると、`sf->cur_func` は再開時に呼び出し元がすでに解放したオブジェクトを指し得る。ジェネレータ/async 経路がわざわざ `js_dup` している非対称性は、この借用の危うさをquickjs-ng自身が「中断可能にする経路でだけ」埋め合わせていることの直接証拠。

## 2e. 再開後もフレームチェーンの中間・末端は古い `prev_frame` を持ち続ける——レビュー指摘により追記

- 再開処理（`quickjs.c:18242-18243`）が書き換えるのは **今から再開する1フレームの `sf->prev_frame`（`rt->current_stack_frame` に接続し直す）だけ**であり、そのフレームが（サスペンド前に）呼んでいた子フレームや、そのフレームの外側で保持されている「連鎖」全体は更新対象にならない。
- `async_func_init`（`quickjs.c:21121-21153`）は`sf->prev_frame`を一切書き込まない（フィールド一覧に無い）——**初期状態では未初期化**であり、`JS_CallInternal`側の再開処理（`quickjs.c:18242`）が呼ばれて初めて`rt->current_stack_frame`から値が入る。つまり非同期関数フレームの`prev_frame`は「サスペンドしていた間」は不定値、「再開後」は直近の再開時点の呼び出し元、という寿命を持つ。
- **具体的な懸念（事実からの直接の帰結）:** 仮にL2のVMスタックが複数のJSフレームを連鎖させて丸ごとサスペンドする設計を取る場合、チェーンの末端（一番古い呼び出し元）の`prev_frame`は、サスペンド時点で見えていたCスタック上の`sf_s`（`js_call_c_function`等のホスト側フレーム、`quickjs.c:18005`/`6508`/`6639`で push）を指したままになる。そのホスト側C関数はサスペンドの過程で（Cスタックの巻き戻りにより）とうに戻っているので、後から`build_backtrace`（`quickjs.c:8062`起点の`sf->prev_frame`ループ）や`JS_GetScriptOrModuleName`（`sf->prev_frame`を`n_stack_levels`回辿る）のようなフレームウォーカーがこのチェーンを最後まで辿ると、解放済みのCスタックメモリを`JSStackFrame`として読むことになる。現行コードはこのシナリオに到達しない（yield/awaitは1フレームだけを吊るし、その`prev_frame`は次の`async_func_resume`呼び出し時点の`rt->current_stack_frame`で毎回上書きされる——`quickjs.c:18242`）。L2で「フレームの連鎖ごとサスペンド」を導入する場合にのみ顕在化する。

## 2.5 `JS_CallInternal` 自身の C ローカル変数キャッシュ — フレームへの「もう一組」の生ポインタ

`quickjs.c:18183-18196`（レビュー指摘により追記。行番号は本追記時点）:

```c
    JSStackFrame sf_s, *sf = &sf_s;
    uint8_t *pc;
    int opcode, arg_allocated_size, i;
    JSValue *local_buf, *stack_buf, *var_buf, *arg_buf, *sp, ret_val, *pval;
    JSVarRef **var_refs;
```

- **`JS_CallInternal` は毎回の呼び出しで `sf->arg_buf`/`sf->var_buf`/`sf->var_refs` の値を `local_buf`/`arg_buf`/`var_buf`/`stack_buf`/`sp`/`pc`/`var_refs` という**C関数ローカル変数にコピーし、以降のディスパッチループは（`sf` 自体ではなく）これらのローカルを直接読み書きする**。通常呼び出し（2a節）の構築時（`quickjs.c:18285-18312`、旧節の `alloca` 直後）と、ジェネレータ/async 再開時（`quickjs.c:18236-18241`、`sf = &s->frame` の直後に `local_buf = arg_buf = sf->arg_buf; var_buf = sf->var_buf; stack_buf = sf->var_buf + b->var_count; sp = sf->cur_sp; pc = sf->cur_pc;` と再代入）の**両方の経路でこの再バインドが起きる。**
- 各 call 系 opcode の `call_argv = sp - call_argc;`（例: `quickjs.c:18685`, `18706`, `18725`, `18746`, `18876`）は **`sf` 経由ではなくこの C ローカル `sp` から直接** 引数配列の先頭を算出する。`OP_push_this`（`sf->cur_sp`ではなく`sp`を直接触る）等、フレーム内の値を読む opcode ハンドラの大半は `sf->arg_buf`/`sf->var_buf` ではなくこれらのローカルを使う。
- **L3（移動可能スタック）にとっての意味:** `sf` の3フィールド（`arg_buf`/`var_buf`/`var_refs`）を移動後に更新しても、**`JS_CallInternal` の呼び出し中のC関数フレームに残っている `local_buf`/`arg_buf`/`var_buf`/`stack_buf`/`sp`/`pc`/`var_refs` という「もう一組」のコピーは追従しない**。これらは `sf` から独立したCスタック上のスカラー値であり、`sf`を書き換える操作（フレーム移動）だけでは無効化に気づけない。あるopcodeハンドラの実行中（＝この関数のC再帰1段の内側）にフレーム領域を動かす設計にする場合、動かした直後にこれらのローカルをすべて `sf` から引き直す処理が要る——さもなければ古いアドレスへの書き込み・読み出しが継続する。
- 対照的に、**C再帰で1段深くなった先の `JS_CallInternal`（呼び出された側）は、常に自分自身の `sf`・ローカル変数を新規に持つ**ため、この危険は「呼び出し中の1関数呼び出しの内側」に閉じている。呼び出しをまたいで（＝別のC関数フレームに）古いローカルが残ることはない。危険なのは同一C関数呼び出し内で、あるopcodeの処理中に非同期にフレームを移動するケース。

## 3. `rt->current_stack_frame` — 全呼び出し規約共通のスレッドローカル（ランタイムローカル）根

`quickjs.c:310`: `JSRuntime` のフィールド `struct JSStackFrame *current_stack_frame;`。

push/pop している箇所（全6箇所、すべて対称に push→呼び出し→pop）:
1. `js_call_c_function_data` 系: `quickjs.c:6447-6449`（push）/ `quickjs.c:6455`（pop）。
2. `js_call_c_closure`: `quickjs.c:6578-6580`（push）/ `quickjs.c:6586`（pop）。
3. `js_call_c_function`: `quickjs.c:17944-17946`（push）/ `quickjs.c:18048`（pop）。
4. `JS_CallInternal` 通常呼び出し: `quickjs.c:18256-18257`（push）/ `quickjs.c:20864`（pop、`done`/`done_generator` どちらを通っても実行される共通コード）。
5. `JS_CallInternal` ジェネレータ再開（`JS_CALL_FLAG_GENERATOR`）: `quickjs.c:18183-18184`（push）。pop は同じ `quickjs.c:20864`（このパスも同じ関数末尾を通る）。
6. `JS_CallConstructorInternal` は独自の push/pop を持たず `JS_CallInternal` に委譲する前に `js_poll_interrupts` のみ行う（`quickjs.c:20970`）。

読み出し箇所（生存フレームを辿る／現在フレームを見る）:
- `is_strict_mode()`（`quickjs.c:2799`）: `sf` が NULL なら non-strict 扱い。
- `JS_ThrowError`（`quickjs.c:8265-8267`）: `sf` があり、かつそのフレームがバイトコード関数でない（＝ネイティブ関数から直接投げた）場合だけ即座にバックトレースを付ける。
- `build_backtrace`（`quickjs.c:8062`, `8067`, `8075`）: `sf_start = rt->current_stack_frame;` からループで `sf = sf->prev_frame` を辿る。停止条件は `sf == NULL`、`i >= stack_trace_limit`（最大 `countof(csd)` = 64、`quickjs.c:7994`, `8024`）、または `backtrace_barrier`（`quickjs.c:8130-8132`、`b->backtrace_barrier` が立つ関数＝`JS_EVAL_FLAG_BACKTRACE_BARRIER` で評価されたフレームで打ち切る）。
- `js_build_mapped_arguments`（`quickjs.c:16811`）: `ctx->rt->current_stack_frame->cur_func` を `callee` プロパティとしてコピー（値をdupするので生ポインタの保持ではない）。
- `JS_GetActiveFunction`（`quickjs.c:17561`、関数名は周辺コードからの推定—読んだ範囲では直前の行に関数シグネチャが写っていないため実際の関数名は**未確認**、内容は確実）: `return ctx->rt->current_stack_frame->cur_func;`
- `JS_GetScriptOrModuleName`（`quickjs.c:31537-31564`）: `sf = ctx->rt->current_stack_frame;` の後 `n_stack_levels` 回 `sf->prev_frame` を辿る。
- `__JS_EvalInternal`（直接eval、`quickjs.c:38237-38243`）: `sf = ctx->rt->current_stack_frame;` を取り、`p->u.func.var_refs` と合わせて後段の `js_closure` に**生ポインタのまま**渡す（4節参照）。
- `JS_EvalInternal`（`quickjs.c:38350`）: `!rt->current_stack_frame` を「トップレベルから呼ばれた」の判定に使い、`ctx->error_back_trace` をクリアする。フレームそのものへのアクセスではない。

**`JS_MarkContext`（GCの `mark_children` の `JS_GC_OBJ_TYPE_JS_CONTEXT` 分岐、`quickjs.c:2626-2684`）は `rt->current_stack_frame` を一切辿らない。** GCのマーク走査はヒープ上の `JSGCObjectHeader` 連結リスト（`rt->gc_obj_list` 等）だけを対象にし、実行中のCスタックフレーム（`alloca`されたローカル値）はGCのルート集合に含まれない。QuickJSの参照カウント方式では、Cスタック上のJSValueは自身の参照カウントで生存が保証されており、循環コレクタ（`gc_decref_child`/`gc_scan_incref_child` 等、`quickjs.c:7162` 以降）は「循環しているヒープオブジェクト」だけを探す設計であるため。**L2/L3でVMフレームをCスタック外へ移す場合、そこに置かれたJSValueの生存はGCのマークフェーズでは検出されない**（現状のCスタック値と同じ扱いになるという意味で、これは新しい制約ではないが、「フレームを動かせばGCが自動的に追従する」という誤解を避けるために明記する）。実際にフレーム内の値を解放するのは `close_var_refs`＋ループでの `JS_FreeValue`（`quickjs.c:20854-20862`）や `async_func_free`（下記5節）であり、これらはフレームウォーカー自身の責務。

## 4. `JSVarRef` — フレーム内変数への「借用」ポインタ

構造体定義 `quickjs.c:405-425`:

```c
typedef struct JSVarRef {
    union {
        JSGCObjectHeader header;
        struct { int __gc_ref_count; uint8_t __gc_mark; uint8_t is_detached; uint8_t is_lexical; uint8_t is_const; };
    };
    JSValue *pvalue;                 /* スタック上 or 自前の value への生ポインタ */
    union {
        JSValue value;                              /* is_detached == true のとき使用 */
        struct { uint16_t var_ref_idx; JSStackFrame *stack_frame; }; /* is_detached == false のとき使用 */
    };
} JSVarRef;
```

- **未検出（is_detached == false）の間、`pvalue` はフレームの `arg_buf`／`var_buf` 内の1要素を直接指す生ポインタであり、`stack_frame` はそのフレームへの生ポインタ。** フレームが（2a節の意味で）Cスタック上にある限り、このポインタはそのC関数呼び出しが生きている間しか有効でない。
- **検出済み（`close_var_ref` 後、is_detached == true）になると、`pvalue = &var_ref->value`（自分自身の埋め込みフィールド）に切り替わり、`value` はフレームから複製された値を持つ。以後はフレームと無関係にヒープ上で自立する。**

生成:
- `get_var_ref`（`quickjs.c:17582-17641`）: `vd->is_captured`（コンパイル時に事前計算されたキャプチャフラグ）が真なら、`sf->var_refs[var_ref_idx]` に既存の `JSVarRef` があればそれを再利用（refcount++、`quickjs.c:17606-17611`）、無ければ新規 `js_malloc` して `is_detached = false; var_ref_idx = ...; stack_frame = sf; pvalue = pvalue(=フレーム内アドレス);` とし `sf->var_refs[var_ref_idx] = var_ref;` に登録（`quickjs.c:17614-17626`）。**GCオブジェクトには登録しない**（`add_gc_object` を呼ばない — 循環コレクタは非検出var_refを見ない。理由は `mark_children` の `JS_GC_OBJ_TYPE_VAR_REF` 分岐に `assert(var_ref->is_detached);`（`quickjs.c:7132`）があることからも裏付けられる）。
  - `is_captured` が偽の場合（コンパイラがそもそもクロージャに取り込まれないと判定した変数、例: eval内から見える末端の変数）は、最初から `is_detached = true` の複製版を作る（`quickjs.c:17627-17639`）——**フレームへの生ポインタを一切持たない経路もある。**
- `js_create_var_ref`（`quickjs.c:17565-17580`）: 最初から detached な `JSVarRef` を作る補助関数。**呼び出し元判明（レビュー指摘により追記）:** `js_build_mapped_arguments`（`quickjs.c:16894`）が、legacy `arguments` オブジェクトの「宣言された仮引数の数を超える実引数」（`arg_count <= i < argc`）1個ごとに `js_create_var_ref(ctx, true)` を呼ぶ。これらの余剰引数はどの局所変数スロットにも対応しないため、フレームへの生ポインタを持たない detached な `JSVarRef` として最初から作られる（対して `i < arg_count` の範囲は同じ関数内で `get_var_ref(ctx, sf, i, true)`、`quickjs.c:16887`、フレームの `arg_buf[i]` を指す非detached版）。
- `js_create_module_var`（`quickjs.c:31176-31193`）: モジュールのトップレベル束縛用。**常に `is_detached = true` で生成され、どの `JSStackFrame` も指さない。** `pvalue = &var_ref->value` から開始。

解放・検出（close）:
- `close_var_ref`（`quickjs.c:17883-17890`）: `var_ref->value = js_dup(*var_ref->pvalue); var_ref->pvalue = &var_ref->value; var_ref->is_detached = true; add_gc_object(...)`。**この時点で初めて循環コレクタの対象になる。**
- `close_var_refs`（`quickjs.c:17892-17903`）: フレームの `var_refs[]` 全件を `close_var_ref` する。呼び出し箇所:
  - `JS_CallInternal` 通常関数の `done:`（`quickjs.c:20854-20858`）—— **ジェネレータ／async（`b->func_kind != JS_FUNC_NORMAL`）はこの分岐を通らない**（`quickjs.c:20849`のコメント: 「ローカル変数の解放は呼び出し元の責任。generatorではdoneに到達してはならない」）。
  - `async_func_free`（`quickjs.c:21116-21138`）—— ジェネレータ／async側の相当箇所（5節）。
- `close_lexical_var`（`quickjs.c:17905-17917`）: 単一の変数インデックスだけを閉じる。呼び出し箇所は `OP_close_loc`（`quickjs.c:19186-19191`）の**1箇所のみ**——for文の各イテレーションでlet束縛を再バインドする際、**フレームがまだ実行中のまま、当該変数の `JSVarRef` だけをフレームから切り離す。** これはフレーム全体の解放を待たない、実行中の途中断面でのポインタ無効化の実例。
- `free_var_ref`（`quickjs.c:6691-6707`）: refcountが0になったとき、`is_detached` なら `JS_FreeValueRT` して `remove_gc_object`。**非detachedのまま参照カウントが0になるケース**では `assert(sf->var_refs[var_ref->var_ref_idx] == var_ref); sf->var_refs[...] = NULL;`（`quickjs.c:6701-6702`）——つまり **`sf` はまだ生きている前提**でこの分岐に入る（非detachedなvar_refのrefcountが0になるのは、そのフレームがまだ実行中で、キャプチャしたクロージャがすべて先に解放された場合のみ起こりうる。フレーム終了時は先に`close_var_refs`が呼ばれてdetached化されているため、この分岐には来ない——という順序保証は本節の記述からの**推定**であり、コード上に明示コメントはない）。

closure構築時の参照:
- `js_closure2`（`quickjs.c:17643-17693`）: `JS_CLOSURE_LOCAL`/`JS_CLOSURE_ARG` は `get_var_ref(ctx, sf, ...)` で**呼び出し元が渡した `sf`**（実行中の親フレーム）から取得。`JS_CLOSURE_REF`/`JS_CLOSURE_GLOBAL_REF` は `cur_var_refs[cv->var_idx]`（親関数オブジェクト自身が既に保持している `JSVarRef` 配列）から再利用、refcount++のみで**新しいフレーム参照は作らない**。
- `js_closure`（`quickjs.c:17722-17778`）は `js_closure2` の薄いラッパー。
- **直接 `eval()`** (`__JS_EvalInternal`, `quickjs.c:38235-38243`, `38327`): `sf = ctx->rt->current_stack_frame;` と `var_refs = p->u.func.var_refs;` を取得し、パース完了後 `JS_EvalFunctionInternal(ctx, fun_obj, this_obj, var_refs, sf)`（`quickjs.c:38327`）→ `js_closure(ctx, fun_obj, var_refs, sf)`（`quickjs.c:38182`）に**その生ポインタをそのまま渡す**。パース処理自体は `JS_CallInternal` を再入しない（構文解析のみ）ため `sf` は関数全体を通じて有効。**未確認:** 間接eval（`eval_type != JS_EVAL_TYPE_DIRECT`）では `sf = NULL; var_refs = NULL;`（`quickjs.c:38246-38248`）で渡るため `js_closure2` 内の `JS_CLOSURE_LOCAL`/`JS_CLOSURE_ARG` 分岐で `sf` を参照すると NULL 参照になるはずだが、パーサが間接evalの生成関数に外側スコープのローカルをクロージャ変数として持たせないという不変条件を前提にしている（パーサ側コードは本タスクの範囲外につき未検証）。

その他の `JSVarRef` 経路（フレームとは無関係、GC対象・生存管理の一貫性のために記載）:
- `js_bytecode_function_finalizer`/`js_bytecode_function_mark`（`quickjs.c:6764-6817` 付近）: 関数オブジェクトが保持する `p->u.func.var_refs[]`（クロージャがキャプチャした変数）の解放・マーク。
- `js_mapped_arguments_finalizer`/`_mark`（`quickjs.c:16764-16796`）: legacy `arguments` オブジェクトの `u.array.u.var_refs[]`（各要素が引数へのvar_ref）。
- `OP_make_loc_ref`/`OP_make_arg_ref`/`OP_make_var_ref_ref`（`quickjs.c:19194-19226`）: `with`文やインダイレクトevalの変数バインディング用に、`JS_PROP_VARREF` 型プロパティへ `JSVarRef` を格納するオブジェクトを作る。`OP_make_var_ref_ref` は既存 `var_refs[idx]`（現フレームの配列）を再利用、他2つは `get_var_ref` で新規/既存取得。

## 5. `async_func_mark` / `async_func_free` — ジェネレータ・asyncフレームのGC連携と解放

`quickjs.c:21096-21138`。

- `async_func_mark`（`quickjs.c:21096-21114`）: `sf->cur_sp` が非NULL（＝関数が**実行中でない**、`quickjs.c:18181`参照）の場合のみ `sf->arg_buf` から `sf->cur_sp` までを1要素ずつ `JS_MarkValue`。**実行中（`cur_sp == NULL`）はマークしない**——コメント原文（`quickjs.c:21106-21109`）:「実行中はcur_spが不明なのでスタックをマークできない。実行中の関数が削除可能な循環の一部になることはないので、変数のマークは不要」。
- `async_func_free`（`quickjs.c:21116-21138`）:
  1. `sf->var_ref_count != 0` なら `close_var_refs(rt, sf)`（`quickjs.c:21125-21127`）でクロージャ変数を切り離す。
  2. `assert(sf->cur_sp != NULL);`（`quickjs.c:21130`）——**実行中のフレームを解放しようとするとassertで落ちる。** これは「実行中はフレームを動かせない／壊せない」という不変条件がコードレベルで保証されていることを示す直接証拠。
  3. `sf->arg_buf` から `sf->cur_sp` までを `JS_FreeValueRT` で解放し、`js_free_rt(rt, sf->arg_buf);` で確保していたヒープブロックそのものを解放。
  4. `sf->cur_func` と `s->this_val` を解放。
- 呼び出し元（フレーム解放のトリガ）:
  - `free_generator_stack_rt`（`quickjs.c:21170-21177`）: `s->state == JS_GENERATOR_STATE_COMPLETED` なら何もしない（既に解放済み）。それ以外は無条件で `async_func_free` → `state = COMPLETED`。ジェネレータの finalizer（`quickjs.c:21179-21187`）と明示close両方から到達。
  - `js_async_function_terminate`（`quickjs.c:21338-21344`）: `s->is_active` を見て `async_func_free` → `is_active = false`。
  - `js_async_generator_free`（`quickjs.c:21575-21594`）: キューに溜まった `JSAsyncGeneratorRequest` を先に全解放した後、`state` が `COMPLETED`/`AWAITING_RETURN` でなければ `async_func_free`。
  - `js_async_generator_complete`（`quickjs.c:21731-21738`）: 正常終了時に `state = COMPLETED` としてから `async_func_free`。

## 6. `build_backtrace` の詳細（frame walk の代表例）

`quickjs.c:7981-8140`台。

- スタック上に `JSCallSiteData csd[64];`（`quickjs.c:7994`）——**バックトレース深さは64フレームでハードキャップ**（`stack_trace_limit = min_int(stack_trace_limit, countof(csd));`、`quickjs.c:8024`）。`Error.stackTraceLimit` 相当の値と本数のうち小さい方。
- `sf_start = rt->current_stack_frame;`（`quickjs.c:8062`）からループ `for (sf = sf_start; sf != NULL && i < stack_trace_limit; sf = sf->prev_frame)`（`quickjs.c:8075`）。
- 各フレームで `p = JS_VALUE_GET_OBJ(sf->cur_func); b = js_class_has_bytecode(p->class_id) ? p->u.func.function_bytecode : NULL;`（`quickjs.c:8081-8088`、要約）。
- `b && sf->cur_pc` が真の場合のみ行・列番号を計算（`quickjs.c:8103-8116`）。**`sf->cur_pc` は3節で見た通り「主要なチェックポイントopcode（呼び出し・例外・yield/awaitなど）の直前」でしか更新されないため、それ以外の地点で `build_backtrace` を呼ぶような経路（現状は無いはず）があれば `cur_pc` は古い値になりうる。** コード中に既知のFIXMEコメントあり（`quickjs.c:8118-8120`）:「`JS_CallInternal`のあるバイトコードハンドラで`sf->cur_pc = pc`が欠けている。ユーザから観測可能なのは例外を投げるProxyをインターセプトする場合くらい」。
- `backtrace_barrier`（`b->backtrace_barrier`、`JS_EVAL_FLAG_BACKTRACE_BARRIER` 由来）が立つフレームでループを打ち切る（`quickjs.c:8130-8132`）——直接evalの境界を越えてバックトレースが漏れないようにする仕組み。

## 7. `js_poll_interrupts` — 既存の「中断」機構とセーフポイントとの違い

`quickjs.c:8456-8476`。

- `ctx->interrupt_counter` を毎回デクリメントし、0以下になったら `rt->interrupt_handler` を呼ぶ（`__js_poll_interrupts`, `quickjs.c:8456-8467`）。ハンドラが非ゼロを返すと `JS_ThrowInterrupted(ctx)` して `-1`。
- 呼び出し箇所（frameポインタに関係する事実として）: `JS_CallInternal` の**入口**（`quickjs.c:18164`）、`JS_CallConstructorInternal` の入口（`quickjs.c:20970`）、および**インタプリタループ内では後方分岐opcode（`OP_goto`/`OP_goto16`/`OP_goto8`/`OP_if_true`/`OP_if_true8`/`OP_if_false`/`OP_if_false8`、`quickjs.c:19241-19334`付近）でのみ**。加えて `for-in` 列挙のプロトタイプ鎖ループ2箇所（`quickjs.c:16913`, `16972`）。
- **重要な事実:** ここで言う「中断」は**例外による巻き戻し**であり、`goto exception` と同じ経路（`quickjs.c:20816`〜）を通って `close_var_refs` とローカル値の解放が即座に走る。**フレームの内容を保存して後で再開する仕組みではない。** `sf->cur_sp`/`sf->cur_pc` を保存する `done_generator:` 経路（`quickjs.c:20850-20852`）はジェネレータ/async関数の `yield`/`await`/`initial_yield` 専用であり、`js_poll_interrupts` の中断経路とは別物。
- インタプリタループは基本、**全opcodeで `sf->cur_pc` を更新するわけではない**（3節末尾の grep 結果参照）。中断可能な地点＝`sf->cur_pc`が最新化される地点は、呼び出し・例外・特定の分岐命令に限られる。本書の仕様案が言う「選択したopcodeでの中断確認」は、既存コードのこの制約と整合する（既存の `js_poll_interrupts` 呼び出し配置がまさにその「選択したopcode」の先例）。

## 8. `memcpy`/`alloca` によるフレーム領域の直接操作

- `alloca` によるフレーム確保は2a節の `quickjs.c:18226` の1箇所（バイトコード関数本体）と、2c節で見た `js_call_c_function`/`js_call_c_function_data`/`js_call_c_closure` 内の**引数コピー専用**の小さな `alloca`（`quickjs.c:17956`, `6439`, `6569`）のみ。これらは `argc < arg_count` のときに「未渡し引数を`JS_UNDEFINED`で埋めた作業用配列」を作るためで、フレーム全体の再配置ではない。
- フレーム領域を対象にした `memcpy` は**本セッションで検索した範囲では見当たらなかった**（`alloca`確保直後は要素ごとのループ代入 `arg_buf[i] = js_dup(argv[i]);` 等で埋めており、バルクコピーではない）。**未確認:** `memcpy` というキーワードそのものでのファイル全体grepは行っていない（時間の都合でフレーム関連の呼び出し元コンテキストからのみ判断した）。

## 9. L2/L3/L5 に効く点（判断ではなく、上記事実から素直に読み取れる制約の要約）

- 通常呼び出し（2a）はCスタック上の `alloca` にフレームを置き、C再帰で1段ごとにネイティブスタックを消費する。L2の「移動しない独立VMスタック」は、この`alloca`ブロックのレイアウト（arg_buf/var_buf/stack_buf/var_refs配列が1つの連続領域）をそのままVM管理領域に移す形が最小差分になりうる。**ただし置き換え対象は `alloca` ブロックだけではない——`JSStackFrame sf_s`自体（`quickjs.c:18191`）も`JS_CallInternal`のC関数フレーム上のローカル変数であり、`rt->current_stack_frame`（`quickjs.c:18243`再開時／`18316`通常時）・呼び出された側フレームの`prev_frame`・非detached`JSVarRef.stack_frame`（`quickjs.c:17682`、読み出しは`free_var_ref`、`quickjs.c:6759`）の3系統から生ポインタで指される。L2で「フレームを移動可能にする」なら、この`JSStackFrame`本体を`alloca`ブロックと同じ管理領域に埋め込む必要がある——`async_func_init`が`JSAsyncFunctionState`に`JSStackFrame frame;`を値埋め込みする設計（`quickjs.c:888-893`、`sf = &s->frame;`は`async_func_init`、`quickjs.c:21121`）が示す通り、quickjs-ng自身がヒープ常駐フレームで既にこの形を取っている。**
- ジェネレータ/async（2b）は既に「ヒープ上の安定アドレスを持つフレーム」を実装済み。`sf->cur_sp == NULL` が「実行中」を表す既存の不変条件（`quickjs.c:18181`, `21106-21109`, `21130`のassert）であり、L2のセーフポイント設計はこの不変条件を壊さない形に合わせる必要がある。
- `JSVarRef` は「フレームに生ポインタで居候」→「`close_var_ref`でヒープに複製して独立」という2段階のライフサイクルを既に持つ。L3の「相対参照による移動可能スタック」は、非detached `JSVarRef.pvalue`/`stack_frame` をどう補正するかが核心になる——現状はこの2フィールドが素の生ポインタである。
- GCのマークフェーズ（`JS_MarkContext`）は実行中Cスタックのフレームを一切見ない。フレームを独自プールへ移しても、値の生存管理はGCではなく「フレームウォーカー自身のfree/close処理」が担い続ける前提になる。
- 既存の中断機構（`js_poll_interrupts`）は例外による巻き戻し専用で、限られたopcode（後方分岐・呼び出し入口）でしか呼ばれない。L1/L2が言う「中断確認ポイント」はこの配置を土台にできるが、意味論（巻き戻し vs 一時停止）はゼロから作り直しが要る。
- ネイティブ関数呼び出し用の `JSStackFrame`（2c）は `var_ref_count`/`var_buf`/`var_refs`/`cur_pc`/`cur_sp` が未初期化。汎用フレームウォーカーを書く場合は `class_id` によるバイトコード関数判定なしにこれらへ触れてはならない。

## 未確認

- `JS_GetActiveFunction`（`ctx->rt->current_stack_frame->cur_func` を返す関数、`quickjs.c:17561`）の正式な関数名・シグネチャ。読んだ範囲の直前行（`quickjs.c:17560`）に開き波括弧しか見えず、シグネチャ行は読んでいない。
- 間接eval（`sf = NULL; var_refs = NULL;` を `js_closure2` に渡す経路）で、パーサが本当に外側スコープの `JS_CLOSURE_LOCAL`/`JS_CLOSURE_ARG` を生成しないという保証。パーサ本体（`js_parse_program` 等）は本タスクの範囲外につき未検証。
- `free_var_ref`（`quickjs.c:6691-6707`）で非detachedなvar_refのrefcountが0になる分岐に実際に到達しうるか、到達する場合のフレーム生存状態。コード上に明示的な保証コメントは無く、本ファイルの記述は呼び出し順序からの推定。
- `memcpy` によるフレーム領域操作の有無を、ファイル全体に対する `memcpy` 直接grepでは確認していない（フレーム関連の周辺コードを読む中で見当たらなかったのみ）。
- **本台帳群の行番号の基準:** 本ファイルの`quickjs.c`引用は `git rev-parse HEAD`（本ワークツリーでは `d9ef1f9`）に、`apps/vmprobe/` 計測フック用の未コミット差分（`quickjs.c`に+59行、6箇所のhunk）が当たった状態を読んで書かれている（本追記分および冒頭の主要な行番号はすべて本追記時点のセッションで直接再確認済み）。この差分がコミットされる、または別セッションが`quickjs.c`を編集すれば行番号は再びずれる——file:line を不変の参照先として扱わず、引用コード片が現物と一致するかを都度確認すること。
- generator/async再開時に `func_obj` を `JSAsyncFunctionState*` として扱う際（`quickjs.c:18167-18189`）、`JS_CALL_FLAG_GENERATOR` が立つのは `async_func_resume`（`quickjs.c:21140-21153`）からの呼び出しのみか、他の呼び出し元があるかは本セッションでは全呼び出し元を洗っていない（`async_func_resume` の呼び出し元は複数確認したが、それ以外に直接 `JS_CALL_FLAG_GENERATOR` を立てて `JS_CallInternal` を呼ぶ箇所が無いことまでは確認していない）。
- `JSCallSiteData` / `js_new_callsite_data` / `js_new_callsite_data2`（`Error.prepareStackTrace` 用）が `sf` から何をコピーするか、コピー後に生ポインタを保持し続けないかは、`quickjs.c:8090-8091`（呼び出し箇所）しか読んでおらず、`js_new_callsite_data` の本体は未読。
- `JS_UpdateStackTop`（`quickjs.c:2791-2795`）と `update_stack_limit` がフレームポインタ管理とどう関係するか（Cスタックオーバーフロー検出用の別系統と見えるが、本ファイルのスコープ外として深追いしていない）。
