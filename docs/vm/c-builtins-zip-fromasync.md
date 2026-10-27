# `Iterator.zip` / `Iterator.zipKeyed` / `Array.fromAsync` を C で書き直し、ROM intrinsics に戻す（設計、2026-10-27）

**状態: 実装済み（issue #91、§9 に記録）。** 以下の §0〜§8 は実装前の設計で、数字は当時の測定と推定のまま。 数字の区分は [harden.md](harden.md) と同じ 3 つ: **実測(host)**（この文書のために WSL で測った値。手順は §8）、**推定**、**コード**（ソースで確かめた事実、`quickjs.c` は `8a828059` の行番号）。

## 0. 結論

| 項目 | 結論 |
|---|---|
| 何が無いか | ROM 版（`CONFIG_POCKET_VM_ROM_INTRINSICS=y`、既定）では `Iterator.zip`・`Iterator.zipKeyed`・`Array.fromAsync` が `undefined`（実測(host)、§1.4）。上流 quickjs-ng 0.14 は 3 つを **JS で書いて bytecode として埋め込み**（`builtin-*.h`、計 6,078 B）、最初の読みで `js_bytecode_autoinit` がコンパイルする（コード、`quickjs.c:10401`）。bytecode は IC・T1 が書くので flash に置けず、生成器が落とした（[rom-intrinsics.md](rom-intrinsics.md) RD5） |
| 順序 | **zip / zipKeyed を先に、fromAsync を後に。** 主線の提案を支持する（§6.1）。zip は同期で、既存の `JS_CLASS_ITERATOR_HELPER` の枠に 1 つの kind を足すだけで済む。fromAsync は await ごとの継続を C で張る必要があり、形は決まった（§2.2）が検査の面が広い |
| zip の設計 | 新しい class id は**要らない**。結果は仕様どおり `%IteratorHelperPrototype%` を [[Prototype]] に持つので、`JS_CLASS_ITERATOR_HELPER` に kind `ZIP` を足し、`JSIteratorHelperData` を先頭に持つ拡張レコードに iterator・next・padding・key の列を C の可変長配列で置く（§2.1）。finalizer / mark は kind を見て列を歩く |
| fromAsync の設計 | **async 関数の `await` と同じ部品で、C の明示的な状態機械にする**: `js_promise_resolve(ctx, ctx->promise_ctor, v)` で包み、`perform_promise_then(promise, {fulfilled, rejected}, {undefined, undefined})` で続きを登録する（`quickjs.c:24344-24362` の await の実装そのもの。cap 側に `undefined` を渡せるのは `promise_reaction_tail` の明示的な拡張、`60435` 行）。状態は RAM の 1 オブジェクト（新 class）に持ち、継続の C 関数は呼び出しごとに 2 つ（fulfilled / rejected、`JS_NewCFunctionData`）を**一度だけ**作って全 await で使い回す（§2.2）。候補 4 つの比較は §2.3 |
| ROM / harden | 作るものはすべて RAM、ROM へ書くものは無い、`realm == NULL` で動く（§3）。生成器は `dropped_value` を外すだけでよいが、**`mode`・`padding`・`shortest`・`longest`・`strict` の 5 atom が未定義**なので、predefined atom に足して ROM atom 表と intrinsics 表を再生成する（§3.3）。非 ROM ビルドでは F2 の遅延リストの普通の項目になり、`JS_AUTOINIT_ID_BYTECODE` の機構と 3 つの `.h` は丸ごと消せる |
| コスト | flash: C 本体 +7〜10 KB（推定、§4.1）、消える bytecode 6.1 KB ＋ autoinit の C 約 2 KB で**正味 ±1 KB 前後**（推定）。ROM データ +約 0.4 KB（計算）。RAM: 使わないアプリは **0**（今の RAM 版は `Iterator.zip` を読むだけで +7.3 KB、3 つで +15.7 KB、実測(host, 64 bit)）。使うときは zip の iterator 1 つ約 150 B、fromAsync 1 回約 0.6 KB 常駐 ＋ await ごとに約 0.25 KB の一時（推定、§4.2） |
| 検査の基準 | test262 の 3 ディレクトリ 177 ファイル・350 (test, mode) は、今の RAM 版 JS 実装で **350/350 pass**（実測(host)）。`--harden` では 314 pass / 36 fail で、**36 件すべて harden の規則どおりの失敗**（属性 20、組み込みの書き換え 12、harness の `new Function` 4。§5.1）。C 版はこの 2 つの集合に一致させる。ROM 版は今 10 件が「`undefined` を呼んで TypeError」で**理由違いの pass** |
| 決めること | 5 atom の足し方、上流の `.h` を消すか、fromAsync を本当に入れるか、test262 の SUBSET への追加、実機での確かめ方（§6.3） |

## 1. 上流の JS 実装が何をしているか

ソースは上流 `quickjs-ng/quickjs` v0.14.0 の `builtin-iterator-zip.js`（7,951 B）・`builtin-iterator-zip-keyed.js`（7,666 B）・`builtin-array-fromasync.js`（1,706 B）。このリポジトリには **コンパイル済みの bytecode（`.h`）しか無い**ので、JS の本文は上流から取って読んだ。配線は `quickjs.c` の次の箇所（コード）:

- `JS_DefineAutoInitProperty(ctx, Iterator, JS_ATOM_zip / zipKeyed, JS_AUTOINIT_ID_BYTECODE, ...)`（`63894-63899`）、`Array` の `fromAsync`（`63936-63939`）。属性は `WRITABLE | CONFIGURABLE`。
- 初回の読みで `js_bytecode_autoinit`（`10401`）が `JS_ReadObject` → `JS_EvalFunction` → 外側の関数を呼び（`js_bytecode_eval`、`10377`）、依存する組み込み（`Array`・`TypeError`・`InternalError`・`Object.defineProperty`・`Symbol.iterator`・monkey-patch 耐性の `call`・`hasOwnEnumProperty`・`getOwnPropertyKeys`・`%IteratorHelperPrototype%`）を**引数で渡す**。zip の結果は `JS_SetConstructorBit(false)`。
- 渡す側で `JS_NewCFunction(ctx, js_array_constructor, "Array", 0)` のように**毎回新しい C 関数オブジェクトを作っている**（RAM）。zip 系は `js_call_function`・`js_getOwnPropertyKeys`・`js_hasOwnEnumProperty`（`10332-10375`）の 3 つの補助 C 関数を持ち、これは JS 版のためだけに存在する。

### 1.1 `Iterator.zip(iterables, options)`（仕様 sec-iterator.zip、proposal-joint-iteration）

JS 版が実装している手順（コード、上流 JS を読んだ）:

1. `iterables` が object でなければ TypeError。**options はまだ読まない**（test `iterables-primitive.js` は options の getter が呼ばれないことを見る）。
2. `options` が `undefined` なら `{__proto__: null}`、object でなければ TypeError。`mode = options.mode`、`undefined` なら `"shortest"`。`"shortest" | "longest" | "strict"` のどれかと**厳密等価**でなければ TypeError（ToString しない。String オブジェクトは落ちる）。
3. `mode === "longest"` のときだけ `padding = options.padding` を読み、`undefined` でなければ object であること。
4. `iterables[Symbol.iterator]()` で外側の iterator を取り、`next` を一度取って回す。各 item で `.done` を**先に**読み、`.value` を後に読む（順序が観測される）。値は **GetIteratorFlattenable(value, reject-primitives)**: object でなければ TypeError、`value[Symbol.iterator]` があれば呼んで iterator に、無ければ値そのものを iterator とみなす。`iters[i]`・`nexts[i] = iter.next` を保存。
   - 外側の `next()` が投げたら、**外側は閉じず**、開いた `iters` を全部閉じて再送出。値が不正で TypeError のときは外側も閉じる。
5. `padding` が object なら `padding[Symbol.iterator]()` を回して `count` 個まで取る。途中で `next`・`.done`・`.value` が投げたら padding の iterator は**閉じず**、`iters` を閉じて再送出。`count` 個取っても `done` でなければ padding の iterator を **IteratorClose**。足りない分は `undefined`。`padding` が無ければ全部 `undefined`。
6. 結果は `{__proto__: IteratorHelper, next(){}, return(){}}` のオブジェクト。状態は `0 new / 1 suspend / 2 execute / 3 complete` の数値。
7. `next()`: `execute` なら TypeError（"running zipper"）、`complete` なら `{undefined, true}`。各 i について昇順に:
   - `iters[i]` が無い（longest で尽きた）→ `results[i] = pads[i]`。
   - `call(iter, nexts[i])` が投げたら `iters[i] = undefined` にして（**投げた iterator は閉じない**）残りを逆順に閉じ、再送出。
   - `.done` が偽: `strict` で既に誰かが `done` なら全部閉じて TypeError（"mismatched inputs"）。そうでなければ `results[i] = result.value`。
   - `.done` が真: `iters[i] = undefined`。`shortest` → 残りを逆順に閉じ（close の例外は最初の 1 つが勝つ）、complete。`longest` → 全部尽きたら complete、そうでなければ `results[i] = pads[i]`。`strict` → 先に値を返した iterator があれば全部閉じて TypeError、無ければ続けて残りの `next()` を**呼ぶだけ**（test `strict-checks-remaining-done.js`: 3 本とも同時に尽きるときは 3 本すべての `next` が呼ばれ、`return` は呼ばれない）。
   - 1 つも値が無ければ complete。結果は毎回**新しい配列**（`result-arrays-are-fresh.js`）。
8. `return()`: `new` なら**先に complete にしてから**全部逆順に閉じる（閉じている最中に再入した `it.return()` は TypeError ではなく `{undefined, true}` を返す。test `suspended-start-iterator-close-calls-return.js`）。`suspend` なら `execute` にして閉じ、complete。`execute` なら TypeError。閉じたときの例外は最初の 1 つが勝ち、後の `return` も呼ばれる（仕様 IteratorCloseAll: 逆順、completion は最初の throw を保つ）。

### 1.2 `Iterator.zipKeyed(iterables, options)`

zip と同じ骨格で、違いは 3 つ（コード）:

- 列は `iterables` の**自身のキー**（`getOwnPropertyKeys` = 文字列と Symbol の両方、`JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK`、`10342-10347`）。**先に全キーを集めてから**、キーごとに「今も自身の enumerable なプロパティか」を確かめ（getter が後のプロパティの可視性を変えうる）、値が `undefined` でないものだけを残す（`iterables-iteration-deleted / -enumerable / -inherited / -undefined / -symbol-key.js`）。仕様の `[[OwnPropertyKeys]]` → `[[GetOwnProperty]]` → `Get` の順そのもの。
- padding は iterator ではなく `padding[keys[i]]` の **Get**（キーごと）。
- 結果は **`OrdinaryObjectCreate(null)`** に `CreateDataPropertyOrThrow(key, value)`（`results-object-has-default-attributes.js`: [[Prototype]] は null、属性は C/W/E、キー順はキー列の順）。

### 1.3 `Array.fromAsync(asyncItems, mapfn, thisArg)`（仕様 sec-array.fromasync）

JS 版は `async function fromAsync` 1 つ（コード、上流 JS）。仕様との対応と、**仕様から外れている箇所**:

1. `mapfn` が `undefined` でなく関数でもなければ TypeError（async 関数の中なので **reject**）。
2. `usingAsyncIterator = asyncItems[Symbol.asyncIterator]`、null/undefined なら `usingSyncIterator = asyncItems[Symbol.iterator]`。読みの順は観測される（`asyncitems-operations.js`: asyncIterator → iterator → length → 0 → 1）。
3. どちらかがあれば **iterator の経路**: `A = IsConstructor(this) ? new this() : Array()`、`iter = method.call(asyncItems)`、ループで `await iter.next()`。**JS 版は async/sync を同じ `iter.next()` で回し、sync のときだけ値を `await value`** する。仕様は sync の method のとき `CreateAsyncFromSyncIterator` を通す（値の await はラッパーの `next` が行う）。観測上の差は、仕様では `next` の結果（Promise）を Await した後に `done` を読み `value` を読むが、JS 版は分割代入 `{value, done}` で **`value` を先に読む**。test262 はこれを捉えていない（350/350 pass）が、C 版は `JS_IteratorGetCompleteValue`（`done` → `value`、`19001`）で仕様の順にできる。
4. `mapfn` があれば `await mapFn.call(thisArg, value, i)`（1 回だけ await。`mapfn-result-awaited-once-per-iteration.js`）。
5. `Object.defineProperty(result, i++, {value, configurable, writable})` = **CreateDataPropertyOrThrow**（enumerable も true）。`this-constructor-operations.js` は「construct → defineProperty A[0], A[1] → set A.length」の順を見る。
6. 終了時 `result.length = i`（strict なので Set(..., true)）。
7. **閉じ方**: JS 版は `finally { if (iter.return) iter.return() }` で、**正常終了（`done`）のあとも `return` を呼び、戻り値を await しない**。仕様は abrupt のときだけ `AsyncIteratorClose`（`return` を呼び、その結果を **Await** し、元の例外を優先）。test262 は正常終了後の `return` 呼び出しを数えていない（通っている）ので、上流の簡略化が見えていないだけ。C 版は仕様どおりにする（§2.2 の CLOSING 状態）。また `iter.next()` の結果が object でない・`done`/`value` の読みが投げる、は**閉じない**（iterator が壊れているとみなす）。
8. どちらも無ければ **array-like の経路**: `length = +arrayLike.length || 0`（仕様は ToObject → LengthOfArrayLike = ToLength。`+x || 0` は NaN → 0 は同じだが負や大きい値の扱いが違う: `asyncitems-arraylike-too-long.js` は通っている）、`A = IsConstructor(this) ? new this(length) : Array(length)`、各 k で `value = await arrayLike[k]`（**常に await**、`asyncitems-arraylike-promise.js`）、mapfn があれば await、defineProperty。この経路に閉じるものは無い。
9. 返すのは **本物の Promise**（async 関数の戻り値 = `NewPromiseCapability(%Promise%)`）。`this` が何であっても Promise の種類は変わらない。

**harden との関係（規則 4「species を見ない」、[harden.md](harden.md) §1）**: fromAsync の `this` の扱いは species ではなく **`IsConstructor(this) ? Construct(this)`** で、`Array.from` の C 版（`js_array_from`、`47865-47869`、`JS_IsConstructor(this_val) ? JS_CallConstructor : JS_NewArray`）と同じ。harden は `constructor` と `Symbol.species` の読みだけを消す（`JS_SpeciesConstructor`、`46845`）ので、`Array.fromAsync.call(MyArray, ...)` は harden でも仕様どおり `MyArray` を構築する。**変える理由は無い**: アプリが明示的に呼ばなければ起きない経路で、「組み込みが勝手に呼ぶ」species とは違う。C 版も `js_array_from` に揃える。

### 1.4 今の状態（実測(host)、2026-10-27、`8a828059`、WSL、asan）

| ビルド | test262 の 3 ディレクトリ（177 ファイル、350 (test, mode)） | 備考 |
|---|---|---|
| RAM（`asan`、harden なし） | **350 pass / 0 fail** | 上流 JS 版の完全合格。基準線 `test262-baseline.txt` にはこの 3 ディレクトリが**無い**（SUBSET 外、§5.1） |
| RAM（`asan --harden`） | 314 pass / **36 fail** | 36 件すべて harden の規則どおり（§5.1 の内訳）。`--force-yield` でも同じ 314 / 36 |
| ROM（`asan-rom --harden`） | 10 pass / 340 fail | 3 つが `undefined`。10 件の pass は `Iterator.zip()` が TypeError を期待する試験で、「関数が無いので `not a function`」が偶然一致したもの（`iterables-primitive`・`iterator-non-iterable`・`non-constructible`）。**理由違いの pass** |

ROM 版で `typeof Iterator.zip, typeof Array.fromAsync` → `undefined undefined`、`Object.getOwnPropertyNames(Iterator)` → `length,name,concat,from,prototype`（RAM 版は `...,zip,zipKeyed`）。

今の RAM 版で**読むだけ**の代償（`vmrun --profile device --harden --stats`、host 64 bit、`qjs_malloc_size`。実機は JSValue 8 B・ポインタ 4 B なのでおおむね 55〜60%、推定）:

| スクリプト | `qjs_malloc_size` | 差 |
|---|---:|---:|
| `var x = 1` | 42,736 | — |
| `var x = Iterator.zip` | 50,048 | **+7,312** |
| `var x = Iterator.zipKeyed` | 50,344 | +7,608 |
| `var x = Array.fromAsync` | 45,408 | +2,672 |
| 3 つとも | 58,484 | **+15,748** |
| zip と zipKeyed を実際に使う（2 本ずつ） | peak 59,189（空 40,594） | +18.6 KB（peak） |

bytecode の読み込みと関数のコンパイルの分で、F2 で遅延にしても「初めて触った時」に全部払う。ROM 版の空スクリプトは 22,976 B で、`Iterator.zip` を読んでも変わらない（無いから）。

## 2. C での書き方

### 2.1 zip / zipKeyed: `JS_CLASS_ITERATOR_HELPER` に kind を 1 つ足す

**class id を足さない理由（コード）**: 仕様の結果は `CreateIteratorFromClosure(..., "Iterator Helper", %IteratorHelperPrototype%)` で、test `result-is-iterator.js` が `Object.getPrototypeOf(iter) === %IteratorHelperPrototype%` を見る。`%IteratorHelperPrototype%.next` は `js_iterator_helper_next` で、先頭で `JS_GetOpaque2(ctx, this_val, JS_CLASS_ITERATOR_HELPER)`（`50863`）を取る。別の class id のオブジェクトに同じ prototype を付けると `next` が TypeError になる。だから **同じ class id** で、`JSIteratorHelperKindEnum`（`991`）に `JS_ITERATOR_HELPER_KIND_ZIP` を足し、`js_iterator_helper_next` の `switch (it->kind)` に 1 case 足す（`case ZIP: ret = js_iterator_zip_step(ctx, it, magic, pdone); goto done_zip;`）。既存の `done:` ラベルは `it->done = magic == NEXT ? *pdone : 1`（`51109`）と `it->executing = 0` で、zip にもそのまま合う。`fail:` の `JS_IteratorClose(ctx, it->obj, true)`（`51114`）は zip では使わない（閉じる対象が列）ので、zip の case は自分で閉じてから `done` へ飛ぶ。

**レコード**: `JSObject.u.iterator_helper_data` は `JSIteratorHelperData *`（`1266`）。先頭に同じ構造体を置いた拡張レコードにする。

```c
/* Iterator.zip / zipKeyed: one record, JSIteratorHelperData first so the
   helper's next/return, finalizer and mark see an ordinary helper. The
   per-iterator columns follow as flexible storage: iters[count],
   nexts[count], then pads[count] (mode longest only) and keys[count]
   (zipKeyed only). A closed iterator is JS_UNDEFINED in iters[]. */
typedef struct JSIteratorZipData {
    JSIteratorHelperData h;  /* kind = ZIP; h.obj/next/func/inner unused (UNDEFINED) */
    uint32_t count;          /* iterators (zipKeyed: kept keys) */
    uint32_t alive;          /* iterators not yet done */
    uint8_t mode;            /* 0 shortest, 1 longest, 2 strict */
    uint8_t keyed;           /* zipKeyed: keys[] present, result is a null-proto object */
    uint8_t started;         /* suspended-start vs suspended-yield (return() differs) */
    JSValue cols[];          /* iters, nexts, [pads], [keys] */
} JSIteratorZipData;
```

- `h.executing` は「running zipper」の TypeError に、`h.done` は complete に使う（既存の検査 `50866-50873` がそのまま効く）。`started` は `return()` の違いのため: suspended-start では**先に** `h.done = 1` にしてから閉じ（再入した `return()` が `{undefined, true}` を返す）、suspended-yield では `executing` を立てて閉じる（再入は TypeError）。§1.1 の 8。
- 列を JS 配列ではなく C の配列に置くのは `JSIteratorConcatData.values[]`（`50130`）と同じ流儀。配列オブジェクト 3〜4 個分（1 個 48 B ＋ 値）を払わない。
- **finalizer / mark**（`50827`・`50840`）は `if (it->kind == ZIP)` で `cols[]` を歩く行を足す。`js_free_rt(rt, it)` はブロック全体を解放するので、拡張レコードでもそのまま正しい。
- `count` が 0 の zip（`Iterator.zip([])`）は `cols[]` が空。`alive == 0` なので最初の `next()` で complete。

**生成（`js_iterator_zip(ctx, this_val, argc, argv, magic)`、magic 0 = zip, 1 = zipKeyed）**:

1. 引数の検査（§1.1 の 1〜3）。`mode` の比較は値が文字列なら `JS_ValueToAtom` で atom にして `JS_ATOM_shortest / longest / strict` と比べる（既存の atom に昇格するだけで、無い文字列は一時 atom を作ってすぐ `JS_FreeAtom`）。`options.mode`・`options.padding` の読みは `JS_GetProperty(ctx, options, JS_ATOM_mode / JS_ATOM_padding)`。**この 5 つは predefined atom に無い**（§3.3）。
2. 列を集める。count が事前に分からない（外側は iterator）ので、`iters`・`nexts` は一時の `JSValue *` を `js_realloc` で伸ばし（`JS_GetOwnPropertyNamesInternal` の `JSPropertyEnum` と同じ扱い）、確定後にレコードを 1 回 `js_malloc` して写す。zipKeyed は `JS_GetOwnPropertyNamesInternal(ctx, &atoms, &len, p, JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK)` で全キーを取り、キーごとに `JS_GetOwnPropertyInternal`（enumerable の確認、`11277`）→ `JS_GetProperty` → `undefined` なら飛ばす。
3. 各値に GetIteratorFlattenable: object でなければ TypeError、`JS_GetProperty(v, JS_ATOM_Symbol_iterator)` が null/undefined でなければ `JS_GetIterator2(ctx, v, method)`（`18776`、結果が object でなければ TypeError）、無ければ v 自身。`next` は `JS_GetProperty(iter, JS_ATOM_next)`。
4. padding（§1.1 の 5、zipKeyed は `JS_GetProperty(padding, keys[i])`）。
5. 失敗時は「開いた列を逆順に閉じる」`js_iterator_zip_close_all(ctx, cols, count, /*pending*/true)`（下）を通してから一時配列を解放する。外側の iterator と padding の iterator を閉じるかは §1.1 の 4・5 の区別どおりで、閉じるときは `JS_IteratorClose(ctx, outer, true)`。
6. `obj = JS_NewObjectClass(ctx, JS_CLASS_ITERATOR_HELPER)`（`js_create_iterator_helper` と同じ、`50484`）、`JS_SetOpaqueInternal(obj, rec)`。prototype は `ctx->class_proto[JS_CLASS_ITERATOR_HELPER]`（ROM 版では ROM の番地。RAM のオブジェクトが ROM の proto を持つのは今の `Iterator.prototype.map` と同じで、`set_proto` の `is_prototype` の store は R1 が守っている）。

**閉じる**: 

```c
/* IteratorCloseAll: reverse order, the first throw wins but every return()
   still runs. JS_IteratorClose(.., true) keeps the pending exception and
   swallows the close's own, which is exactly that rule once one is pending. */
static int js_iterator_zip_close_all(JSContext *ctx, JSValue *iters, uint32_t count, bool pending)
{
    for (uint32_t i = count; i-- > 0;) {
        JSValue it = iters[i];
        if (JS_IsUndefined(it)) continue;
        iters[i] = JS_UNDEFINED;
        if (JS_IteratorClose(ctx, it, pending) < 0) pending = true;
        JS_FreeValue(ctx, it);
    }
    return pending ? -1 : 0;
}
```

`JS_IteratorClose`（`18910`）は `return` を `JS_GetProperty` で取り、null/undefined なら何もしない、呼んだ結果が object でなければ TypeError、という仕様の IteratorClose そのもの。

**`next` / `return`（`js_iterator_zip_step`）**: §1.1 の 7・8 を素直に書く。`JS_IteratorNext2(ctx, iter, next, 0, NULL, &done)`（`18831`）は `done == 2` のとき「結果オブジェクトを返したので `done`・`value` を自分で読め」の意味で（`js_iterator_concat_next` の使い方、`50204-50222`）、**`done` を先に `value` を後に**読む仕様の順と、「`value` は `done` が偽のときだけ読む」をこの経路で守れる。結果は zip なら `js_create_array(ctx, count, results)`（`49905`）、zipKeyed なら `JS_NewObjectProto(ctx, JS_NULL)` に `JS_DefinePropertyValue(..., JS_PROP_C_W_E)`。`JS_ITERATOR_NEXT_DEF` の経路が `{value, done}` に包む（`js_create_iterator_result`、`19041`）。

**既存の helper との違いで気を付けること**: `js_iterator_helper_next` の `fail:` は `it->obj` を閉じる前提なので、zip の case は `fail` を使わず、自分で `close_all` してから `ret = JS_EXCEPTION; goto done`。strict の「残りの `next` を呼ぶだけ」は `JS_IteratorNext2` の結果を `done` だけ読んで捨てる（`value` を読まない。`strict-checks-remaining-done.js`）。

### 2.2 fromAsync: 状態オブジェクト ＋ 継続 2 つ、`await` と同じ部品で

**C 関数は中断できない**。L2 の中断・再開と L2b の flat call、TCO の guard（[vm-L2-design.md](vm-L2-design.md)、[vm-tco-design.md](vm-tco-design.md)）はどれも **bytecode のフレーム**（`JSStackFrame` のセグメント）を対象にしていて、C のスタックにいる関数を途中で止めて戻す機構は無い。だから fromAsync の `await` は、C では「ここまでの状態を RAM に置き、Promise が settle したら続きの C 関数を呼んでもらう」継続に**明示的に**分ける。副産物として、**await ごとが L1 のジョブ境界**になる（settle → `promise_reaction_job` → 継続、は普通の microtask）ので、`--budget-jobs` や `--force-yield` で止めても意味が変わらない検査がそのまま効く。

**状態機械**（仕様の手順と 1 対 1）:

```
fromAsync(C=this, items, mapfn, thisArg):
  cap = NewPromiseCapability(%Promise%)          JS_NewPromiseCapability(ctx, resolving)  (60902)
  以後の失敗はすべて cap.reject へ（IfAbruptRejectPromise）。関数自身は promise を返す。
  mapfn !== undefined && !callable → reject TypeError
  asyncM = GetMethod(items, @@asyncIterator); 無ければ syncM = GetMethod(items, @@iterator)
  if (asyncM || syncM):                        ---- iterator の経路
     A = IsConstructor(C) ? Construct(C) : ArrayCreate(0)
     iter = JS_GetIterator2(items, asyncM)  または  JS_CreateAsyncFromSyncIterator(JS_GetIterator2(items, syncM))  (61634)
     next = Get(iter, "next");  k = 0
     state NEXT:    r = Call(next, iter) → await(r) → ON_NEXT
     ON_NEXT(r):    r が object でなければ TypeError（閉じない）; done = Get(r,"done")（閉じない）
                    done → Set(A,"length",k,true) → resolve(A)
                    v = Get(r,"value")（閉じない）
                    mapfn なら m = Call(mapfn, thisArg, v, k)  失敗 → CLOSING(err);  await(m) → ON_MAPPED
                    無ければ DEFINE(v)
     ON_MAPPED(m):  DEFINE(m)
     DEFINE(x):     CreateDataPropertyOrThrow(A, k, x)  失敗 → CLOSING(err);  k++ → NEXT
     CLOSING(err):  ret = GetMethod(iter,"return");  無ければ reject(err)
                    r = Call(ret, iter)  失敗 → reject(err)（元の例外が勝つ）
                    await(r) → ON_CLOSED
     ON_CLOSED:     reject(err)   （fulfilled でも rejected でも。AsyncIteratorClose は throw completion を優先）
  else:                                         ---- array-like の経路
     O = ToObject(items); len = LengthOfArrayLike(O)
     A = IsConstructor(C) ? Construct(C, len) : ArrayCreate(len);  k = 0
     state VALUE:   k == len → Set(A,"length",len,true) → resolve(A)
                    v = Get(O, k) → await(v) → ON_VALUE
     ON_VALUE(v):   mapfn なら m = Call(mapfn, thisArg, v, k) 失敗 → reject;  await(m) → ON_MAPPED
                    無ければ DEFINE(v)
     （この経路に閉じるものは無い）
  rejected 側の継続: どの await の reject も → iterator の経路の ON_NEXT 待ち中なら reject(err)（next 自体の失敗は閉じない）、
                    ON_MAPPED 待ち中なら CLOSING(err)、ON_CLOSED 待ち中なら reject(保存した err)、array-like なら reject(err)
```

**`await(p)` の部品**（コード、`quickjs.c:24342-24366` の async 関数の await そのもの）:

```c
/* Await(v): PromiseResolve(%Promise%, v), then our two continuations. The
   capability side is undefined: promise_reaction_tail accepts that as the
   "no throwaway promise" extension the interpreter's await relies on
   (quickjs.c:60435), so a step costs no derived promise. */
promise = js_promise_resolve(ctx, ctx->promise_ctor, 1, &v, 0);   /* 60907 */
JSValueConst none[2] = { JS_UNDEFINED, JS_UNDEFINED };
perform_promise_then(ctx, promise, s->cont /* {fulfilled, rejected} */, none);  /* 61354 */
```

`js_promise_resolve` は値が本物の Promise で `constructor` が `%Promise%` なら**そのまま返す**（`60915-60926`、仕様 PromiseResolve）ので、ユーザーの Promise を二重に包まない。harden では `constructor` の読みが残るが、これは species ではなく PromiseResolve の規定で、async 関数の await も同じ経路を通っている。

**状態の置き場**: 新しい class `JS_CLASS_ARRAY_FROM_ASYNC`（`JSClassShortDef` を `js_async_class_def`、`61801` 付近に 1 行足す。ROM 版は `js_rom_runtime_classes` が Promise 群を登録する（`69787-69796`）のでそこにも 1 行）。opaque:

```c
typedef struct JSArrayFromAsyncData {
    JSValue resolving_funcs[2];   /* the capability of the returned promise */
    JSValue cont[2];              /* fulfilled / rejected continuations (JS_CLASS_C_FUNCTION_DATA, data[0] = this state object) */
    JSValue result;               /* A */
    JSValue iter, next;           /* iterator path; UNDEFINED on the array-like path */
    JSValue array_like;           /* array-like path */
    JSValue mapfn, this_arg;
    JSValue pending_error;        /* CLOSING: the completion to rethrow after return() settles */
    int64_t k, len;
    uint8_t state;                /* NEXT.. ON_CLOSED */
    uint8_t mapping : 1, is_iter : 1;
} JSArrayFromAsyncData;
```

継続 2 つは `JS_NewCFunctionData(ctx, js_array_fromAsync_step, 1, /*magic*/0 or 1, 1, &state_obj)`（`7856`、`7815`）で**呼び出しの最初に一度だけ**作り、全 await で同じものを `perform_promise_then` に渡す。`js_promise_all_resolve_element`（`61039`）が要素ごとに作るのと違い、ここは await が直列なので 2 つで足りる。状態オブジェクトと継続は互いに参照する（state → cont、cont.data[0] → state）ので GC の mark を両方に書く（`js_c_function_data_mark` は既にあり、`js_array_from_async_mark` を足す）。環が解けるのは、settle 後に `s->cont` を解放する `finish()`（resolve / reject の直前）で。

**継続の中身**（`js_array_fromAsync_step(ctx, this, argc, argv, magic, func_data)`）: `magic` が fulfilled / rejected、`argv[0]` が値または理由。`s->state` で分岐して §2.2 の表を 1 歩進め、次の await を登録して `JS_UNDEFINED` を返す。**継続は投げない**（投げると `promise_reaction_tail` が `undefined` の cap を呼ぼうとして何も起きず、エラーが消える）。失敗はすべて `reject(JS_GetException(ctx))` へ。再入（`iter.next()` が同期的に `resolve` を呼ぶなど）は Promise のジョブ経由なので起きない。

**同期の経路**: `CreateAsyncFromSyncIterator` を使えば、同期 iterator の `next` の結果と値の await は既存の `js_async_from_sync_iterator_next`（`61677`）が引き受ける（値を `js_promise_resolve` で包み、`js_async_from_sync_iterator_unwrap`（`61582`）で `{value, done}` に戻す）。fromAsync の C はラッパーの Promise を await するだけでよく、同期・非同期の 2 経路を 1 つに畳める。test `asyncitems-asynciterator-sync.js`（`@@asyncIterator` が同期 generator を返す）はラッパーを通らない経路で、`r.value` が Promise でも await しないのが正しい（`async-iterable-input-does-not-await-input.js`）。

**閉じる順の検査**（`mapfn-sync-throws-close-async-iterator.js`・`this-constructor-with-unsettable-element-closes-sync-iterator.js`・`sync-iterable-with-rejecting-thenable-closes.js`）: CLOSING は `return` を呼んで**結果を await してから** reject する。sync のラッパーの `return` は `js_async_from_sync_iterator_next(GEN_MAGIC_RETURN)` で、中の `return` を呼んで Promise を返す。

### 2.3 候補の比較（fromAsync の継続）

| 候補 | 何か（コード） | 向き・不向き | 判定 |
|---|---|---|---|
| (a) `JS_NewCFunctionData` の閉包に状態を**全部**持たせる（`Promise.all` の要素 resolver、`61039`・`61191`） | 状態を `func_data[]`（値の配列・残数の配列・index）に分け、`JS_DefinePropertyValueUint32` で JS オブジェクトを書いて進める | 要素が並列で独立なら合う。fromAsync は**直列**で、状態が 10 個近くあり、await ごとに閉包を作り直すと 1 歩 2 オブジェクト（約 150 B）の確保が増える | 部品（`JS_NewCFunctionData`）だけ借りる |
| (b) `AsyncFromSyncIterator` の `next`（`61677`）: 値を `js_promise_resolve` で包み、`perform_promise_then(wrapper, {unwrap, undefined}, cap)` で 1 段変換 | **1 回の await** を C で書く最小形。cap に本物の resolving funcs を渡し、変換の戻り値が外の Promise を settle する | 多段には向かない: 継続の戻り値が cap を settle するので「まだ続く」が表せない。ただし `perform_promise_then` の使い方はこれが手本 | **部品を採る**。`CreateAsyncFromSyncIterator` 自体も再利用（§2.2） |
| (c) async module の継続（`js_async_module_execution_fulfilled / rejected`、`35047`・`35004`、`js_load_module_fulfilled`、`34697`）: RAM の状態（`JSModuleDef`）＋ 継続 C 関数 2 つ（`func_data[0]` に状態へのポインタ）、settle したら次の段を進めて自分を再登録 | **形が同じ**: 状態は 1 か所、継続は 2 つ、await は直列。どちらも `js_promise_then` で登録する（`34733`・`35113`）ので、await ごとに**派生 Promise 1 つと resolving funcs 2 つ**を余計に作る（fromAsync ではこれを (d) の `perform_promise_then` に替える） | 状態が `JSModuleDef`（GC オブジェクトではない生ポインタを `JS_MKPTR(JS_TAG_MODULE)` で運ぶ）なのは module 固有。fromAsync の状態は JS 値を 9 個持つので GC に見える**オブジェクト**にする | **この形を採り**、状態は新 class のオブジェクト、継続の登録は (b)/(d) の `perform_promise_then` に `undefined` の cap で |
| (d) async 関数の `await`（`24342-24366`）: `js_promise_resolve` ＋ `JS_CLASS_ASYNC_FUNCTION_RESOLVE / REJECT` の専用 class（`call` フックが状態へ飛ぶ、`24409`）＋ `perform_promise_then(..., {undefined, undefined})` | 派生 Promise を作らない唯一の経路（`promise_reaction_tail` の明示的な拡張、`60435-60437`）。専用 class なら継続は 48 B のオブジェクト 2 つで済み `JSCFunctionDataRecord` の 24 B × 2 が要らない | 専用 class を 2 つ足す（`JSClassShortDef` と `call` フック）のは (c) の `JS_NewCFunctionData` より行数が多く、ROM 版の `js_rom_runtime_classes` にも足す | **await の部品は採る**。継続のオブジェクトは (c) の `JS_NewCFunctionData` で始め、RAM が気になれば後で (d) の専用 class に替えられる（48 B 差、推定） |
| (e) JS 版を残し、RAM の overlay で ROM の `Array` に載せる | RD5 で捨てた案 | hot path に判定が入る | 捨てる（RD5 のまま） |

結論: **(c) の形 × (b)/(d) の部品**。1 回の fromAsync で作る継続は 2 つ、await ごとの確保は `js_promise_resolve` のラッパー（値が Promise でなければ 1 つ）と `perform_promise_then` の reaction 2 つ ＋ ジョブ 1 つで、どれも settle 後に消える。

## 3. ROM / harden の制約の確認

### 3.1 作るものはすべて RAM、ROM へ書かない

| 何 | どこに生まれるか | ROM へ書くか |
|---|---|---|
| `Iterator.zip` / `zipKeyed` / `Array.fromAsync` の関数オブジェクト | **ROM**（生成器が `js_iterator_funcs` / `js_array_funcs` の項目として焼く、他の C 関数と同じ）。`realm == NULL` | 読むだけ |
| zip の結果（`JS_CLASS_ITERATOR_HELPER`）とレコード | RAM（`JS_NewObjectClass` + `js_malloc`）。proto は ROM の `class_proto[ITERATOR_HELPER]` | `set_proto` の `is_prototype` は R1 で「既に立っていれば書かない」（§4.9 の処置、ROM は全オブジェクト `is_prototype = 1` で焼く）。今の `Iterator.prototype.map` と同じ経路 |
| 結果の配列・null-proto オブジェクト・`{value, done}` | RAM | — |
| fromAsync の Promise・resolving funcs・状態オブジェクト・継続 2 つ・ラッパー Promise・reaction・ジョブ | RAM。継続の proto は ROM の `function_proto` | — |
| 一時 atom（`mode` の比較で作る可能性） | RAM の atom 表（`JS_ValueToAtom` → すぐ `JS_FreeAtom`） | ROM の文字列は atom に昇格させない（R2、`__JS_NewAtom` は写しを作る） |

ROM の C 関数は `js_cfunc_realm`（R1）で呼び手の ctx を使う。zip / fromAsync の中で ctx を使うのは `ctx->class_proto[...]`・`ctx->promise_ctor`・`ctx->function_proto` で、どれも**呼び手の ctx の値**でよい（context は 1 つ、RD2）。`js_call_c_function_data` の継続は `realm` を切り替えない（`7801` の TODO）が、継続は RAM の呼び出しごとのオブジェクトで、`ctx` は登録したときの ctx がジョブに乗って来る（`JSJobEntry.ctx`、`2615`）。

### 3.2 harden の 4 規則との関係

- 規則 1（eval なし）: 関係なし。むしろ **JS 版が消えることで、harden の context から「文字列ではなく bytecode から関数を作る」最後の経路（`js_bytecode_eval` → `JS_ReadObject` → `JS_EvalFunction`）が無くなる**。
- 規則 2（prototype の付け替えなし）: `js_bytecode_eval` が `JS_SetPrototypeInternal(result, function_proto)` をしていた（`10396`）。C 関数なら生まれたときから `function_proto`。
- 規則 3（組み込みは書き換え不可）: `Iterator.zip` は他の C 関数と同じく non-writable・non-configurable で焼かれる。test262 の `prop-desc / length / name / builtin` が落ちるのは今の RAM 版 `--harden` と同じ（§5.1）。
- 規則 4（species なし）: §1.3 のとおり fromAsync は species を使わない。`Array.fromAsync.call(C)` の `C` の構築は仕様どおり残す。

### 3.3 生成器に要る変更

1. `gen_rom_intrinsics.c` の `dropped_value`（70〜84 行）と `kept_props` の除外（389 行）を外す。C 関数は `JS_CLASS_C_FUNCTION` として他のメソッドと同じ経路で焼かれる。「bytecode 関数が残っていたら FAIL」（ヘッダのコメント 23 行）は残す: 以後 bytecode の組み込みは**存在してはいけない**ので、負の対照になる（§5.4）。
2. **atom**: `mode`・`padding`・`shortest`・`longest`・`strict` は `quickjs-atom.h` に無く、F1 の ROM atom 表（`quickjs-rom-atoms.h`、625 番まで）にも無い（実測(host)、grep）。C から `JS_GetProperty(options, JS_ATOM_mode)` と書くには predefined atom に足す（`quickjs-atom.h` に `DEF(mode, "mode")` ほか 5 行）。すると `JS_ATOM_END` が動くので `tools/vmtest/floor/gen_rom_atoms.sh` で ROM atom 表を、続けて `gen_rom_intrinsics.sh` で intrinsics 表を再生成する（`_Static_assert(JS_ROM_PREDEF_END == JS_ATOM_END)`、`3503-3504` 行、が陳腐化を止める。intrinsics 表は atom 番号を埋め込むので両方）。flash は 5 × 8 B ＋ 文字 32 B ≈ **72 B**（計算）。代案は実行時に `JS_NewAtom(ctx, "mode")`（RAM の atom を 1 つ作る）か文字列比較で、どちらも ROM の意味（確保 0）に合わないので採らない。
3. `JS_AUTOINIT_ID_BYTECODE` の枝（`629`・`10474`）、`js_bytecode_autoinit`・`js_bytecode_eval`・`js_call_function`・`js_getOwnPropertyKeys`・`js_hasOwnEnumProperty`（`10327-10470`）、3 つの `.h` の `#include`、`JS_DefineAutoInitProperty` の 3 呼び出し（`63894`・`63897`・`63936`）を消し、`js_iterator_funcs`（`51119`）に `JS_CFUNC_MAGIC_DEF("zip", 1, js_iterator_zip, 0)`・`("zipKeyed", 1, js_iterator_zip, 1)`、`js_array_funcs` に `JS_CFUNC_DEF("fromAsync", 1, js_array_fromAsync)` を足す。`.h` ファイル自体を消すかは §6.3。

### 3.4 非 ROM ビルド（RAM、`CONFIG_POCKET_VM_ROM_INTRINSICS=n`）での F2 / F3

- 関数リストの項目になるので、F2（遅延索引）の普通の項目として扱われる: `Iterator`・`Array` の shape に最初に触れるまで入らず、触ったら C 関数オブジェクト 1 つ（≈ 147 B ＋ 3.4 ブロック、rom-intrinsics §2.2 の実測(host)）。**今の 2.7〜7.6 KB（host）のコンパイルが消える**。
- F3（群の遅延）は Map/Set・型付き配列などの**クラス**の遅延で、`Iterator` と `Array` は eager な基本群。関係なし。
- `JSIteratorHelperKindEnum` の値を足しても `JSIteratorHelperData.kind : 8` に収まる。
- 新 class `JS_CLASS_ARRAY_FROM_ASYNC` は Promise 群と同じ `js_async_class_def` に載せ、`JS_AddIntrinsicPromise`（RAM）と `js_rom_runtime_classes`（ROM）の両方から登録される。

## 4. コスト

### 4.1 flash（推定。根拠は host x86-64 `-O2` の関数サイズ、実測(host)、`nm -S .cache/vmtest/obj-o2/quickjs.o`）

| 相手 | host x86-64 -O2 | 使い方 |
|---|---:|---|
| `js_iterator_concat`（生成） / `_next` / `_return` / finalizer / mark | 869 / 1,234 / 546 / 274 / 156 B | zip の生成・next・return の下限の目安 |
| `js_create_iterator_helper` / `js_iterator_helper_next` | 935 / 3,063 B | 既存 helper。zip の case はこの関数に足す |
| `js_array_from` | 2,649 B | fromAsync の 2 経路の同期版 |
| `js_async_from_sync_iterator_next` / `_unwrap` | 1,678 / 97 B | 1 回の await の C |
| `js_promise_all` / `_resolve_element` | 2,845 / 974 B | 継続つきの組み込みの実例 |
| `JS_IteratorClose` / `js_create_iterator_result` | 518 / 301 B | 既存、増えない |
| **消える**: `js_bytecode_autoinit` / `js_bytecode_eval` / `js_call_function` / `js_getOwnPropertyKeys` / `js_hasOwnEnumProperty` | 1,136 / 512 / 54 / 24 / 267 B ≈ **2.0 KB** | ＋ bytecode blob **6,078 B**（`qjsc_builtin_*`、rodata。ROM 版でも `js_bytecode_autoinit` から参照されリンクに残っている） |

推定（Xtensa は x86-64 とおおむね同じ桁、ビルド後に `idf.py size-files` で確認）: zip ＋ zipKeyed の C 本体 **4〜6 KB**（生成 1.5〜2.0、step 2.0〜2.5、keyed の差分 0.7、finalizer/mark 0.2）、fromAsync **3〜4 KB**（状態機械 1 本 ＋ 継続 ＋ class）。消える分 8.1 KB と相殺して**正味 −1〜+2 KB**。ROM データ: C 関数オブジェクト 3 × 48 B ＋ プロパティ 3 × (8 + 8 + 4) ＋ 名前の文字列 3 × 20 B ＋ 文字 21 B ≈ **290 B**、atom 5 個 ≈ 72 B（計算）。snapshot のヘッダ `quickjs-rom-intrinsics.h` は今 587 KB のソースで、数十行増える。

### 4.2 RAM（推定、実機レイアウト: JSValue 8 B・ポインタ 4 B・`JSObject` 48 B・tlsf 最小 12 B）

| 何 | 常駐 | 一時 |
|---|---:|---:|
| 使わないアプリ | **0**（ROM）。RAM 版は F2 で触るまで 0、触ったら ≈ 150 B/関数 | — |
| `Iterator.zip` の iterator 1 つ（2 本、shortest） | `JSObject` 48 ＋ レコード（`JSIteratorHelperData` 44 ＋ 拡張 12 ＋ 列 2 × 2 × 8 = 32）≈ 88 ＋ ヘッダ → **約 150 B** | 生成中の一時配列（2 × count × 8） |
| 同、`next()` 1 回 | — | 結果配列 48 ＋ 値 count × 8、`{value, done}` 48 ＋ prop ≈ **約 130 B**（呼び手が捨てれば消える） |
| zipKeyed（2 キー、longest） | ＋ pads 16 ＋ keys 16 → 約 180 B | 結果は null-proto オブジェクト 48 ＋ shape（キーごと）→ 配列より重い |
| `Array.fromAsync` 1 回（settle まで） | Promise 48 ＋ data ≈ 90、resolving funcs 2 × ≈ 70、状態 48 ＋ struct ≈ 100、継続 2 × (48 ＋ 24) ≈ 150、sync ならラッパー ≈ 70 → **約 0.6 KB** | await 1 回: ラッパー Promise ≈ 90（値が Promise なら 0）、reaction 2 × ≈ 40、ジョブ 8 ＋ 5 × 8 = 48 → **約 0.25 KB**、settle で消える |
| 比較: 今の JS 版（RAM ビルド）を初めて読む | **+7.3 KB（zip）/ +15.7 KB（3 つ）、実測(host, 64 bit)**。実機換算 約 4〜9 KB（推定） | コンパイルのピーク |

評価のピーク（CLAUDE.md、DERBY で約 28.9 KB の余裕）には関係しない: どれも呼ばれたときの確保で、ソースの解析には効かない。

### 4.3 速さ

zip の `next()` 1 回は、JS 版の bytecode 数十命令 ＋ `call` の C 往復 count 回が、C の直接呼び出し count 回になる。測っていない（使うアプリが無いので DERBY のターンにも乗らない）。実機の数字は実装後の `idf.py size` と、必要なら host の `--stats` の確保数で見る。

## 5. 検査

### 5.1 test262（`tools/vmtest/test262.py`、pinned `72faf8ec`）

対象: `built-ins/Array/fromAsync`（95 ファイル）、`built-ins/Iterator/zip`（38）、`built-ins/Iterator/zipKeyed`（44）。`feature` は `Array.fromAsync` / `joint-iteration` で、`SKIP_FEATURES` には無い。実行は

```bash
python3 tools/vmtest/test262.py -j 8 --variant asan built-ins/Array/fromAsync built-ins/Iterator/zip built-ins/Iterator/zipKeyed
python3 tools/vmtest/test262.py -j 8 --variant asan --vmrun-flag=--harden ...           # 「--vmrun-flag --harden」は argparse が option と読むので = で
python3 tools/vmtest/test262.py -j 8 --variant asan-rom --vmrun-flag=--harden ...
```

基準（実測(host)、§1.4）: **RAM 版 350/350**、**RAM `--harden` 314/350**、`--force-yield` でも同じ。C 版の受け入れは、この 2 つの集合に**一致**（RAM 版の pass 集合、harden の pass 集合）し、ROM 版 `--harden` が RAM `--harden` と**同じ 314**であること。harden の 36 件の内訳（実測(host)、各 sloppy/strict の 2 件ずつ）:

| 分類 | 件数 | 試験 | harden の規則 |
|---|---:|---|---|
| 属性（writable / configurable / extensible の検査） | 20 | fromAsync: `builtin` `length` `name` `prop-desc`、zip / zipKeyed: `length` `name` `prop-desc` | 3（書き換え不可で生まれる） |
| 組み込みの書き換え | 12 | fromAsync: `asyncitems-bigint / -boolean / -number / -symbol`（`Number.prototype.length = 2` など）、`asyncitems-uses-intrinsic-iterator-symbols`、`non-iterable-input-does-not-use-array-prototype`（`ArrayIteratorPrototype.next = ...`） | 3 |
| harness の `new Function` | 4 | zip / zipKeyed: `result-is-iterator`（`wellKnownIntrinsicObjects.js` 378 行が `new Function(...)` で intrinsic を引く） | 1（eval なし） |

**SUBSET への追加**: 3 ディレクトリは `test262.py` の `SUBSET` に無く、基準線にも無い（「350 newly passing (not in baseline)」）。実装のコミットで SUBSET に足し、基準線の行をそのコミットで足す（T0 の 2026-10-26 の追加と同じ扱い、`test262.py` 60〜63 行のコメント）。足さなければ、将来の退行を誰も見ない。

### 5.2 コーパス（`tools/vmtest/corpus/`、`run.sh`、asan と asan-rom の両方）

新しいファイルは先頭に `// vmrun-flags: --harden` を置き（`rom_immortal.js` と同じ）、RAM の harden 版と ROM 版で**同じ期待値**を使う。

- `iterator_zip.js`: 3 mode の基本、`Iterator.zip([])`、padding（iterator が短い・長い・途中で投げる）、`next` が投げたときに**投げた本人は閉じず他は逆順**に閉じるログ、strict の mismatch（i = 0 と i ≠ 0）と「全部同時に尽きる」、`return()` を new / suspend / 閉じている最中の再入 / complete の 4 状態で、`close` の例外が最初のものになること、結果配列が毎回別物、`Object.getPrototypeOf(it) === Object.getPrototypeOf([].values().map(x=>x))`、`typeof Iterator.zip.prototype === "undefined"`、`new Iterator.zip` が TypeError。
- `iterator_zip_keyed.js`: 文字列キーと Symbol キーの順、enumerable でないキー・値が `undefined` のキーを飛ばす、getter が後のキーを消す、padding はキーごとの Get、結果は null-proto で C/W/E。
- `array_fromasync.js`: 同期 iterable（値に thenable を混ぜる）、async generator、array-like（holes、Promise の要素）、`mapfn` 同期・async、`mapfn` が投げる → `return` が呼ばれて**その結果が settle してから** reject（ログの順）、`this` にコンストラクタ、`Array.fromAsync(null)` の reject、`next` の結果が object でないときに `return` を呼ばないこと、`length` の設定。`--budget-jobs 3` と `--force-yield` で出力が変わらないこと（await ごとがジョブ境界なので、`run.sh --budget-jobs 3` がそのまま検査になる）。
- `--fail-alloc` の掃引（`tools/vmtest/floor/sweep_fail_alloc.sh` の形）を新ファイルに掛け、LSan で漏れが無いこと。継続と状態の環（§2.2）が OOM の途中で切れても GC で回収されるかは、ここでしか見えない。

### 5.3 ROM の関所（rom-intrinsics §9 R2 の流用）

- `gen_rom_intrinsics.sh --check` が通る（表の再生成、`--stats` で C 関数 548 → 551、オブジェクト 613 → 616 になること。計算）。
- `rom_walk.sh`: 空の context の RAM は変わらない（増えるのは ROM だけ）。`vmrun-asan-rom --stats` で空スクリプトの `qjs_malloc_size` が今の 22,976 B（host 64 bit、実測）から動かないこと。
- N-R2b 同一性: `Iterator.zip === Iterator.zip`、`Object.getOwnPropertyNames(Iterator)` が RAM の harden 版と一致（今は `zip,zipKeyed` の 2 名が ROM 側に無い。§1.4）。
- `tools/vmtest/floor/r1_faults.sh` の全変種が従来どおり。

### 5.4 負の対照（`r1_faults.py` の形: 守りを 1 つ外した写しで、検査が落ちることを見せる）

| 対照 | 外すもの | 落ちるべき検査 |
|---|---|---|
| `zip-close-forward` | `close_all` の逆順を昇順に | コーパスの閉じる順のログ、test262 `iterator-zip-iteration-*-iterator-close-abrupt-completion` |
| `zip-close-thrower` | 投げた iterator を閉じない規則を外す | test262 `iterator-zip-iteration-iterator-step-value-abrupt-completion` |
| `zip-strict-skip` | strict の「残りの `next` を呼ぶ」を省く | `iterator-zip-iteration-strict-checks-remaining-done` |
| `zip-return-reenter` | suspended-start の「先に complete」を後にする | `suspended-start-iterator-close-calls-return` |
| `fromasync-close-no-await` | CLOSING で `return` の結果を待たず reject | コーパスの順序ログ（test262 は捉えていない。§1.3 の 7） |
| `fromasync-cont-throws` | 継続が例外を投げる経路を 1 つ残す | 「エラーが消える」= Promise が永遠に pending → `run.sh` の `exit=` と未処理 rejection の行 |
| `gen-keeps-dropped` | `dropped_value` を残したまま | `--check` が差分を出す、ROM 版で `typeof Iterator.zip === "undefined"` |

### 5.5 実機

使うアプリが無いので、実機の検査は **サイズと退行**: `idf.py -B build_xxx size-files` で `quickjs.c.obj` の増減（§4.1 の推定との照合）、`tools/check_flash.py`、`memlog.py --port --check`（`js=` は変わらないはず）、`smoke_device.py`、`test_app_resume.py`。動作は PLAYGROUND のソースに `Iterator.zip([[1,2],[3,4]]).toArray()` と `Array.fromAsync([1,2]).then(...)` を打つ手動確認か、`tools/vmtest/device_*.py` の形の 1 本（§6.3）。

## 6. 順序・リスク・決めること

### 6.1 順序

1. **zip / zipKeyed**（1 コミット）: atom 5 つ ＋ 表の再生成、kind ZIP、生成器の `dropped_value` から zip の 2 つを外す（fromAsync は一時的に残す: `dropped_value` を `JS_CLASS_ASYNC_FUNCTION` だけに絞る）、コーパス 2 本、test262 の SUBSET 追加。関所: §5.1〜5.4。
2. **fromAsync**（1 コミット）: 新 class、状態機械、継続、コーパス 1 本、`JS_AUTOINIT_ID_BYTECODE` と `.h` の撤去。関所: 同じ ＋ `--budget-jobs` / `--force-yield` / `--fail-alloc`。
3. 実機: サイズと退行（§5.5）。

zip を先にする理由は主線の提案どおりで、確かめた上で支持する: (i) 同期なので関所が test262 とコーパスの出力一致だけで閉じる、(ii) 新しい機構が「kind 1 つ ＋ 列の mark/free」だけで、R1/R2 の守りに新しい穴を開けない、(iii) atom の追加と表の再生成、生成器の変更、SUBSET の追加という**手順の整備**を小さい方で済ませ、fromAsync は中身だけに集中できる。逆順（fromAsync 先）にする利点は見つからない。

### 6.2 リスク

- **fromAsync の継続が投げる経路を 1 つでも残すと、Promise が永遠に pending になる**（cap が `undefined` なので `promise_reaction_tail` が何もしない）。すべての失敗を `reject` に落とす規律が要る。負の対照 `fromasync-cont-throws` で検出できることを先に見せる。
- 状態オブジェクトと継続の環は、settle で解く。OOM で settle に至らないと GC が拾う前提で、`--fail-alloc` の掃引で漏れ（LSan）を見る。
- zip の `JSIteratorHelperData` 拡張は「先頭が同じ構造体」という C の約束に乗る。`offsetof(JSIteratorZipData, h) == 0` を `_Static_assert` で固定する。
- atom を足すと `JS_ATOM_END` が動き、ROM atom 表・intrinsics 表の**両方**を再生成する。片方だけだと `_Static_assert` で止まる（意図どおり）。
- harden の 36 件は「規則どおりの失敗」として受け入れる前提（§5.1）。受け入れないなら harden 側の話で、この文書の範囲外。
- 上流 quickjs-ng が将来この 3 つを C 化すれば差分が小さくなるが、今は JS のまま。vendored の差分が増える。
- flash の推定は host の関数サイズからの類推で、Xtensa の実測は実装後にしか出ない。正味がプラスなら `tools/check_flash.py` の線を見る。

### 6.3 決めること（発注者へ）

1. **5 atom を predefined に足す**（推奨、ROM と同じ「確保 0」）か、実行時に `JS_NewAtom` で作るか。前者は 2 つの表の再生成を伴う。
2. **上流の 3 つの `.h`（bytecode）を削除する**か、参照されない状態で残すか。削除すれば flash −6.1 KB（RAM 版・ROM 版とも）。上流との差分管理の都合で決める。
3. **fromAsync を本当に入れるか。** 使うアプリは無く、3〜4 KB の C と新 class が増える。zip だけで止める選択もある（RD5 の「無くて驚かない」をもう一度適用）。入れるなら §2.2 の形で。
4. **test262 の SUBSET に 3 ディレクトリを足し、基準線に 350 行を足す**ことの承認（基準線は L0 で一度だけ記録する方針で、追加はコミット単位の例外として扱ってきた）。
5. **harden の下での 36 件の失敗を「規則どおり」として記録に残す**ことの確認。
6. 実機での動作確認の形: PLAYGROUND での手動確認で足りるか、`device_*.py` を 1 本足すか。
7. fromAsync の継続オブジェクトを `JS_NewCFunctionData`（簡単、＋24 B × 2）にするか、async 関数と同じ専用 class（`call` フック、−48 B/呼び出し、推定）にするか。最初は前者でよい。

## 7. 参照

- 上流: quickjs-ng v0.14.0 `builtin-iterator-zip.js` / `builtin-iterator-zip-keyed.js` / `builtin-array-fromasync.js`（本文は上流から取得。このリポジトリには bytecode の `.h` のみ）。
- 仕様: ECMA-262 sec-array.fromasync、proposal-joint-iteration（`Iterator.zip` / `zipKeyed`、`IteratorZip`、`IteratorCloseAll`、`GetIteratorFlattenable`）。test262 の `info:` に手順が引用されている（`iterator-zip-iteration-strict-checks-remaining-done.js`、`suspended-start-iterator-close-calls-return.js`、`results-object-has-default-attributes.js`）。
- このリポジトリ: [rom-intrinsics.md](rom-intrinsics.md) RD2・RD5・§4.9・§6.5・§12、[harden.md](harden.md) §1・§3.6、`tools/vmtest/floor/gen_rom_intrinsics.c`、`tools/vmtest/floor/r1_faults.sh`。

## 8. 再現（WSL、実測(host) の手順）

```bash
bash tools/vmtest/build.sh asan && bash tools/vmtest/build.sh asan-rom && bash tools/vmtest/build.sh o2
D="built-ins/Array/fromAsync built-ins/Iterator/zip built-ins/Iterator/zipKeyed"
python3 tools/vmtest/test262.py -j 8 --variant asan $D                            # 350 / 0
python3 tools/vmtest/test262.py -j 8 --variant asan --vmrun-flag=--harden $D      # 314 / 36
python3 tools/vmtest/test262.py -j 8 --variant asan-rom --vmrun-flag=--harden $D  # 10 / 340
echo 'var x = Iterator.zip' > /tmp/t.js; .cache/vmtest/vmrun-asan --profile device --harden --stats /tmp/t.js   # qjs_malloc_size
nm --size-sort -S .cache/vmtest/obj-o2/quickjs.o | grep -E 'js_iterator_concat|js_bytecode|qjsc_builtin'  # §4.1 の表
```

`.cache/test262` はルートの checkout を worktree へ symlink した（`test262.py` は `ROOT/.cache/test262` 固定）。

## 9. 実装の記録（2026-10-27）

決めたこと（§6.3 への答え）: 5 atom は predefined に足す。上流の bytecode の `.h` 3 本は削除。fromAsync まで入れる。test262 の 3 ディレクトリを SUBSET と基準線に足す。harden の 36 件は規則どおりとして受け入れる。継続は `JS_NewCFunctionData`（§6.3 の 7 の簡単な方）。

### 9.1 何をしたか

- **zip / zipKeyed**: §2.1 のとおり `JS_CLASS_ITERATOR_HELPER` の kind `ZIP`。レコードは `JSIteratorHelperData` を先頭に、列（iters・nexts・pads・keys）を後ろに置く。`js_iterator_helper_next` は実行中・完了の検査の後で zip を `js_iterator_zip_step` に渡す。
  - 上流の JS から変えた点（仕様に合わせた）: `next()` が例外で抜けたら完了にする（上流は「実行中」のまま固まり、以後の呼び出しが TypeError）。`Symbol.iterator` の結果が object でなければ TypeError。`done` が真のとき `value` を読まない。
- **fromAsync**: §2.2 の状態機械。**新しい class は足していない**。状態は fulfilled の継続（`JS_CLASS_C_FUNCTION_DATA`）の `data[]` に置き、rejected の継続はその関数を `data[0]` に持つ。await は `js_promise_resolve` ＋ `perform_promise_then(.., {undefined, undefined})`。
- **closeOnRejection**（ES2025 の AsyncFromSyncIteratorContinuation）: 同期の iterable の値が reject されたら同期 iterator を閉じる。quickjs-ng のラッパーには無かったので足したが、**fromAsync が作るラッパーだけで有効**にした。`for await` は同じ場面でループの側から閉じるので、両方で有効にすると `return()` が 2 回呼ばれる（`yield_async_from_sync` が捉えた）。
- 撤去: `JS_AUTOINIT_ID_BYTECODE`、`js_bytecode_autoinit`・`js_bytecode_eval` と補助の C 関数 3 つ、`builtin-*.h` 3 本、`JS_DefineAutoInitProperty` の 3 呼び出し。生成器の `dropped_value` は何も落とさなくなり、bytecode 関数に出会ったら FAIL する。F1 の生成器の bytecode の除外も消した。
- ROM の表: オブジェクト 613 → 616、ROM atom 625 → 630。

### 9.2 結果（実測(host)）

| 検査 | 結果 |
|---|---|
| test262 の 3 ディレクトリ（350 項目）、RAM | 350 / 350（上流の JS 版と同じ集合） |
| 同、RAM `--harden` | 314（落ちる 36 は §5.1 の内訳と同じ） |
| 同、ROM `--harden` | 314（RAM `--harden` と一致。前は「関数が無い」で 10） |
| コーパス（`iterator_zip`・`array_fromasync` を足して 87 本）、素・force-yield・budget-jobs 3 | 全通過。2 本の出力は asan-rom でも同じ |
| `lazy_builtins` | `Array` のキーの並びが `...from,of,prototype,fromAsync` → `...from,fromAsync,of,prototype`（関数表に入ったため）。期待値を更新 |

**`--fail-alloc` の 3 本**（`oom_resolving_functions`・`_module`・`oom_with_resolvers`）は、atom を 5 つ足したことで狙いの確保が 5〜7 回ずれた。出力は同じでも狙いを外すので、**修正を戻したエンジン（ASan）で報告が出る番号を探し直した**（550・487・572、`vmrun-rom-lb-li-flags` の行。桁を変えると源の長さが変わり、それだけで番号が動く）。今後のために `tools/vmtest/oom_targets.sh` を足した: 修正を戻した写しを作り、各試験の番号で ASan が実際に報告することを確かめる。
