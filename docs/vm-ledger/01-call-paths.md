# 01: JS 呼び出し経路の台帳

対象: `components/quickjs-ng/quickjs-ng/quickjs.c`（quickjs-ng 0.14.0 + immutable-buffer patch）。
scope: JS コードが呼ばれるすべての経路。JS_CallInternal の構造、各 call 系 opcode、JS_Call 系公開 API、getter/setter、Proxy trap、eval、bound function、C 関数呼び出し（js_call_c_function 系）、組み込みが JS へ再入する箇所、C スタック深さガード。

各項目は「事実」であり、L2/L3/L5 への含意は末尾の一覧にまとめる。行番号は 2026-09-12 時点の本ファイルの内容を指す。

## 1. JS_CallInternal の構造

### 1.1 シグネチャと入口

`quickjs.c:18124` `static JSValue JS_CallInternal(JSContext *caller_ctx, JSValueConst func_obj, JSValueConst this_obj, JSValueConst new_target, int argc, JSValueConst *argv, int flags)`。

- `quickjs.c:18164` 入口で `js_poll_interrupts(caller_ctx)` を呼ぶ。true なら `JS_EXCEPTION` を返す（フレームを一切積まずに、この時点で中断相当の終了ができる唯一の場所）。
- `quickjs.c:18167-18193` `func_obj` のタグが `JS_TAG_OBJECT` でない場合、`flags & JS_CALL_FLAG_GENERATOR` が立っていればジェネレータ/async の再開経路（1.2 節）へ、立っていなければ `not_a_function` で `JS_ThrowTypeErrorNotAFunction` を返す。
- `quickjs.c:18194-18204` `p->class_id != JS_CLASS_BYTECODE_FUNCTION` の場合、`rt->class_array[p->class_id].call`（非 bytecode 関数のディスパッチ、3節）を呼んで即座に return する。**この経路は JS_CallInternal のフレームを一切使わない** — C 関数・bound function・Proxy などは別関数の C スタックフレームで動く。

### 1.2 ジェネレータ/async 再開の特殊入口

`quickjs.c:18168-18189`: `func_obj` が `JS_TAG_OBJECT` でなく `JS_CALL_FLAG_GENERATOR` が立つ場合、`func_obj` の生ポインタは `JSAsyncFunctionState*` として解釈される（`JS_MKPTR(JS_TAG_INT, s)` で偽装、`async_func_resume` `quickjs.c:21149` が構築）。この場合:

- `sf = &s->frame`（**ヒープ上のフレームを再利用する** — `alloca` しない）。
- `local_buf = arg_buf = sf->arg_buf`、`var_buf = sf->var_buf`、`stack_buf = sf->var_buf + b->var_count`、`sp = sf->cur_sp`（前回中断時に保存した SP）、`pc = sf->cur_pc`（前回中断時に保存した PC）。
- `sf->cur_sp = NULL`（「実行中は cur_sp が NULL」という不変条件、`quickjs.c:18181` のコメント）。GC マーカー `async_func_mark` `quickjs.c:21096-21114` はこの不変条件を読み、`cur_sp` が NULL なら stack 領域をマークしない（「実行中の関数は削除可能循環の一部になり得ない」という前提、コメントに明記）。
- `s->throw_flag` が立っていれば `goto exception`、そうでなければ `goto restart`（通常の call/return フレーム構築を一切通らずディスパッチループへ入る）。

これは**quickjs-ng が既に持っている「中断・再開」の唯一の実装**であり、L2 の設計対象そのものである（8節参照）。

### 1.3 通常フレームの構築（bytecode function 呼び出し）

`quickjs.c:18205-18258`。

- `b = p->u.func.function_bytecode`。
- `arg_allocated_size`: `argc < b->arg_count` または `JS_CALL_FLAG_COPY_ARGV` が立つ場合のみ `b->arg_count`、それ以外は 0（**呼び出し元の argv をそのまま arg_buf として使う最適化** — `arg_buf = (JSValue *)argv;` `quickjs.c:18221`。この場合 argv の生存は呼び出し元责任）。
- `alloca_size = sizeof(JSValue) * (arg_allocated_size + b->var_count + b->stack_size) + sizeof(JSVarRef *) * b->var_ref_count;` `quickjs.c:18213-18215`。
- `quickjs.c:18216-18218` `js_check_stack_overflow(rt, alloca_size)` を通ってから **初めて** `alloca` する。overflow なら `JS_ThrowStackOverflow` を返し、この時点でもまだフレームは積まれていない。
- `quickjs.c:18226` `local_buf = alloca(alloca_size);` — 単一の alloca 呼び出しで arg/var/operand-stack/var_refs 配列をまとめて確保する。**L2 の「非移動セグメントへの確保」はこの alloca 呼び出し 1 箇所を置き換える設計になる。**
- `arg_allocated_size` が真なら `arg_buf = local_buf` にコピー、argv 側は `js_dup` で参照カウントを増やしてから積む（呼び出し元の argv 解放と独立させるため）。不足分は `JS_UNDEFINED` で埋める。
- `var_buf = local_buf + arg_allocated_size;` 各要素は `JS_UNDEFINED` で初期化 `quickjs.c:18242-18244`。
- `stack_buf = var_buf + b->var_count;`（オペランドスタックの先頭）。
- `sf->var_refs = (JSVarRef **)(stack_buf + b->stack_size);`（クロージャ用 var_ref ポインタ配列は alloca ブロックの末尾）。**要素は生ポインタ配列であり、この alloca ブロックが移動すると `sf->var_refs` 自体の指すアドレスとその中身の意味の両方が壊れる。**
- `sp = stack_buf; pc = b->byte_code_buf;`
- `sf->cur_pc = NULL;`（コメント `quickjs.c:18254`: 「sf->cur_pc は再帰呼び出しの前に必ず設定されていなければならない」— 各 call 系 opcode は呼ぶ直前に `sf->cur_pc = pc` を書く。これは backtrace 用のみで、中断・再開の PC 保存とは別目的）。
- `sf->prev_frame = rt->current_stack_frame; rt->current_stack_frame = sf;`（**フレームは C 再帰の連鎖として rt に単方向リンクされる。** リンクはグローバルな `rt->current_stack_frame` 経由であり、runtime 全体で「今実行中のフレーム」は 1 個しか表現できない — マルチ VM タスク化＝L5 で最初に壊れる不変条件）。
- `ctx = b->realm;`（**呼び出し先の realm へ切り替わる。** 呼び出し元の `caller_ctx` とは別の `JSContext*` になり得る）。

### 1.4 ディスパッチループ

`quickjs.c:18266` `restart:` ラベルの直後に `for (;;) { SWITCH(pc) { ... } }`。`DIRECT_DISPATCH` が有効ならスイッチはトランポリン無しの `goto *dispatch_table[opcode]`（`quickjs.c:18152-18161`）、無効なら通常の `switch`。**`restart:` は catch ハンドラへ飛ぶときと、ジェネレータ再開時にも使われる共通の再入点。**

### 1.4.1 `done`/`done_generator`の分岐はコンパイル時の`b->func_kind`だけで決まる——レビュー指摘による追記

`quickjs.c:20904-20922`（1.5節）の `if (b->func_kind != JS_FUNC_NORMAL) { done_generator: ... } else { done: ... }` という分岐は、**実行時のフレームの状態や呼び出し規約ではなく、そのバイトコード関数がコンパイルされた時点で決まる`b->func_kind`（`JSFunctionBytecode`の固定フィールド）だけで選ばれる。** `JSStackFrame`自体には「このフレームはVMが解放責任を持つか」を示すフラグは無い。

- **この結び付きが暗黙の不変条件を作っている:** 通常関数（`JS_FUNC_NORMAL`）は常に`alloca`されたCスタックフレーム（2a節）を持ち`done`へ、generator/async/async generatorは常にヒープ常駐フレーム（2b節）を持ち`done_generator`へ、という対応が**バイトコードのコンパイル結果と呼び出し経路（2a vs 2b）の両方で独立に維持されている**ことが前提。`quickjs.c:20905-20907`のコメント「ローカル変数はジェネレータの場合は呼び出し元が解放する。doneに到達してはならない」は、この対応が崩れると何が起きるかを直接示している：
  - もし通常関数（`alloca`フレーム）が誤って`done_generator`に到達すれば、`sf->cur_pc`/`sf->cur_sp`を書き込むだけでローカル変数を一切解放しない——**そのフレームのJSValueはCスタックの巻き戻りとともに（`JS_FreeValue`されずに）ただ消える、参照カウントのリーク。**
  - もしgenerator/asyncフレーム（ヒープ常駐）が誤って`done`に到達すれば、`close_var_refs`+`JS_FreeValue`ループでフレーム内容を解放してしまう——**その後`async_func_free`（5節）が同じ値をもう一度`JS_FreeValueRT`しようとする、二重解放。**
- **L2への含意:** L2が「任意のバイトコード関数のフレームをサスペンド可能にする」（＝コンパイル時に`func_kind`で決め打たず、実行時にサスペンドするかどうかを選べるようにする）方向へ一般化する場合、この`b->func_kind`によるコンパイル時ゲートを、`JSStackFrame`（またはそれを包む構造体）が持つ**実行時フラグ**（「このフレームはVM管理領域にあり、解放はフレームウォーカー側の責任」）に置き換える必要がある。現状はこの判断がバイトコード側にハードコードされており、フレーム自身には委ねられていない。

### 1.5 正常終了（done / done_generator）

`quickjs.c:20714-20719`: `OP_return` は `ret_val = *--sp; goto done;`。`OP_return_undef` は `ret_val = JS_UNDEFINED; goto done;`。

`quickjs.c:20849-20863`:
- `b->func_kind != JS_FUNC_NORMAL`（generator/async/async generator）の場合は `done_generator:` へ。`sf->cur_pc = pc; sf->cur_sp = sp;` だけ行い、**ローカル変数もオペランドスタックも解放しない**（コメント: 「ローカル変数はジェネレータの場合は呼び出し元が解放する。done ラベルに到達してはならない」）。フレームはヒープ上 (`JSAsyncFunctionState`) に生存し続ける。
- 通常関数の場合は `done:` へ。`sf->var_ref_count != 0` なら `close_var_refs(rt, sf)`（生存中のクロージャへ値をコピーして切り離す）、その後 `local_buf` から `sp` までを `JS_FreeValue`。
- 共通: `rt->current_stack_frame = sf->prev_frame; return ret_val;`（**alloca したブロックは C 関数からの return と同時に暗黙に解放される** — 明示的な free 呼び出しは存在しない。L2 でセグメントを明示的に pop する箇所はここに対応する）。

### 1.6 例外の巻き戻し

`quickjs.c:20816-20845` `exception:` ラベル。

- `needs_backtrace(rt->current_exception) || JS_IsUndefined(ctx->error_back_trace)` なら `sf->cur_pc = pc` を保存してから `build_backtrace` を呼ぶ（backtrace 構築時点の PC が正しく分かる必要があるため、この代入は例外発生後に行われる — 1.3 節の「call 直前に cur_pc を書く」規約と役割が異なる）。
- `!JS_IsUncatchableError(rt->current_exception)` の場合のみ、`while (sp > stack_buf)` でオペランドスタックを 1 要素ずつ `JS_FreeValue` しながら pop する。`JS_TAG_CATCH_OFFSET` を見つけたら:
  - `pos == 0`: for-in/for-of の enumerator クローズ（`JS_IteratorClose`。ここは **C->JS 再入**、下記 exception 巻き戻し中に別の JS 呼び出しが発生する数少ない箇所）。
  - `pos != 0`: 例外値を `sp` に積み直し、`pc = b->byte_code_buf + pos;` として `goto restart;`（catch ブロックへジャンプ、フレームは再利用）。
  - **uncatchable error（`JS_ThrowInterrupted` が立てるもの、7節）はこの while ループを一切通らない** — catch もオペランドスタックの解放もされないまま次の分岐へ落ちる。実質的に「巻き戻しをスキップして即終了する」経路であり、finally ブロックを実行しない。
- ループを抜けたら（catch されなかった、または uncatchable）`ret_val = JS_EXCEPTION;` として 1.5 節と同じ `done`/`done_generator` 分岐に合流する。

## 2. call 系 opcode

いずれも `JS_CallInternal` のディスパッチループ内で直接処理される（別関数を呼ばない = C 再帰を増やさない call 準備部分と、`JS_CallInternal`/`JS_CallConstructorInternal` を呼ぶ箇所＝C 再帰する部分がある）。

| opcode | 行 | 準備 | 呼び出し先 | tail call | 中断確認 |
| --- | --- | --- | --- | --- | --- |
| `OP_call0`..`OP_call3` | `quickjs.c:18614-18619` | `call_argc = opcode - OP_call0` | `has_call_argc` 経由で下記 `OP_call` と合流 | — | — |
| `OP_call` | `quickjs.c:18620-18643` | `call_argc = get_u16(pc)` | `JS_CallInternal(ctx, call_argv[-1], JS_UNDEFINED, JS_UNDEFINED, call_argc, vc(call_argv), 0)` | いいえ | 呼び出し前に `sf->cur_pc = pc`。呼び出し自体に中断確認は無い（JS_CallInternal 入口の `js_poll_interrupts` に依存） |
| `OP_tail_call` | 同上 | 同上 | 同上 | `goto done`（ret_val 解放の for ループと `sp` の巻き戻しをスキップし、呼び出し先の戻り値をそのまま自関数の戻り値にする。**C スタック上は依然として通常の再帰呼び出しであり、C フレームは消費される** — 「末尾呼び出し」は JS フレーム値のみを畳み、C 再帰は畳まない） | 同上 |
| `OP_call_constructor` | `quickjs.c:18644-18661` | `call_argv[-2]`=func, `call_argv[-1]`=new_target | `JS_CallConstructorInternal(ctx, call_argv[-2], call_argv[-1], call_argc, vc(call_argv), 0)` | いいえ | 同上 |
| `OP_call_method` | `quickjs.c:18662-18683` | `call_argv[-2]`=this | `JS_CallInternal(ctx, call_argv[-1], call_argv[-2], JS_UNDEFINED, call_argc, vc(call_argv), 0)` | いいえ | 同上 |
| `OP_tail_call_method` | 同上 | 同上 | 同上 | `goto done` | 同上 |
| `OP_call_constructor`（super）| `quickjs.c:18741-18758` `OP_init_ctor` | `super = JS_GetPrototype(ctx, func_obj)` | `JS_CallConstructor2(ctx, super, new_target, argc, argv)` | — | `sf->cur_pc = pc` のみ |
| `OP_array_from` | `quickjs.c:18684-18695` | — | `JS_NewArrayFrom`（JS 呼び出しではない。配列生成のみ） | — | — |
| `OP_apply` | `quickjs.c:18697-18713` | `magic` | `js_function_apply(ctx, sp[-3], 2, vc(&sp[-2]), magic)`（10節） | — | `sf->cur_pc = pc` のみ |
| `OP_eval` | `quickjs.c:18811-18841` | `scope_idx` | `ctx->eval_obj` と同一なら `JS_EvalObject(..., JS_EVAL_TYPE_DIRECT, scope_idx)`（direct eval）、そうでなければ通常の `JS_CallInternal` | — | `sf->cur_pc = pc` のみ |
| `OP_apply_eval` | `quickjs.c:18843-18876` | `build_arg_list(ctx, &len, sp[-1])` | 同上の分岐 + `JS_Call(ctx, sp[-2], JS_UNDEFINED, len, vc(tab))` | — | `sf->cur_pc = pc` のみ |

`goto has_call_argc` は `OP_call0..3` と `OP_call`/`OP_tail_call` が同じ呼び出しコードを共有するための合流点であり、**opcode 種別（tail か否か）は呼び出し後の `if (opcode == OP_tail_call)` 判定でのみ分岐する** — 呼び出し自体は無条件に同じ `JS_CallInternal` を通る。

例外はすべて `if (unlikely(JS_IsException(ret_val))) { goto exception; }` の形で即座に 1.6 節の巻き戻しへ合流する（各 opcode で共通のパターン）。

## 3. 非 bytecode 関数のディスパッチテーブル（`rt->class_array[class_id].call`）

`JS_CallInternal`（`quickjs.c:18195-18204`）と `JS_CallConstructorInternal`（`quickjs.c:20981-20990`）は、`p->class_id != JS_CLASS_BYTECODE_FUNCTION` の場合に共通してこのテーブルを引く。登録箇所:

| class_id | call 関数 | 登録行 |
| --- | --- | --- |
| `JS_CLASS_C_FUNCTION` | `js_call_c_function` | `quickjs.c:2001` |
| `JS_CLASS_C_FUNCTION_DATA` | `js_call_c_function_data` | `quickjs.c:2002` |
| `JS_CLASS_C_CLOSURE` | `js_call_c_closure` | `quickjs.c:2003` |
| `JS_CLASS_BOUND_FUNCTION` | `js_call_bound_function` | `quickjs.c:2004` |
| `JS_CLASS_GENERATOR_FUNCTION` | `js_call_generator_function` | `quickjs.c:2005` |
| `JS_CLASS_PROXY` | `js_proxy_call` | `quickjs.c:53976`（実行時に条件付きで設定） |
| `JS_CLASS_PROMISE_RESOLVE_FUNCTION` / `JS_CLASS_PROMISE_REJECT_FUNCTION` | `js_promise_resolve_function_call` | `quickjs.c:57351-57352` |
| `JS_CLASS_ASYNC_FUNCTION` | `js_async_function_call` | `quickjs.c:57353` |
| `JS_CLASS_ASYNC_FUNCTION_RESOLVE` / `JS_CLASS_ASYNC_FUNCTION_REJECT` | `js_async_function_resolve_call` | `quickjs.c:57354-57355` |
| `JS_CLASS_ASYNC_GENERATOR_FUNCTION` | `js_async_generator_function_call` | `quickjs.c:57356` |

これらは**JS_CallInternal のフレームを使わない C 関数**であり、それぞれが独自に `JSStackFrame sf_s` をスタック上（C 変数として）確保する（`js_call_c_function`: `quickjs.c:17922-18050`、`js_call_c_function_data`: `quickjs.c:6415-6457`、`js_call_c_closure`: `quickjs.c:6549` 以降、`js_call_bound_function`: `quickjs.c:18052-18085`）。

### 3.1 js_call_c_function（通常の C 関数 = 大半の組み込みメソッド）

`quickjs.c:17922-18050`。

- `quickjs.c:17940` `js_check_stack_overflow(rt, sizeof(arg_buf[0]) * arg_count)` を通ってから、必要なら `arg_buf = alloca(...)` で不足引数を `JS_UNDEFINED` 埋め。
- `sf->prev_frame = prev_sf; rt->current_stack_frame = sf;`（1.3 節と同じリンク規約）。
- `ctx = p->u.cfunc.realm;`（realm 切り替え）。
- `cproto`（`JSCFunctionEnum`）で分岐し、`func.generic` / `func.generic_magic` / `func.getter` / `func.setter` / `func.getter_magic` / `func.setter_magic` / `func.f_f` / `func.f_f_f` / `func.iterator_next` のいずれかを直接呼ぶ（`quickjs.c:17967-18046`）。**これらは C 関数であり、内部でさらに `JS_Call` 系を呼べば C->JS 再入になる**（10節に列挙する組み込みの大半がここ経由で呼ばれる）。
- `default: abort();`（`quickjs.c:18045` — 未知の `cproto` は握りつぶさず即死。フェイルセーフではない）。

### 3.2 js_call_bound_function（`Function.prototype.bind`）

`quickjs.c:18052-18085`。`bf->argc + argc` 個の引数を alloca でまとめ（`bf->argv` を前、呼び出し時の `argv` を後ろに連結）、`JS_CALL_FLAG_CONSTRUCTOR` なら `JS_CallConstructor2(ctx, bf->func_obj, new_target, ...)`、そうでなければ `JS_Call(ctx, bf->func_obj, bf->this_val, ...)` を呼ぶ。**bound function は自分ではフレームを持たず、束縛先を即座に呼び直すだけ** — bind の多重適用は C 再帰段数分だけ `js_call_bound_function` が積み重なる。

## 4. JS_Call 系公開 API

`quickjs.c:20868-21048`。すべて最終的に `JS_CallInternal` または `JS_CallConstructorInternal` へ委譲する薄いラッパー。

- `JS_Call`（`quickjs.c:20868-20873`）: `JS_CallInternal(ctx, func_obj, this_obj, JS_UNDEFINED, argc, argv, JS_CALL_FLAG_COPY_ARGV)`。**常に `JS_CALL_FLAG_COPY_ARGV` を立てる** — 公開 API 経由の呼び出しは argv を必ずコピーする（1.3 節の最適化を使わない）。
- `JS_CallFree`（`quickjs.c:20875-20882`）: `JS_Call` 相当を呼んだ後 `func_obj` を `JS_FreeValue` する。組み込み内の大半の再入がこちらを使う（呼び出し先を取得した一時参照をその場で消費するため）。
- `JS_CallConstructorInternal`（`quickjs.c:20961-21012`）:
  - `quickjs.c:20970` 入口で `js_poll_interrupts(ctx)`。
  - `p->is_constructor` でなければ `JS_ThrowTypeErrorNotAConstructor`。
  - 非 bytecode 関数なら 3 節のテーブルを `JS_CALL_FLAG_CONSTRUCTOR` 付きで呼ぶ。
  - `b->is_derived_class_constructor` なら `JS_CallInternal(ctx, func_obj, JS_UNDEFINED, new_target, ...)`（`this` は関数内の `OP_init_ctor`/super 呼び出しで後から決まる — derived class の `this` 未初期化規則）。
  - そうでなければ「legacy constructor」: `js_create_from_ctor` で `this` オブジェクトを先に作り（`new_target.prototype` を継承）、`JS_CallInternal(ctx, func_obj, obj, new_target, ...)` を呼び、戻り値がオブジェクトなら戻り値を、そうでなければ先に作った `obj` を返す（`quickjs.c:21002-21010`、仕様の `[[Construct]]` の分岐そのもの）。
- `JS_CallConstructor2` / `JS_CallConstructor`（`quickjs.c:21014-21029`）: 薄いラッパー。前者は `new_target` を明示、後者は `func_obj` 自身を `new_target` にする。
- `JS_Invoke`（`quickjs.c:21031-21040`）: `JS_GetProperty(ctx, this_val, atom)` でメソッドを取得し（**この GetProperty がすでに getter を呼び得る C->JS 再入点**）、`JS_CallFree(ctx, func_obj, this_val, argc, argv)`。
- `JS_InvokeFree`（`quickjs.c:21042-21048`）: `JS_Invoke` の後 `this_val` を解放。

## 5. getter/setter 呼び出し

- **getter**: `JS_GetPropertyInternal`（`quickjs.c:8931` 開始）内、通常プロパティの getter は `quickjs.c:8994-9001`（`JS_CallFree(ctx, func, this_obj, 0, NULL)`）、exotic メソッドの `get_own_property` 経由の getter は `quickjs.c:9070`（同じ `JS_CallFree` パターン）。どちらも「フィールドが getter 実行中に削除され得る」ことをコメントで明示している。
- **setter**: `call_setter`（`quickjs.c:10214-10229`）が唯一の呼び出し口。`JS_CallFree(ctx, func, this_obj, 1, vc(&val))`。プロパティ設定経路（`JS_SetPropertyInternal` 系）はすべてここを通る（未確認: 全呼び出し元の列挙はスコープ外）。
- **共通点**: getter/setter 呼び出し前に対象関数を `js_dup` してから呼ぶ（コールバック中にプロパティが消されても解放済み関数を呼ばないための対策）。

## 6. Proxy trap

`js_proxy_exotic_methods`（`quickjs.c:53866`）が `JS_CLASS_PROXY` の exotic メソッドテーブルであり、5節の `em->get_property`/`em->get_own_property` や `JS_SetPropertyInternal` 内の同種の分岐から呼ばれる。各 trap 実装はすべて `get_proxy_method`（`quickjs.c:52983`、trap 関数を handler から `JS_GetProperty` で取得。`quickjs.c:52990` に `js_check_stack_overflow` あり — 6節末で理由を補足）経由でハンドラのメソッドを取得し、`JS_CallFree` で呼ぶ:

| trap | 関数 | 行 |
| --- | --- | --- |
| `[[GetPrototypeOf]]` | `js_proxy_getPrototypeOf` | `quickjs.c:53011`（呼び出し `53024`） |
| `[[SetPrototypeOf]]` | `js_proxy_setPrototypeOf` | `53055`（`53073`） |
| `[[IsExtensible]]` | `js_proxy_isExtensible` | `53105`（`53119`） |
| `[[PreventExtensions]]` | `js_proxy_preventExtensions` | `53135`（`53149`） |
| `[[Has]]`（`in` 演算子）| `js_proxy_has` | `53167`（`53190`） |
| `[[Get]]` | `js_proxy_get` | `53214`（`53239`） |
| `[[Set]]` | `js_proxy_set` | `53267`（`53293`） |
| `[[GetOwnProperty]]` | `js_proxy_get_own_property` | `53367`（`53392`） |
| `[[DefineOwnProperty]]` | `js_proxy_define_own_property` | `53473`（`53507`） |
| `[[Delete]]` | `js_proxy_delete_property` | `53577`（`53600`） |
| `[[OwnPropertyKeys]]` | `js_proxy_get_own_property_names` | `53643`（`53664`） |
| `[[Construct]]` | `js_proxy_call_constructor` | `53772`（trap 呼び出し `53798`、trap 未定義時は `JS_CallConstructor2(ctx, s->target, ...)` を直接呼ぶ `53788`） |
| `[[Call]]` | `js_proxy_call` | `53809`（trap 呼び出し `53840`、trap 未定義時は `JS_Call(ctx, s->target, ...)` を直接呼ぶ `53830`） |

`js_proxy_isArray`（`quickjs.c:53847`）にも `js_check_stack_overflow`（`53854`）があり、これは trap 呼び出しではなく `target` を再帰的に辿る `Array.isArray` 判定用（Proxy の target が別の Proxy を無限に連鎖できるため）。

**Proxy は多段にネストできる**（`revoked` チェックのみで段数制限は無い、未確認: 明示的な深さ上限の有無は本節の調査範囲では見つからず — 7節の `js_check_stack_overflow` に依存していると推測、推定）。1 段ごとに trap 呼び出し = `js_call_c_function` の C フレーム + trap がユーザー定義関数なら `JS_CallInternal` の C フレームが積まれる。

## 7. eval / direct eval

- **間接 eval**（`eval(x)` を `eval !== globalThis.eval` として呼ぶ、あるいは `eval.call(...)`）: 2節の `OP_eval`/`OP_apply_eval` で `call_argv[-1]`（呼び出し対象）が `ctx->eval_obj` と同一かどうかで分岐し、同一でなければ通常の `JS_CallInternal`/`JS_Call` として扱われる（=間接 eval は通常の関数呼び出しと同じグローバルスコープ実行になる、という仕様どおりの経路）。
- **直接 eval**: `js_same_value(ctx, call_argv[-1], ctx->eval_obj)` が真の場合のみ `JS_EvalObject(ctx, JS_UNDEFINED, obj, JS_EVAL_TYPE_DIRECT, scope_idx)`（`quickjs.c:18825-18826`、`18862-18863`）。`scope_idx` はコンパイル時にバイトコードへ埋め込まれた「呼び出し元のレキシカルスコープ」を指す（direct eval が呼び出し元の変数を見える仕様の実装）。
- **関数としての eval 呼び出し口**: `JS_EvalFunctionInternal`（`quickjs.c:38173-`）は `JS_TAG_FUNCTION_BYTECODE` なクロージャを `js_closure` で包んでから `JS_CallFree`（`38183`）で呼ぶ。モジュールの場合は `JS_CallFree` を経由せず `js_create_module_function`/`js_link_module`/`js_evaluate_module`（別経路、10.7 節参照）。

## 8. 非同期実行モデル（generator / async function）とセーフポイント相当の既存実装

これは L2 の設計対象と直接比較すべき「既存の中断・再開」実装である。

- `async_func_init`（`quickjs.c:21051-21094`）: `JSAsyncFunctionState` 内の `JSStackFrame` に対し、`sf->arg_buf = js_malloc(ctx, alloc_size)` で **ヒープ上に**フレームを確保する（1.3 節の alloca と対照的）。`alloc_size` の内訳は 1.3 節の `alloca_size` と同じ式（`arg_buf_len + var_count + stack_size` 個の `JSValue` + `var_ref_count` 個の `JSVarRef*`）。**generator/async はフレームが最初からヒープにあるため、L2 が「移動しない専用領域」に載せ替える対象そのもの。**
- `async_func_resume`（`quickjs.c:21140-21153`）: `js_check_stack_overflow(ctx->rt, 0)` の後、1.2 節の特殊入口経由で `JS_CallInternal` を呼ぶ。
- `async_func_free`（`quickjs.c:21116-21138`）: `sf->cur_sp != NULL` を assert（=「実行中でない」ことの表明）した上で `arg_buf` から `cur_sp` までを解放して `js_free_rt`。
- **中断（yield/await）の実体**: `OP_await`/`OP_yield`/`OP_yield_star`/`OP_async_yield_star`/`OP_return_async`/`OP_initial_yield`（`quickjs.c:20750-20763`）はいずれも `ret_val` に `FUNC_RET_AWAIT`/`FUNC_RET_YIELD`/`FUNC_RET_YIELD_STAR`/`JS_UNDEFINED` を積んで `goto done_generator;`。1.5 節のとおり `done_generator` は `sf->cur_pc = pc; sf->cur_sp = sp;` のみでフレームを保持したまま `JS_CallInternal` から抜ける。**この 6 opcode が、quickjs-ng における唯一の「安全に中断できる地点」の実装**であり、L2 の opcode セーフポイントはこれの汎用化にあたる。
- `js_generator_next`（`quickjs.c:21211-`）が `state`（`SUSPENDED_START` / `SUSPENDED_YIELD` / `SUSPENDED_YIELD_STAR` / `EXECUTING` / `COMPLETED`）を見て `async_func_resume` を呼ぶかどうかを決める。
- **async function の実行**: `js_async_function_resume`（`quickjs.c:21406-`）が `async_func_resume` の戻り値（`FUNC_RET_AWAIT` かどうか）を見て、await 対象を thenable として `JS_Call(ctx, s->resolving_funcs[...], ...)` で resolve/reject 関数に橋渡しする（`21418`, `21438`）。**async function は「1 opcode 実行するごとに JS_CallInternal を抜けて Promise ジョブキューに戻る」を繰り返す** — quickjs-ng は元々ジョブキュー越しにしか await を続けられない設計であり、L1 のジョブ境界制御と親和性が高い。

### 8.1 `async_func_resume` が「取り戻さない」もの2点——レビュー指摘による訂正・追記

- **`JSStackFrame` は `this_obj`／`new_target`／呼び出し `flags`／呼び出し元 `caller_ctx` のいずれも保持しない。** `async_func_resume`（`quickjs.c:21199-21212`）が再開時に `JS_CallInternal` へ渡すのは `s->this_val`（`JSAsyncFunctionState` 側が別途保持、6節参照）と、`new_target` は常に `JS_UNDEFINED` 固定（`quickjs.c:21209-21211`）。`new_target` を読む opcode（`OP_special_object` の `NEW_TARGET` 分岐、`quickjs.c:18483` 付近）や `OP_init_ctor`（`quickjs.c:18800`、derived class の super 呼び出し）は、**現状 generator/async 関数の内部では発生しない構文パターン**（`new.target` や `super()` を含む generator/async は仕様上存在しないか、存在しても別経路でコンパイルされる）だからこそこの欠落が表面化していないと読める——ただし本ledgerではその不変条件をパーサ側コードで確認していない（推定、未確認セクション参照）。**L2でこのサスペンド機構を「任意のバイトコード関数」へ一般化する場合、`this_obj`/`new_target`/呼び出し`flags`のうちフレームが今は運んでいない情報を、別途どこかに保存する設計変更が要る。**
- **`async_func_resume` は C 再帰そのものを除去しない。** 8節冒頭の「L2はこの機構の一般化」という要約は生成物のみに注目したもので、C スタック使用量の観点では誤解を招く。`async_func_resume` 自身が `js_check_stack_overflow(ctx->rt, 0)`（`quickjs.c:21203`）でCスタック残量をチェックしてから、通常どおり `JS_CallInternal` を**Cの関数呼び出しとして**再入する（`quickjs.c:21209`）。この再開後、レジューム対象の関数本体が別のJS関数を呼べば、それは通常呼び出しと全く同じ `JS_CallInternal` のC再帰（1.3節/2節）として積まれる（`quickjs.c:18685`等の`call_argv`計算を経由）。**generator/asyncのサスペンド機構が取り除いているのは「フレームのデータ（ローカル変数・オペランドスタック）をCスタック上に置く」ことだけであり、「JS呼び出しの深さに比例してCスタックを消費する」という性質そのものは temporaly resume後もそのまま残る。** L2の目標（spec 7節「C スタック使用量が深さに比例しない」）に対して、既存の generator/async 機構は**部分的な**先例に過ぎない——ここを「L2はこれの一般化」と表現すると、C再帰の除去まで既に実装済みであるかのように読めてしまう。

## 9. C スタック深さガード（`js_check_stack_overflow`）

`quickjs.c:1946` が定義（本ファイル内の実装詳細は未読 — 別ledgerの対象、ここでは呼び出し側のみ扱う）。呼び出し元の一覧（JS 呼び出しに関係するもののみ抜粋、全リストは下記コード表参照）:

| 行 | 関数 | 文脈 |
| --- | --- | --- |
| `6436` | `js_call_c_function_data` | C(data) 関数呼び出し前の alloca 前 |
| `6566` | `js_call_c_closure` | C closure 呼び出し前の alloca 前 |
| `17940` | `js_call_c_function` | 通常 C 関数呼び出し前の alloca 前 |
| `18064` | `js_call_bound_function` | bound function の引数連結 alloca 前 |
| `18216` | `JS_CallInternal` | 通常 bytecode フレームの alloca 前（1.3 節） |
| `21144` | `async_func_resume` | ジェネレータ/async 再開前（alloca は伴わない。0 バイトチェック＝純粋な深さチェック） |
| `31307` | `js_inner_module_linking` | モジュールリンクの再帰 |
| `31898` | `gather_available_ancestors` | モジュール依存グラフの再帰 |
| `31947` | `js_async_module_execution_rejected` | モジュール評価失敗伝播の再帰 |
| `45124` | `JS_FlattenIntoArray` | `Array.prototype.flat`/`flatMap` の再帰 |
| `50338` | `lre_check_stack_overflow` | 正規表現エンジン（libregexp）からの再帰チェック委譲 |
| `51828` | `json_parse_value` | JSON.parse の再帰下降パーサ |
| `52074` | `internalize_json_property` | JSON.parse の reviver 適用の再帰 |
| `52385` | `js_json_to_str` | JSON.stringify の再帰 |
| `52990` | `get_proxy_method` | Proxy trap 取得（6節） |
| `53854` | `js_proxy_isArray` | Proxy target 連鎖の再帰判定（6節） |

**このガードは alloca 前にのみ置かれ、`JS_CallInternal`/`js_call_c_function` 系の C 再帰そのものは防がない** — 実際に C スタックを使い切るのは「この関数を再帰的に何段呼べるか」であり、ガードは「次の 1 段を積む前に確保量が安全か」だけを見る。L2 が「C スタック使用量が深さに比例しない」を完了条件に掲げている（spec 7節）のは、まさにこの alloca 依存を断つという意味である。

## 10. 組み込みが JS へ再入する箇所（C->JS reentry、分野別）

全て `JS_Call` / `JS_CallFree` / `JS_Invoke` / `JS_InvokeFree` / `JS_CallConstructor` / `JS_CallConstructor2` のいずれかを通る。**すべて 4 節のラッパー経由で最終的に 1 節の `JS_CallInternal` か 3 節のテーブルへ落ちる** — 呼び出し機構そのものに例外は無い。件数は grep ベースの実測（本ファイル内の `JS_Call(ctx`/`JS_CallFree(ctx`/`JS_Invoke`/`JS_CallConstructor` 出現数、行 8000 以降）: **139 箇所**（1〜9節で個別に扱った呼び出し機構本体・ラッパー内部の呼び出しを除く）。代表例を分野別に挙げる（行番号は代表 1 箇所、同一関数内に複数ある場合は関数名だけ書く）。

- **Array.prototype**: `js_array_every`（`every`/`some`/`forEach`/`map`/`filter`、コールバック呼び出し `quickjs.c:44030`）、`js_array_reduce`（`reduce`/`reduceRight`、`44187`）、`js_array_find`（`find`/`findIndex`/`findLast`/`findLastIndex`、`44454`）、`js_array_toString`（`join` 相当、`44503`）、`JS_FlattenIntoArray`（`flat`/`flatMap` のマッパー呼び出し、`45140`）、`js_array_cmp_generic`（`sort` の比較関数、`45272` — **rqsort という C 側クイックソートの比較コールバックとして、`JS_CallInternal` の外側で C スタックを消費しながら呼ばれる**）、`js_array_from`/`js_array_of`（`Array.from`/`Array.of` のコンストラクタ・mapfn 呼び出し、`43488`〜`43549`）、`JS_ArraySpeciesCreate`（`Symbol.species` コンストラクタ呼び出し、`43672`）。
- **TypedArray**: `js_typed_array_create`（`60847`）、`js_typed_array_from`（`60985`）、`js_typed_array_find`（`61229`）、`js_TA_cmp_generic`（`TypedArray.prototype.sort` の比較関数、`62067`、Array 版と同型で rqsort 経由）。
- **Iterator ヘルパー**（ES2025 Iterator helpers）: `js_iterator_proto_func`（`map`/`filter`/`take`/`drop` 等、`46163`〜`46254`）、`js_iterator_proto_reduce`（`46337`）、`js_iterator_helper_next`（helper オブジェクトの内部反復、`46552`〜`46678`）、`js_iterator_from`/`js_iterator_concat_return`（`45960`, `45879`）。
- **Map/Set**: `js_map_constructor`（初期化時の adder 呼び出し、`54238`）、`js_map_getOrInsert`（`54597`）、`js_map_forEach`（`54714`）、`js_map_groupBy`/`js_object_groupBy`（`54777`, `41994`）、`js_set_isDisjointFrom`/`isSubsetOf`/`isSupersetOf`/`intersection`/`difference`/`symmetricDifference`/`union`（set-like オブジェクトの `keys`/`has` 呼び出し、`55194`〜`55713` に多数）。
- **JSON**: `internalize_json_property`（`JSON.parse` の reviver、`52166`）、`js_json_check`（`JSON.stringify` の `toJSON`/replacer、`52321`, `52335`）。
- **RegExp**: `JS_RegExpExec`（`exec` メソッドまたは `Symbol.replace`/`match` からの custom exec、`50749`）、`js_regexp_Symbol_matchAll`/`js_regexp_Symbol_split`（species コンストラクタ、`50999`, `51455`）、`js_string_match`/`js_string_replace`/`js_string_split`（`String.prototype` 側から `Symbol.match`/`Symbol.replace`/`Symbol.split` を呼ぶ、`48027`, `48211`/`48262`, `48326`）。
- **Promise**: `promise_reaction_job`（then/catch ハンドラ本体、`56004`, `56018`）、`js_promise_resolve_thenable_job`（`56112`）、`js_promise_constructor`（executor 呼び出し、`56355`）、`js_new_promise_capability`（species コンストラクタ、`56418`）、`js_promise_all`/`js_promise_race`（`Promise.resolve` と `then` の呼び出し連鎖、`56719`, `56768`, `56860`, `56868`）、`js_promise_then_finally_func`/`js_promise_finally`（`57026`, `57082`）、`js_async_from_sync_iterator_next`（`57258`）。
- **Reflect / Object**: `js_reflect_construct`（`52801`）、`js_object_toLocaleString`（`42230`）。
- **Function.prototype**: `js_function_apply`（`OP_apply` の実体、`call`/`apply`/`Reflect.apply` 相当、`42898`〜`42907`）、`js_function_call`（`42917`, `42919`）。
- **モジュール / 動的 import**: `js_inner_module_linking`（`31472`）、`js_load_module_rejected`/`js_load_module_fulfilled`（`31627`, `31647`）、`JS_LoadModuleInternal`（`31679`）、`js_dynamic_import_job`/`js_dynamic_import`（`31746`, `31857`）、`js_set_module_evaluated`（`31870`）、`js_async_module_execution_rejected`（`31975`）、`js_evaluate_module`（`32248`, `32258`）。
- **マイクロタスク / FinalizationRegistry**: `js_microtask_job`（キューされた JS ジョブ本体の呼び出し、`41290`）、`js_finrec_job`（`FinalizationRegistry` のクリーンアップコールバック、`64101`）。
- **エラー生成**: `information`（`Error.prepareStackTrace` 相当の `prepare` 呼び出し、`8162`）。
- **その他**: `js_call_function`（`Function` コンストラクタ経由の関数生成直後の呼び出しに使われるヘルパ、`8763`）、`js_bytecode_eval`（`8816`）。

## 10.1 C->JS 再入は列挙可能な有限リストではない——レビュー指摘による訂正

10節は分野別代表例（139件、現ワークツリーでの再grepは152件——後述の版ずれ参照）を「列挙」の体裁で示しているが、**これは網羅的なリストとして読めるものではなく、むしろ値変換の経路を辿ればほぼ任意の opcode から到達しうる**ことを補足する。

- `JS_ToPrimitiveFree`（`quickjs.c:12163`台）は `Symbol.toPrimitive`／`valueOf`／`toString` を `JS_CallFree` で呼ぶ（`quickjs.c:12198`, `12225`）。呼び出し元は `OP_add`（`quickjs.c:15743`, `15749` 付近の `HINT_NONE` 変換）、比較演算子（`quickjs.c:16107`, `16112` `HINT_NUMBER`）、`OP_concat`/文字列化に絡む複数の opcode、`JS_ToNumber`/`JS_ToString` を内部で呼ぶほぼ全ての算術・比較・テンプレートリテラル系 opcode など、**個別の opcode 種別を列挙して尽くすのが現実的でない箇所に広く分布する**。
- プロパティアクセス系 opcode はどれも getter/setter（5節、`quickjs.c:9060` getter・`quickjs.c:10273` `call_setter`）を経由しうるため、「このopcodeはC->JS再入しない」という opcode 単位の分類自体が保てない——実際に再入するかどうかは実行時のプロパティディスクリプタ次第であり、静的な opcode の種類では決まらない。
- `Error.prepareStackTrace` は `build_backtrace`（`quickjs.c:8221`、`JS_Call(ctx, prepare, ...)`）の内部から呼ばれ、`build_backtrace` 自身は `exception:` ラベル（`quickjs.c:20879`）——**すなわち例外を投げた瞬間そのもの**からも到達する。「例外送出」という、通常は「JSの実行から抜ける」側の操作が、実は新たなC->JS再入点になりうる。
- **L2への含意（事実の列挙）:** 「このopcodeの後にC呼び出しが挟まるか」を、opcode種別ごとの静的な表で判定するのは不可能——値変換・プロパティアクセス・例外送出のいずれもが実行時の値やディスクリプタの形に依存して再入するかどうかが決まる。ネイティブ再入深さを追うなら、`JS_CallInternal`/`js_call_c_function`系の**関数入口ごとにカウンタをインクリメント/デクリメントするランタイムトラッキング**（10節冒頭のいずれかの呼び出し規約を必ず通るという3〜4節の事実に立脚できる）が必要で、opcodeバイトコードだけを見る静的分類では代替できない。

## 11. 分類まとめ

- **JS->JS**: 2節の call 系 opcode（`OP_call*`, `OP_call_method`, `OP_tail_call*`, `OP_call_constructor`）が呼び出し先も bytecode function の場合。C 側では `JS_CallInternal` の再帰呼び出し 1 段として現れる（`quickjs.c:18628` 等）。
- **JS->C**: 同じ opcode が呼び出し先を非 bytecode function に解決した場合、`JS_CallInternal` 冒頭の `rt->class_array[p->class_id].call` 分岐（3節）で完結する。C フレームは `JS_CallInternal` の 1 段の代わりに `js_call_c_function` 等の 1 段になる。
- **C->JS（同期再入）**: 10節に列挙した組み込み関数内部からの `JS_Call`/`JS_Invoke` 系呼び出し。`js_call_c_function`（3.1節）の中から発生するため、**C スタック上は「JS_CallInternal → js_call_c_function → （組み込みの中身）→ JS_Call → JS_CallInternal」という交互のネストになる** — L2 が「C 再帰が残る間の一般的な中断はできない」（spec L2a 表）としている理由はここにある。組み込み 1 個の実装がループの中で `JS_Call` を呼ぶ場合（`forEach` 等）、コールバックが呼ばれるたびに C フレームが 1 段積まれては降りる、を要素数分繰り返す。
- **ヒープ常駐フレームとの往復**（8節）: generator/async は `JS_CallInternal` を「呼んでは抜ける」を繰り返すが、フレーム自体は `JS_CallInternal` の C フレームより長生きする。これは JS->JS/C->JS のどちらでもなく、**「フレームの生存期間が呼び出し C フレームの生存期間から独立している」唯一の既存例**であり、L3 の相対参照設計の参考にできる（生ポインタでなく `JSAsyncFunctionState*` 経由で再取得する規約が既にある）。

## 12. 既存の割り込み確認機構（spec の opcode セーフポイントとの比較用）

`js_poll_interrupts`（`quickjs.c:8469-8476`、実体 `__js_poll_interrupts` `quickjs.c:8456-8467`）:

- `ctx->interrupt_counter` を毎回デクリメントし、0 以下になったら `__js_poll_interrupts` を呼ぶ（カウンタ初期値 `JS_INTERRUPT_COUNTER_INIT`、本ファイル内での定義箇所は未確認）。
- `__js_poll_interrupts` はカウンタを `JS_INTERRUPT_COUNTER_INIT` に再初期化し、`rt->interrupt_handler` が設定されていれば呼ぶ。ハンドラが非 0 を返すと `JS_ThrowInterrupted(ctx)`（`quickjs.c:8450-8453`、`JS_SetUncatchableError` で**捕捉不能**としてマークする）を呼び、`-1` を返す。
- 呼び出し元は `if (unlikely(js_poll_interrupts(ctx))) goto exception;` の形で 1.6 節の例外巻き戻しに合流する。**uncatchable なので巻き戻しは while ループを通らず、finally も実行されずに即座に `done`/`done_generator` へ落ちる**（1.6 節の該当箇所）。

呼び出し箇所（JS 実行に関係するもの、9 箇所）:

| 行 | 文脈 | 方向 |
| --- | --- | --- |
| `18164` | `JS_CallInternal` 入口 | 呼び出しのたび |
| `20970` | `JS_CallConstructorInternal` 入口 | 呼び出しのたび |
| `19241` `OP_goto` | 分岐後（前方/後方を問わない） | ジャンプ命令ごと |
| `19247` `OP_goto16` | 同上 | 同上 |
| `19253` `OP_goto8` | 同上 | 同上 |
| `19272` `OP_if_true` | 分岐後（条件成立時に pc を書き換えた後） | 同上 |
| `19292` `OP_if_false` | 同上 | 同上 |
| `19312` `OP_if_true8` | 同上 | 同上 |
| `19332` `OP_if_false8` | 同上 | 同上 |
| `8706`, `16913`, `16972`, `42572`, `42659` | prototype chain / Proxy 連鎖の C ループ（call opcode ではない） | ループ 1 周ごと |

**spec（L2 opcode セーフポイント節）との相違点（事実として記録、判断はしない）**:

1. 既存の確認は **無条件**（前方分岐でも後方分岐でも、条件成立でも不成立でも毎回呼ぶ）。spec案は「後方分岐を中心に」絞る設計だが、既存実装はその絞り込みを行っていない。
2. 既存の「中断」は **例外として実装され、捕捉不能フラグで区別している**。spec 3節は「中断は例外ではない」と明記しており、既存の `JS_ThrowInterrupted` は spec の要求する意味論とは異なる（finally をスキップする点は一致するが、GC からの到達可能性・状態保存という意味では例外パスをそのまま再利用しているため、spec の言う「失敗・キャンセル・正常完了を別の状態として扱う」設計にはなっていない）。
3. `call` 系 opcode 自体（`OP_call`/`OP_call_method`/`OP_call_constructor`/`OP_apply`/`OP_eval`）には `js_poll_interrupts` の直接呼び出しは無い — 呼び出し先の `JS_CallInternal`/`JS_CallConstructorInternal` の入口チェック（1 個目、2 個目の行）に委譲している。spec の表は「呼び出し準備状態の所有権が確定する地点」を call 系 opcode 自身の監査対象としているが、既存実装はその地点そのものにチェックを置いていない。

## L2/L3/L5 に効く点（判断は含まない、着目点のみ）

- 1.3 節の単一 `alloca` 呼び出しが L2a の「非移動セグメントへの確保」の直接の置換対象。`sf->var_refs` が同じ alloca ブロック内の生ポインタ配列であることに注意。
- 8節の generator/async の `JSAsyncFunctionState` は quickjs-ng がすでに持つ「ヒープ常駐フレーム + 明示的な再開」の実例であり、L2 の設計はこれの汎用化に近い。`async_func_mark`/`async_func_free` の「cur_sp が NULL なら実行中」という不変条件は、L2 のセーフポイント設計でも同種の状態表現が要る。
- 2節の call 系 opcode の一覧はそのまま L2 の「呼び出し準備状態の所有権が確定する地点」の監査対象と一致する。tail call は C 再帰を畳まないことに注意（JS フレームの畳み込みと C フレームの畳み込みは別物）。
- 12節の `js_poll_interrupts` は L1/L2 が置き換える対象の**既存実装**であり、無条件チェック・例外による中断という 2 点で spec の設計方針と異なる。両者の関係（既存を残すか、置き換えるか、二重化するか）は本ledgerの範囲外。
- 6節の Proxy trap 連鎖と 10節の `js_array_cmp_generic`/`js_TA_cmp_generic`（rqsort 経由）は、**C 側の非 `JS_CallInternal` フレーム（trap 取得の `js_call_c_function`、qsort の再帰）が JS 呼び出しの間に挟まる**代表例。L2 の「ネイティブ実行深さを追跡し、C が同期的な JS 戻り値を待つ区間ではホストまで戻らない」の対象範囲を具体化する材料になる。
- 9節の `js_check_stack_overflow` 呼び出し一覧は、L0 の「C スタック最大使用量」計測のトレースポイント候補になる。

## 未確認

- `js_check_stack_overflow` 自体の実装（何を「残り」と見なすか、閾値の実測値）は本ledgerの範囲外（別entryで扱うべき）。
- `JS_INTERRUPT_COUNTER_INIT` の定義箇所・値は未確認（quickjs.c 内で定義されているか、ヘッダかも未確認）。
- `call_setter` の全呼び出し元（`JS_SetPropertyInternal` 系のどの分岐から呼ばれるか）は個別に洗っていない。プロパティ設定経路全体は別entryのスコープと判断した。
- Proxy の多段ネストに対する明示的な深さ上限の有無（`js_check_stack_overflow` 依存のみか）は未確認、推定にとどまる。
- `js_call_generator_function` / `js_promise_resolve_function_call` / `js_async_function_call` / `js_async_function_resolve_call` / `js_async_generator_function_call`（3節のテーブルに載る残り 5 関数）の内部実装は行番号のみ把握し、本文は読んでいない。
- 10節の 139 件という数はテキストパターンマッチ（`JS_Call(ctx`/`JS_CallFree(ctx`/`JS_Invoke`/`JS_CallConstructor`）による機械的カウントであり、コメント中や無関係な同名パターンの混入・見落としを人手で全数検証してはいない。分野別の代表例は目視で選んだもので、列挙した行以外にも同一関数内に追加の呼び出しがある場合がある（特に Promise・Set 関連は 1 関数内に複数箇所ある）。**訂正・補足（レビュー指摘）: 本追記時点の`quickjs.c`（未コミットのvmprobe差分込み）で同じgrepを再実行すると152件。139という数はこの機械的カウントの性質上、版が変わるたびにそのまま動く——10.1節で述べた通り、そもそも「呼び出し箇所を数え上げる」アプローチ自体がL2の設計判断には不十分。**
- **本台帳群の行番号の基準:** 本ファイルの大半のfile:line引用は `git rev-parse HEAD`（本ワークツリーでは `d9ef1f9`）に対し、`apps/vmprobe/` 計測フック用の未コミット差分（`quickjs.c`に+59行、6箇所のhunk：`@@ -47,+6` `@@ -424,+14` `@@ -2134,+18` `@@ -2155,+6` `@@ -2188,+10` `@@ -2305,+11` あたり）が当たった状態を直接読んで書かれている（本追記時点のセッションで確認）。ただし本ファイルの初稿がどの版を見て書かれたかは記録されておらず、本追記で訂正した10.1節・8.1節・1.4.1節以外の既存の行番号（特に18000番台以降）は、vmprobe差分の影響を受けて実際とずれている可能性がある。次に読む側は、引用されたコード片が現物と一致するかを個別に確認すること。
- `js_function_bind`（`Function.prototype.bind` 自体の生成処理）や `js_object_seal`/`defineProperty` 系など、10節の分野分けから漏れている可能性のある C->JS 再入（特に `Object.defineProperties` の `desc.getter`/`desc.setter` 経由、`Array.prototype.toSorted`/`toReversed` 等 ES2023 以降の非破壊系メソッド）は個別に洗っていない。
- 例外巻き戻し中の `JS_IteratorClose`（1.6 節、`pos == 0` 分岐）が呼ぶ JS 側 `return()` メソッドの呼び出し経路（`JS_IteratorClose` 内部）は本entryでは行番号を追っていない（`quickjs.c:17194` 付近と推定、未確認）。
