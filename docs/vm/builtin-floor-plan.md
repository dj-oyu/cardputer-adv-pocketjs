# 起動床の削減計画: 組み込みの名前と索引を flash に置く

`vm/main` 上の新しい線（**F 系列**、§1.3）。VM の段（L0〜L5、[仕様](quickjs-freertos-vm-spec.md) §4）とは
独立で、対象は**ゲストが JS ソースを 1 バイトも読む前に払うヒープ**である。計測は
`tools/vmtest/floor/`（§9）で再現できる。数値はすべて **実測(host, 実機レイアウト)** / **実測(host)** /
**計算** / **推定** のどれかを付ける。**F1・F2 とも実装・実機確認まで済み、既定 y**（§12・§13）。アプリごとの `js=` は F 系列の前から実測(device)で
35.6〜38.9 KB 減（hello 85,988 → 50,412）。

## 1. 目的と結論

### 1.1 問題

アプリを起動するたびに `JSRuntime` と `JS_NewContext`（全 intrinsic、`components/pocketjs_guest/src/guest.c:612`）を
新しく作る。出荷アプリの `js=` は 93〜117 KB で、その大半がこの**床**である（hello はソース 414 B で
`js=93,310`、実測(device)）。[common-api.md](../api/common-api.md) §7.1 の「床 ＋ ソース 1 バイトあたり約 4.5 B」の
床のほう。ゲスト上限 160 KiB（`main/app_session.c:756`）のうち床が 6 割近くを占め、capability を足すたびに
ゲストの部屋が減る（CLAUDE.md）。

### 1.2 結論（先に数字）

実機レイアウト（`-m32 -malign-double`、JSValue 8 B・ポインタ 4 B）で、実機ゲストと同じ課金
（tlsf ブロック長 = 4 B 切り上げ・最小 12 B、`js=` が数えるもの）で測った床は
**js=64,420 B / 1,020 ブロック**（実測(host, 実機レイアウト)）。tlsf のブロックヘッダ 4 B × 1,020 ≈ 4 KB は
`js=` に見えないぶん別にある（計算）。

採る設計は 2 段（§5）:

| 段 | 何を flash へ | 床の減り（計算、実機レイアウト） |
| --- | --- | --- |
| F1: ROM atom | 組み込みの名前（文字列 atom 543 個）を flash の記録にする | −21,720 B |
| F2: 組み込みの索引を持たない | 関数リスト由来のプロパティ（AUTOINIT 407・GETSET 50）を最初に触るまで shape・prop 配列に載せない | −10,368 B（slot）−3,648 B（accessor 関数オブジェクト 57 個） |
| 合計 | | **64,420 → 28,684 B（−35,736 B、−55%）** |

保守的な見積りである: 値プロパティ（`Math.PI`、関数の `length`/`name`）は残す前提で数え、`atom_array`/`atom_hash`
（4,892 B）の縮小は数えていない。**すべてのアプリの床が減る**（TypedArray や Date を使うアプリも含む）。
**減らないもの**: アプリ自身が作るオブジェクトと atom、ソース解析の 4.5 B/byte、および床のうち
ファーム側の面（§2.3 の約 13 KB、未計算）。

実行時の代償（§7）: 組み込みオブジェクトでの own-property の**外れ**が表探索になる。出荷アプリで最悪の
imucal が約 53 回/フレーム（実測(host)、外れの回数）。1 回 0.1〜1 µs（**推定、未計測**）なら ≈53 µs/フレーム、
imucal の JS ターン 4.4 ms（[kasane-text-damage.md](../perf/kasane-text-damage.md) §7、実測(device)）の ≈1.2%。

### 1.3 線の名前 — なぜ L6 ではないか

仕様 §4 の L0〜L5 は「VM スタックをどこに置き、どう動かすか」の梯子で、番号が前提関係を表す
（L3 は L2 の上、L4 は L3 の後）。床の削減はその梯子に前提を持たず、L2 の結果（`vm-L2`）の上で
どの段とも独立に進められる。L5（研究）の次に置くと「L5 の後」と読めてしまうので、**別の系列 F
（floor）** とし、F0（計測、この文書）→ F1 → F2 と番号を振る。決定表は **FD1〜** で、L2/L3 の D1〜D59 と
衝突しない。ブランチは [vm-branching.md](vm-branching.md) のとおり `vm/f1-rom-atoms` のように切り、
関所を通したら `vm/main` へ `--no-ff` で戻してタグ `vm-F1` を打つ。

## 2. 床の内訳

`tools/vmtest/floor/floor32.sh` の出力（実測(host, 実機レイアウト)、2026-09-25、`vm/main` 29f6f35）:

```
sizeof: JSValue=8 ptr=4
runtime        js= 16176 blocks= 235 hdr=  940 | obj   0 prop    0 shape   0 atom 230 atomB 13515
full_context   js= 48244 blocks= 785 hdr= 3140 | obj 175 prop  965 shape 104 atom 328 atomB 12949
TOTAL_FLOOR js=64420
raw+Base       js= 21548 blocks= 342 hdr= 1368 | obj  68 prop  415 shape  38 atom 165 atomB  6406
TypedArrays    js= 10456 blocks= 187 hdr=  748 | obj  53 prop  233 shape  32 atom  49 atomB  1922
MapSet         js=  3228 blocks=  53 hdr=  212 | obj  16 prop   82 shape  10 atom  11 atomB   444
Date           js=  3240 blocks=  53 hdr=  212 | obj   3 prop   57 shape   2 atom  45 atomB  1787
DOMException   js=  3216 blocks=  41 hdr=  164 | obj   5 prop   65 shape   2 atom  29 atomB  1284
RegExp         js=  2632 blocks=  51 hdr=  204 | obj  14 prop   50 shape   5 atom  18 atomB   701
Promise        js=  1960 blocks=  34 hdr=  136 | obj  10 prop   40 shape   9 atom   5 atomB   178
WeakRef        js=   772 blocks=  15 hdr=   60 | obj   4 prop   15 shape   4 atom   3 atomB   110
Proxy          js=   200 blocks=   4 hdr=   16 | obj   1 prop    4 shape   1 atom   1 atomB    38
JSON           js=     0
Eval           js=     0
RegExpCompiler js=     0
BigInt         js=   384 blocks=   6 hdr=   24 | obj   2 prop    9 shape   2 atom   0 atomB     0
```

- **runtime 16,176 B のうち atom の文字列が 13,515 B**（`atomB` は `JS_ComputeMemoryUsage` の `atom_size`。
  `atom_array`/`atom_hash` の表を含む）。`JS_NewRuntime` はオブジェクトを 1 つも作らず、予定義 atom
  229 個（`quickjs-atom.h` の `DEF` 行数）の `JSString` を作る。
- **context 48,244 B** は 175 オブジェクト・965 プロパティ・104 shape・328 atom。`raw+Base`
  （`JS_NewContextRaw` + `JS_AddIntrinsicBaseObjects`）21,548 B が最大で、TypedArrays 10,456 B が次。
- 各 intrinsic の行は Base の上に**単独で**足したときの増分（計算の前提: 順序依存の共有 atom は
  最初に足したものに付く）。

### 2.1 何が床を作っているか（heap walk）

`tools/vmtest/floor/lazyfloor.sh`（`quickjs.c` を `#include` して `rt->atom_array` と `rt->gc_obj_list` を
中から歩く。実測(host, 実機レイアウト)）:

```
floor_js=64420 blocks=1020
walked: atoms_str=21720 (n=543, symbols kept 14) atom_tables=4892 objects=8400 (n=175)
        prop_arrays=8464 own_shapes=16432 -> sum=59908 (93% of floor; rest = shared shapes,
        other gc objects n=1, misc)
props: autoinit=407 getset=50 plain=508; accessor fn objects=57 (~64 B each)
SAVE flash_atoms=21720 lazy_slots=10368 lazy_accessor_fns=3648 total=35736
FLOOR now=28684 (after)
```

| 項目 | B | 備考 |
| --- | --- | --- |
| 文字列 atom 543 個 | 21,720 | 全部が組み込みの名前（アプリはまだ無い）。1 個 = `tl(20 + len + 1)` |
| symbol atom 14 個 | （据え置き） | 予定義 well-known symbol。値としての同一性が要る |
| `atom_array` + `atom_hash` | 4,892 | 縮小は F の数字に入れていない |
| `JSObject` 175 個 | 8,400 | 48 B/個 |
| prop 配列 | 8,464 | `JSProperty` 8 B × `prop_size`（成長規則で余りを持つ） |
| 専有 shape（ref_count 1） | 16,432 | `get_shape_size` = hash×4 + `sizeof(JSShape)` + `prop_size`×8 |
| 歩いた合計 | 59,908 | 床の 93%。残りは共有 shape・その他 |

**プロパティ 965 個の内訳: AUTOINIT 407、GETSET 50、値 508。** AUTOINIT は「初めて読まれたら関数
オブジェクトを作る」印だけで、関数本体はまだ無い（§3）。つまり床のうち shape と prop 配列は、**まだ何も
作られていないものの索引**である。

### 2.2 実機の床との差

`js=` の床は文書上 **約 77.3 KiB**（common-api §7.1、deskclock `js=80,653` からの逆算、実測(device)）で、
ここで測った 64,420 B とは **約 13 KB** ずれる。差はファームが `JS_NewContext` の後に注入するもの
（`console`、`pocket` の骨組みと `capabilities`、quickjs-libc の helper、`pocketjs_guest_quickjs_install_once`）と
考えられるが、**未計算**である。この文書の削減額はエンジン側の床（64,420 B）に対するもので、ファーム側の
13 KB には効かない。F1 で組み込み名を ROM にすると、ファームが `JS_NewCFunction` 等で足す名前のうち
ROM に載っていないもの（`pocket`、`capabilities`、…）は今までどおりヒープ atom になる。ファームの面の
名前も ROM 表に含めるかは F1 の実装時に決める（表は生成物なので追加は容易。FD5）。

## 3. データはどこにあるか（コードで確認したこと）

`components/quickjs-ng/quickjs-ng/quickjs.c` の行番号は `vm/main` 29f6f35 時点。

- **組み込みの定義は `static const JSCFunctionListEntry` の表**（例 `js_typed_array_base_proto_funcs`
  65218 行）と予定義 atom の名前列 `js_atom_init`（1305 行、`quickjs-atom.h` の 229 個）。すべて `const` →
  `.rodata` → **flash**。ESP32-S3 では cache 経由でメモリマップされるので、読むためのコピーは要らない。
- **runtime 生成時**、`JS_InitAtoms`（3473 行）→ `__JS_NewAtomInit`（3746 行）が名前ごとにヒープの
  `JSString` を確保して文字を memcpy する。`struct JSString`（752 行）のヘッダは 32-bit で 20 B
  （ref_count、len/wide、hash/kind/atom_type、hash_next、first_weak_ref）。"subarray"（8 文字）は
  `tl(20+8+1)` = 32 B のヒープ。
- **メソッドの関数オブジェクトは既に遅延**: `JS_InstantiateFunctionListItem`（43798 行）は `JS_DEF_CFUNC` を
  `JS_PROP_AUTOINIT` のプロパティとして表の項目を指すだけにし、関数オブジェクトは最初の読みで作る。
  getter/setter（`JS_DEF_CGETSET`、43842 行）は即時に作る（上流のコメント "XXX: use autoinit again ?"）。
  したがってヒープに在るのは**索引**（オブジェクト・shape・prop 配列）と**名前**（atom 文字列）である。
- **const atom は既に不死**: index < `JS_ATOM_END` を `__JS_AtomIsConst`（3266 行）が判定し、`JS_DupAtom`/
  `__JS_FreeAtom` が参照数を触らない。`rt->atom_array[` の参照は 33 箇所（§11）。
- **bytecode を flash から実行する道は無い**: `quickjs.h:1342` の `JS_READ_OBJ_ROM_DATA` は
  「obsolete, broken by ICs」で 0。§4-B。

## 4. 検討して捨てた案

| 案 | 内容 | 捨てた理由 |
| --- | --- | --- |
| **A. intrinsic を群ごとに遅延** | TypedArrays・Date・Map/Set・DOMException・WeakRef・BigInt・Proxy を最初に触るまで作らない | 出荷アプリはどれも使わない（`pocket.fs/io/net/capture` が返す ArrayBuffer だけがネイティブ側で作られる）ので **約 21.5 KB**（計算、§2 の合計）減るが、使うアプリには 0。§5 の設計は群単位でなく名前・プロパティ単位で同じ節約を全アプリに与え、A を包含する。**F2 の後で群遅延を重ねる価値があるかは、F2 の実測で決める**（残る値プロパティ・オブジェクト本体 8,400 B の一部が対象） |
| **B. bytecode の ROM 実行** | 組み込みや事前コンパイル済みアプリを flash の bytecode から直接動かす | `JS_READ_OBJ_ROM_DATA` が「IC で壊れた」として 0 固定。インラインキャッシュが bytecode に書き込む設計なので、この engine では in-place 実行できない。事前コンパイルが省けるのは解析の一時領域だけで、床には効かない |
| **C. 名前を flash に置いたまま書き込みを握りつぶす** | `JSString` を `.rodata` に置き、ref_count の増減が flash への書き込みになっても無視されることに賭ける | ESP-IDF の cache エラーハンドラ（`components/esp_system/port/soc/esp32s3/cache_err_int.c`）は "Dbus write to cache rejected" と "Write back error occurred while dcache tries to write back to flash" を panic として列挙する。後者は cache line の追い出し後に来る**非同期の割り込み**なので、該当 store を飛ばす細工ができない。文書化されていない「黙って落ちる」挙動に依存するのは不健全。**実機で試していない**（IDF v6.0.1 `C:\esp\v6.0.1\esp-idf` の同ファイル 84 行・99 行に両文言があることは確認。挙動は IDF ソースからの推論。§10） |
| **D. 参照数の増減すべてにアドレス判定を足す** | `JS_DupValue`/`JS_FreeValue` で flash 番地なら触らない | インタプリタの**最も熱い経路**に分岐を足す。名前だけのために全値の操作を遅くする理由が無い |

## 5. 採る設計

### 5.1 F1: ROM atom — flash 常駐の atom 記録

**FD1. ROM atom は `JSString` ではなく専用の記録にする。**

```c
/* One per builtin name, in .rodata. No ref_count, hash_next or weak-ref
 * fields: nothing ever writes to it, so it can live in flash. */
typedef struct { uint32_t hash_type; uint16_t len; uint16_t off; } JSRomAtom;   /* 8 B */
extern const JSRomAtom js_rom_atoms[];   /* generated */
extern const char      js_rom_chars[];   /* one blob, all ASCII */
```

- **番号空間**: `1 .. ROM_END` が ROM atom、それより上が今までのヒープ atom。`rt->atom_array` は
  `atom - ROM_END` で引く（ROM ぶんの 4 B × 約 550 = 2.2 KB のポインタ表を持たない。計算）。
  `__JS_AtomIsConst` 相当の判定 `atom < ROM_END` を、`rt->atom_array[` の 33 箇所（§11）が分岐に使う。
- **ヒープに残す const atom**: well-known symbol 14 個（値としての同一性が要る）と空文字列
  `JS_ATOM_empty_string`（`js_empty_string` 4411 行が**値として**毎回返す。ROM だと毎回 materialize に
  なる）。これらは番号 `1 .. K` に**並べ替え**て置き、`K .. ROM_END` を ROM 文字列にする。これで
  「番号 < K → ヒープの const、< ROM_END → ROM、それ以上 → 動的」の 3 区間になり、記録に kind を持たせずに済む。
  `quickjs-atom.h` の順序は生成し直す（bytecode 形式が変わる。仕様 §12.2「キャッシュ形式を識別・無効化」）。
- **名前 → atom の検索**（parser の識別子、`JS_NewAtom*`、`find_atom` 43755 行）は、**ビルド時に生成した
  ROM 表**（`hash & mask` で引く固定ハッシュ表、または hash 順ソート + 二分探索。生成器が決める）を先に
  引き、外れたら今までの動的表へ行く。ROM 表はハッシュの再構築（`JS_ResizeAtomHash` 3442 行が全 atom の
  `hash_next` を書き換える）に**関与しない** — ROM 記録には `hash_next` が無い。
- **文字列値への脱出**（`__JS_AtomToValue` 3982 行 / `JS_AtomToString` / `JS_AtomGetStrRT` 3949 行:
  `Object.keys(Math)`、`fn.name`、エラー文言など）は、その場でヒープの `JSString` を materialize する。
  普通の文字列値なので参照数で消える。**キャッシュは任意**（FD3 で「まず持たない、脱出回数を数えてから」）。
  materialize した文字列を `JS_NewAtomStr` に戻したときは ROM 検索が同じ番号を返すので、`p->atom_type` に
  頼る同一性は壊れない。
- **ROM の名前一覧**: 予定義 229 個 ＋ `JS_NewContext` 直後に存在する全文字列 atom（=組み込みの関数
  リストが作る名前。今日の数で 543 個）。**ホストの生成器**が `JS_NewContext` 後の atom 表を dump して
  表を生成する（`lazyfloor.c` の walk がその原型）。
- **flash の費用（推定）**: 記録 543 × 8 = 4,344 B ＋ 文字 ≤ 10,860 B（21,720 − 543 × 20、切り上げ込みの
  上界）＋ 検索表 ≈ 1〜2 KB → **≤ 15 KB flash**（計算）。`.rodata` の 543 個の `js_atom_init` 文字列は
  blob に置き換わるので、その分は相殺される。

**FD2. ROM 記録に `is_numeric` の答えを焼く。** `JS_AtomIsArrayIndex`（4021 行）と
`JS_AtomIsNumericIndex1`（4047 行）は文字列の純関数なので生成時に計算し、`hash_type` の空きビットに置く。
組み込み名に配列添字は無いが、`"Infinity"`・`"NaN"` は予定義 atom で、`JS_AtomIsNumericIndex1` が
それを数値として扱う（TypedArray の `[[DefineOwnProperty]]` 用）。生成器は `JS_AtomIsNumericIndex1` の
現行実装で答えを作り、対照（現行ビルド）と同じ答えになることを検査に含める（§8 の負の対照 N1c）。

**FD3. 脱出文字列のキャッシュは最初は持たない。** F1 の計装で「ROM atom → 文字列値」の回数をアプリ
ごとに数え（§8 F1 の計装）、フレームあたりの回数が 0 でないアプリが出たときに検討する。

### 5.2 F2: 組み込みの索引を持たない

**FD4. 組み込みオブジェクトは、関数リスト由来のプロパティ（AUTOINIT のメソッドと GETSET の accessor）を
最初に触るまで shape に載せず、flash の関数リストだけを持つ。**

- `JS_SetPropertyFunctionList(ctx, obj, tab, len)` は、対象オブジェクトに「未展開のリスト」
  `(tab, len, insert_index)` を記録するだけになる。`insert_index` はそのときの `prop_count`
  （定義順を保つため。§6 リスク「プロパティ順序」）。表の項目は生成時に **ROM atom 番号**を持つ
  （`JSCFunctionListEntry.name` が文字列なので、生成器が番号付きの並列表を出すか、`find_atom` の結果を
  一度だけ引いて `uint16_t` の表に置く。整数比較で探せることが要点）。
- **own-property の外れ**（`find_own_property` が NULL）で、対象が未展開リストを持てば表を探す
  （整数比較、二分探索または線形。1 表 ≤ 40 項目程度）。当たれば**その 1 項目だけ**を今日と同じ形で shape に
  materialize する（AUTOINIT なら AUTOINIT のまま、GETSET なら accessor 関数を作って GETSET）。
  同一性 `Array.prototype.map === Array.prototype.map` は、2 回目からは shape にあるので保たれる。
- **全展開**: 列挙（`getOwnPropertyNames` / `ownKeys` / for-in / `JSON.stringify`）、`delete`、
  `freeze`/`seal`/`preventExtensions`、`defineProperty`、`setPrototypeOf`、Proxy の target 化、
  `JS_GetOwnPropertyNames` を通る C API — の**前**に、リストを定義順に `insert_index` の位置へ全部
  materialize して「展開済み」の旗を立てる。以後の挙動は今日と同一。
- **1 オブジェクトが複数のリストを受ける場合**（global、TypedArray の base prototype 等。個数は F2 の
  生成器で数える。§10）は、リストの配列を持つか、リストごとに `insert_index` を持つ小さな配列にする。
- **代償**: 外れ経路の表探索（§7）と、全展開が起きたときのまとまった確保（今日は起動時に払っているもの）。
  Kasane や `pocket.*` の C 側が組み込みの prototype を列挙する箇所があれば起動時に全展開が走って節約が
  消えるので、F2 の計装で「どのオブジェクトがいつ全展開されたか」を出す（§8 F2 の計装）。

**FD5. ファームが `JS_NewContext` の後に足す名前を ROM 表へ入れるかは F1 の実機計測後に決める。**
入れると §2.2 の 13 KB のうち atom ぶんが減るが、表の生成にファームの面の一覧が要り、面を足すたびに
生成し直すことになる。入れなくても正しさは変わらない（動的 atom になるだけ）。

### 5.3 F1 と F2 の関係

F2 は F1 に依存する（表の項目を ROM atom 番号で持つため）。F1 単独でも 21,720 B は取れるので、
F1 を先に `vm/main` へ戻し、F2 は別の枝で進める。

## 6. リスク

| # | リスク | 扱い |
| --- | --- | --- |
| R1 | **プロパティ順序**: 遅延 materialize の順が定義順と違うと `Object.keys` の並びが変わる（Test262 が見る） | 全展開を「列挙の前」に定義順・`insert_index` 位置で行う（FD4）。部分展開の状態で列挙に入らないことを、列挙の全入口（`JS_GetOwnPropertyNamesInternal`、for-in の enum 生成、Proxy）で保証。コーパスに順序検査（§8 N2a） |
| R2 | **脱出経路の漏れ**: ROM atom の `JSAtomStruct*` を期待して `atom_array` を引く経路を見落とすと、`atom - ROM_END` が負になって範囲外読み | 33 箇所の台帳（§11）を 1 行ずつ閉じる。ホストの負の対照 N1b（ROM 表を読み取り専用ページに置き、書けば SEGV）と ASan |
| R3 | **symbol**: well-known symbol は値の同一性（`Symbol.iterator === Symbol.iterator`）とプロパティ鍵の両方で使われる | ヒープに残す（FD1）。`JS_NewSymbolFromAtom` 3924 行の `descr` が ROM 文字列のときは materialize |
| R4 | **`JS_ResizeAtomHash`**: 動的表の再ハッシュが ROM atom を鎖に混ぜると `hash_next` への書き込みになる | ROM 記録は動的表に入らない（別表で先に引く）。N1b が検出する |
| R5 | **UTF-8 / wide 文字列からの `JS_NewAtom`**: ROM 名は ASCII だが、同じ文字列が 16-bit の `JSString` で来ることがある（`String.fromCharCode`、`toUpperCase` の結果など） | ROM 検索は `js_string_memcmp` と同じく幅の違う比較を受け付ける。ハッシュは `hash_string` と同じ式で生成（生成器がホストの `hash_string` を呼ぶ） |
| R6 | **GC / 解放**: ROM atom は解放されない（不死）。`JS_FreeRuntime` の解放ループと leak 検査が ROM を飛ばす必要 | 台帳 #1・#2 |
| R7 | **`JS_AtomIsNumericIndex`** の `"Infinity"`/`"NaN"` | FD2、N1c |
| R8 | **解析時の検索性能**: ソースの識別子ごとに ROM 表を 1 回引く | 固定ハッシュ表なら 1 probe ＋ memcmp。今日も動的ハッシュ表を引いているので、ROM 表は**追加**の 1 回（推定: 数十 ns/識別子、解析時のみ。実測は F1 の関所 §8 で `timing.py`） |
| R9 | **外れ経路の性能**（F2） | §7。1 回の単価は未計測 |
| R10 | **ファームの C 側が prototype を列挙して全展開を誘発**（F2） | 計装で検出（§8 F2） |
| R11 | **bytecode 形式**: atom 番号の付け替えで `JS_WriteObject` の出力が変わる | 保存済み bytecode はこのプロジェクトに無い（毎回ソースから解析）。台帳 #26・#27 で形式の版を上げる |
| R12 | **JSString の atom_type**: materialize した文字列を再び atom にするとき、`JS_NewAtomStr` は `p->atom_type != 0` なら既存 atom とみなす | materialize する `JSString` は `atom_type = 0` で作り、通常の検索経路で ROM 番号に戻す |

## 7. 実行時の代償（F2）の実測(host)

`tools/vmtest/floor/lazyprobe.sh`: 実物の QuickJS と実物の `pocket.kasane`、他の `pocket.*` は
stub（JS 側の小さな代替）。300 フレーム、30 フレームごとに Enter。`find_own_property` に計数を挟んだ
`quickjs.c` の写しを使う（リポジトリの `quickjs.c` は触らない）。数えたのは
**`JS_NewContext` 直後に存在したオブジェクト（=組み込み）での own-property の外れ**と、
**関数リスト由来プロパティの初回ヒット**（F2 なら materialize になるもの）。

```
apps/hello/main.js          startup: miss=8   first=1 | per frame: miss=5.1  first=0.00
apps/imucal/imucal.js       startup: miss=16  first=3 | per frame: miss=58.1 first=0.01
apps/pet/pet.js             startup: miss=171 first=5 | per frame: miss=18.5 first=0.00
apps/companion/companion.js startup: miss=17  first=2 | per frame: miss=6.5  first=0.00
apps/deskclock/deskclock.js startup: miss=6   first=0 | per frame: miss=5.0  first=0.00
apps/kasane/demo.js         FAIL (stub 不足: TypeError: not a function)
```

| アプリ | 起動時の外れ | 外れ/フレーム | アプリ起因/フレーム（ハーネス自身の `frame()` 呼び出しの床 ≈5 を引く。deskclock は `frame` を持たず 5.0 なのでそれが床） | 初回展開 |
| --- | --- | --- | --- | --- |
| hello | 8 | 5.1 | ≈0 | 1 |
| deskclock | 6 | 5.0 | 0（床） | 0 |
| companion | 17 | 6.5 | ≈1.5 | 2 |
| pet | 171 | 18.5 | ≈13.5 | 5 |
| imucal | 16 | 58.1 | ≈53 | 3（+0.01/フレーム） |

外れは主に「prototype 鎖を上る途中で組み込み prototype を通過する」もの（アプリ自身のオブジェクトの
プロパティを読むと `Object.prototype` で外れる）。**1 回の単価は未計測**: 整数比較の二分探索で 0.1〜1 µs
（推定）。imucal で ≈53 µs/フレーム ≈ JS ターン 4.4 ms の 1.2%（計算）。参考の速度事実: 呼び出し ≈3.4 µs、
メソッド呼び出し ≈6.8 µs（[vm-L2-results.md](vm-L2-results.md) §3.5 から計算）。imucal だけが JS を
フレームの最大項に持つ（他は描画が支配）。Kasane デモは stub 不足で未計測（§10）。

## 8. 段階ごとの移行計画と関所

各段は `CONFIG_POCKET_VM_*` のビルド時選択で従来経路へ戻せるようにする（仕様 §12.2）。関所は
「既存の合格項目を維持」（仕様 §12.1）に、**新しい機構ごとの負の対照**を足す — 検出器は、検出すべき
ものを一度検出してみせるまで信用しない（[vm-L3-results.md](vm-L3-results.md) の毒の対照と同じ流儀）。

### F0（この文書、完了）

- 床の内訳・節約の計算・外れ回数の実測(host)。道具は `tools/vmtest/floor/`（§9）。

### F1: ROM atom（`CONFIG_POCKET_VM_ROM_ATOMS`、既定 n → 関所後 y）

1. 生成器 `tools/vmtest/floor/gen_rom_atoms.py`（予定）: ホストで `JS_NewContext` 後の atom を dump し、
   `quickjs-rom-atoms.h`（記録・blob・検索表・並べ替えた `quickjs-atom.h`）を出す。生成物はリポジトリに
   入れ、生成器の再実行で差分 0 を検査に含める。
2. 台帳 §11 の 33 箇所を 1 行ずつ閉じ、台帳を `vm-ledger/10-rom-atoms.md` に移して「各行の処置」を書く。
3. 計装（既定 n）: ROM atom → 文字列値の脱出回数、ROM 表の検索回数と衝突長。
4. **関所（ホスト）**: `tools/vmtest/run.sh` の全変種（asan・o2・`--force-yield`・`-keepsrc`）でコーパス
   バイト一致。`tools/vmtest/test262.py` が `test262-baseline.txt` と同一（asan と o2、`--force-yield`）。
   `timing.py` で解析時間の退行が配置差の床（15%）以内。
   - **N1a（負の対照: 検出器が働く）**: ROM 表から名前を 1 つ（例 `subarray`）わざと抜いたビルドで、
     その名前が動的 atom として作られ、コーパスが通る（フォールバックが生きている証拠）。
   - **N1b（負の対照: 書き込み）**: ホストで ROM 記録と blob を `mprotect(PROT_READ)` したページに置き、
     33 箇所のどれかが書けば SEGV。わざと 1 箇所を戻して SEGV が出ることを確認してから、全部閉じた
     状態で全変種を通す。
   - **N1c**: `JS_AtomIsNumericIndex1` の焼き込み結果が現行実装と全 ROM 名で一致。
5. **関所（実機、`build_*` を分ける）**: `tools/smoke_device.py --cycles 20`、`tools/test_settings.py`、
   `tools/memlog.py --map ... --port COM3 --check`。アプリごとの `js=` が **≈ −21.7 KB**（計算値。実機の
   値は tlsf の実長課金なので多少ずれる）。既定ビルドの `idf.py size` で flash の増分 ≤ 15 KB（推定）を確認。
   `benchmark_app.py` で JS ターンが動いていないこと（同一バイナリ内比較でないので 15% 未満の差は主張しない）。

### F2: 組み込みの索引を持たない（`CONFIG_POCKET_VM_LAZY_BUILTINS`、既定 n → 関所後 y）

1. `JS_SetPropertyFunctionList` の遅延化、外れ経路の表探索、全展開の入口の列挙（§5.2）。
2. 計装（既定 n）: オブジェクトごとの外れ回数・初回展開・全展開の発生と誘発元（C API か JS か）。
3. **関所（ホスト）**: F1 と同じ全変種・Test262 同一。
   - **N2a（負の対照: 順序）**: コーパスに「全組み込みオブジェクトの `Object.getOwnPropertyNames` と
     descriptor を、触る前・1 つ触った後・全展開後に印字」する検査を足し、期待値は**現行ビルド**で作る。
     わざと `insert_index` を無視した実装で失敗することを一度見せる。
   - **N2b（負の対照: 同一性）**: `Array.prototype.map === Array.prototype.map` 等を全リスト項目で
     機械的に生成して比較。materialize を「毎回作り直す」誤実装で落ちることを見せる。
   - **N2c**: 「起動時に全展開」モードのビルドが、現行ビルドとコーパス・Test262 で同一（遅延を切れば
     今日に戻ることの証明）。
4. **関所（実機）**: F1 と同じ。`js=` が合計で **≈ −35.7 KB**（計算、F1 と合わせて）。`lazyprobe` の外れ
   回数を実機の計装でも出し、ホストの表（§7）と桁が合うこと。imucal の JS ターンが `benchmark_app.py` で
   退行していないこと（配置差の床 15% 以内なら「不変」）。

### F3（任意、F2 の実測後に判断）

→ 2026-09-26 に F3a（hash 表、§16）と F3b（型付き配列の遅延、§17）を実施。残りは backlog F3c 以降。

- `atom_array`/`atom_hash` の初期サイズ縮小（4,892 B のうち）。
- 脱出文字列のキャッシュ（FD3）。
- 群単位の intrinsic 遅延（§4-A）を F2 の上に重ねる価値があるか。
- §2.2 のファーム側 13 KB の計算と、ファームの面の名前を ROM 表に入れる（FD5）。

## 9. 再現（`tools/vmtest/floor/`）

すべて WSL。PowerShell から `wsl -e bash <ファイル>` で呼ぶ（`bash -lc "…$…"` は `$` が壊れるので
スクリプトをファイルに置いてある。Git Bash は `/mnt/c` を書き換えるので PowerShell から呼ぶ）。

```powershell
wsl -e bash tools/vmtest/floor/floor32.sh     # §2: intrinsic ごとの床（-m32 -malign-double、.cache/vmtest32 を作る）
wsl -e bash tools/vmtest/floor/lazyfloor.sh   # §2.1・§6: heap walk と F1/F2 の節約（floor32.sh の後）
wsl -e bash tools/vmtest/floor/lazyprobe.sh   # §7: 出荷アプリの外れ回数（64-bit host、.cache/vmtest-floor）
```

| ファイル | 何を測るか | 前提 |
| --- | --- | --- |
| `floor32.c` / `floor32.sh` | `JS_NewRuntime2` と各 `JS_AddIntrinsic*` の `js=` を、実機ゲストの課金（tlsf ブロック長）で | `tools/vmtest/m32_sysroot.sh`（初回は i386 の deb を取得）、`build.sh o2` を `VMTEST_OUT=.cache/vmtest32` で |
| `lazyfloor.c` / `lazyfloor.sh` | `quickjs.c` を `#include` して atom 表と GC リストを歩き、§5 の 2 段で消えるバイトを数える | `floor32.sh` が作った `.cache/vmtest32/obj-o2` |
| `lazyprobe_patch.py` / `lazyprobe.c` / `lazyprobe.sh` | `find_own_property` に計数を足した `quickjs.c` の写しで、出荷アプリを Kasane ごと 300 フレーム走らせる | `tools/make_font.py`、`apps/pet/assets/pets-compact.bin`、`tools/hostshim/` |

`lazyfloor.c` の節約は**計算**である（walk した現状の shape/prop 配列から、値プロパティだけで作り直した
ときの大きさを `resize_properties` の成長規則で求めている）。実装後の実測と一致する保証は無く、F1/F2 の
関所で `js=` を測って置き換える。

## 10. 計測の穴（正直に）

| 穴 | 状態 |
| --- | --- |
| F2 の外れ 1 回の単価 | **未計測**。0.1〜1 µs は推定。F2 の実装で `callbench` 流に同一バイナリ内で測る |
| 床のうちファーム側の約 13 KB | **未計算**（§2.2）。`pocketjs_guest_quickjs_install_once` をホストでリンクして `floor32` と同じ課金で測るのが次の手 |
| 実機の確認 | **無い**。数字はすべてホスト（実機レイアウト）か計算。実機の `js=` は tlsf の実長課金（backlog #9）なので、切り上げの分だけずれる |
| Kasane デモの外れ回数 | stub 不足で未計測 |
| 案 C（flash への書き込み）の実機挙動 | 未検証（IDF v6.0.1 の `cache_err_int.c` に panic 文言があることは確認。実際に書いて panic するかは実機で試していない） |
| 1 オブジェクトが受ける関数リストの個数 | 未集計（F2 の生成器で出す） |
| 解析時の ROM 検索の費用 | 未計測（R8） |
| 4.5 B/byte のうち atom が占める割合 | 未計測。アプリの識別子が ROM 名と一致するぶん（`length`・`push`・`map` …）は F1 で動的 atom にならないので、ソース由来の atom も少し減るはずだが数えていない |

## 11. 付録: `rt->atom_array[` の 33 箇所（`quickjs.c`、29f6f35）

F1 の台帳の原型。処置の分類: **境界**（`< ROM_END` の分岐を足す）、**除外**（ROM は通らない/飛ばす）、
**読替**（ROM 記録から同じ情報を読む）、**脱出**（`JSString` を materialize）、**索引**（`atom - ROM_END`）。
「？」は読み切れていないもの。

| # | 行 | 関数 | 何をしているか | 処置 |
| --- | --- | --- | --- | --- |
| 1 | 2706 | `JS_FreeRuntime`（`ENABLE_DUMPS` の leak 検査） | 全 atom を走査し `i >= JS_ATOM_END` か ref_count≠1 を漏れとして印字 | 除外（動的区間だけ走査） |
| 2 | 2761 | `JS_FreeRuntime` | 全 atom を `js_free_rt` | 除外（動的区間だけ） |
| 3 | 3419 | `JS_DumpAtoms` | ハッシュ鎖を辿って印字 | 除外（ROM は鎖に無い。ROM 表を別に印字） |
| 4 | 3430 | `JS_DumpAtoms` | 全 atom を印字 | 索引・読替 |
| 5 | 3456 | `JS_ResizeAtomHash` | 鎖の全要素の `hash_next` を書き換える | 除外（ROM は鎖に無い。N1b で検出） |
| 6 | 3510 | `JS_DupAtomRT` | `!__JS_AtomIsConst` なら ref_count++ | 境界（既存の判定を `ROM_END` に） |
| 7 | 3523 | `JS_DupAtom` | 同上 | 境界 |
| 8 | 3538 | `JS_AtomGetKind` | `p->atom_type` を読む | 読替（`hash_type` に型がある） |
| 9 | 3568 | `js_get_atom_index` | `JSAtomStruct*` から番号へ（`p->hash_next` を索引に使う経路） | 除外？ ROM は `JSAtomStruct*` として現れないはずだが、materialize した文字列が渡らないことを要確認 |
| 10 | 3572 | `js_get_atom_index` | 同上（線形探索側） | 同上 |
| 11 | 3604 | `__JS_NewAtom` | 動的ハッシュ鎖で既存 atom を探す | 境界（この前に ROM 表を引く） |
| 12 | 3672 | `__JS_NewAtom` | `atom_array` を伸ばして自由リストを繋ぐ | 索引 |
| 13 | 3714 | `__JS_NewAtom` | 自由スロットから次を取る | 索引 |
| 14 | 3715 | `__JS_NewAtom` | 新 atom を格納 | 索引 |
| 15 | 3771 | `__JS_FindAtom` | C 文字列で既存 atom を探す（作らない） | 境界（ROM 表を先に） |
| 16 | 3796 | `JS_FreeAtomStruct` | 鎖から外す（先頭） | 除外（ROM は解放されない） |
| 17 | 3804 | `JS_FreeAtomStruct` | 鎖から外す（途中） | 除外 |
| 18 | 3813 | `JS_FreeAtomStruct` | スロットを自由リストへ | 除外・索引 |
| 19 | 3831 | `__JS_FreeAtom` | ref_count-- して 0 なら解放 | 境界（呼び出し側の const 判定を `ROM_END` に） |
| 20 | 3920 | `JS_NewSymbolInternal` | symbol atom を値にする | 除外（symbol はヒープ、番号 < K） |
| 21 | 3932 | `JS_NewSymbolFromAtom` | 説明 atom の `JSString` で symbol を作る | 脱出（`descr` が ROM なら materialize） |
| 22 | 3960 | `JS_AtomGetStrRT` | 文字を C バッファへ（エラー文言・dump） | 読替（blob から読む） |
| 23 | 3993 | `__JS_AtomToValue` | atom を文字列値に | 脱出 |
| 24 | 3999 | `__JS_AtomToValue` | symbol の説明が無いとき空文字列 | 除外（空文字列はヒープ、FD1） |
| 25 | 4032 | `JS_AtomIsArrayIndex` | `atom_type` と `is_num_string` | 読替（FD2 の焼き込み） |
| 26 | 4059 | `JS_AtomIsNumericIndex1` | 文字列を数値へ変換して正準性を見る | 読替（FD2） |
| 27 | 4175 | `JS_AtomSymbolHasDescription` | symbol の説明の有無 | 除外（symbol はヒープ） |
| 28 | 4411 | `js_empty_string` | 空文字列 atom を値として返す（高頻度） | 除外（ヒープに残す、FD1） |
| 29 | 8519 | `JS_ComputeMemoryUsage` | `atom_array` の大きさを `atom_size` に足す | 索引（動的区間だけ数える。`js=` の床が減る根拠） |
| 30 | 8522 | `JS_ComputeMemoryUsage` | 各 atom の文字列サイズを足す | 除外（ROM は数えない） |
| 31 | 42224 | `JS_WriteObjectAtoms` | bytecode 書き出し。const は番号、他は文字列 | 境界（ROM も番号で書く。形式の版を上げる、R11） |
| 32 | 43574 | `JS_ReadObjectRec` | bytecode 読み込みで symbol atom を値に | 境界？ 読み込み側の const 判定を `ROM_END` に。symbol 以外の ROM 番号が来る経路が無いか要確認 |
| 33 | 43755 | `find_atom` | 関数リストの `name`（`[Symbol.x]` 記法つき）から atom | 境界（ROM 表を先に。F2 はここを生成時に済ませる） |

「境界」は 33 箇所のほかに、`__JS_AtomIsConst` を呼ぶ側（3509・3521・3592 行など）と、`JS_ATOM_END` を
直接比べる箇所（2708 行、`JS_InitAtoms` の 3488 行）にもある。F1 の台帳はこれらも行にする。

## 12. F1 の結果（2026-09-25、`vm/f1-rom-atoms`）

`CONFIG_POCKET_VM_ROM_ATOMS`。実装は `quickjs.c` の `#ifdef` の中、表は生成物
`components/quickjs-ng/quickjs-ng/quickjs-rom-atoms{,-defs}.h`（`tools/vmtest/floor/gen_rom_atoms.sh`、
`--check` で陳腐化を検出）。

### 12.1 表（実測(host)、生成器の出力）

flash の名前 **542 個**（予定義 214、組み込みの関数リスト由来 328）。§2.1 の 543 から空文字列を除いた数と一致。
表は文字 5,713 B ＋ 記録 4,464 B ＋ 検索表 4,096 B（開番地、平均 1.16 回・最大 4 回で当たる）。生成時の検算
（ランタイムのハッシュと一致、配列添字でない、8-bit）は全件通過。数値索引は `"Infinity"` だけ。

### 12.2 計画との違い

| 計画 | 実装 | 理由 |
| --- | --- | --- |
| FD1: 予定義の番号を並べ替え、ヒープ定数を `1..K` に寄せる | **並べ替えない**。`[1, JS_ATOM_END)` は従来の番号のまま、flash の名前は `atom_array` の枠を NULL にする。組み込み由来の名前は `[JS_ATOM_END, 558)` | `quickjs-atom.h` の順序にはキーワード範囲（`JS_ATOM_LAST_KEYWORD` 等）と symbol 範囲の判定が依存している。並べ替えずに済み、NULL 枠は取り違えを即座に落とす検出器にもなる（N1b） |
| FD3: 脱出文字列のキャッシュは最初は持たない | **32 枠の直接写像キャッシュを持つ**（`JSRuntime.rom_cache`、枠ごとに参照 1 つ） | `typeof` の答え（`"number"` 等）が毎回文字列値として外へ出る。キャッシュ無しでは `typeof` のたびに確保になる |
| N1b: ROM を `mprotect` して書けば SEGV | ホストでは `const` 表がそもそも読み取り専用ページにあり、flash の名前は NULL 枠なので、取り違えた処置は NULL 参照で必ず落ちる。負の対照として「1 箇所の処置を外す」ビルドで全 77 件が落ちることを見せた | 同じことを追加の仕掛け無しで満たす |
| 台帳を `vm-ledger/10-rom-atoms.md` に移す | 33 箇所の処置はコードの各 `#ifdef` のコメントと §11 の分類のとおり。**別文書にはしていない** | §11 の分類から外れた処置は無かった。#9/#10/#32（「？」）はいずれも ROM の名前が通らない経路だった（`js_get_atom_index` はヒープの atom 構造体からしか呼ばれず、`JS_ReadObjectRec` の symbol は ROM に無い） |

### 12.3 ホストの関所

| 検査 | 結果 |
| --- | --- |
| コーパス（asan・o2・asan-rom・o2-rom・asan-reloc-rom、-rom 2 種の `--force-yield`） | すべて **77/77**。新規 `corpus/rom_atoms.js` が脱出経路（`typeof`、列挙順、`.name`、`Symbol` の説明と一意性、型付き配列の `"Infinity"`、16-bit 文字列の組み込み名、JSON・for-in、エラー文言）を突く |
| 確保番号で固定した OOM 回帰 3 件 | -rom では起動時の確保が約 440 回少ない。確保トレースを `# ready` の後で突き合わせ、**同じ 72 B の確保**に付け替えた（`// vmrun-rom-flags:`、1351→913・1369→931・1289→849） |
| Test262（o2-rom・asan-rom・o2-rom `--force-yield`） | すべて **7501 / 194 / 0、退行 0**（基準と同一） |
| 負の対照（`tools/vmtest/floor/f1_faults.sh`） | 表が古い（組み込み由来の名前が無い）→ 通る（確保番号固定の 3 件だけが番号ずれで落ちる、除外して判定）。flash を引かない → 4 件で検出。処置 1 箇所を外す → 77 件全部。`JS_ROM_NUMERIC` を無視 → `rom_atoms` だけが検出（**既存のコーパスでは見逃していた**） |
| `timing.py`（o2 と o2-rom、各 5 回） | 8 本すべて揺れの範囲（`bench_alloc` の中央値 +8%、最小値どうし +6%。ホスト） |

### 12.4 実機の関所（実測(device)、同じツリーの既定ビルドと -rom ビルド、各アプリ 3 回）

| アプリ | `js=` 既定 | `js=` ROM | 差 |
| --- | --- | --- | --- |
| hello | 85,988 | 65,896 | **−20,092** |
| imucal | 100,708 | 79,264 | **−21,444** |
| pet | 106,164 | 85,296 | **−20,868** |
| companion | 98,396 | 76,952 | **−21,444** |

計算の −21,720 B に対し −20.1〜21.4 KB。差の分は脱出文字列とキャッシュ（`typeof` などで作る）と推定。
アプリ実行中の空き `app_free` は 139,600 → 157,940（+18.3 KB）、最大空きブロック 98,304 → 114,688。
アイドル時の空き・静的 DIRAM は不変（`idle_free=234528`、DIRAM ±0）。`memlog --check` 予算内、
`smoke_device.py --cycles 20` 通過（空きは 10 周目と 20 周目で同じ）・故障回復 6 種、`test_settings.py` 通過。

書き込むイメージは **+15,504 B**（`.bin` の大きさ、実測）。`memlog` の `flash=` は命令部分だけで +1,244 B。
既定（n）のビルドも +28 B 動いた（配列を歩く 4 箇所の NULL 検査と、後ろの `assert` の行番号のずれ）。

**未計測**: JS ターンの速さ（`benchmark_app.py`。同一バイナリでの比較でないので 15% 未満の差は主張しない
ため、今回は取っていない）と、脱出文字列の回数（FD3 の計装）。
### 12.5 既定を y に（2026-09-25）

関所を通したので `CONFIG_POCKET_VM_ROM_ATOMS` の既定を y にした。既定設定で作ったファームは、実機で測った
-rom ビルドとロードされる全セクションの大きさ・全シンボル（番地と大きさ）が一致する。ホストの
`tools/vmtest/build.sh` も素の種類を ROM にし、従来の経路は `-norom` で作る（「素の種類はファームの既定と同じ
経路」の決まり）。`-keepsrc` と組み合わせたときの確保番号は `// vmrun-rom-keepsrc-flags:`（914・932。
module は 849 のまま）。asan・o2・asan-norom・o2-norom・asan-keepsrc・asan-norom-keepsrc・asan-reloc・
o2/asan `--force-yield` のコーパスはすべて 77/77。
## 13. F2 の結果（2026-09-26、`vm/f2-lazy-builtins`）

`CONFIG_POCKET_VM_LAZY_BUILTINS`（既定 y）。実装は `quickjs.c` の `#ifdef` の中。

### 13.1 何をしたか

- `JS_SetPropertyFunctionList` は、対象オブジェクトの関数リストを `rt->lazy`（オブジェクトのアドレス順の配列、
  1 件 24 B）に記録するだけにした。リストの項目は初めて触ったときにシェイプへ入る。印は GC ヘッダの未使用ビット
  （`dummy0` の最下位）で、ランタイム生成時にそのビットが本当にどちらの見方からも空いていることを確かめてから使う。
- 探索が外れたときに表を探す入口: 取得（`JS_GetPropertyInternal`）、自分のプロパティ（`JS_GetOwnPropertyInternal2`、
  `in`・`hasOwnProperty` もここ）、代入（自分とプロトタイプをたどる経路の両方）、定義（`JS_DefineProperty`）。
  インタプリタの速い経路（`get_field`・`get_field2`・`get_length`）は、遅延オブジェクトに当たったら遅い経路へ回す。
- 削除は印を付けるだけ。列挙（`JS_GetOwnPropertyNamesInternal`、for-in、`JS_WriteObject`）は残りを全部入れ、
  シェイプを定義順に作り直す（リストごとに「登録時点でその前にあった普通のプロパティの数」を持ち、削除でずらす）。
  列挙が「列挙可能なものだけ」で、残りに列挙可能な項目が無ければ展開しない（for-in がプロトタイプを通るたびに
  展開しないため）。
- 別名（`JS_ALIAS_DEF`、例: `Array.prototype[Symbol.iterator]`）は登録時に今までどおり入れる（参照先の登録時点の値を
  指すのが仕様どおりの同一性）。グローバルオブジェクトと特殊オブジェクト（配列を除く）は対象外。
- 組み込みの生成関数はリストの長さに合わせてシェイプを先に確保している（`JS_NewObjectProtoClassAlloc(.., n)`、
  `JS_NewCFunction3(.., n + 3)`）。遅延にするとその枠が空のまま残るので、登録時に `compact_properties` で縮める。
  **これを入れる前は −5.7 KB、入れた後は −12.2 KB**（ホスト計算）。

### 13.2 見つけた誤り

- **印のビットが初期化されていなかった**: 上流はオブジェクト生成時に GC ヘッダの未使用ビットを初期化しない（読む者が
  いなかった）。解放済みのブロックを再利用すると前のオブジェクトの印が残る。ASan は新しいメモリを 0 にしないので、
  asan のときだけ `%ThrowTypeError%` が起動中に「全部展開」され、確保の並びが o2 と 1 回ずれて見つかった。
  `JS_NewObjectFromShape` で消すようにした。
- **負の対照がコーパスの穴を 2 つ見せた**: 「未展開の項目の削除」と「プロトタイプ上の未展開の setter・読み取り専用への
  代入」を、コーパスが一度も試していなかった（その前に列挙で全部展開されていた）。`lazy_builtins.js` の先頭、何も
  触る前の区間に足してから、5 種の壊し方すべてが検出されるようになった。

### 13.3 ホストの関所

| 検査 | 結果 |
| --- | --- |
| 床（実機レイアウト、ホスト計算、`tools/vmtest/floor/floor_parts.c`） | F1 のみ 47,608 → F1+F2 **35,444 B**（−12,164 B。うち遅延の記録 2,304 B を払った後） |
| コーパス（遅延のあり・なし × ROM のあり・なし × `-keepsrc`、移動強制、`--force-yield`） | すべて **78/78**。新規 `corpus/lazy_builtins.js`: 何も触る前の代入と削除、触る順序を崩した後の列挙順、未展開の記述子、同一性、`in`、削除（未展開・設定変更不可）、普通のプロパティを消した後のリストの位置、再定義、凍結・拡張禁止、for-in、影に隠す |
| 確保番号で固定した OOM 回帰 3 件 | 組み合わせごとの番号（`// vmrun-rom-lb-flags:` など、`run.sh` は先頭 10 行を見る）。確保トレースの突き合わせで、どれも同じ 72 B の確保 |
| Test262（遅延あり o2・asan・o2 `--force-yield`） | すべて **7501 / 194 / 0、退行 0** |
| 負の対照（`tools/vmtest/floor/f2_faults.sh`） | 並べ替えなし・削除の記録なし・代入経路の探索なし・速い経路の判定なし・展開済みの記録なし、の 5 種すべて検出 |
| `timing.py`（o2 と遅延あり、各 5 回） | 揺れの範囲（遅延ありのほうが速い項目もある。ホスト） |

### 13.4 実機（実測(device)、同じツリーの F1 のみと F1+F2、各アプリ 3 回）

| アプリ | F1 のみ | F1+F2 | F2 の差 | F 系列の前（§12.4）との差 |
| --- | --- | --- | --- | --- |
| hello | 65,904 | 50,412 | **−15,492** | −35,576（−41%） |
| imucal | 79,288 | 64,036 | **−15,252** | −36,672（−36%） |
| pet | 85,328 | 67,288 | **−18,040** | −38,876（−37%） |
| companion | 76,944 | 61,228 | **−15,716** | −37,168（−38%） |

ホスト計算（−12.2 KB）より大きいのは、ファームが自分で入れる `pocket.*` の面も `JS_SetPropertyFunctionList` を
使っていて同じく遅延になったためと考えている（**推定、内訳は未計測**。§2.2 の「ファーム側の約 13 KB」の一部）。
アプリ実行中の空き `app_free` は 157,940 → 172,700、最大空きブロック 114,688 → 131,072。アイドル時の空き・
静的 DIRAM は不変。`memlog --check` 予算内、`smoke_device.py --cycles 20`・故障回復 6 種・`test_settings.py` 通過。
書き込むイメージは F1 のみに対し +3,520 B。既定を y にした設定のファームは、測ったビルドと全セクション・全シンボルが
一致する。

**未計測**: 外れ 1 回の単価（F2-3）と JS ターンの速さ（F1-6）。ホストの `timing.py` では退行は見えていない。
→ §14 で実測した。

## 14. 計測の穴を埋める（2026-09-26、`vm/f-measure`）

`CONFIG_POCKET_VM_FLOORPROBE`（既定 n）。数えるのは `quickjs.c` の F1/F2 の経路で、カウンタとマクロは
`quickjs-vmprobe.h` に置き、`quickjs.c` では**既にある行の末尾に**呼び出しを足しただけ（`assert()` が `__LINE__` を
埋め込むので、行が 1 本ずれると既定のビルドのコードが変わる。最初の版は `quickjs.c` と `main.c` に行を足して
+8 B・`app_main` の 5 バイトが変わっていた）。既定のビルドは vm/main と `.flash.text`・`.iram0.text`・
`.flash.rodata`・`.dram0.data`・`.dram0.bss` がバイト単位で一致する（違うのは appdesc のビルド時刻だけ）。

- `FLOOR stage=<名前> js=<バイト>`: ゲストを作った直後と、ファームの面を 1 つ入れるたびの `js=`。
- `FLOORPROBE ...`: セッション中の回数（開始時に捨て、停止時に読む）。`lazy_miss` は遅延オブジェクトでの探索の
  **呼び出し回数**で、当たりも含む。外れ = `lazy_miss − lazy_hit`。
- USB の `(`: 同じバイナリ内のマイクロベンチ（8000 回 × 5 ラウンド、1 ラウンド 1 フレーム）。
- `tools/vmtest/floor/device_floor.py --port COM3 [--bench-only]` が全部を回して JSON にする。

以下、すべて実測(device)。2 回走らせて回数は同じ値を再現した。

### 14.1 ファーム側の床（F0-a）

| 段 | js= | 差 |
| --- | --- | --- |
| ゲスト作成直後（runtime + context） | 40,208 | — |
| `console` | 40,352 | +144 |
| `pocket`（土台と遅延名前空間） | 43,556 | **+3,204** |
| `random`〜`bridge`（12 面） | 45,364 | +1,808（各 0〜288） |
| `app` | 46,240 | +876 |
| `workspace` | 46,372 | +132 |
| `pet-hub`（pet・companion のみ） | +248 | |

**ファームの面は合計 約 6.2 KB**（§2.2 の「ファーム側の約 13 KB」は F2 の前の値）。残る大物は `pocket` の土台だけで、
各面は名前空間の遅延（`pocket_api_lazy()`）で小さく済んでいる。ゲスト作成直後の 40.2 KB がホスト計算の床
（35,444 B、§13.3）より 4.8 KB 大きい理由は未調査。

### 14.2 回数（F1-5・F2-4・F0-b、各 20 秒）

| アプリ | フレーム | 脱出 | うち新規文字列 | 外れ | 外れ/フレーム | 全展開 |
| --- | --- | --- | --- | --- | --- | --- |
| hello | 609 | 72 | 62 | 118 | 0.19 | 0 |
| imucal | 1000 | 84 | 70 | 8,736 | **8.7** | 0 |
| pet | 599 | 1,960 | 83 | 742 | 1.24 | 0 |
| companion | 599 | 76 | 64 | 373 | 0.62 | 0 |
| Kasane デモ | 578 | 80 | 68 | 1,382 | 2.39 | 0 |

- **全展開はどのアプリでも 0**（F2-4）。列挙で節約を失っているアプリは無い。未展開のまま削除されたものも 0。
- 脱出はほぼ起動時の 70 回前後。pet だけ毎フレーム脱出する（1,960 回）が、新規は 83 回で 32 枠のキャッシュが
  96% を吸っている（F1-5。FD3 の判断はこのままでよい）。Symbol の説明と数値添字の ROM 経路は 0 回。

### 14.3 単価（F2-3・F1-6、同じバイナリ、8000 回あたり ms、5 ラウンドの中央値）

| 比較 | 遅延のまま | 展開後 | 1 回あたり |
| --- | --- | --- | --- |
| 無い名前を普通のオブジェクト経由で（`Object.prototype` のリストを探す） | 115 | 47 | **約 8.5 µs** |
| 無い名前を `Math` で（`Math` と `Object.prototype` のリスト）※2000 回 | 90 | 25 | **約 30 µs** |
| ある名前を `Math` で（当たった後） | 66 | 67 | 差なし |
| `typeof` | 48 | — | 空ループ 44 との差 ≤ 1 µs |
| 実行時の文字列→キー: ROM にある名前 / 無い名前 | 209 / 214 | — | ROM 検索は遅くない |

計画の推定（外れ 1 回 0.1〜1 µs）は**10〜30 倍外れた**。外れは連鎖上の遅延リストを全部なめ（`Math` は 40 項目強）、
項目ごとに名前を比べるので、リストの長さに比例する。一度当たった名前は普通のシェイプ探索になるので、当たりと
既存の経路には影響が無い。

アプリでの重さ: imucal は 8.7 回/フレーム × 8.5〜30 µs = **0.07〜0.26 ms/フレーム**（33 ms の 0.2〜0.8%、推定。
どのオブジェクトで外れたかは数えていない）。他のアプリは 1 桁小さい。今のアプリでは払ってよい額だが、外れの
多いアプリほど線形に効く。

F1-6（JS ターンの速さ）は、ROM 検索が入る唯一の実行時経路（文字列→キー）が同じバイナリ内で遅くないことで代える。
ビルド間の A/B は命令キャッシュの配置で ±15% 動くので取らない。

### 14.4 次の手（→ §15 で F2-5 として実施）

外れを安くする: リストごとに名前の小さな索引（ブルームフィルタ 1 語、または ROM atom 番号の範囲）を登録時に作り、
入っていない名前はリストをなめずに外れにする。記録 1 件（24 B）に 4〜8 B 足せば、imucal の外れの大半は定数時間になる
見込み（推定）。

## 15. F2-5: 外れを安くする（2026-09-26、`vm/f2-5-miss-index`）

### 15.1 索引ではなく比べ方を直した

§14.4 のブルームフィルタは採らなかった。1 語（16〜32 ビット）では `Math` の 40 項目強でビットの 7〜9 割が立ち、
ほとんど何も弾かない。64 ビットにすると記録 96 件で約 0.8 KB、床を削る系列でそれは払いにくい。

代わりに、外れの中身を見た。旧 `lazy_name_is` は**項目ごとに**、atom が ROM か heap かを引き直し、項目名の
`strlen` を flash から数えてから比べていた。新しい形（`LazyKey`）は探索 1 回につき atom を一度だけ
「文字列・長さ・先頭 1 バイト」に解き、項目ごとには**名前の先頭 1 バイトだけ**を見る。先頭が合ったときだけ
残りを比べる。記録の形は変えないので**メモリの増減は 0**、コードは +132 B。

規則は元と同じ: 添字 atom は何にも当たらない、`[Symbol.x]` 項目は組み込みの well-known symbol だけが名指せる、
`"[Symbol.x]"` と綴った普通の文字列は当たらない、16 ビット文字列は符号単位で比べる。加えて、NUL を含む
キー（`'max\0'`）が項目名の終端を越えて読まないようにした。

### 15.2 関所

| 検査 | 結果 |
| --- | --- |
| コーパス（asan・o2・遅延なし・ROM なし・`o2-keepsrc`・`--force-yield` 2 種） | すべて **78/78**。`lazy_builtins.js` に `cold-key` 行（接頭辞 `Math.ma`、`'max\0'`、`'[Symbol.toStringTag]'` と綴った文字列、空文字列、16 ビット文字列）。期待値は遅延なしのビルドの出力で、遅延ありの 3 種とバイト一致 |
| Test262（o2・asan・o2 `--force-yield`） | すべて **7501 / 194 / 0、退行 0** |
| 負の対照（`f2_faults.sh`） | 既存 5 種＋新規 2 種（`key-prefix`: 終端を見ない、`key-sym-string`: 綴りを symbol と取り違える）の **7 種すべて検出**。既存の `no-done-mark` は、§14 の計数を同じ行に足した 01df9b2 から**当てる場所が見つからず黙って走っていなかった**ので直した |
| 遅延なしのファーム | vm/main の同設定と `.flash.text`・`.iram0.text`・`.flash.rodata`・`.dram0.data`・`.dram0.bss` がバイト一致（行数を変えていない） |

### 15.3 実機（実測(device)、FLOORPROBE ビルド、同じバイナリ内の遅延 / 展開後、8000 回あたり ms）

| 比較 | F2-5 前（§14.3） | F2-5 後 | 外れ 1 回 |
| --- | --- | --- | --- |
| 無い名前を普通のオブジェクト経由で（`Object.prototype`） | 115 / 47 | **79** / 47 | 8.5 → **4.0 µs** |
| 無い名前を `Math` で（2000 回） | 90 / 25 | **46** / 25 | 約 30 → **約 10.5 µs** |
| ある名前を `Math` で | 66 / 67 | 66 / 67 | 差なし |

**2〜3 倍速くなったが、定数時間にはなっていない。** 残りは項目ごとの flash 読み（項目の `name` ポインタと先頭 1 バイト、
項目・文字列が rodata に散らばる）と見ている（推定）。imucal は 8.7 回/フレーム × 4〜10.5 µs =
**0.03〜0.09 ms/フレーム**（推定、33 ms の 0.3% 未満）。`js=`・各アプリの回数は §14.2 と同じ値。

これより先（定数時間）は、記録ごとの RAM の索引（約 0.8 KB）か、生成器が flash に焼く索引が要る。今のアプリでの
重さ（0.1 ms/フレーム未満）に対しては払わない。

`device_floor.py` を全アプリ通しで回すと、最後のベンチが 1 行で止まったことが 1 回あった（原因未調査）。
`--bench-only` では毎回取れている。

## 16. F3a: atom の hash 表を小さく始める（2026-09-26、`vm/f3-floor`）

### 16.1 F2 の後の床（実測(host, 実機レイアウト)、`floor32.sh`）

F2-5 の後で 35,444 B。runtime 8,536 B のうち **atom の表が約 6 KB**（`atom_array` 814 枠 × 4 B = 3,256 B、
`atom_hash` 512 枠 × 4 B = 2,048 B、ヒープに残る予定義 atom 16 個）。context 26,908 B のうち TypedArrays が
5,764 B で最大（§17）。

**§14.1 の「作成直後の 40.2 KB とホスト計算の差 4.8 KB」は計上の違いだった。** 実機の `js=` は QuickJS の
`malloc_size` で、確保 1 回ごとに `MALLOC_OVERHEAD`（8 B）を足す。`floor32` の `js=` は tlsf のブロック長の和で、
それを足さない。ホストの実機レイアウト版 vmrun で空のスクリプトを流すと `qjs_malloc_size=40,236`（実機 40,208〜
40,240）で一致し、差は床のブロック約 450 個 × 8 B ≈ 3.6 KB と `js_std` の helper 約 1.2 KB。

**tlsf が実際に払うヘッダは 4 B なので、160 KiB の上限はブロックあたり 4 B 多く数えている**（床だけで約 1.8 KB、
アプリのブロック数に比例して増える）。上限の意味を変える判断なので、ここでは直していない（backlog F0-c）。

### 16.2 何をしたか

`JS_InitAtoms` の `JS_ResizeAtomHash(rt, 512)` は予定義 atom 504 個を全部 hash に入れる前提の大きさで、F1 の後は
ヒープで hash に入るのは十数個しかない。ROM atom があるときは **64 枠から始め**、既存の規則（数が枠の 2 倍に
なったら倍）で伸ばす。同じ行の中の定数式で、ROM なしのビルドは 512 に畳まれる。

`atom_array` の ROM 範囲の空き枠（約 2.2 KB）は残した。33 箇所の `rt->atom_array[` を番号の付け替えで通す必要が
あり、1 KB 台のために触る範囲が広すぎる。

### 16.3 結果

| | 前 | 後 | 差 |
| --- | --- | --- | --- |
| 床（ホスト計算） | 35,444 | 33,652 | −1,792 |
| hello（実測(device)、`js=` ソース評価後） | 50,412 | 48,640 | −1,772 |
| imucal | 64,036 | 62,480 | −1,556 |
| pet | 67,288 | 65,796 | −1,492 |
| companion | 61,228 | 59,740 | −1,488 |
| Kasane デモ | 64,672 | 63,076 | −1,596 |

アプリの atom が増えると hash が 128・256 枠に伸びるので、床の差（1,792 B）より少し小さい。関所: コーパス
（asan・o2・ROM なし・`--force-yield`）78/78、Test262 退行 0。

## 17. F3b: 型付き配列のクラスを初めて使うときに作る（2026-09-26、`vm/f3-floor`）

`CONFIG_POCKET_VM_LAZY_INTRINSICS`（関所の後で既定 y）。

### 17.1 何をしたか

§4-A の群単位の遅延を、F2 の上で一番大きい TypedArrays（5,764 B）にだけ当てた。出荷アプリはどれも型付き配列の
コンストラクタを使わず、ファームが `pocket.fs`・`pocket.io`・`pocket.capture` で返す `Uint8Array` はネイティブ側で
作られる。

- `SharedArrayBuffer`・12 種の型付き配列・`DataView` のグローバル名は、`JS_AddIntrinsicTypedArrays` がコンストラクタを
  定義していた位置に **autoinit の束縛**として置く（キーの順序と属性は元のまま）。読まれたときにそのクラスの
  コンストラクタとプロトタイプを作って返す。
- **群ではなくクラスごとに作る。** autoinit の関数は解決中のオブジェクトを変えてはいけない
  （`JS_AutoInitProperty`）ので、`Uint8Array` の解決中に同じグローバルへ `Int8Array` を定義できない。クラスごとなら、
  `Uint8Array` しか見ないファームのアプリが払うのも 1 組で済む。
- ネイティブ側の入口: `class_proto` を読む 4 箇所（`JS_GetClassProto`、`JS_NewObjectClass`、`js_create_from_ctor` の
  2 経路）で JS_NULL を見たら作る。まだ束縛が autoinit のまま残っていれば**その束縛を解決する**ので、グローバルと
  `prototype.constructor` は即時版と同じく同一のオブジェクトになる。
- `ArrayBuffer`（ファームが返す）・`Atomics`・`%TypedArray%` 自身は即時のまま。`%TypedArray%.prototype.toString` は
  **元の** `Array.prototype.toString` と同一でなければならず、それを保証できるのは context 生成時だけ。
- `JSContext` に `ta_base`（`%TypedArray%`）を 1 つ足した。登録しない context（`JS_NewContextRaw` だけのもの）は上流の
  まま（プロトタイプは JS_NULL）。
- 変更は既存の行の末尾（`quickjs-vmprobe.h` のマクロ）とファイル末尾だけ。**n のビルドは F3a のコミットと
  `.text`・`.rodata`・`.data`・`.bss` が一致**（同じフラグでホストの gcc で `quickjs.c` を比べた）。

### 17.2 関所

| 検査 | 結果 |
| --- | --- |
| 床（ホスト計算） | 33,652 → **28,824 B**（−4,828） |
| コーパス（既定・o2・遅延なし・ROM なし・F2 なし・`o2-keepsrc`・`--force-yield`） | すべて **79/79**。新規 `corpus/lazy_intrinsics.js`: ネイティブが先に作る（`host.bytes` = `JS_NewUint8ArrayCopy`、しかも `prototype.constructor` を先に書き換えてから束縛を読む）、グローバルのキー順と属性、触る前の削除と上書き、`%TypedArray%` の同一性、静的プロパティ、species・継承・`DataView`・`SharedArrayBuffer`・`Atomics`。期待値は即時版の出力 |
| 確保番号で固定した OOM 回帰 3 件 | 遅延ありは context 生成の確保が 154 回（ROM なし 140、F2 なし 124）少ない。ready 以降の確保の大きさの列は一致するので、番号をその分ずらした行（`// vmrun-rom-lb-li-flags:` など、`run.sh` の `li` 札）を足した |
| Test262（既定 asan・o2・o2 `--force-yield`） | すべて **退行 0**（`$262.createRealm` の別 realm から `new.target` 経由で作る試験を含む） |
| 負の対照（`tools/vmtest/floor/f3_faults.sh`） | 束縛を解決せず横に作る・ネイティブ経路の判定なし・`toString` の別名なし・束縛が列挙可能、の **4 種すべて検出**。最初の 1 種は、束縛を読む前に `prototype.constructor` を書き換える試験を足すまで検出されなかった |
| 確保失敗の総当たり（`lazy_intrinsics.js` の 1,159 回すべて） | ASan・リーク・assert・異常終了 **0**。失敗を注入しても正常終了した 95 回は、出力がすべて通常と一致 |

### 17.3 実機（実測(device)、FLOORPROBE ビルド、`js=` ソース評価後）

| アプリ | F3a のみ | F3a+F3b | F3b の差 | F2-5 の後（§15）からの差 |
| --- | --- | --- | --- | --- |
| hello | 48,640 | 42,752 | −5,888 | −7,660（−15%） |
| imucal | 62,480 | 56,612 | −5,868 | −7,424 |
| pet | 65,796 | 59,928 | −5,868 | −7,360 |
| companion | 59,740 | 53,892 | −5,848 | −7,336 |
| Kasane デモ | 63,076 | 57,356 | −5,720 | −7,316 |

ゲスト作成直後の `js=` は 40,240 → 32,580。既定の設定（F3b y、計測なし）で `smoke_device.py --cycles 20`・
故障回復 6 種・`test_settings.py` 通過、`memlog --check` 予算内（アプリ実行中の空き `app_free` 179,956、
最大空きブロック 139,264。§13.4 の F2 の後は 172,700 と 131,072）。静的 DRAM は不変、書き込むイメージは
コード +356 B・rodata +368 B。

**実機で確かめていないこと**: ネイティブが `Uint8Array` を先に作る経路（`pocket.fs` の read など）は、ホストで同じ
`JS_NewUint8ArrayCopy` を通して確かめただけで、それを使う出荷アプリが無いので実機では通っていない。

### 17.4 次の候補

同じ仕組みで Map/Set（1.8 KB）・DOMException（1.4 KB）・WeakRef（0.7 KB）も遅延にできる（ホスト計算、F2 の後の
§16.1 の floor32 の各行）。Promise は async 関数が内部で使うので対象外。→ §18。

## 18. F3c: Map/Set・WeakRef・DOMException も初めて使うときに作る（2026-09-26、`vm/f3c-lazy-intrinsics`）

`CONFIG_POCKET_VM_LAZY_INTRINSICS`（F3b と同じ設定、既定 y）の対象を広げた。

### 18.1 何をしたか

- F3b の仕組みを「群」に一般化した。`JS_AddIntrinsicMapSet`・`JS_AddIntrinsicWeakRef`・`JS_AddIntrinsicDOMException` の
  先頭（既存の行）で、残りの代わりに `js_lazy_register` を呼ぶ。グローバルには元と同じ位置・順序・属性の autoinit
  束縛を置き、`ctx->lazy_groups` にその群を登録したことを記録する。
- 対象: `Map`・`Set`・`WeakMap`・`WeakSet`、`WeakRef`・`FinalizationRegistry`、`DOMException`。WeakRef と DOMException の
  **クラス登録（ランタイム側）は登録時に即時**のままで、待つのはコンストラクタとプロトタイプの組だけ。
- **Map/Set のイテレータのプロトタイプ**（`JS_CLASS_MAP_ITERATOR`・`SET_ITERATOR`）はグローバル名を持たない。
  初めて `entries()`/`values()` を呼んだとき、つまり `JS_NewObjectClass` の判定（F3b の 4 箇所の 1 つ）で作る。
- DOMException は即時版が失敗をすべて無視していた（context 生成は OOM カナリアで後から検査する）。遅延版は
  途中で失敗したら組ごと捨てる。
- 変更は既存の行とファイル末尾だけ。**n のビルドは vm/main と `.text`・`.rodata`・`.data`・`.bss` が一致**（同じ
  フラグでホストの gcc で `quickjs.c` を比べた）。

### 18.2 関所（ホスト）

| 検査 | 結果 |
| --- | --- |
| 床（ホスト計算、実機レイアウト） | 28,824 → **24,928 B**（−3,896）。floor32 の MapSet・DOMException・WeakRef の行はどれも 0 B（束縛の分だけ） |
| 空のスクリプトの `js=`（実機レイアウト版 vmrun、`malloc_size`） | 32,832 → **28,448** |
| コーパス（既定・o2・遅延なし・ROM なし・F2 なし・`o2-keepsrc`・`o2-noli-nolb-keepsrc`・`--force-yield`） | すべて **79/79**。`lazy_intrinsics.js` に、触る前の削除と上書き、束縛の属性、Map のイテレータのプロトタイプ連鎖、`Map.groupBy`、Set の集合演算（内部で Set を作る）、WeakMap・WeakRef、DOMException の定数 25 個と継承を追加。期待値は遅延なしの出力 |
| 確保番号で固定した OOM 回帰 3 件 | context 生成の確保が遅延なしより 270 回（F3b だけの時は 154 回）少ない。`li` の行を付け直した |
| Test262（既定 asan・o2・o2 `--force-yield`） | すべて **退行 0** |
| 負の対照（`f3_faults.sh`） | 既存 4 種＋新規 1 種（`JS_NewObjectClass` の判定なし → イテレータのプロトタイプが null）の **5 種すべて検出**。既存 2 種は F3c の書き換えで当てる場所が消え、そのままだと黙って走らなくなるところだった（`patch failed` で止まるので気づいた） |
| F2 の負の対照 | 7 種すべて検出 |
| 確保失敗の総当たり（`lazy_intrinsics.js` の 1,665 回すべて） | ASan・リーク・assert・異常終了 **0**。失敗を注入しても正常終了した 195 回は出力がすべて通常と一致 |
| ファームのビルド | 通る。静的 DRAM は不変 |

**実機の関所は未**（COM3 を Kasane の線に譲っている間。backlog F3c）。F3b の実機の差（−5.9 KB）と床の差（−4.8 KB）
の比から、アプリの `js=` は 4〜5 KB 減る見込み（推定）。