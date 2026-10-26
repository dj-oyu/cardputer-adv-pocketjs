# harden: アプリのコンテキストから「驚き」を消す（2026-10-26）

**状態: 実装済み（`vm/harden`）。host の検査と、実機の全 JS アプリの起動は通過（§6、§7）。速さは未測定。** 数字の区分は、実測（host または実機で測った値）、推定、コード（ソースで確かめた事実）の3つ。

## 1. 結論

アプリのコンテキストは、作った直後に `JS_PocketHarden()` を通る（`components/pocketjs_guest/src/guest.c`）。以後、次の4つが成り立つ。

| # | 規則 | 違反したとき |
|---|---|---|
| 1 | 文字列からコードを作らない: `eval`（直接・間接）、`Function` / `AsyncFunction` / `GeneratorFunction` / `AsyncGeneratorFunction` のコンストラクタ | `EvalError: code generation from strings is disabled` |
| 2 | 作った後の [[Prototype]] を変えない: `Object.setPrototypeOf`、`Reflect.setPrototypeOf`、`__proto__` への代入 | `TypeError: changing an object's prototype is disabled` |
| 3 | 組み込み（intrinsics）は生まれたときから書き換え不可: プロパティは書き換え不可・削除不可、オブジェクトは追加不可。組み込みのグローバル名（`Math`、`Array` …）も書き換え不可 | `TypeError`（strict でなくても投げる） |
| 4 | species を見ない: `map` `filter` `slice` `splice` `concat` `flat`、`then` `finally`、型付き配列・`ArrayBuffer`・`RegExp` の結果は、常に本物の組み込みで作る | 起きない（`constructor` を読まなくなる） |

残るもの（禁止しない）:

- 生成時に prototype を決める書き方: `{__proto__: p}`、`Object.create(p)`、`class ... extends`。
- 自分のオブジェクトの書き換え。`F.prototype.m = ...` も、`F.prototype` はアプリのオブジェクトなので書ける。
- 組み込みと同じ名前を、自分のオブジェクトに付けること（`o.toString = f`）。§3.3。

ヒープの代償は、新しいコンテキストで +424 B、組み込みを一通り触ったスクリプトでピーク +271 B（実測(host)、実機レイアウト、§6.2）。test262 は harden を掛けない通常のコンテキストで退行 0（§6.1）。

## 2. なぜ

どれも、アプリにとって利点が無く、コードに驚きを足し、VM の最適化を妨げる。

- **eval / Function**: ネットから来た文字列がコードになる経路。76 ファイルのアプリのどこにも無い（コメントを除く）。
- **prototype の付け替え・組み込みの書き換え**: アプリは一度も書いていない（`__proto__`・`setPrototypeOf`・`Object.create`・`class` すべて 0 件）。書き換えられうる限り、VM は「`Math.max` は本物か」「`Array.prototype` に添字のプロパティが無いか」（`std_array_prototype`）を毎回確かめるか、変化を見張る状態を持つ必要がある。書き換え不可なら、どちらも要らない。T1 で組み込みの呼び出しを特殊化するときの前提になる（[interpreter-stages.md](interpreter-stages.md)）。
- **species**: `arr.constructor = X` だけで、以後の `arr.map()` が X を呼ぶ（`JS_ArraySpeciesCreate`）。アプリが呼んでいない関数を組み込みが勝手に呼ぶ経路で、しかも `map` などは毎回 `constructor` と `Symbol.species` の2回の検索を払う。サブクラスの結果をサブクラスにするための仕組みだが、アプリはサブクラスを作らない。

`.constructor` の書き換えは、`instanceof`（prototype の鎖を見る）を壊さない。species が消えれば、`.constructor` は読む人を騙すだけの値になる。

## 3. 設計

### 3.1 命令番号ではなく属性で止める

「ある範囲の命令は書き換えに使えない」という形では止められない。守る対象は命令ではなくオブジェクトで、書き込みは命令（`put_field`・`put_array_el`・`define_field`）だけでなく、組み込み関数（`Object.defineProperty`・`Object.assign`・`Reflect.set`・`Array.prototype.push.call(Array.prototype, x)`）からも来る。しかも同じ `put_field` が、普段はアプリのオブジェクトに、ときどき `Array.prototype` に書く。

そこで、JS がもともと持つ属性を使う。プロパティの「書き換え可」「削除可」、オブジェクトの「追加可」は、すべての書き込みの経路が**すでに**確かめている。組み込みをこれらを外した状態にすれば、新しい確認は1つも増えず、漏れる経路も無い。

### 3.2 凍結せず、生まれたときから書き換え不可にする

`Object.freeze` を全組み込みに掛けると、遅延の組み込み（F2: 関数リストは最初に触るまで shape に入らない。F3: 型付き配列・Map/Set などは最初に使うときに作る。[builtin-floor-plan.md](builtin-floor-plan.md)）を全部作ってしまう。だから「今あるものは書き換え不可にし、まだ無いものは書き換え不可で生まれる」ようにする。

- **今あるもの**（`JS_PocketHarden`）: グローバルの組み込みの束縛と、`ctx->class_proto[]`・`native_error_proto[]`・`function_proto` などから届くオブジェクトを、shape にある値・アクセサ・prototype をたどって鍵をかける（`js_harden_lock`）。グローバルオブジェクト自体は追加可のまま（ファームとアプリが足す）。
- **後で生まれるもの**: 3箇所。
  - `lazy_define`: 鍵のかかったオブジェクトの関数リストの項目は、書き換え不可で shape に入る。
  - `JS_AutoInitProperty`: 鍵のかかったオブジェクトが作って渡すもの（`Math` のメソッドの関数オブジェクトなど）は、鍵をかけて渡す。グローバルは鍵をかけないので、「`JS_PocketHarden` が書き換え不可にした束縛」かどうかを属性で見分ける。ファームが後で足す束縛は書き換え可なので、鍵はかからない（`pocket` の名前空間に C から足す処理を壊さない）。
  - `js_lazy_make`（F3）: 後で作るクラスのコンストラクタと prototype に鍵をかける。
- **鍵の印**: 「このオブジェクトは組み込みで、渡すものも鍵付きで生まれる」を、GC ヘッダの空きビット（`dummy0` の bit 1）に持つ。F2 の遅延の印（bit 0）と同じ方法で、ランタイムの生成時に、どちらの見方からも空いていることを確かめてから使う（`rt->lock_ok`。確かめられなければ `JS_PocketHarden` は失敗し、アプリは起動しない）。**この印は書き込みの経路では読まない。** 読むのは上の3箇所、つまり組み込みを作る瞬間だけ。追加不可かどうかで組み込みを見分けられないのは、F2 が追加不可のオブジェクトでも項目を作るため（`lazy_define` の注記）。

### 3.3 「override mistake」を緩める

仕様では、prototype の鎖の上に書き換え不可のデータプロパティがあると、受け手のオブジェクトに同じ名前を付ける代入も失敗する。組み込みを書き換え不可にすると、`o.toString = f` が失敗するようになり、それ自体が驚きになる。ファームにも実例がある: `pocket_api.c` の `error_prototype()` は、`Error.prototype` を継いだオブジェクトに `name` を代入する。

harden では、鎖の上の書き換え不可は代入を止めない（`JS_SetPropertyInternal2`、1箇所）。受け手自身が書き換え不可・追加不可なら、今までどおり失敗する。値を読まないので、遅延の項目を作らずに済む。

### 3.4 失敗した書き込みは必ず投げる

strict でないコードでは、書き換え不可への代入・追加不可への追加・削除不可の削除が黙って失敗する。それも驚きなので、harden では strict と同じく投げる（`js_write_fail_throws`、`JS_PROP_THROW_STRICT` を見る5箇所）。strict の他の性質（宣言の無い代入、`this`、`arguments`）は変えない。アプリを丸ごと strict にするかは別の判断（§8）。

### 3.5 shape を共有したまま鍵をかける

C の関数は、ほとんどが同じ shape（`length`・`name`）を共有している。オブジェクトごとに属性を書き換えると、その数だけ shape の複製ができる。新しいコンテキストで鍵をかける 73 個のうち 25 個が共有の shape で、複製で約 2 KB 増えた（実測(host)、実機レイアウト）。

そこで、共有の shape には「鍵のかかった対の shape」を1つだけ作り、ハッシュ表に入れて使い回す（`js_harden_share_shape`、対応表は `JSContext` に8組）。共有の shape はハッシュ表に入っていることを `add_property` が前提にしている（`assert`）ので、対の shape もハッシュを計算し直して入れる。属性の変換は決まった変換なので、元の shape を共有していたもの同士が対の shape を共有しても正しい。

### 3.6 species と eval の入口

- species を読むのは `JS_SpeciesConstructor` と `JS_ArraySpeciesCreate` の2箇所だけで、harden ではどちらも既定のコンストラクタを返す。`Promise.prototype.finally` は既定値に `JS_UNDEFINED` を渡していて、harden では本物の `Promise` が要るので、harden のときだけ `ctx->promise_ctor` を渡す（通常のコンテキストは上流と同一）。
- 直接 eval・間接 eval・`Function` のコンストラクタは、3つとも `JS_EvalObject` を通る。アプリの読み込み（C からの `JS_Eval`）は通らない。だから止めるのはこの1箇所。

## 4. 変更したファイル

| ファイル | 内容 |
|---|---|
| `components/quickjs-ng/quickjs-ng/quickjs.c` | 上記すべて。`ctx->harden` が立っていなければ、通常のコンテキストの動作は変わらない |
| `components/quickjs-ng/quickjs-ng/quickjs.h` | `JS_PocketHarden()` |
| `components/pocketjs_guest/src/guest.c` | コンテキストを作った直後、ヘルパーとファームの面より前に呼ぶ。失敗したらゲストを作らない |
| `tools/vmtest/vmrun.c` | `--harden` |
| `tools/test_stress_app.c`、`tools/test_lessons.c`、`tools/games/test_{derby,lcd_catch,big_wave}_host.c` | 実機と同じく harden を掛ける |
| `tools/vmtest/corpus/harden.js` | 4つの規則、残るもの、遅延の経路 |

Kconfig は足していない。この挙動は無くしたいもので、切り替えの口を残す理由が無い。host では `vmrun --harden` の有無で比べられる。

## 5. 互換

- ECMAScript からは外れる。harden のコンテキストで test262 を走らせると、eval・prototype の付け替え・組み込みの書き換え・species を使う試験が落ちる。だから test262 と既存のコーパスは harden を掛けずに走らせ、harden は `harden.js` と、アプリのハーネスで確かめる。
- 既存のコーパスを `--harden` で走らせると、84 件のうち 13 件が変わる。どれも規則どおり: eval 8件（closures、coro_closure_gc、g5_gaps、l2b_flat_calls、seg_boundary_bigframe、special_calls、tco_guards、try_finally）、組み込みの書き換え・prototype の付け替え 4件（builtin_reentry、lazy_builtins、lazy_intrinsics、math_fast_call）、species 1件（yield_job_tails）。対の shape を使い回す前は、`--fail-alloc` の確保番号がずれた 3件（oom_*）も変わっていた。

## 6. 検証（host）

### 6.1 検査

- コーパス 84 件（`harden.js` を含む）: asan、`--force-yield`、`--budget-jobs 3` で通過。
- test262（harden なし）: 21,671 pass / 247 fail、退行 0。`--force-yield` も同じ。
- アプリのハーネス（harden あり、asan）: STRESS、LCD CATCH、BIG WAVE、DERBY（オッズの較正、500 seed のコーナー）すべて PASS。
- `harden.js` の負の対照: 4つの規則が外れていれば、それぞれの行が `ok` に変わる（実装中に、`Promise.prototype.finally` の既定値の誤りをこのファイルとコーパスで見つけた）。

### 6.2 ヒープ（実測(host)、i386 の実機レイアウト、`vmrun --profile device --stats`）

| スクリプト | harden なし | harden あり | 差 |
|---|---:|---:|---:|
| 空 | 25,812 | 26,236 | +424 |
| 組み込みを一通り触る（`map`・`Map`・`Uint8Array`・`padEnd`・`JSON`・`Promise`） | 30,581 | 30,852 | +271 |

ピークのバイト数。「harden なし」は `JSContext` の対応表（+64 B）を含む。対の shape を使い回す前は、それぞれ +2,120 B / +3,036 B だった。

## 7. 実機（2026-10-26、`80ce1f69`）

broker でビルドと書き込み。テストは COM3 から直接（リースを持ったまま）。

| アプリ | 確かめ方 | 結果 |
|---|---|---|
| HELLO | `kasane_hello_device_test.py --cycles 3`（broker の hello も） | PASS |
| DERBY WATCH | broker の `derby`（デモ1周、写真判定、HOME に戻る）、`derby_fixed`（3レース） | 1周を観測、3レース完走、エラーなし |
| STRESS TEST | `stress_app.py --seconds 60` | PASS（OOM の疑い 0） |
| IMU CAL・POCKET PET・PET COMPANION | `test_app_resume.py`（中断→再開→退去） | PASS |
| MEGADEMO・GRID LAB・VIDEO LAB・LCD CATCH・BIG WAVE | 行を順に起動して 8 秒（`EVAL_ERROR`・`START_FAILED`・例外の行を探す） | 例外・起動失敗なし |
| TUTORIAL・PLAYGROUND の既定ソース | host の `test_lessons.c` を harden 付きで（教材に eval・prototype の書き換えは無い） | PASS（host） |

ヒープ: DERBY の読み込み直後のゲスト `js=116,072`、空き 45,780 B（実測(device)）。前回の実機（`9433aba9`、harden なし）は 115,864 / 45,920 で、差は +208 B / −140 B。間に PR #80・#81 も入っているので、すべてが harden の分ではない。host の予想（+271〜424 B）と同じ桁。

`tools/smoke_device.py` は失敗したが、harden とは無関係: 今のメニューは HELLO の行まで `u` を押す必要があり（`kasane_hello_device_test.py` はそうしている）、この古いスクリプトは別のアプリを開いて `HELLO_COUNT` を待っていた。

速さは測っていない。書き込みの経路に確認を足していないので変わらない見込みで、species の検索2回が消える分だけ速くなる（推定）。

## 8. この先

- **アプリを strict にする**: 宣言の無い代入（タイプミスがグローバルを作る）、`with`、`arguments` の別名付けも、驚きを足して最適化を妨げる同じ種類のもの。アプリが strict で動くかは未確認。
- **組み込みを flash に置く**: 書き換え不可になった組み込みは RAM に置く必要が無い。別の検討（[rom-intrinsics.md](rom-intrinsics.md)、検討中）。
- **T1 の組み込みの呼び出しの特殊化**: 組み込みが差し替えられないので、呼び先の検査が要らなくなる。
