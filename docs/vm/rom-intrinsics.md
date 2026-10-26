# ROM intrinsics: 固めた組み込みオブジェクトを flash に置く（考察と設計、2026-10-26）

**状態: 設計の考察。実装は無い。** F 系列（[builtin-floor-plan.md](builtin-floor-plan.md)、F1 ROM atom・F2 遅延索引・F3 群の遅延）の次の段で、
`vm/harden`（アプリの context を「生まれた時点で読み取り専用の intrinsic」にする線。設計書は未着手）に依存する。
数字はすべて **実測(host, 実機レイアウト)**（`-m32 -malign-double`、JSValue 8 B・ポインタ 4 B、tlsf のブロック長で課金）／
**実測(device)**（他文書からの引用、出典つき）／**計算**／**推定** のどれかを付ける。再現は §11。

## 1. 結論（先に数字）

### 1.1 かかっている RAM

| 項目 | 値 | 区分 |
| --- | --- | --- |
| F1〜F3 後の床: runtime + `JS_NewContext` | **24,512 B**（runtime 6,608 ＋ context 17,904。context は 253 ブロック） | 実測(host, 実機レイアウト)、`floor32.sh`・`rom_walk.sh` |
| 同、実機の `js=`（確保ごとに `MALLOC_OVERHEAD` 8 B を足す数え方） | 28,112 B（ゲスト作成直後） | 実測(device)、plan §18.3 |
| context 17,904 B のうち **harden の下で不変 → flash に置ける分** | **約 14.0 KB**（組み込みオブジェクト 76 個の本体 3,648 ＋ prop 配列 2,040 ＋ 専有 shape 約 4,860 ＋ 関数名の文字列 1,120 ＋ F2 の遅延表 2,304） | 計算（§2.1 の内訳から） |
| context のうち **RAM に残る分** | 約 3.9 KB（global object 2,144、`global_var_obj` 416、`class_proto` 配列 520、`JSContext` 328、新規オブジェクト用の雛形 shape 424） | 計算 |
| アプリが起動後に触ったメソッドの分（今は触るたびに RAM に materialize） | DERBY の 27 名で **+3,968 B（93 ブロック）**、出荷アプリの和集合 52 名で **+7,260 B（168 ブロック）**、`Math` 全部で +6,476 B | 実測(host, 実機レイアウト)、§2.2 |
| ROM 化で DERBY のゲストから消える見込み | 床 14.0 KB ＋ ブロックヘッダ分 約 1.9 KB ＋ 触った分 約 4.7 KB ≈ **約 20 KB** | 推定（host の実測の足し算。実機の `js=` で確認が要る） |
| 完全に materialize した intrinsic グラフ（= 焼く snapshot の中身） | オブジェクト 639（うち C 関数 559）、shape 117、prop 配列、関数名 533 → **raw 64,264 B ＋ 文字列 11〜15 KB ≈ 75〜80 KB の flash** | 実測(host)の raw サイズ ＋ 推定 |

DERBY WATCH のゲスト最大 134.1〜134.8 KB（140,000 B の線まで約 5.2 KB、CLAUDE.md、実測(device)）に対して、約 20 KB は**線の位置を変える額**である。
評価のピークの余裕（約 28.9 KB）も、床は評価より前に払うので同じ額だけ広がる（計算）。

### 1.2 採る設計（§5 の比較、§6 の決定）

**(a′) ビルド時 snapshot。harden した intrinsic グラフを、全メソッドの関数オブジェクトまで含めて C の `const` データとして
flash に焼く。ROM のオブジェクトは「不死」で、GC リストに載らない。** 違いの核心は 2 つ:

1. **参照数は符号で不死を表す。** ROM の `ref_count` は負の定数。`JS_FreeValue` は「減らしてから店に書く」順を「減らして、0 以下なら分岐、
   それ以外を書く」に組み替えると、RAM のオブジェクトでは**命令数が今と同じ**で、ROM では書かずに戻る。`js_dup` は符号を見る分岐が
   **1 命令**増える（§4.1、§7.1）。アドレス範囲の判定は hot path には置かない。
2. **C 関数オブジェクトの `realm` は NULL。** 呼び出し側の context を使う（1 context しか無い。§4.5）。これで snapshot は RAM への
   ポインタを 1 つも持たず、完全に `const` になる — **host では `.rodata` が読み取り専用ページに載るので、見落とした書き込みは
   SEGV として必ず出る。** 検出器を別に作らずに済む（F1 の N1b と同じ流儀）。

副産物として F2（遅延索引）・F3（群の遅延）・F1 の脱出キャッシュ `rom_cache` は engine の intrinsic については要らなくなる（§6.7）。
ファームの `pocket.*` の面は RAM のまま（capability の値が実機ごとに違う。§9 の段 R4）。

### 1.3 代償とリスク（§7、§10）

- **実行時**: `js_dup` が +1 命令（DERBY のターンで +0.5〜1% の退役命令、推定）。`Array.prototype.push` や `Math.sin` の探索が
  **flash（D キャッシュ経由）の読み**になる。QIO の STRESS で `D_STALL` は 63 k サイクル/ターン（4%、実測(device)、turn-cpi §3）で、
  ここに hot な ROM 行の分が足される。最悪（毎ターン冷えている）で DERBY の 27 名 × 6 行 × 450 サイクル ≈ 73 k サイクル ≈ **0.3 ms/ターン**
  （推定）、温かければ 0 に近い。**実機の `D_STALL` でしか決まらない。**
- **正しさ**: flash への store は ESP32-S3 では cache が拒否して panic する（plan §4-C）。harden が「拒否は最初の store より前」を
  **すべての**書き込み経路で守っていることが前提で、host の読み取り専用ページがそれを検出する唯一の網。`is_prototype` や
  `extensible` のような「同じ値をもう一度書く」store も拒否される（§4.9）。
- **未検証**: 実機では何も測っていない。harden の実装も未マージで、ここで仮定した意味（§0）が最終形かは分からない。
  mJS と Espruino の ROM 化の詳細は一次資料で確認できていない（§3）。

## 0. 前提: harden が約束すること（この文書の仮定）

`vm/harden` の規則（発注者の説明。ブランチは本文書の時点で `a77c47de` から差分なし）:

1. 文字列からのコード生成なし（`eval`、`Function` コンストラクタ）。
2. 生成後の [[Prototype]] 変更なし（`Object.setPrototypeOf`、`Reflect.setPrototypeOf`、`__proto__` setter）。
3. intrinsic は生まれた時点で読み取り専用: 自身のプロパティは non-writable・non-configurable、オブジェクトは non-extensible、
   グローバル束縛は non-writable。**属性で表し、freeze の走査はしない**（F2 の遅延を壊さないため）。継承した読み取り専用
   プロパティを自分のオブジェクトで隠すのは許す。失敗した書き込みは必ず投げる。
4. species は無視する。

この文書が足す要求は 1 つ: **拒否は、対象のオブジェクト・shape・prop 配列への最初の store より前に起きる**こと（§4.9）。
属性の検査は QuickJS ではもともと store より前にある（`JS_DefineProperty` の `!p->extensible`、`delete_property` の
`JS_PROP_CONFIGURABLE` 検査、`JS_SetPropertyInternal2` の writable 検査）が、§4.9 に挙げる「無条件の store」が数か所ある。

## 2. 何が RAM にあるか（実測(host, 実機レイアウト)、`tools/vmtest/floor/rom_walk.sh`、2026-10-26、`a77c47de`）

`quickjs.c` を `#include` して `rt->gc_obj_list`・`rt->lazy`・各 shape を歩く（`floor_parts.c` と同じ流儀）。
課金は実機のゲストと同じ tlsf のブロック長（4 B 切り上げ、最小 12 B）。実機の `js=` はさらに確保 1 回あたり 8 B を足す（plan §16.1）。

### 2.1 生まれたての context（`JS_NewContext` 直後、ソース 0 B）

```
sizeof: JSValue=8 ptr=4 JSObject=48 JSShape=48 JSShapeProperty=8 JSProperty=8 JSString=20 JSContext=328 class_count=65
runtime js=6608 blocks=21 | context js=17904 blocks=253 | total 24512
fresh (no global)  obj 76 (3648 B) props 2040 B | shapes own 53 (6536 B) shared 5 (424 B) | cfunc 39 (2792 B) | strings 36 (1120 B)
  global_obj: shape 1328 B props 768 B (66 props) | global_var_obj 416 B
  lazy table 2304 B (43 lists, 96 slots) | class_proto 520 B | JSContext 328 B
```

| context 17,904 B の内訳 | B | ROM へ | 備考 |
| --- | ---: | --- | --- |
| 組み込みオブジェクト 76 個（`JSObject` 48 B） | 3,648 | ○ | prototype・コンストラクタ・`Math` 等 55 ＋ C 関数 39（即時に作られるもの: コンストラクタ、accessor、`throw_type_error`…）＋ 特殊 class 6 |
| その prop 配列 | 2,040 | ○ | F2 の `compact_properties` 後 |
| その専有 shape（`ref_count` 1、unhashed） | 約 4,860 | ○ | 走査の 6,536 から global の shape 1,328 と `global_var_obj` の分を除いた値（計算） |
| 関数名の文字列（`name` の値、重複除く 36） | 1,120 | ○ | F1 の ROM atom を値に出したもの。`rom_cache` 経由で共有 |
| F2 の遅延表 `rt->lazy`（43 リスト、96 枠 × 24 B） | 2,304 | 消える | ROM では全部 materialize 済みなので表そのものが不要 |
| global object（本体 48 ＋ shape 1,328 ＋ prop 768、66 プロパティ） | 2,144 | × | アプリが `var`/関数宣言で足す。RAM |
| `global_var_obj` | 416 | × | `let`/`const` の置き場 |
| `ctx->class_proto[65]` | 520 | × | ROM を指す RAM の配列（context ごと） |
| `JSContext` | 328 | × | |
| 雛形 shape（`array_shape`・`arguments_shape`・`regexp_shape`…、hashed、5 個） | 424 | ×（残す） | 新しい RAM オブジェクトが `js_dup_shape` で参照数を書くので、ROM にすると §4.1 の判定が shape 側にも要る。424 B のために触らない |
| 合計の確認 | 17,804 ≒ 17,904 | | 残り約 100 B は遅延表の周辺（未分解） |

**動かせるのは約 14.0 KB（78%）、残るのは約 3.9 KB。** runtime 側 6,608 B（atom 表 3,512、heap に残る定数 atom 16 個、`JSRuntime`、
shape のハッシュ表 256）は対象外。

### 2.2 アプリが触ると増える分

`rom_walk <script>` で、組み込みの名前を読むだけのスクリプトを評価し、GC の後に差分を取る。触る名前は出荷アプリの grep から
作った（`tools/vmtest/floor/rom_touch/`。受け手の型は手で当てたので近似）。

| 台本 | 名前 | `js=` の増分 | ブロック | 内訳 |
| --- | ---: | ---: | ---: | --- |
| `derby.js`（`apps/derby/*.js` の和集合） | 27 | **+3,968 B** | 93 | C 関数オブジェクト 27 個 1,736（48 ＋ prop 16）、名前の文字列 26 個 708、`Math`・`Array.prototype` の prop 配列の伸び 620、shape の伸び 328 |
| `typical.js`（出荷アプリ全部の和集合） | 52 | **+7,260 B** | 168 | C 関数 51 個 3,344、文字列 1,376、prop 1,204、shape 792 |
| `all_math.js`（`Math` の全プロパティ） | 44 | +6,476 B | 137 | |

**メソッド 1 つ ≈ 147 B ＋ 3.4 ブロック**（実機の `js=` では ≈ 175 B）。これは F2 が「触ったものだけ」に絞った後の額で、
ROM 化では**全部 0** になる（関数オブジェクトも名前も prop 配列も flash）。

### 2.3 グラフ全体（= snapshot の中身）

F2 の全展開（`js_lazy_all`）だけでは項目が AUTOINIT の印として shape に入るだけで、関数オブジェクトは初めて読んだときに作られる。
そこで全オブジェクトの全プロパティを `JS_GetOwnProperty`（getter を呼ばずに AUTOINIT を解決する）で読んだ:

```
FULL + AUTOINIT resolved: context 98064 B (2039 blocks); bytecode builtins 8
resolved (no global)  obj 639 (30672 B) props 15688 B | shapes own 113 (19320 B) shared 4 (336 B) | cfunc 559 (37312 B) | strings 533 (16728 B)
  raw obj+prop+shape 64264 B
```

- **RAM に全部作ると 98 KB、2,039 ブロック**（= F 系列の前の世界に近い。plan §2 の 64 KB は AUTOINIT のまま数えた値）。
- flash に置く量（tlsf の切り上げ無し、shape のハッシュ表はプロパティ数に合わせて右寄せ）: オブジェクト 639 × 48 ＝ 30,672、
  prop 配列（`prop_count` ぶんだけ）約 10.6 KB、shape 117 個 約 23 KB → **64,264 B**（実測(host)の raw 和）。
- 関数名の文字列 533 個: 別の `JSString` として焼けば 20 B ＋ 文字 ≈ 15 KB。F1 の ROM atom 表（文字 5,713 B）と**統一**して
  「ROM atom ＝ flash の `JSString`」にすれば、増分は 542 × 20 B ＝ 10.8 KB で、`rom_cache` も要らなくなる（§6.7）。
- **bytecode で定義された組み込みが 8 個**（`Array.fromAsync`、`Iterator.zip`、`Iterator.zipKeyed` の 3 つの AUTOINIT_BYTECODE から
  作られる関数）。bytecode は IC と T1 が書き込むので ROM に置けない（plan §4-B）。§6.5。
- 内訳: 普通のオブジェクト 55、`Array.prototype`（fast array）1、`Error.prototype` 1、`Number/String/Boolean.prototype`（object_data）各 1、
  C 関数 559、`JS_CLASS_C_FUNCTION_DATA`（`Iterator.prototype.constructor` の accessor）1。`Date.prototype`・`RegExp.prototype`・`Promise.prototype`・
  `Map.prototype` 等は普通のオブジェクト。

### 2.4 実機の `js=` との対応

実機は確保ごとに 8 B 多く数える（plan §16.1）。生まれたての context は 253 ブロックで、ROM 化後に RAM に残るブロックは十数個なので、
**ブロックヘッダ分だけで約 1.9 KB** が実機の `js=` から消える（計算）。§1.1 の「約 20 KB」はこれを含む。

## 3. 先行例

| エンジン | 何をしているか | 確認できたこと・できなかったこと |
| --- | --- | --- |
| **XS（Moddable）** | **preload**: モジュールの本体をリンク時に実行し、できたクロージャとオブジェクトを ROM に置く。「built-in の prototype、全関数（class のコンストラクタ含む）、preload した class の prototype を**リンカが自動で freeze**」。書けるかもしれないオブジェクトには **RAM に 1 ポインタ（4 B）**を持ち、変更は RAM の alias に書く。freeze されたものはポインタを持たない。「built-in のインスタンスの多くは preload できるが alias できない: preload 後は不変で、`Map.prototype.set` や `Date` の setter は TypeError」。リンカは「not frozen」「no const」を警告する | [preload.md](https://github.com/Moddable-OpenSource/moddable/blob/public/documentation/xs/preload.md)、[XS linker warnings.md](https://github.com/Moddable-OpenSource/moddable/blob/public/documentation/xs/XS%20linker%20warnings.md) を読んで確認。XS は参照数を持たない（mark-sweep）ので ROM への store の問題が構造的に無い。「ROM Colors」（ROM 上のオブジェクトの探索を速くするリンカの最適化）は文書索引の記述を見ただけで本文は未確認 |
| **MicroQuickJS（Bellard、2025-12）** | 「標準ライブラリ全体が ROM にあり、コンパイル時に生成される」（`mquickjs_build.c` が C の構造体に落とす）。「プロパティは標準ライブラリのオブジェクトについては ROM に置ける」「C 関数は 1 つの値として格納でき、その場合プロパティを足せない」。**GC は参照数ではなく tracing・compacting**。bytecode も `JS_RelocateBytecode()` の後に ROM から実行できる。10 kB RAM・約 100 kB ROM | [README](https://github.com/bellard/mquickjs)、[NuttX の文書](https://nuttx.apache.org/docs/latest/applications/interpreters/mquickjs/index.html) で確認。**QuickJS とは別の engine**（書き直し）で、参照数を捨てたことが ROM 化を自然にしている。ROM オブジェクトへの書き込みが何を起こすかは文書に無い |
| **Espruino** | フォーラムの回答で「組み込みの変数・関数はシンボル表に置かない（RAM が無い）」「シンボル表の多くを flash に移した実験ビルドで jsvars が増えた」。`jswrapper.c` にビルドが生成するシンボル表 | [Internals](https://www.espruino.com/Internals) とフォーラム（[1](https://forum.espruino.com/comments/6815)、[2](https://forum.espruino.com/conversations/372543)）。**一次資料（ソース）で未確認**。Espruino の組み込みはそもそも JS オブジェクトとして RAM に存在しないので、ここでの問いとは形が違う |
| **mJS（Cesanta）** | 約 25 KB flash・1 KB 未満の RAM と称する | 組み込みの置き方は**何も確認できなかった** |
| QuickJS の fork | ROM 化した fork は検索で見つからなかった。本家の `JS_READ_OBJ_ROM_DATA` は「IC で壊れた、obsolete」で 0（plan §3） | |

結論: **参照数を持つ engine で intrinsic を ROM に置いた先例は見つからなかった。** XS と mquickjs は tracing GC なので「ROM に書かない」が
自然に成り立つ。QuickJS では参照数の store（§4.1）が唯一の本質的な障害で、他は cold path の個別対応である。

## 4. QuickJS の前提と、読み取り専用の flash で壊れるもの

`components/quickjs-ng/quickjs-ng/quickjs.c`（`a77c47de`）の行番号。「侵襲」は触る箇所の数で表す。

### 4.1 参照数（`JSGCObjectHeader.ref_count`、`JSRefCountHeader`）

**前提**: `js_dup`（1760 行）は `p->ref_count++`、`JS_FreeValueRT`（8248 行）は `if (--p->ref_count <= 0) js_free_value_rt`。
ROM の値（`Math`、`Array.prototype.push`、関数名の文字列）がスタックに載るたびに store が起きる。`Math.sin(x)` 1 回で dup 2・free 2。

**変更**: ROM の `ref_count` を**負の定数**（例 `INT32_MIN / 2`）にし、

```c
/* js_dup: +1 instruction (a sign test) on the RAM path */
int rc = p->ref_count;
if (likely(rc >= 0)) p->ref_count = rc + 1;

/* JS_FreeValueRT: same instruction count as today on the RAM path. Today:
   load, add, STORE, branch. Here: load, add, branch, store -- the store
   moves after the branch the code already has, and a negative (immortal)
   count takes the branch and stores nothing. */
int rc = p->ref_count - 1;
if (unlikely(rc <= 0)) { if (rc == 0) { p->ref_count = 0; js_free_value_rt(rt, v); } return; }
p->ref_count = rc;
```

plan §4-D は「全値の操作にアドレス判定を足す」のを hot path の理由で捨てたが、ここでの判定は**すでに load している値の符号**で、
アドレス比較ではない。free は 0 命令、dup は 1 命令（Xtensa の `bltz`、前方・通常は不成立）。退役命令で測れる（§7.1）。

`ref_count` を読んで判断する他の箇所（62 の書き手、約 30 の読み手を grep）: `js_shape_prepare_update`（12551 行、hashed のときだけ
`!= 1`）、文字列の in-place 連結（6101 行、`== 1`）、`JS_FreeRuntime` の leak 検査（2766・2779 行、リストの要素だけ）、GC（§4.2）。
負の値は `== 1` を満たさないので in-place の最適化は自然に避けられる。

**侵襲**: hot path 2 関数（`js_dup`・`JS_FreeValueRT`）＋ `js_dup_shape`（6438 行、雛形 shape を RAM に残せば不要）＋ GC の 3 コールバック（§4.2）。

### 4.2 GC のリストと mark ビット

**前提**: すべての GC オブジェクトは `rt->gc_obj_list` に載り、`gc_decref`（8428 行）が全要素の子の `ref_count` を減らして `mark = 1` を書き、
`gc_scan` が戻す。子に向かう 3 つのコールバック（`gc_decref_child` 8418、`gc_scan_incref_child` 8450、`gc_scan_incref_child2` 8462）は
`p->ref_count` を書く。

**変更**: ROM オブジェクトはリストに載せない（走査されない）。RAM の親から ROM の子へ向かうコールバックは、§4.1 と同じ符号で
`if (p->ref_count < 0) return;` と戻る（3 箇所、GC 時のみ）。`gc_scan_incref_child` の「`ref_count == 1` なら `gc_obj_list` に戻す」は負の値では
起きない。`mark_children` 自体は ROM の親に対しては呼ばれない（リストに無い）。`JS_RunGC` の assert（`p->ref_count > 0`）はリストの
要素だけ。

**侵襲**: 3 箇所 ＋ 1 行ずつ。cold。

### 4.3 shape（hashed・共有・`js_shape_prepare_update`）

**前提**: prototype・コンストラクタの shape は `JS_NewObjectProtoClassAlloc`（6932 行）で作られ **unhashed**（上流のコメント
"WARNING: the shape is not hashed"）。関数オブジェクトの shape（`length`・`name`）は hashed で共有される（2.3 で共有 4 個）。
`js_shape_prepare_update`（12543 行）は hashed かつ `ref_count != 1` なら clone、unhashed なら**その場で書く**。`add_shape_property`
（6618 行）は unhashed shape に直接書く。

**変更**: ROM の shape はすべて `is_hashed = 0`、`rt->shape_hash` に入れない。→ `find_hashed_shape_*` は ROM の shape を返さないので、
新しい RAM オブジェクトが ROM の shape を共有することはない（RAM の関数は RAM の hashed shape を使う。ROM と RAM で `length`・`name` の
shape が 2 系統になるだけ）。`js_shape_prepare_update` が ROM の shape に達するのは「ROM オブジェクトのプロパティを変える」ときだけで、
harden がその前に拒否する（§4.9）。shape の `proto` は ROM のオブジェクトを指すポインタ（const）。

**侵襲**: engine のコードは 0 行。生成器が unhashed・負の `ref_count` で出すだけ。**負の対照**: ROM の shape に `add_shape_property` を
通す経路（例: `Math.x = 1` を harden 無しで）を host で走らせ SEGV することを一度見せる。

### 4.4 `JSProperty` の値が heap のオブジェクトを指す

**前提**: prop 配列の `u.value` は `JSValue`（ポインタ込み）。ROM の prop 配列が RAM を指すと snapshot が `const` にならない。

**変更**: ROM のグラフは**閉じている**必要がある。生成器は到達可能なすべての値が ROM か即値（int・double・bool・undefined・ROM 文字列・
定数 atom の symbol）であることを検査し、違えば生成を失敗させる。RAM を指すものは 3 種で、いずれも §6 で消す: `realm`（§4.5）、
AUTOINIT の realm（§4.8）、bytecode の組み込み（§6.5）。**global object は ROM に入れない**ので「global → ROM」の向きだけ残り、逆は無い。

### 4.5 C 関数の `realm`

**前提**: `JS_NewCFunction3`（7231 行）が `p->u.cfunc.realm = JS_DupContext(ctx)` を保存し、`js_call_c_function`（19500 行）が
`ctx = p->u.cfunc.realm` で realm を切り替える。`JS_GetFunctionRealm`（23566 行）、Math の速い経路（19677・19689 行）も読む。
`js_c_function_mark`・`finalizer`（7986・7996 行）は既に `if (realm)` で守られている。

**変更**: ROM の関数は `realm = NULL`。読む 3 箇所で `realm ? realm : ctx`。このファームは runtime に context が 1 つしか無いので、
意味は変わらない（複数 realm の `Array.prototype.map` が呼び手の realm の配列を作る、という差は現れない）。

**侵襲**: 3 箇所に `?:` を足す。native 呼び出しの prologue に 1 分岐（`js_call_c_function` は 586 B の関数で、これは誤差）。

### 4.6 weak 参照

**前提**: `JSObject.first_weak_ref` は WeakRef・WeakMap/WeakSet・FinalizationRegistry が対象に**書く**リンク（58386 行付近）。
WeakMap の値は鍵の `mark_children` → `mark_weak_map_value` 経由でしか mark されない。

**変更**: ROM のオブジェクトを weak の対象にすることを**拒否する**（TypeError。符号で判定、3 入口）。不死のオブジェクトへの weak 参照は
意味上 strong 参照と同じで、許すなら「鍵が ROM なら値を strong に mark する」特例が map の mark に要る。harden の「驚きを足す挙動は消す」に
合わせて拒否を採る。

**侵襲**: 3 箇所。cold。

### 4.7 F2 の遅延 materialize

**前提**: `js_lazy_touch`（44625 行）は外れたときに関数リストの項目を shape と prop 配列に**書き足す**。`rt->lazy` が対象をアドレスで持つ。

**変更**: ROM のグラフは**全展開済み**なので、engine の intrinsic に F2 は要らない。`JS_SetPropertyFunctionList` が遅延登録するのは
ファームの `pocket.*` の面（RAM、`lazy_eligible` 44871 行）だけになる。snapshot の生成は F2 の全展開（`js_lazy_all`）と AUTOINIT の解決を
host で一度だけ走らせて行う（2.3 の手順そのもの）。

**侵襲**: 0 行（生成器が使う）。将来 `pocket.*` も snapshot にすれば F2 は丸ごと消せる（§9 R4）。

### 4.8 AUTOINIT の記録

**前提**: `JS_PROP_AUTOINIT` の prop は `realm_and_id`（RAM の context へのポインタ）と `opaque` を持ち、初回の読みで
`JS_AutoInitProperty`（10304 行）が shape の flags と prop の値を**書き換える**。

**変更**: snapshot には AUTOINIT を**残さない**。生成器がすべて解決する（2.3 で「AUTOINIT slots left 0」）。残るのは bytecode の 3 枠（§6.5）。

### 4.9 見つけた「無条件の store」と、その他

| 箇所 | 何を書くか | 処置 |
| --- | --- | --- |
| `set_proto` 9958 行: `proto->is_prototype = true` | RAM のオブジェクトの prototype に ROM を指定（`Object.create(Math)`、`class extends Array`）すると **ROM の proto に書く** | snapshot は全オブジェクトを `is_prototype = 1` で焼き、store を `if (!proto->is_prototype)` で囲む（同じ値でも store は拒否される） |
| `JS_PreventExtensions` 11141 行: `p->extensible = false` | `Object.freeze(Math)`・`preventExtensions` が無条件に書く | `if (p->extensible)` で囲む。`Object.freeze` の残り（flags の再定義）は `js_update_property_flags` が「違うときだけ」書くので、harden 済みの flags なら store 無し |
| `JS_WriteObjectRec` 42662 行: `p->tmp_mark = 1` | 直列化。ファームでは `JS_WriteObject` は使っていない（`JS_ReadObject` だけ、`app_session.c:1323`） | 符号で ROM を飛ばす（ROM は参照で書く、または直列化を拒否） |
| `ctx->std_array_prototype`（9963・11472・11526 行） | `Array.prototype` へのインデックス追加・proto 変更で false にする | ROM では起きない。harden の下で**常に true と証明できる**（T1 の素材、§8） |
| `JS_NewAtomStr` → `__JS_NewAtom` 3886 行 | 文字列値を鍵に使うと、その `JSString` を atom に**昇格して書く**（`atom_type`・`hash_next`） | ROM の文字列はすべて F1 の ROM atom 表にある名前なので、`js_rom_find` が先に当たり昇格は起きない（3906 行、F1 が入れた順序）。生成器は「ROM の全文字列が ROM atom 表にある」を検査する |
| `JSString.first_weak_ref`（symbol の weak） | ROM の文字列は symbol ではない | 対象外 |
| 雛形 shape の `js_dup_shape` | 新規オブジェクト生成ごと | RAM に残す（§2.1） |
| `JSContext` の `error_stack_trace_limit`・`error_prepare_stack` | `Error.stackTraceLimit` の accessor が書く先は ctx（RAM） | 問題なし。harden が `Error.stackTraceLimit = n` を許すかは harden 側の決め |

**検出器**: host では snapshot が `const` → `.rodata` → 読み取り専用ページ。見落とした store はどれも SEGV。負の対照として、上の処置を
1 つ外したビルドで該当のコーパスが SEGV することを示す（F1 の N1b・F2 の `f2_faults.sh` と同じ形）。実機の cache エラー（panic）は
最後の網で、頼らない。

## 5. 候補の比較

| 案 | RAM の減り（計算） | flash | 実行時 | 正しさの危険 | 判定 |
| --- | --- | --- | --- | --- | --- |
| **(a) 全 snapshot、不死（参照数の番兵、GC リスト外）** | 床 −14.0 KB ＋ ブロック −1.9 KB ＋ 触った分 −3.9〜7.3 KB（DERBY 約 20 KB） | +75〜80 KB（推定、§2.3）。統一しなければ文字列 +15 KB の側 | dup +1 命令、free ±0、探索が flash 読み（§7） | 書き込み経路の網羅（§4.9）。host の `const` で検出可能 | **採る** |
| (b) shape と prop 配列だけ flash、オブジェクト本体は RAM | 本体 76 × 48 ＝ 3.6 KB が RAM に残り、床の減りは約 10.4 KB。**ただし** prop 配列の値が指す関数オブジェクトは本体ごと flash（でないと 559 × 48 ＝ 27 KB の RAM）→ 参照数の判定は結局要る。関数を RAM の遅延（F2）に残すなら shape が RAM に要り（F2 は shape に書く）、flash に置けるのは何も残らない ＝ 今日の F2 | (a) より小 | (a) と同じ判定が要る | (a) と同じ | 捨てる: 判定を避けられず、RAM だけ 3.6 KB 損 |
| (c) copy-on-touch（XS の alias） | (a) と同じ床 | (a) ＋ alias 表（オブジェクトごとに 4 B、約 2.5 KB RAM） | **探索ごとに alias 表の参照**が hot path に入る（XS が払っている値段） | 書けるので検出の必要は減る | 捨てる: harden が書き込みを禁じるので、alias は何も買わない |
| (d) (a) ＋ ROM atom ＝ flash の `JSString` の統一 ＋ F2/F3/`rom_cache` の撤去 | (a) ＋ `rom_cache` 32 枠 ＋ F3 の束縛分（数百 B） | 統一の増分 +10.8 KB（文字 5.7 KB は既存の表から移る） | `typeof` の答えなどの脱出が**確保 0** になる（今は 32 枠のキャッシュで 96% を吸っている。plan §14.2） | F1 の脱出経路の台帳を書き直す | **(a) に含めて採る**（§6.7） |

(a) と (d) は同じ設計の段で、§6 では合わせて (a′) と書く。

## 6. 決定（RD1〜）

**RD1. 不死は参照数の符号で表す**（§4.1）。アドレス範囲の判定（flash の DROM 0x3C00_0000〜）は、生成器の検査と負の対照にだけ使う。

**RD2. ROM の関数は `realm = NULL`、呼び手の context を使う**（§4.5）。runtime に context が 2 つ以上ある構成は、このファームには無い。
`JS_NewContext` を 2 回呼ぶ（test262 の `$262.createRealm`）host の検査では、ROM の関数が呼び手の realm で動く差が出うる。
harden 版の test262 の基準線で確認する（§9 R2）。

**RD3. RAM に残すもの**: `JSContext`、`class_proto[]`、global object、`global_var_obj`、雛形 shape 5 個、`throw_type_error` 以外のすべての
ctx の値は ROM を指す。`JS_NewContext` は「ROM の番地を ctx に配る ＋ global object を作る」だけになり、組み込みの生成コード
（`JS_AddIntrinsic*` の大半）は **host の生成器にだけ要る**（ファームのバイナリから落ちる。flash の +75 KB の一部を相殺する。
額は未計測）。

**RD4. snapshot は C ソースとして生成する**（バイナリ blob ではない）。フィールド名の指定初期化子で出すので、host 64-bit と実機 32-bit で
**同じ生成物**がコンパイルできる（レイアウトはコンパイラが決める）。ポインタは `&rom_obj[k]` のシンボル参照、C 関数は**関数名**。
関数名はポインタから `nm`（非 PIE でリンクした生成器自身の静的シンボル）で引く。`JSShape` は「ハッシュ表が構造体の前にある」
（`prop_hash_end`）ので、`{ uint32_t hash[N]; JSShape sh; JSShapeProperty prop[M]; }` と同じレイアウトの鏡の構造体を shape ごとに生成し、
`_Static_assert(offsetof(...))` でフィールド位置を固定する。生成物はリポジトリに入れ、`--check`（F1 の `gen_rom_atoms.sh` と同じ）で
陳腐化を止める。ROM atom の番号を埋め込むので、`quickjs-atom.h`／ROM atom 表が変わったら再生成。

**RD5. bytecode の組み込み（`Array.fromAsync`、`Iterator.zip`、`Iterator.zipKeyed`）は harden の下で定義しない**（§6.5）。
ROM の `Array` コンストラクタに「初回に RAM へ書く」プロパティは置けない。3 つとも出荷アプリは使わず、意味的にも ES2025〜 の付録。
残す道は「context ごとに RAM で即時に作る」（評価のたびに 3 関数のコンパイル、数 KB）か「RAM の overlay」（XS の alias、hot path に判定）で、
どちらも払わない。

**RD6. weak の対象に ROM は取れない**（§4.6）。TypeError。

**RD7. F2・F3・`rom_cache` は engine の intrinsic から外す**（§4.7、§5-(d)）。ROM atom の文字列値は flash の `JSString` を直接返す
（`__JS_AtomToValue` の脱出が確保 0）。F2 は `pocket.*` の面にだけ残す。F3 の `LAZY_CLASS_MISSING` 判定（`class_proto` の 4 箇所）は不要になる。

**RD8. hot な ROM の行を DRAM に置く手は、実機の `D_STALL` を見てから**（§7.2）。P1 と同じく DRAM は 1:1 で払うので、
数字が出る前には決めない。

### 6.5 なぜ bytecode の組み込みを落とすか

`JS_AUTOINIT_ID_BYTECODE` の枠は、初回の読みで `js_bytecode_autoinit` が組み込みの bytecode をコンパイルし、関数オブジェクトを
**prop の値に書く**。ROM の prop には書けない。bytecode 本体も RAM にしか置けない（IC・T1 の quickening が書く）。従って ROM の
`Array`・`Iterator` に、この 3 つの名前を持たせる方法が無い。harden の規則 1（コード生成なし）と同列の「無くて驚かない」削除として扱う。

### 6.7 F1 との統合

F1 の ROM atom 表は `{hash_type, len, off}` 8 B と文字 blob。これを「flash の `JSString`（20 B ＋ 文字 ＋ NUL、負の `ref_count`、
`atom_type = STRING`、`hash_next = 自分の番号`）」の配列に置き換えると、(1) `__JS_AtomToValue` が materialize せずその番地を返せる、
(2) snapshot の関数名が同じ記録を指せる、(3) `rom_cache`（RAM 32 枠 ＋ 参照 32 個）が消える。F1 の台帳 33 箇所のうち「脱出」に分類された
3 箇所（#21・#23・#30 相当）が「ROM の `JSString` を返す」に変わる。`JS_NewAtomStr` が ROM の `JSString` を受けたときは
`atom_type != 0` なので番号をそのまま返す（3893 行の経路。`js_get_atom_index` は `hash_next` に番号を持つ heap 側の約束に揃える）。

## 7. 実行時の代償

### 7.1 命令数（決定的で、配置の揺れを受けない）

`js_dup` +1 命令、`JS_FreeValueRT` ±0、`js_call_c_function` +1 分岐、GC のコールバック +1 比較。DERBY のターンは dispatch 10,946
（host、interpreter-stages §9.2）、退役命令 1,035,065（実測(device)、p1 §9）。dup はおおむね dispatch 1〜2 回に 1 回（`get_loc`・`dup`・
`get_field`・`get_var` が各 1 回）なので **+5〜10 k 命令/ターン ≈ +0.5〜1%**（推定）。P1 の A/B/A で退役命令は 1,035,065 → 1,035,449 と
ビルド間でも 0.04% しか動かなかったので、**この増分は実機の退役命令カウンタでそのまま読める**（§9 R1 の関所）。

### 7.2 D キャッシュ（flash の読み）

今日、`Array.prototype.push` の探索は RAM を歩く。ROM 化後は `p->shape->proto` から先が flash: オブジェクト本体（48 B、2 行）、shape の
ヘッダと `prop_hash_end`（1〜2 行）、`prop[]` の項目（1 行）、prop 配列の値（1 行）、見つけた関数オブジェクトの本体（`class_id`・`u.cfunc`、
2 行）。**1 名あたり 6〜7 行（32 B 行）**、同じ名前の 2 回目以降は同じ行。DERBY が触る 27 名 ＋ `Math`・`Array.prototype`・`String.prototype`
の表で、hot な ROM の作業集合は **約 5 KB**（推定）。D キャッシュは 32 KiB・8 way・32 B 行（`sdkconfig`、`CONFIG_ESP32S3_DATA_CACHE_32KB`）。

競合するのは flash の `.rodata` を読むものだけ（内部 SRAM は cache を通らない）: `dispatch_table` 1 KiB、F2 の外れ経路の関数リスト、
Kasane のフォントと plan、エラー文言。描画がフレームごとにフォントの glyph を流すので、ターンの頭は冷えている可能性がある。

- 最悪（毎ターン全部冷える）: 27 名 × 6 行 × 450 サイクル（r5-icache の模型の単価、I キャッシュの値を借用）≈ 73 k サイクル ≈ **0.3 ms/ターン**、
  DERBY の JS 10.5 ms の 3%（推定）。
- 最良（温かい）: SRAM との差はヒット時の数サイクル × 数百回 ≈ 無視できる（推定。S3 の cache ヒットの遅延は未測定）。
- 参考: STRESS QIO の `D_STALL` は 63 k サイクル/ターン（1.51 M の 4%、実測(device)、turn-cpi §3）。ROM 化はこの項を増やす。

**`D_STALL` は TURNPERF で取れ、コードの配置ではなくデータの配置で決まる**ので、P1 の `I_STALL_BUSY` と同じ読み方ができる（§9 R3）。
悪ければ RD8（hot な ROM の行だけ DRAM の `.data` に写す。`Math` と `Array.prototype` で 2〜3 KB）。

### 7.3 F2 の外れが消える

今日、組み込みで外れた探索（`Object.prototype` の遅延リストを 1 名ずつ比べる）は 1 回 4.0〜10.5 µs（実測(device)、plan §15.3）、
imucal で 8.7 回/フレーム。ROM では shape のハッシュ探索 1 回に戻るので、この項は消える。

## 8. T1（quickening）と組み込み呼び出しの特殊化との関係

[interpreter-stages.md](interpreter-stages.md) の T1 は「呼び先が安定したら専用命令に書き換え、guard が外れたら汎用経路に落とす」。
`math-call-fast.md` の試作は「呼んだ時点の呼び先の値そのもの」を guard にしている。

ROM intrinsic が変えるのは次の 3 点:

1. **guard の期待値が link 時の定数になる。** harden で `Math` の束縛は non-writable、`Math.sin` は non-writable・non-configurable → 値は
   ビルド時に決まる ROM の番地。T1 の専用命令は期待値を**命令の中に持たず、RAM の副表にも持たず**、`&rom_math_sin` との比較だけでよい。
   「guard が free」は命令数ではなく **RAM 0・無効化の仕組み 0** の意味。比較 1 回は残る（`const sin = Math.sin` は同じオブジェクトで通り、
   `function sin(){}` で隠した場合は外れて汎用経路）。
2. **S0（コンパイル時）に畳める。** `Math.sin` という式は、`Math` が ROM の global 束縛に解決され、どのチャンクにも `let Math` の
   グローバル lexical 宣言が無ければ、`push_const <ROM の値>` に畳める（guard 不要: 不死なので参照数も無効化も無い）。
   `pocket.app.load` の後のチャンクが `let Math` を宣言すると前提が崩れるので、**harden に「intrinsic の名前をグローバル lexical で
   宣言しない」規則を足す**（宣言したチャンクは読み込み時に拒否。構造的に不可能にする側）。
3. **`std_array_prototype` が定数 true になる**（§4.9）。`OP_get_array_el` などの判定（12175・21912・48576 行）が消せる。

逆向きの制約: T1 が**書く**のは bytecode（RAM）だけで、ROM のオブジェクトには触らない。IC（inline cache）を shape の番地で持つ設計なら、
ROM の shape の番地はビルド時定数で、**IC の shape 比較も定数との比較**になる。§9.4 の「DERBY では IC は割に合わない」判断は変わらない。

## 9. 段階と関所

各段は `CONFIG_POCKET_VM_ROM_INTRINSICS`（既定 n → 関所後 y）で従来経路へ戻せる。関所は F 系列と同じ「既存の合格項目を維持 ＋
新しい機構ごとの負の対照」。**harden（`vm/harden`）が `vm/main` に入ってから R2 以降に進む。R1 は harden 無しで進められる。**

### R0（この文書、完了）

床の内訳・触った分・snapshot の大きさ（§2）、壊れるものの台帳（§4）、候補の比較（§5）。道具は `tools/vmtest/floor/rom_walk.sh`。

### R1: 不死の機構だけ（snapshot 無し）

1. `js_dup`・`JS_FreeValueRT` の符号判定（§4.1）、GC の 3 コールバック（§4.2）、`realm` の NULL 許容（§4.5）、§4.9 の無条件 store 4 箇所。
2. 検査用 API `JS_MakeImmortal(JSValue)`（host のみ）: 任意の RAM オブジェクトの `ref_count` を負にする。
3. **関所（host）**: `tools/vmtest/run.sh` の全変種でコーパスがバイト一致、`test262.py` が基準線と同一。
   - **N-R1a（負の対照）**: `JS_MakeImmortal(Math)` した上で `Object.create(Math)`・`new WeakRef(Math)`・GC 1,000 回・`Math` を 10 万回 dup/free して、
     ASan のビルドでエラー 0、`ref_count` が元の値のまま。処置を 1 つ外したビルド（例: `gc_decref_child` の判定を抜く）で assert が落ちることを見せる。
   - **退役命令**: host の opprof で dup の回数を数え、+1 命令 × 回数が §7.1 の推定の桁にあること。
4. **関所（実機）**: TURNPERF で DERBY の退役命令/ターンを R1 前後で比べる（配置に依らない）。増分が §7.1 の推定（+0.5〜1%）の範囲。
   `smoke_device.py --cycles 20`、`memlog --check`。`js=` は変わらない（この段では何も flash に置いていない）。

### R2: 生成器と snapshot（host）

1. `tools/vmtest/floor/gen_rom_intrinsics.sh`: harden した context を作り、F2 の全展開 ＋ AUTOINIT の解決（§2.3 の手順）→ 到達可能な
   グラフが閉じていることを検査（§4.4）→ C ソースを出す（RD4）。`--check` で陳腐化を検出。
2. `JS_NewContext` を「ROM の配線 ＋ global object の生成」に置き換える（従来の `JS_AddIntrinsic*` は生成器のビルドにだけ入る）。
3. ROM atom の `JSString` 化と `rom_cache` の撤去（RD7）。F2 の `lazy_eligible` を `pocket.*` だけに。
4. **関所（host）**: コーパス（harden 版の期待値）バイト一致、test262（harden の基準線）同一。生成器の再実行で差分 0。
   - **N-R2a（負の対照、書き込み）**: 処置を 1 つ外した（例: `JS_PreventExtensions` の `if` を抜いた）ビルドで `Object.freeze(Math)` が
     **SEGV する**ことを見せ、全部入れた状態で全変種を通す。「落ちない検出器」を信用しない。
   - **N-R2b（同一性・順序）**: `Array.prototype.map === Array.prototype.map`、全組み込みの `getOwnPropertyNames` と記述子が
     harden 版の従来ビルドと一致（F2 の N2a・N2b を流用）。
   - **N-R2c（閉包）**: 生成器にわざと RAM を指す値（`realm` を残す）を与えて生成が失敗すること。
   - **床**: `rom_walk` で context の `js=` が約 3.9 KB（§2.1 の「残る分」）、`derby.js` の触った分が 0。実機レイアウトの vmrun で
     空スクリプトの `qjs_malloc_size`。
5. flash の増分: host の生成物の `.rodata` サイズ（`size`）。推定 75〜80 KB との照合。

### R3: 実機

- `build_*` を分けて、既定 n と y を焼く。**各アプリの `js=`**（hello・imucal・pet・companion・Kasane デモ・DERBY）、`app_free`、最大空きブロック、
  `memlog.py --port --check`、`idf.py size` の flash 増分、`tools/check_flash.py`。
- **速さ**: TURNPERF の DERBY レース区間で、退役命令（R1 で既知）、`I_STALL_BUSY`、**`D_STALL`**。P1 §5.2 の読み方（ms ではなくカウンタ、
  各脚 3 回、A/B/A）。`D_STALL` の増分が §7.2 の推定（最悪 73 k サイクル）のどこにあるかで RD8 を決める。I キャッシュの揺れ（15%）は
  退役命令と `D_STALL` には乗らない。
- `smoke_device.py --cycles 20`、`test_settings.py`、`stress_app.py`（確保失敗からの回復。ROM では「初回の読みの OOM で名前を失う」
  R4b の経路そのものが無い）。
- 受け入れ: DERBY のゲスト最大が約 20 KB 下がる（推定の確認）、JS のターンが `D_STALL` 込みで悪化していない（配置差の床 15% 以内なら不変）。

### R4（任意）: ファームの面

`pocket.*` の面（実機で約 6.2 KB、plan §14.1）のうち、関数リスト由来の部分は同じ生成器で ROM にできる。capability の値
（`pocket.capabilities`、実機ごと）と `console` は RAM。入れれば F2 は丸ごと消える。R3 の実測の後で判断。

## 10. 計測の穴（正直に）

| 穴 | 状態 |
| --- | --- |
| 実機の数字 | **無い**。§1.1 の「約 20 KB」は host の実測の足し算（推定） |
| `D_STALL` の増分 | 推定のみ（§7.2）。ヒット時の遅延、ターン間の追い出しの有無は未測定 |
| dup/free の回数/ターン | 未計測。+0.5〜1% は dispatch の構成からの推定 |
| harden の最終形 | ブランチ未マージ・設計書なし。§0 の 4 規則とこの文書が足す 2 規則（store 前の拒否、intrinsic 名のグローバル lexical 宣言の禁止）が受け入れられるか |
| 触る名前の受け手 | `rom_touch/*.js` は grep の名前に受け手の型を手で当てた近似。Kasane の面経由で触るもの（`pocket.kasane` が返すオブジェクトのメソッド）は数えていない |
| flash の増分 | raw の和 64,264 B は実測(host)、文字列と `JS_AddIntrinsic*` の撤去分の相殺は推定。app partition は 3 MB（`partitions.csv`）、image は 2,175,184 B（実測(device)、keystate.md、時点が古い） |
| 先行例 | Espruino・mJS は一次資料で未確認。XS の「ROM Colors」は索引の記述だけ |
| test262 の別 realm | RD2 の「呼び手の realm」が `$262.createRealm` の試験で差を出すか未確認 |

## 11. 再現（`tools/vmtest/floor/`、WSL）

```powershell
wsl -e bash tools/vmtest/floor/floor32.sh      # -m32 のオブジェクトと intrinsic ごとの床（§1.1 の 24,512）
wsl -e bash tools/vmtest/floor/rom_walk.sh     # §2: 生まれたての context、全展開、rom_touch/*.js ごとの触った分
```

`rom_walk.c` は `quickjs.c` を `#include` する（`floor_parts.c` と同じ）。出力の「accounted」行は global の shape を二重に数えるので、
§2.1 の表は手で引き直した値である。

## 12. 実装の記録（2026-10-26〜27、`vm/rom-builtins`）

R1 と R2 を実装した。区分は §0 と同じ（実測(host)／実測(device)／推定）。

### 12.1 R1: 解放しない値（`822f09d6`）

- `js_dup` は符号が負なら書かない（分岐 1 つ）。`JS_FreeValueRT` と `js_free_string` は、上流がすでに持つ分岐の後ろに store を移した。RAM の値では命令数が同じ。
- GC の子のコールバック 3 つは、解放しない子を数えない。
- C 関数の `realm` が NULL なら呼び手の context で動く（`js_cfunc_realm`。Math の速い経路は `ctx` を受け取るようにした）。
- 先に検査してから書く: `is_prototype`、`extensible`。直列化（`tmp_mark`）は拒否。弱参照の対象にできない（RD6）。**harden で固めた組み込みも弱参照の対象から外した**。ROM の有無で JS から見える答えが変わらないようにするため。
- host の検査: `vmrun --harden --immortal` は、全組み込みを作ってから RAM のまま「解放しない」にする（`JS_PocketImmortalizeIntrinsics`、host のみ）。コーパスは harden のみの出力と 3 変種で一致した。違うのは `--fail-alloc` の 3 件だけで、これは番号をずらすと同じ出力になる（539・447・557）。test262 の合格の集合も一致（20,459）。負の対照 `r1_faults.sh`: 守りを外したエンジン 3 つが、どれも検出された。
- `--immortal=lazy`（組み込みを作らずに適用）は、最初の遅延生成が「解放しない」shape に書いて assert で落ちる。ROM では起きない書き込みを、検査が拾っている。

### 12.2 F1 の表の欠け（`b211e5b6`）

`Math`・`JSON`・`Atomics` は AUTOINIT で、中身は最初の読みで作られる。F1 の生成器はそれを作らずに名前を集めていたので、メソッド名 67 個が表に無かった（`Math` を触るアプリは、今のファームでもその分の heap atom を作っていた）。表は 558 → 625 番（flash 約 +0.8 KB）。ROM では、表に無い atom は番号が実行時に決まるので焼けない。

### 12.3 R2: 生成器と ROM の context（`28b31c9b`、`CONFIG_POCKET_VM_ROM_INTRINSICS`、既定 n）

- `tools/vmtest/floor/gen_rom_intrinsics.sh` が `quickjs-rom-intrinsics.h` を書く。オブジェクト 613（C 関数 548）、shape 101、プロパティ 1,861、文字列 531、Symbol 13、global の束縛 65。欄は名前で書くので、64 bit の host と NaN boxing の実機で同じ生成物がコンパイルできる。NaN boxing でポインタを持つ JSValue は 64 bit 整数の定数にできないので、2 語の union の欄（`JSProperty.u.getset`、`JSObject.u.array`）を通して「下位にアドレス、上位にタグ」で書く。C 関数は生成器自身のシンボル表（`nm`）から名前で書く。`--check` で陳腐化を止める。
- ROM の context: `JS_NewContext` は ROM の番地を配り、RAM の部分だけを作る。global object（65 の束縛を表から）、`global_var_obj`、雛形 shape 5 個、`JS_AddIntrinsic*` が兼ねていた runtime のクラス登録（RawJSON、Proxy、Promise と async 群、WeakRef、FinalizationRegistry、CallSite、DOMException）、`performance`。
- **`performance` は ROM に置かない。** `timeOrigin` が context を作った時刻を値として持つため。RAM に作ってから固める。
- 13 の well-known symbol は、`JS_InitAtoms` がその atom 番号に ROM の struct を置く。ROM のプロパティと runtime が同じ symbol を指す。
- RD5 のとおり `Array.fromAsync`・`Iterator.zip`・`Iterator.zipKeyed` は無い（`Array.fromAsync` は async 関数だった）。
- 生成器で見つかった漏れ: 最初の使用で prototype を作るクラス（Map と Set の iterator）を、展開の段で作っていなかった。ROM 版で null になり、`Map.prototype.entries()` の prototype が null になった。
- ROM の文字列は atom に昇格させない（`__JS_NewAtom` は写しを作る）。runtime の後始末は ROM の symbol を解放しない。

**host の結果（実測(host)、asan、`--harden`）:**

- コーパス: ROM 版は harden のみの RAM 版と、3 変種（素・force-yield・budget-jobs 3）とも、`--fail-alloc` の 3 件を除いて一致した。
- test262: RAM の harden 版 20,459 に対し ROM 版 20,446（失った 7 件はすべて `$262.createRealm` を使う試験）。ROM では 2 つ目の realm も harden されて生まれ、eval が断られる。ファームは context を 1 つしか作らない（RD2 の想定の範囲）。
- 32 bit の host（NaN boxing、`-m32`）でも同じ出力。

| ヒープ（`qjs_malloc_size`、実機レイアウトの 32 bit） | RAM（harden） | ROM | 差 |
|---|---:|---:|---:|
| 空のスクリプト | 28,788 B（確保 384） | 14,004 B（確保 58） | −14.8 KB |
| `Math`・`JSON`・`Map`・`Promise` を少し触る | 30,220 B | 14,004 B | −16.2 KB |
| 小さな検査（`r2_smoke.js`） | 42,828 B | 15,920 B | −26.9 KB |

実機の `js=` は確保ごとに 8 B を足すので、確保の数が減る分（384 → 58 で約 2.6 KB）がさらに乗る（計算）。

### 12.4 R1 の実機の代償（実測(device)、2026-10-26）

測定用の枝（固定の種と TURNPERF）で、R1（`822f09d6`）と harden（`c40b304c`）を同じ手順で回した。broker の `derby`（デモ 1 周、レース 1000001、種 A27C59EB）。レース区間の命令の窓は 2 つずつで、窓の位置は 2 本で一致する（同じ種・同じ着順）。

| 窓 | 命令/ターン harden → R1 | サイクル/ターン harden → R1 |
|---|---|---|
| レースの前半 | 1,031,651 → 1,038,430（+0.66%） | 2,406,975 → 2,412,224（+0.22%） |
| レースの後半 | 957,844 → 965,917（+0.84%） | 2,331,138 → 2,339,452（+0.36%） |
| 読み込み直後 | 544,876 → 548,311（+0.63%） | |

- 命令は +0.6〜0.8%。§7.1 の推定（+0.5〜1%）の範囲に入った。時間に効くのはサイクルの +0.2〜0.4%。
- ヒープは変わらない（`js=` 116,172 → 116,192 B）。R1 では何も flash に置いていないため。
- 前回の A/B/A（§7.1 は harden.md）は `derby_fixed` の 3 レースだったが、broker の再起動後、`derby_fixed` が事前検査で断られたので、`derby` を使った。

### 12.5 R2 の実機（R3 の一部、実測(device)、2026-10-26）

測定用の枝 `vm/r2-tp`（`28b31c9b` ＋ 固定の種 ＋ TURNPERF ＋ `CONFIG_POCKET_VM_ROM_INTRINSICS=y`）。手順は §12.4 と同じ（`derby`、レース 1000001、種 A27C59EB、着順は 3 本とも 3,5,1,2,8,6,4,7）。

**ヒープ（DERBY）**

| | R1（RAM） | R2（ROM） | 差 |
|---|---:|---:|---:|
| ゲストの作成直後（最初のチャンクの読み込み前の `used`） | 49,472 B | 33,264 B | −16.2 KB |
| 読み込み直後の `js=` | 116,192 B | 97,168 B | **−19.0 KB** |
| 読み込みのピーク（最後のチャンク） | 120,092 B | 101,072 B | −19.0 KB |
| 読み込み直後の空き heap | 45,524 B | 63,312 B | +17.8 KB |

§1.1 の推定（約 20 KB）とほぼ同じ。

**速さ（レース区間の 2 つの窓、harden との比較）**

| 窓 | 命令/ターン | サイクル/ターン | JS µs/フレーム | D キャッシュ待ち/ターン |
|---|---|---|---|---|
| 前半 | 1,031,651 → 1,018,293（−1.3%） | 2,406,975 → 2,382,718（−1.0%） | 10,608 → 10,430 | 75,503 → 72,899 |
| 後半 | 957,844 → 954,305（−0.4%） | 2,331,138 → 2,298,301（−1.4%） | 10,253 → 10,094 | 89,981 → 87,186 |

- **遅くならず、1〜1.5% 速い。** R1 の +0.6〜0.8% の命令を、遅延生成（F2・F3）の外れと確保の減少が上回った。
- §7.2 で心配した D キャッシュの待ちは増えていない（むしろ 3% 減）。RD8（hot な行を DRAM に写す）は要らない。
- 窓は 1 本につき 2 つだけで、各脚 1 回。配置の揺れ（ビルド間で最大 15%）はサイクルにも乗るので、±1% の速さの差は確かな値ではない。命令とヒープの差は配置に依らない。

**flash**: app の image が 2,275,376 → 2,344,752 B（+69.4 KB、F1 の表の +0.8 KB を含む）。推定は 75〜80 KB。

**ほかのアプリ（R2 のファームで起動、エラー 0）**

| アプリ | `js=`（起動直後） |
|---|---:|
| hello | 23,912 |
| imucal | 37,000 |
| pet | 41,556 |
| companion | 34,768 |
| stress | 43,172 |
| megademo | 79,456 |
| gridlab | 76,428 |
| videolab | 34,880 |
| lcdcatch | 51,752 |
| derby | 97,280 |
| bigwave | 82,772 |

`tools/stress_app.py --seconds 15` は `STRESS_APP_PASS`（確保失敗からの回復を含む）。HELLO の broker の検査も通過した。測っていないもの: 各アプリの RAM 版との差（DERBY だけ測った）、`test_app_resume.py`、`memlog --check`。
