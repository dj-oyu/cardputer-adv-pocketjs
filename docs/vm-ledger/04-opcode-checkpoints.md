# 04: opcode によるチェックポイント候補台帳

対象: `components/quickjs-ng/quickjs-ng/quickjs.c`（quickjs-ng 0.14.0 + immutable-buffer patch）。
範囲: `docs/quickjs-freertos-vm-spec.md` §7「opcode による中断確認」の監査対象命令群。
全エントリは file:line 引用と、その行が実際に行っている動作の FACT。設計上の結論は末尾の「L2/L3/L5 に効く点」のみに限定し、判断はしていない。

## 0. dispatch 機構（このビルドの設定）

- `quickjs.c:51-55`

  ```c
  #if defined(EMSCRIPTEN) || defined(_MSC_VER)
  #define DIRECT_DISPATCH  0
  #else
  #define DIRECT_DISPATCH  1
  #endif
  ```

  `EMSCRIPTEN` も `_MSC_VER` も定義されない。ESP-IDF v6.0.1 の xtensa-esp-elf / riscv32-esp-elf は GCC ベースで `__GNUC__` は立つが `_MSC_VER` は立たない。よって **`DIRECT_DISPATCH` はこのビルドで `1`**（コンパイル時マクロなので実行時分岐ではなく、翻訳単位ごとに一意に決まる）。

- `components/quickjs-ng/CMakeLists.txt:26-29` — `target_compile_definitions` は `QUICKJS_NG_BUILD` と `_GNU_SOURCE` のみを追加している。`DIRECT_DISPATCH` / `CONFIG_COMPUTED_GOTO` / `USE_COMPUTED_GOTO` を上書き・強制する記述はこのファイルにも `components/quickjs-ng/` 配下の他の CMake/Kconfig 関連ファイル（`grep -rn` で他にヒットなし）にも存在しない。**ビルド設定側は `quickjs.c` 冒頭の条件式に一切手を入れていない**（推定ではなく `grep` で確認した事実）。

- `quickjs.c:18146-18162`

  `DIRECT_DISPATCH == 1` の分岐（18151-18162）が有効。要点:
  - `dispatch_table[256]` を `&&case_OP_xxx` の computed-goto ラベル配列として静的初期化（18152-18157）。`OP_COUNT..255` は `&&case_default`（18156）。
  - `SWITCH(pc)` は `goto *dispatch_table[opcode = *pc++]`（18158）。**opcode バイトを読んで `pc` を1進めるのと、その opcode へジャンプするのが同一のマクロ展開の中で起きる**。
  - `CASE(op)` は `case_ ## op` というラベル（18159）、`BREAK` は次の `SWITCH(pc)` の再展開（18161）——つまり `BREAK` は C の `break` ではなく次命令への `goto` そのもの。
  - `!DIRECT_DISPATCH`（18147-18150）は素の `switch (opcode = *pc++)` で、こちらもこのビルドでは到達しないが構造は同じ（opcode を読んで `pc` を1進めてから分岐）。

  **FACT: どちらの経路でも「opcode バイトのデコードで `pc` が1進む」のは `SWITCH` マクロの実行時点であり、各 `CASE` ハンドラに入った時点で既に完了している。** 以降の「オペランドのデコードで `pc` が追加で進む」は各命令ハンドラの中の話であり、個々のエントリで記録する。

- `JS_CallInternal` は `JS_CallInternal(JSContext *caller_ctx, ...)`（`quickjs.c:18124`）という1つの関数で、上記の `for (;;) { SWITCH(pc) { ... } }` ループ（`quickjs.c:18267-18815` 相当、`restart:` ラベルは18266）がバイトコード全体を回す。再帰呼び出し（`call` 系）は C 関数呼び出しとして `JS_CallInternal` を再帰する形で実装されている（後述）。

## 1. goto / goto8 / goto16

- `quickjs.c:19239-19256`

  ```c
  CASE(OP_goto):
      pc += (int32_t)get_u32(pc);
  if (unlikely(js_poll_interrupts(ctx))) {
      goto exception;
  }
  BREAK;
  CASE(OP_goto16):
      pc += (int16_t)get_u16(pc);
  if (unlikely(js_poll_interrupts(ctx))) {
      goto exception;
  }
  BREAK;
  CASE(OP_goto8):
      pc += (int8_t)pc[0];
  if (unlikely(js_poll_interrupts(ctx))) {
      goto exception;
  }
  BREAK;
  ```

  - FACT: 3命令とも「分岐先オフセットを `pc` に加算して分岐先 PC を確定した直後」に `js_poll_interrupts(ctx)` を呼んでいる。オフセットは分岐先アドレスへの相対値で、命令自身の直後の値ではなく `pc`（=すでにオフセットフィールドの先頭を指している）を起点に加算するため、加算後の `pc` がそのまま次に実行する opcode の先頭になる。
  - FACT: `js_poll_interrupts` は前方分岐・後方分岐を区別しない。**分岐方向の判定は行われていない**——仕様書 §7 の表にある「後方分岐を中心に」という記述に対し、コード側には方向フィルタが無い。全 `goto`/`goto8`/`goto16` の実行毎に呼ばれる（`--ctx->interrupt_counter <= 0` のときだけ実体 `__js_poll_interrupts` に入るので、呼び出し自体は毎回だが重い処理は間引かれる。`quickjs.c:8469-8476`）。
  - FACT: `js_poll_interrupts` が非0を返すと `goto exception`（分岐先 PC は既に確定済みだが、例外パスに入るので分岐先へは進まない）。この時点で `sp` は変化していない（`OP_goto*` はスタックに触れない）。
  - FACT: 再開契約という観点では、これら3命令の中断確認地点は「`pc` が既に分岐先（次に実行すべき命令の先頭）まで進み切った後」。中断からの再開は「その `pc` から次命令をデコードする」だけでよく、命令自体の再実行は不要（`OP_goto*` 自体には副作用がない）。

## 2. if_true / if_false / if_true8 / if_false8

- `quickjs.c:19257-19335`（4命令とも同型）

  `OP_if_true`（19257-19276）を代表として引用:

  ```c
  CASE(OP_if_true): {
      int res;
      JSValue op1;

      op1 = sp[-1];
      pc += 4;
      if ((uint32_t)JS_VALUE_GET_TAG(op1) <= JS_TAG_UNDEFINED) {
          res = JS_VALUE_GET_INT(op1);
      } else {
          res = JS_ToBoolFree(ctx, op1);
      }
      sp--;
      if (res) {
          pc += (int32_t)get_u32(pc - 4) - 4;
      }
      if (unlikely(js_poll_interrupts(ctx))) {
          goto exception;
      }
  }
  BREAK;
  ```

  - FACT: `pc += 4`（19262）で先にオペランド（4byte のオフセット）ぶん `pc` を進めてから条件値 `op1` を消費する。真偽値化が `JS_ToBoolFree` を経由する場合、**フリー可能な JSValue（オブジェクト等）を消費する副作用がここで起きる**。この呼び出し自体は原則として例外を投げない設計（`ToBoolean` は仕様上例外を投げない）が、ソース中に明示コメントはない。
  - FACT: `sp--`（19268）はスタックポインタを既に1つ減らした後（=`op1` はスタックから既に取り除かれた状態）で分岐判定を行う。真の場合のみ `pc` に追加オフセットを加算（19270）。この2手順（オペランドデコード→値消費→分岐先確定）が終わった**後**に `js_poll_interrupts` を呼ぶ（19272-19274）。
  - FACT: `if_true8`/`if_false8`（19297-19335）はオフセットが1byte（`pc += 1`、`(int8_t)pc[-1] - 1`）以外は同型。ショート形式も漏れなく同じ位置に `js_poll_interrupts` を持つ——仕様書 §7 の「短縮形も漏らさない」は現状コードで満たされている。
  - FACT: 中断確認地点での状態は「`sp` は既に条件値ぶん減算済み、`pc` は分岐後の次命令先頭」。再開契約上は goto と同様「その `pc` から次命令」でよいが、`sp` の巻き戻しは不可（`op1` の所有権は既に `JS_ToBoolFree` で消費済みのため、値としては復元できない）。

## 3. call 系（call, call0〜3, call_method, tail_call, tail_call_method, call_constructor, apply, eval）

- `quickjs.c:18614-18643`（`call0`〜`call3`, `call`, `tail_call`）

  ```c
  CASE(OP_call0):
      CASE(OP_call1):
      CASE(OP_call2):
      CASE(OP_call3):
      call_argc = opcode - OP_call0;
  goto has_call_argc;
  CASE(OP_call):
      CASE(OP_tail_call): {
      call_argc = get_u16(pc);
      pc += 2;
      goto has_call_argc;
  has_call_argc:
      call_argv = sp - call_argc;
      sf->cur_pc = pc;
      ret_val = JS_CallInternal(ctx, call_argv[-1], JS_UNDEFINED,
                                JS_UNDEFINED, call_argc,
                                vc(call_argv), 0);
      if (unlikely(JS_IsException(ret_val))) {
          goto exception;
      }
      if (opcode == OP_tail_call) {
          goto done;
      }
      for (i = -1; i < call_argc; i++) {
          JS_FreeValue(ctx, call_argv[i]);
      }
      sp -= call_argc + 1;
      *sp++ = ret_val;
  }
  BREAK;
  ```

  - FACT: **`js_poll_interrupts` はこの経路に存在しない。** `call`/`call0`〜`3`/`tail_call` の命令ハンドラ自体には中断確認呼び出しが1つもない。存在するのは `sf->cur_pc = pc`（18627）——呼び出し先の再帰 `JS_CallInternal` 実行中に `sf`（現在フレーム）の `cur_pc` を確定させる代入のみ。
  - FACT: 中断確認が実際に効いているのは、**呼び出し先の `JS_CallInternal` の入口**（`quickjs.c:18164-18166`）:
    ```c
    if (js_poll_interrupts(caller_ctx)) {
        return JS_EXCEPTION;
    }
    ```
    これは呼び出し「毎回」実行される（`JS_CallInternal` の関数プロローグの一部であり、opcode 分岐の外）。つまり**呼び出し境界そのものが中断確認地点になっている**が、これは「呼び出し準備状態の所有権が確定する地点」（§7 の記述）よりも一段深い——**呼び出し先の関数フレームがまだ何も構築していない時点**（`local_buf` の確保より前）でのチェックである。
  - FACT: `JS_IsException(ret_val)` が真の場合（18631-18633）に `goto exception` する時点で、**`call_argv[-1]`（呼び出された関数自身）と `call_argv[0..call_argc-1]`（引数）はまだ解放されていない**——`JS_FreeValue` ループ（18637-18639）は例外パスでは実行されない。これらの値は `sp` 上に残ったままなので、`exception:` ラベル（20816-20844）の汎用スタック巻き戻しループ（`while (sp > stack_buf) { JSValue val = *--sp; JS_FreeValue(ctx, val); ... }`）が代わりに解放する。**「部分的に sp を変更した後に例外を投げる」パターンではあるが、解放漏れにはならない**（汎用巻き戻しが担保）。
  - FACT: `OP_tail_call` は成功時に `goto done`（18635）——呼び出し元フレームの引数・関数値を解放しないまま関数を抜ける。`done:` ラベル（20854-20863）で `local_buf` から `sp` までを一括解放するため、ここでも個別の `JS_FreeValue` は不要（`sp` はまだ `call_argv[-1]` 以降を含んだまま）。
  - FACT: `OP_call_method`/`OP_tail_call_method`（18662-18683）も同型（`this` 引数ぶん `call_argv[-2]` から）で、**中断確認はこの命令ハンドラにも無い**。呼び出し先 `JS_CallInternal` 入口の確認に依存する点は `call` と同じ。
  - `quickjs.c:18644-18661`（`OP_call_constructor`）も同型。`JS_CallConstructorInternal` を呼ぶが、その入口にも `js_poll_interrupts` がある（`quickjs.c:20970`、後述3.1）。
  - `quickjs.c:18697-18713`（`OP_apply`）: `js_function_apply` を呼ぶ。`js_function_apply` 自体の内部で中断確認をするかは未確認（下記「未確認」節）。`OP_apply` ハンドラ自体には確認なし。
  - `quickjs.c:18811-18839`（`OP_eval`）: `JS_EvalObject` または `JS_CallInternal` を呼ぶ。`JS_CallInternal` 経由なら入口確認が効くが、`JS_EvalObject` 経路（direct eval）の中断確認有無は未確認。

### 3.1 コンストラクタ呼び出し境界

- `quickjs.c:20961-20972`

  ```c
  static JSValue JS_CallConstructorInternal(JSContext *ctx,
                                            JSValueConst func_obj,
                                            JSValueConst new_target,
                                            int argc, JSValueConst *argv,
                                            int flags)
  {
      JSObject *p;
      JSFunctionBytecode *b;

      if (js_poll_interrupts(ctx)) {
          return JS_EXCEPTION;
      }
  ```

  FACT: `JS_CallInternal` と同じパターンで、**関数の最初の行**として `js_poll_interrupts` を置いている。呼び出し元スタックフレームの状態には一切触れていない（この関数のローカルの `p`/`b` も未初期化のまま）。

### 3.2 call 系の再開契約についての FACT

- 中断が起きるのは呼び出し先の入口（呼び出し元から見ると「深さが1つ増えた地点」）であり、**呼び出し元の opcode ハンドラの途中では止まらない**。再開時に必要な情報は、呼び出し元フレームに対しては `sf->cur_pc = pc`（呼び出し後の次命令位置、18627/18648/18667/18701/18818 のいずれか）で既に確定されている。仕様書 §7 が言う「呼び出し準備状態の所有権が確定する地点」に近いのは実際には**呼び出し先関数の入口**であり、呼び出し元 opcode ハンドラそのものではない、というのが現状コードの構造。

## 4. return 系

- `quickjs.c:18714-18719`

  ```c
  CASE(OP_return):
      ret_val = *--sp;
  goto done;
  CASE(OP_return_undef):
      ret_val = JS_UNDEFINED;
  goto done;
  ```

  FACT: `js_poll_interrupts` は無い。`OP_return` は `sp` を1減算して戻り値を確定した後、即座に `done:` へ飛ぶ。`OP_return_undef` は `sp` に触れない。

- `quickjs.c:20760-20763`

  ```c
  CASE(OP_return_async):
      CASE(OP_initial_yield):
      ret_val = JS_UNDEFINED;
  goto done_generator;
  ```

  FACT: `js_poll_interrupts` は無い。`done_generator:`（20849-20852）は `sf->cur_pc = pc; sf->cur_sp = sp;` のみを行い、`done:` の変数解放ブロックには入らない（呼び出し側 = ジェネレータランタイムが解放責任を持つ、コード内コメント20846-20848「the local variables are freed by the caller in the generator case」）。

- `quickjs.c:20854-20865`（`done:` ラベル本体）

  ```c
  done:
      if (unlikely(sf->var_ref_count != 0)) {
          close_var_refs(rt, sf);
      }
      for (pval = local_buf; pval < sp; pval++) {
          JS_FreeValue(ctx, *pval);
      }
  }
  rt->current_stack_frame = sf->prev_frame;
  return ret_val;
  ```

  FACT: `return`/`return_undef`/通常完了はすべてここを通り、C 関数として `JS_CallInternal` から戻る。**この関数境界（呼び出し元へ戻る瞬間）に中断確認は無い**——戻った先が `call` 系ハンドラであり、そのハンドラにも確認は無い（§3参照）ので、「関数から関数へ戻る」経路は現状どこにも中断確認地点が無い。

  FACT: `return`/`return_undef` の中断確認は「呼び出し元へ戻った後、次に呼び出し元が別の関数を呼ぶか、`goto`/`if_true` 系命令を実行するまで」発生しない。**深い再帰から戻り続ける経路（大量の return が連鎖するだけで goto/call が挟まらない場合）には中断確認地点がない**——これは `return` を戻る側で `js_poll_interrupts` を呼んでいる場所が無いことの直接の帰結。

## 5. catch / gosub / ret / nip_catch

- `quickjs.c:19337-19353`

  ```c
  CASE(OP_catch): {
      int32_t diff;
      diff = get_u32(pc);
      sp[0] = JS_NewCatchOffset(ctx, pc + diff - b->byte_code_buf);
      sp++;
      pc += 4;
  }
  BREAK;
  CASE(OP_gosub): {
      int32_t diff;
      diff = get_u32(pc);
      /* XXX: should have a different tag to avoid security flaw */
      sp[0] = js_int32(pc + 4 - b->byte_code_buf);
      sp++;
      pc += diff;
  }
  BREAK;
  ```

  FACT: `OP_catch` は `pc` を4byte進めるだけ（分岐しない。catch オフセットを値としてスタックに積むのみ）。`OP_gosub` は `diff` ぶん `pc` に加算して分岐する（`quickjs-opcode.h` の定義未確認だが、コードの形は `goto` と同型）。**どちらも `js_poll_interrupts` は無い。**

- `quickjs.c:19354-19370`

  ```c
  CASE(OP_ret): {
      JSValue op1;
      uint32_t pos;
      op1 = sp[-1];
      if (unlikely(JS_VALUE_GET_TAG(op1) != JS_TAG_INT)) {
          goto ret_fail;
      }
      pos = JS_VALUE_GET_INT(op1);
      if (unlikely(pos >= b->byte_code_len)) {
  ret_fail:
          JS_ThrowInternalError(ctx, "invalid ret value");
          goto exception;
      }
      sp--;
      pc = b->byte_code_buf + pos;
  }
  BREAK;
  ```

  FACT: `OP_ret`（`gosub`/`catch` と対になる、finally ブロックからの戻り命令）は `pos` の妥当性検査に失敗すると `goto exception`——この時点で `sp` はまだ減算されていない（`sp--` は妥当性確認の後、19367）。妥当な場合は `sp--` してから `pc` をバイトコード先頭起点の絶対位置 `pos` へ**絶対**ジャンプさせる（他の分岐命令が相対オフセットなのに対しここだけ絶対アドレス）。**`js_poll_interrupts` は無い。**

- `quickjs.c:19439-19454`

  ```c
  CASE(OP_nip_catch): {
      JSValue ret_val;
      ret_val = *--sp;
      while (sp > stack_buf &&
              JS_VALUE_GET_TAG(sp[-1]) != JS_TAG_CATCH_OFFSET) {
          JS_FreeValue(ctx, *--sp);
      }
      if (unlikely(sp == stack_buf)) {
          JS_ThrowInternalError(ctx, "nip_catch");
          JS_FreeValue(ctx, ret_val);
          goto exception;
      }
      sp[-1] = ret_val;
  }
  BREAK;
  ```

  FACT: ループで `sp` を `JS_TAG_CATCH_OFFSET` が見つかるまで巻き戻しながら `JS_FreeValue` する。ループ回数は「例外/catch より上に積まれていた一時値の個数」に依存し静的に有界ではない（無限ループにはならないが、`stack_buf` に達するまでの命令数が命令バイト数に対して非線形になりうる）。**`js_poll_interrupts` は無い。** ループ内で `JS_FreeValue` が任意のデストラクタ相当処理（finalizer 呼び出し等はしないが参照カウント減算・GC連鎖）を引き起こす可能性があり、実行時間はスタック上に何が積まれていたかに依存する。

## 6. for_in / for_of / iterator 系

- `quickjs.c:19372-19402`（`for_in_start`, `for_in_next`, `for_of_start`, `for_of_next`）

  ```c
  CASE(OP_for_in_start):
      sf->cur_pc = pc;
  if (js_for_in_start(ctx, sp)) {
      goto exception;
  }
  BREAK;
  CASE(OP_for_in_next):
      sf->cur_pc = pc;
  if (js_for_in_next(ctx, sp)) {
      goto exception;
  }
  sp += 2;
  BREAK;
  CASE(OP_for_of_start):
      sf->cur_pc = pc;
  if (js_for_of_start(ctx, sp, false)) {
      goto exception;
  }
  sp += 1;
  *sp++ = JS_NewCatchOffset(ctx, 0);
  BREAK;
  CASE(OP_for_of_next): {
      int offset = -3 - pc[0];
      pc += 1;
      sf->cur_pc = pc;
      if (js_for_of_next(ctx, sp, offset)) {
          goto exception;
      }
      sp += 2;
  }
  BREAK;
  ```

  FACT: 4命令とも `sf->cur_pc = pc` を先に設定してから対応するヘルパー関数（`js_for_in_start` 等、`quickjs.c` 内の別関数、本ledgerの範囲外）を呼ぶ。ヘルパーが失敗（非0）を返すと即 `goto exception`。**`js_poll_interrupts` の直接呼び出しはこのハンドラ内には無い。** ヘルパー関数の内部で呼んでいるかは未確認（下記「未確認」節）。
  FACT: `sp` の増減（`sp += 2` 等）はヘルパー呼び出しの**後**——失敗時は `sp` は変更されない。

- `quickjs.c:19403-19417`（`for_await_of_start`, `iterator_get_value_done`）: 同型（`sf->cur_pc = pc` → ヘルパー呼び出し → 失敗なら exception → 成功なら `sp` 更新）。`js_poll_interrupts` 直接呼び出しなし。

- `quickjs.c:19425-19438`（`OP_iterator_close`）

  ```c
  CASE(OP_iterator_close):
      sp--; /* drop the catch offset to avoid getting caught by exception */
  JS_FreeValue(ctx, sp[-1]); /* drop the next method */
  sp--;
  if (!JS_IsUndefined(sp[-1])) {
      sf->cur_pc = pc;
      if (JS_IteratorClose(ctx, sp[-1], false)) {
          goto exception;
      }
      JS_FreeValue(ctx, sp[-1]);
  }
  sp--;
  BREAK;
  ```

  FACT: **`sp` は `JS_IteratorClose`（例外を投げうる）を呼ぶ前に既に2回減算済み**（catch offset を落とし、next メソッドを解放して落とす）。つまりこの命令は「例外を投げる可能性のある呼び出しの前に、既に `sp` を部分的に変更している」実例——仕様書 §7 が言う「結果を確定させてから確認する」設計と対照的に、ここは**呼び出し前にスタックを縮めている**。例外時、最後の `sp--`（iterator 自身を捨てる分、19437）は実行されない——`goto exception` で抜けるため。この場合スタックには iterator の値が1つ残ったままだが、`exception:` の汎用巻き戻しループが回収する。`js_poll_interrupts` の直接呼び出しはこのハンドラ内には無い。

- `quickjs.c:19456-19468`（`OP_iterator_next`）

  ```c
  CASE(OP_iterator_next):
  {
      JSValue ret;
      sf->cur_pc = pc;
      ret = JS_Call(ctx, sp[-3], sp[-4], 1, vc(sp - 1));
      if (JS_IsException(ret)) {
          goto exception;
      }
      JS_FreeValue(ctx, sp[-1]);
      sp[-1] = ret;
  }
  BREAK;
  ```

  FACT: `JS_Call` はユーザー定義のイテレータ `next` メソッドを直接呼び出しうる——これは任意の JS コードへの再入口であり、`JS_Call` の実装が `JSObject` の `call` フィールド経由で `JS_CallInternal` に到達すれば §3 で記録した入口チェック（18164）が効く。`OP_iterator_next` 自身には `js_poll_interrupts` の直接呼び出しは無い。

- `quickjs.c:19470-19503`（`OP_iterator_call`、`return`/`throw` メソッド呼び出し）: `JS_GetProperty` → （あれば）`JS_CallFree` という2段の呼び出し。いずれも `js_poll_interrupts` の直接呼び出しはハンドラ内に無い。`JS_CallFree` 経由でユーザーコードに入りうる点は `iterator_next` と同じ。

## 7. await / yield / async_yield_star / initial_yield

- `quickjs.c:20750-20763`

  ```c
  CASE(OP_await):
      ret_val = js_int32(FUNC_RET_AWAIT);
  goto done_generator;
  CASE(OP_yield):
      ret_val = js_int32(FUNC_RET_YIELD);
  goto done_generator;
  CASE(OP_yield_star):
      CASE(OP_async_yield_star):
      ret_val = js_int32(FUNC_RET_YIELD_STAR);
  goto done_generator;
  CASE(OP_return_async):
      CASE(OP_initial_yield):
      ret_val = JS_UNDEFINED;
  goto done_generator;
  ```

  FACT: 4組（実質6 opcode: `await`, `yield`, `yield_star`, `async_yield_star`, `return_async`, `initial_yield`）はすべて「`ret_val` にタグ値を設定して `done_generator` へ飛ぶ」という同一パターン。`sp`・`pc` はここでは全く変更されない（この命令ハンドラ自体はスタックにもオペランドにも触れない——タグは C 変数 `ret_val` に載るだけ）。**`js_poll_interrupts` は無い。**

- `quickjs.c:20849-20852`

  ```c
  if (b->func_kind != JS_FUNC_NORMAL) {
  done_generator:
      sf->cur_pc = pc;
      sf->cur_sp = sp;
  } else {
  done:
      ...
  }
  ```

  FACT: `done_generator:` は `sf->cur_pc = pc; sf->cur_sp = sp;` の2行のみ——**現在の `pc`/`sp` をフレームに保存して `JS_CallInternal` を抜ける**。これはジェネレータ/async 関数が中断（await/yield）するたびに通る既存の再入可能ポイントである。`ret_val`（`FUNC_RET_AWAIT`/`FUNC_RET_YIELD`/`FUNC_RET_YIELD_STAR`/`JS_UNDEFINED`）を呼び出し元（ジェネレータ/async ランタイム、本ファイルの別関数）が見て次の動作を決める——本ledgerの範囲外だが、**この仕組み自体が「言語レベルの中断点は pc/sp をフレームに保存して C 関数から抜ける」という既存の実例**であり、§7 が要求する「状態保存は実際に中断するときだけ行う」の生きたパターンと一致する構造を持つ。
  FACT: このパスの再開は `JS_CallInternal` 冒頭のジェネレータ復元コード（18167-18189、`flags & JS_CALL_FLAG_GENERATOR` 分岐）が `sf->cur_sp`/`sf->cur_pc` を読み戻して `goto restart`（18188）するため、追加の中断確認呼び出しなしに"次の opcode から"再開する。**ただし `js_poll_interrupts(caller_ctx)`（18164）は、この復元分岐（18168-18189）よりも前に実行される**——つまりジェネレータ再開時も毎回1回はチェックが効く。

## 8. まとめ表（存在する/しない中断確認）

| opcode 群 | 行 | `js_poll_interrupts` 直接呼び出し |
| --- | --- | --- |
| `goto`/`goto8`/`goto16` | 19239-19256 | あり（各命令末尾） |
| `if_true`/`if_false`/`if_true8`/`if_false8` | 19257-19335 | あり（各命令末尾） |
| `call`/`call0`〜`3`/`tail_call` | 18614-18643 | なし（呼び出し先 `JS_CallInternal` 入口 18164 に依存） |
| `call_method`/`tail_call_method` | 18662-18683 | なし（同上） |
| `call_constructor` | 18644-18661 | なし（`JS_CallConstructorInternal` 入口 20970 に依存） |
| `apply` | 18697-18713 | なし（`js_function_apply` 内部は未確認） |
| `eval` | 18811-18839 | なし（`JS_CallInternal` 経由なら入口依存、`JS_EvalObject` 経路は未確認） |
| `return`/`return_undef` | 18714-18719 | なし |
| `return_async`/`initial_yield`/`await`/`yield`/`yield_star`/`async_yield_star` | 20750-20763 | なし（`done_generator` 経由でフレーム離脱、再開時は呼び出し元入口 18164 依存） |
| `catch` | 19337-19344 | なし |
| `gosub` | 19345-19353 | なし |
| `ret` | 19354-19370 | なし |
| `nip_catch` | 19439-19454 | なし |
| `for_in_start`/`for_in_next`/`for_of_start`/`for_of_next`/`for_await_of_start` | 19372-19410 | なし（ヘルパー内部は未確認） |
| `iterator_get_value_done`/`iterator_check_object`/`iterator_close`/`iterator_next`/`iterator_call` | 19411-19503 | なし（`iterator_next`/`iterator_call` はユーザーコード再入りうる） |

## 9. 既存の中断確認位置（opcode ループ外・重複調査用）

`js_poll_interrupts` の**直接**呼び出し箇所は14箇所（レビュー指摘により修正——本節はもともと「10箇所」と書きながら実際には14件を列挙しており、記述と列挙が矛盾していた。以下は現在の行番号での再grep結果）。うち opcode ループ内は §1・§2 の7箇所（goto系3 + if系4、厳密には goto/goto8/goto16 + if_true/if_false/if_true8/if_false8——19241/19247/19253/19272/19292/19312/19332）。残りループ外7箇所:

- `quickjs.c:8765` — `JS_OrdinaryIsInstanceOf` と思われるプロトタイプチェーン走査ループ内（`/* must check for timeout to avoid infinite loop */` というコメント付き）。
- `quickjs.c:16972`, `17031` — `for-in` 用の列挙オブジェクト構築（`JS_GetOwnPropertyNamesInternal` をプロトタイプチェーンに沿って回すループ）内、同じコメント。
- `quickjs.c:18223` — `JS_CallInternal` 関数入口（§3で詳述。旧稿の18164は本ワークツリーの vmprobe 差分適用前の行番号）。
- `quickjs.c:21029` — `JS_CallConstructorInternal` 関数入口（§3.1で詳述）。
- `quickjs.c:42631`, `42718` — プロトタイプチェーン走査ループ2箇所（`instanceof`/`isPrototypeOf` 相当、`/* avoid infinite loop (possible with proxies) */` コメント付き）。

7（opcodeループ内）+ 7（ループ外）= **14箇所**。合計「10箇所」という記述は誤りだったので訂正する。

**別経路: 正規表現エンジン（レビュー指摘により追記）** `js_poll_interrupts`/`interrupt_counter` を経由しない**もう1つの中断確認地点**が `js_regexp_exec` にある。`lre_check_timeout`（`quickjs.c:50400-50406`、libregexp からのコールバック）は `rt->interrupt_handler` を**無条件かつ直接**呼ぶだけで `ctx->interrupt_counter` を一切減算しない。ハンドラが真を返すと libregexp 側が `LRE_RET_TIMEOUT` を返し、それを受けた `js_regexp_exec` が `JS_ThrowInterrupted(ctx)` を呼ぶ箇所が quickjs.c 内に**2箇所**ある: `quickjs.c:50531-50532`（最初の `lre_exec` 呼び出し）と `quickjs.c:50745-50746`（2番目の `lre_exec` 呼び出し、`Symbol.replace`等の経路）。**この2箇所は §9 の14件のカウントに含まれない**——`js_poll_interrupts`という関数を経由しないため`grep js_poll_interrupts`では見つからない。正規表現の実行中は、opcodeベースの中断確認が一切効かないビルトインC関数の内側にいながら、同じ「捕捉不能な`InternalError: interrupted`」が投げられうる、という事実として記録する（03-jobs-interrupts.md の fact 36 も参照——旧稿はこの事実を誤って「libregexp側の挙動は不明」としていたため訂正済み）。

FACT: `__js_poll_interrupts`（実体、8456-8467）は `ctx->interrupt_counter = JS_INTERRUPT_COUNTER_INIT`（8459、`JS_INTERRUPT_COUNTER_INIT == 10000`、`quickjs.c:476`）でカウンタをリセットしてから `rt->interrupt_handler` を呼ぶ。`js_poll_interrupts`（inline、8469-8476）は `--ctx->interrupt_counter <= 0` のときだけ `__js_poll_interrupts` を呼ぶ——**呼び出し箇所が多くても、実際にハンドラが起動する頻度は全呼び出し箇所を横断した共有カウンタで間引かれる**（1つの `JSContext` に1つの `interrupt_counter`。goto ループで急減し、次に call 入口に来た時点で既に0近くなっている、といったことが起こりうる——カウンタは呼び出し箇所ごとに独立していない）。

## L2/L3/L5 に効く点（短い列挙のみ、判断は別レビューで）

- goto系・if系の7箇所は「分岐先確定後」に統一されており、再開契約は「保存した pc から次命令」で閉じている。既存の設計パターンとして流用しやすい形。
- call/return/yield/await/catch/gosub/ret/nip_catch/for_in/for_of/iterator 系には**中断確認が1つも無い**。呼び出し境界（`JS_CallInternal`/`JS_CallConstructorInternal` の入口）だけが暗黙の確認点になっている。§7 の表が挙げる「呼び出し準備状態の所有権が確定する地点」「結果を呼び出し元へ渡し、フレーム寿命を整理した地点」に対応する明示的なチェックポイントは現状のコードに存在しない——新設が必要。
- `interrupt_counter` は `JSContext` 単位の単一カウンタで、全14箇所の呼び出し地点を横断して共有される。新しいチェックポイントを追加する際、このカウンタ機構をそのまま使うか、VM 専用の別カウンタ/フラグにするかは要検討（既存のホスト割り込みハンドラ機構と衝突・相互干渉しうる）。
- `OP_iterator_close`（19425-19438）は「例外を投げうる呼び出しの前に sp を既に縮めている」数少ない実例。§7 が前提とする「結果を確定させてから確認」の逆パターンがコード中に実在する——チェックポイント設計で「sp 部分変更後に throw しうる」ケースの参考になる。
- `nip_catch`（19439-19454）は有界だが静的に一定でないループを含む（スタック上の一時値の個数に依存）。ここも中断確認候補になりうるが、ループ内で毎回 `js_poll_interrupts` を呼ぶと通常経路の性能に影響するため、頻度を検討する必要がある。
- ジェネレータ/async の `done_generator` パス（20849-20852、`sf->cur_pc`/`sf->cur_sp` の保存だけで関数を抜ける）は、VM の中断・再開契約の雛形として既に動いている実装——新チェックポイントの保存フォーマットをこれに合わせられないか検討の価値がある。

## 未確認

- `js_for_in_start` / `js_for_in_next` / `js_for_of_start` / `js_for_of_next` / `js_iterator_get_value_done` の関数本体（`quickjs.c` 内の別箇所、行番号未特定）が内部で `js_poll_interrupts` を呼んでいるかどうか。
- `js_function_apply`（`OP_apply` から呼ばれる）の内部実装が `js_poll_interrupts` を呼ぶかどうか。
- `JS_EvalObject`（`OP_eval` の direct eval 経路）が `js_poll_interrupts` を呼ぶかどうか、また内部で `JS_CallInternal` を経由するかどうか。
- `JS_IteratorClose`（`OP_iterator_close` から呼ばれる）の内部実装。
- `JS_Call` / `JS_CallFree`（`OP_iterator_next`/`OP_iterator_call` から呼ばれる汎用呼び出しラッパー）が `JS_CallInternal` 以外の経路（class call ハンドラ等、18195-18204 の `call_func` 分岐）を取ったときに §3 の入口チェックを経由しない場合があるかどうか——`p->class_id != JS_CLASS_BYTECODE_FUNCTION` のネイティブ関数呼び出し（`call_func(...)`, 18202-18203）には `js_poll_interrupts` が無いように見えるが、ネイティブ関数個別の実装まで辿れていない。
- `OP_gosub` が使う `diff`（`get_u32(pc)`）の符号——`goto` 系と異なり `(int32_t)` キャストなしで `pc += diff` している（19351）ため、`diff` が非負専用（前方ジャンプのみ）という上流の意図なのか単なる型の癖なのか、`quickjs-opcode.h` のエンコーダ側を見ていないため未確認。
- `interrupt_handler` の呼び出しコスト（`rt->interrupt_handler(rt, rt->interrupt_opaque)`, 8461）の実測値。このリポジトリでは PocketJS 側のハンドラ登録有無・実装を未調査。
- computed-goto 版と switch 版で `CASE(OP_call0): CASE(OP_call1): ...` のような多重ラベル（フォールスルー的な複数 CASE の連続）が生成コード上でジャンプテーブルの複数エントリを同一ラベルへ向けるだけなのか、それとも何らかの追加コストがあるのか——コンパイラ依存であり、このセッションでは逆アセンブルしていない。
- **本台帳群の行番号の基準（レビュー指摘により追記）:** `quickjs.c` は `git log -1` の HEAD（本ワークツリーでは `d9ef1f9`）に対し、`apps/vmprobe/` 用の計測フック（vmprobe）がコミットされずに +59行（6箇所のhunk、`@@ -47,+6` `@@ -424,+14` `@@ -2134,+18` 等）当たっている未コミット差分の状態で読んでいる。§9で修正した件数（14箇所）は**この未コミット状態での実地grepに基づく**。今後 vmprobe がコミットされる、あるいは別セッションが `quickjs.c` を編集すると、本台帳群の行番号は再びずれる——どの版に対する行番号かは `git rev-parse HEAD` と `git diff --stat -- components/quickjs-ng/quickjs-ng/quickjs.c` を併記しない限り再現できない。次にこの台帳を使う側は、まず対象行の周辺コードが本台帳の引用コードと一致するかを確認すること。
