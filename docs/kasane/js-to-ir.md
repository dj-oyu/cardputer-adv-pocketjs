# JS 風の言語 → 手続き型 IR のコンパイラと、「IR を flash に置き id で登録」の見積もり

2026-09-30、ブランチ `vm/ir-compiler`（`vm/main` 909decd から）。DERBY WATCH の首振りカメラ（段階 3、`vm/pan-camera` 55956e9、マージしない）が実機の DRAM に収まらない件（`vm/pan-camera` の docs/apps/derby-pan-memory.md、[derby-pan-camera-cost.md](../apps/derby-pan-camera-cost.md)）への、ユーザーの提案「JS → IR コンパイラをチェックするのはどう」の調査と試作。**出荷コード（`apps/`・`main/`）は変えていない。** 試作は [`tools/kasane_ir/`](../../tools/kasane_ir/)。

数値の区別: **host 実測** は WSL の `-m32`（実機と同じ 8 B の JSValue、確保は実機の TLSF の長さで課金）。**ビルド確認** は ESP-IDF のツールチェーンで作ったオブジェクトの `nm`/`size`。**推定** は `sizeof` × 個数などの計算。実機は使っていない。

## 結論

- **(i) コンパイラは作る価値がある。ただし DRAM を減らすのはコンパイラではなく、IR をゲストに持たせないこと。** 手書きの IR はすでにほぼ最適で、コンパイラは静的な命令数を 467 → 453（−3.0%）、DERBY の実入力での実行ステップを −1.6% しか減らさない（confetti だけ −7.5%）。価値は保守性と誤りの防止: 名前と式で書ける、16 レジスタ・64 命令・CUBIC の r0..r7 をビルド時に機械が守る、死んだ命令を見つける（crowd の `I8,0`）、式マクロと展開で同じ式の繰り返し（首振りのニュートン法 4 段）を 1 行にできる。コンパイラはビルド時（host）に走らせるもので、実機のゲストでは走らせない（走らせればゲストのヒープを食う）。
- **(ii) 「IR を flash に置き id で登録」はゲスト側で約 7〜8 KB（host、評価後）効き、native 側の plan の常駐を直せばさらに 11〜24 KB（推定）効く。DERBY の不足（3〜11 KB）を紙の上では超える。** 内訳:
  - ゲスト: plan の文字列 13 本と `prog()` を持たないと、評価後のヒープが **−7,108 B**（基準）/ **−8,104 B**（段階 3）、評価のピークが **−5,600 / −8,068 B**（host 実測）。実機では host の約 1.3 倍になった前例がある（derby-pan-memory.md）ので **−9〜10.5 KB 前後**（推定）。
  - 登録の一時的なピーク: 今は 1 本の登録で `prog()` が **3.1〜10.0 KB** を一時的に確保し、配列の配列 **2.1〜7.1 KB** を `register()` が返るまで保つ（host 実測、命令 1 つで約 119 B）。id 登録では引数の配列（8 個以下の数）だけになる。
  - native: **plan は命令数に依らず 1 本 872 B**（`code[64]` 固定、Xtensa のオブジェクトで確認）で、DERBY は同時に **25 本**（段階 3 で **30 本**）を持つ（host 実測）: **21.8 KB / 26.2 KB**。命令数ぶんだけ確保すれば **−11.0 / −12.0 KB**、flash の IR を指せば **−20 / −24 KB**（どちらも推定）。
- **(iii) 優先順位**: ① native の plan を命令数ぶんだけ確保する（API 不変、アプリの変更なし、推定 −11〜12 KB。可変長の確保による断片化は実機で見る）→ ② 組み込みアプリ用の「flash の plan を id で登録」（API 追加、ゲスト −7〜8 KB host、native はさらに約 −9〜12 KB 推定）と、その表を作るコンパイラ → ③ コンパイラを plan を書く標準の道具にする（②の生成器として入れば追加費用は小さい）。①は ②の前提ではないが、②を入れても JS から配列で登録する経路（保存したプログラム、MEGADEMO）が残るので、①は単独で意味がある。

## 1. 現状（コードで確認）

**JS → IR のコンパイラ・アセンブラ・逆アセンブラはリポジトリに無い。** あるのは 1 命令 1 文字のテキストを配列にする 10 行の `prog()` で、DERBY（[derby_prog.js](../../apps/derby/derby_prog.js)）、MEGADEMO（[proc_megademo.js](../../apps/kasane/proc_megademo.js)、`U` の短縮つき）、BIG WAVE（[big_wave.js](../../apps/bigwave/big_wave.js)）がそれぞれ写しを持つ。文字 → op の表引きと `$n` の置換だけで、検査は native の登録に任せている。[grid_fold.js](../../apps/kasane/grid_fold.js) は symbolic な式 builder だが、対象は型付きメモリ IR（`ksn_proc_grid`）で、この float VM ではない。

**登録時コンパイラ（D3a）はソース言語を持たない。** 入力は数値の行（`[op,dst,a,b,value,color]`）で、`pocket_proc.c` の `register_impl()` が読んで `ksn_proc_plan_prepare()`（[ksn_proc_plan.c](../../main/ui/kasane/ksn_proc_plan.c)）に渡す。prepare がするのは、IR を plan の `code[64]` へ写す、[ksn_proc_analysis.c](../../main/ui/kasane/ksn_proc_analysis.c) で有界 CFG・def-use・生存・失敗条件を取る、同じ基本ブロックで依存のある隣り合った算術 2 命令に融合の印（`fused_at`）を付ける、の 3 つだけ。命令の移動・削除・レジスタの割り当ては無い（「No code is moved or elided」）。PIE の選択は型付き Q14 点列（`affineQ14Points`）と grid の IR に対してで、この float IR には掛からない。

**命令セット**（[ksn_procedural.h](../../main/ui/kasane/ksn_procedural.h)、15 op）: `SET INPUT ADD MUL SIN REPEAT END MOVE PLOT LINE REPEAT_REG BREAK_IF_GT PLOT_COLOR_REG LINE_COLOR_REG CUBIC`。除算・減算・比較分岐・sqrt・floor・レジスタ間の MOV は無い（減算は `×−1` と加算、コピーは `+0`）。`CUBIC` は r0..r7 の 4 点を読む（dst=0 固定、分割 1..64）。制約: レジスタ 16、入力 8、1 plan 64 命令、ループの深さ 8、`REPEAT` 1..255（0 は検証で拒否）、`REPEAT_REG` 0..255 の整数（0 は本体を飛ばす）、1 draw の step 10,000・線分 1,024・ラスタ 8,192、座標 −480..720。`draw()` のたびに `begin` がプログラムを VM へ写して検証し直し、**レジスタは毎回 +0 から始まる**（`pocket_proc.c` は状態を持ち越す `begin_state` を使わない）。算術の結果が非有限、`REPEAT_REG`・色レジスタが範囲外の整数でない、座標が範囲外、のどれでも draw 全体が失敗する。

**メモリ**: `ksn_proc_inst` 12 B、`ksn_proc_plan` **872 B**（`code[64]` 768 B ＋ `fused_at[64]` ＋点列の係数）。Xtensa の `-Os` オブジェクトの `nm -S` で 0x368 = 872 B、m32 host でも同じ（ビルド確認）。plan は `calloc(1, sizeof *plan)` なので、1 命令の plan（DERBY の点列用 `S0,0` が 7 本）も 872 B を取る。VM（996 B）とフレームは共有で、plan の数に比例しない。（その後 [plan-sized-alloc.md](plan-sized-alloc.md) で、`register()` の plan を 40 + 12n B にした。）

## 2. DERBY のメモリ（host 実測、`python3 tools/kasane_ir/derby_heap.py --stage3 DIR`）

`tools/games/test_derby_host.c` を `DERBY_EVAL_ONLY=1` で走らせ、`derby_prog.js` だけを 4 つの形に書き換えて評価後のゲストヒープを比べた。評価中は登録を呼ばないので、アセンブラを消した形も評価までは走る。

| `derby_prog.js` の形 | 基準 909decd 評価後 | 差 | 評価のピーク差 | 段階 3 評価後 | 差 | 評価のピーク差 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| そのまま（文字列 13 本＝3,182 字、`prog()`） | 107,112 | — | —（124,580） | 119,468 | — | —（137,124） |
| `prog()`・`OPS`/`FLD` を消す（文字列は残す） | 104,040 | −3,072 | −1,564 | 117,864 | −1,604 | −1,568 |
| 文字列を小さな整数に（`T.map` の組み立ても消す） | 100,416 | −6,696 | −5,188 | 111,940 | −7,528 | −7,492 |
| `T` 自体を消し、場面が持つ名前で指す（上限） | **100,004** | **−7,108** | **−5,600** | **111,364** | **−8,104** | **−8,068** |

- **(a) 今の plan の文字列と `prog()` の常駐は 7.1 KB**（基準）。atom が 24,794 → 21,269 B（−3.5 KB、文字列の本体）、関数と bytecode が約 −0.6 KB、残りはオブジェクト・シェイプ・ハッシュ表。`prog()` だけの差（−3.1 KB と −1.6 KB）が 2 つの版で倍違うのは、atom 表やシェイプ表の段階的な伸長の閾値をまたぐかどうかで、個々の内訳は ±1.5 KB 程度揺れる。合計（最後の行）は安定。
- **(b) id 登録にしたときのゲスト側の削減の上限**は最後の行: 評価後 −7.1 KB、評価のピーク −5.6 KB。
- **(c) 段階 3 を含めると −8.1 KB / −8.1 KB**。段階 3 自身の増分（+12.4 KB、基準 00a7c26 = 909decd の apps/derby から）のうち id 登録で消えるのは約 1.0 KB で、残りは既存の plan の分。つまり id 登録は段階 3 のコードを軽くするのではなく、既存の場所を空ける。
- **(d) 登録の一時的なピーク**（`heap_probe.c` で `spec(name)` を 1 本ずつ）:

| plan | 命令 | `register()` まで保つ配列 | `prog()` 中のピーク |
| --- | ---: | ---: | ---: |
| gate | 17 | 2,120 | 3,108 |
| turf | 22 | 2,732 | 3,968 |
| crowd / runner(r0) | 40 | 4,788 | 6,800 / 6,896 |
| vis | 54 | 6,468 | 9,300 |
| fr | 56 | 6,684 | 9,556 |
| pt（段階 3） | 60 | 7,116 | 10,020 |

  命令 1 つで約 119 B（行の配列 1 個）。初回の `prog()` だけ 1,524 B が戻らない（1 度だけ、以後 0）。登録は 1 フレームに 1 本だが、その 1 本のピークがターン内の空きの最小を 3〜10 KB 押し下げうる（実機のターン内最小と重なるかは未測定）。

## 3. コンパイラの試作

[`kir.mjs`](../../tools/kasane_ir/kir.mjs)（Node、依存なし）。入力は JS の部分集合:

```js
// conf: confetti. Up to 96 flakes (input 1 of them) on fixed sine tracks,
// drifting with the time (input 0); yellow and 129055 - yellow alternate.
input t = 0, count = 1;
let i = 0, colour = 65504;
repeat (96) {
  i += 1;
  if (i > count) break;
  const x = sin(i * 1.7) * 110 + 120;
  const y = sin(i * 2.1 + (i * .00041 + .037) * t) * 64 + 67;
  move(x, y);
  line(x + 1, y + 1 + 1, colour);
  colour = 129055 - colour;
}
```

手書きはこう（38 命令、16 レジスタのうち 15 を手で割り当て）:
`I2,0 I1,1 S0,0 S9,1 S8,-1 S6,65504 S7,129055 S11,1.7 S12,.037 S13,2.1 S14,.00041 R96 A0,0,9 B0,1 M5,0,11 N3,5 S5,110 M3,3,5 S5,120 A3,3,5 M5,0,14 A5,5,12 M5,5,2 M4,0,13 A4,4,5 N4,4 S5,64 M4,4,5 S5,67 A4,4,5 V3,4 A5,3,9 A10,4,9 A10,10,9 l6,5,10 M6,6,8 A6,6,7 E`

- **言語**: `input 名 = 番号`、`let`/`const`、`= += -= *=`、`+ - * /`（`/` は定数でだけ。2 の冪でなければ警告）、単項 `-`、`sin()`、`in(k)`、`$n`（登録時の引数、今の `prog()` と同じ）、`repeat (n) { }`（定数・`$n` なら `REPEAT`、式なら `REPEAT_REG`）、`if (a > b) break;`、`move/plot/line(x, y[, 色])`（色が式なら `*_COLOR_REG`）、`cubic(分割, 色, 8 つの座標)`、`function f(a, b) { return 式; }`（式マクロ、呼ぶたびに展開）、`unroll (n) { }`（コンパイル時の繰り返し）。
- **パス**: 構文解析 → 仮想レジスタへの低下（定数畳み込み、共通部分式、`x*1`）→ 定数・入力・引数を一番外のループの手前へ置く → 生存解析（ループは不動点）で死んだ定義を消す → 干渉グラフの彩色（Chaitin、CUBIC の点は r0..r7 に固定、同じ値が 2 点に出れば 2 つ目は別レジスタへ）→ 足りなければ定数・入力を使う場所で読み直す（再実体化）→ 最初の書き込みより前の `SET r,0` を消す（レジスタは +0 で始まる）。出力は今と同じ 1 文字テキスト（`prog()` にそのまま渡せる）と、flash 用の C の表（`derby_plans.mjs --emit`）。
- **float の意味を 1 ビットも変えない規則**: オペランドの並べ替え（結合則）をしない。定数の畳み込みは `+` と `*` だけ（double で計算して float に 1 回丸めるのは単精度の演算と一致する）。`sin` は畳み込まない（`sinf` と `Math.sin` は 1 ulp ずれうる）。`x+0` は消さない（`-0` が `+0` になる）。`a−c` は `a+(−c)`、`a−b` は `a+b×(−1)` で、どちらも IEEE で厳密。加算・乗算の交換だけは使う（厳密）。
- **意味の違い 1 点**: 死んだ算術を消すので、手書きなら非有限で draw が失敗した入力で、コンパイル版は描ける場合がある（言語の意味は見える出力で、手書きの失敗点ではない）。DERBY の plan では死んだ算術は 0 本だった（死んでいたのは `INPUT` 1 本で、失敗しない）。

### 既存の plan の再現（`node tools/kasane_ir/derby_plans.mjs`）

13 本すべてを、手書きの IR から逆コンパイルした言語（[`plans/*.kjs`](../../tools/kasane_ir/plans/)）で書き直した。rail・turf・crowd・runner・conf は名前と注記を付けて手で書き直し、段階 3 の `pt` は `function`/`unroll` で書き直した。残りは逆コンパイルのまま（変数名が元のレジスタ名 `r8`、`r14c`）。

| plan | 手書き | レジスタ | コンパイル後 | レジスタ | 差 | 実入力の実行ステップ（手書き → 後） |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| rail | 20 | 9 | 18 | 7 | −2 | 20,516 → 19,716 |
| turf | 22 | 12 | 22 | 10 | 0 | 同じ |
| stands | 29 | 14 | 27 | 12 | −2 | 53,901 → 53,101 |
| crowd | 40 | 16 | 38 | 15 | −2 | 933,552 → 930,352 |
| runner | 40 | 15 | 40 | 10 | 0 | 同じ |
| gate | 17 | 9 | 17 | 9 | 0 | 同じ |
| pole | 37 | 16 | 36 | 12 | −1 | 14,800 → 14,400 |
| photo | 30 | 11 | 28 | 6 | −2 | 189 → 187 |
| conf | 38 | 15 | 37 | 16 | −1 | 260,338 → 240,712（−7.5%） |
| silk | 35 | 10 | 33 | 9 | −2 | 840 → 792 |
| map | 49 | 7 | 48 | 4 | −1 | 19,600 → 19,200 |
| vis | 54 | 14 | 54 | 12 | 0 | 同じ |
| fr | 56 | 16 | 55 | 15 | −1 | 22,400 → 22,000 |
| **計** | **467** | | **453** | | **−14（−3.0%）** | 基準のゲーム全体 1,603,085 → 1,577,409（−1.6%） |
| 段階 3: prail / pt / pc / hl | 49 / 60 / 35 / 26 | 16 / 16 / 16 / 13 | 49 / 59 / 35 / 26 | 13 / 13 / 15 / 13 | 0 / −1 / 0 / 0 | pc −2.7%、他は ±0.3% |

- 減った理由: 定数の重複した `SET` をまとめる、`SET r,0` の省略、`x+0` のコピーの不要な形（rail の `S6,0 A6,6,0` は手で書き直して消えた。`-0` と `+0` の違いだけで、画素は同じ）、crowd の死んだ `I8,0`。増えた plan は無い。レジスタは 13 本中 11 本で減り（runner は 15 → 10、photo は 11 → 6）、conf だけ 15 → 16（定数を全部ループの外に置き、足りない 2 つを読み直す）。
- **手書きはほぼ最適**: 静的には −3%、実行では −1.6%。命令数の削減を目的にコンパイラを作る価値は小さい。

### 等価性（`python3 tools/kasane_ir/check_equivalence.py [--prog DIR]`）

1. host のゲーム（LIGHT・MID・HEAVY の全台本）を、登録の引数と各 draw の入力をログに出す一時コピーで走らせ、実際の入力を捕獲する。
2. plan ごと・引数の組ごと（runner は 8 頭、crowd・stands は段階で別）に、捕獲した入力 最大 400 と、それを摂動した 300（整数は 9 割保つ、10 本に 1 本は ±1000 の乱値。失敗の経路も通す）を作る。
3. [`run_ir.c`](../../tools/kasane_ir/run_ir.c) が手書きとコンパイル後を実物の [ksn_procedural.c](../../main/ui/kasane/ksn_procedural.c) で走らせ、コンパイル後は登録された plan の経路（`ksn_proc_plan_run`、融合あり）でも走らせて、状態・線分（端点と色、順序）・ラスタ step を全部比べる。

**結果: 基準 13 本・段階 3 の 18 本とも、捕獲した入力と摂動した入力のすべてで一致（`EQUIVALENCE PASS`、不一致 0）。** 摂動では両方が同じ入力で失敗し、失敗の種類（INVALID/LIMIT）も一致した。[`test_kir.mjs`](../../tools/kasane_ir/test_kir.mjs) は DERBY が通らない経路（17 値が同時に生きると失敗、再実体化、CUBIC の配置、`/3` の警告、65 命令以上の報告）を見る。

## 4. native 側の案（設計のみ、実装しない）

### A. plan を命令数ぶんだけ確保する（API 不変）

- `ksn_proc_plan` の `code[]` を末尾へ移し（`fused_at` はその前）、`pocket_proc.c` だけが `offsetof(ksn_proc_plan, code) + count × 12` を確保する。prepare の失敗時の `memset(plan, 0, sizeof *plan)` を、確保した長さを越えない形に直す。プローブ（`ksn_proc_limits_device_probe.c` など）と試験は値で持つ 872 B の plan のままでよい。
- DERBY: 25 本・686 命令で 21,800 → 10,832 B（**−10,968 B**）、段階 3 は 30 本・916 命令で 26,160 → 14,112 B（**−12,048 B**）。推定（`sizeof` × 個数、TLSF の 1 ブロックの付加は今と同数）。
- 危険: 同じ 872 B の塊が再利用されていたのが、大きさの違う塊になる。断片化で最大連続が下がりうる（実機で `largest free` を見る）。
- 変更: `ksn_proc_plan.h/.c`、`pocket_proc.c`、`tools/kasane_contract/test_proc_plan.c`（構造体のコピーの契約は値で持つ plan だけに残す）。工数 0.5〜1 日＋実機測定（推定）。

### B. 組み込みアプリの plan を flash に置き、id で登録する

- **ビルド**: `tools/kasane_ir` が `plans/*.kjs` をコンパイルして C の表（`static const ksn_proc_inst ...[]`、`$n` の欄は「pc・欄・引数番号」の patch 表、名前 `"derby.rail"`）を生成する。Node は IDF のビルドに無いので、生成した `.c` をコミットし、host の検査で `.kjs` との一致を見る（`check_flash.py` と同じ流儀）。DERBY 13 本で Xtensa の `.rodata` 5,740 B＋名前 145 B（ビルド確認）。アプリの JS からは plan の文字列 約 3.2 KB が消える。
- **API**: `pocket.kasane.procedural.register('derby.rail', args?, points?)`。第 1 引数が配列なら今の経路（互換）、文字列なら組み込み表を引く。未知の名前は `INVALID_ARGUMENT`。`args` は 8 個以下の数で、patch 表の数と合わなければ拒否。feature-test 用に capability の `limits` か `builtinPlans: true` を出す（実装した面だけ true、CLAUDE.md の規則）。
- **native の RAM**: slot が「表への参照・引数 8 個（32 B）・融合の印」を持つ。融合の印は IR から決まるので、ビルド時に表へ入れれば RAM に要らない。1 本 約 48〜72 B → DERBY 25 本で約 1.2〜1.8 KB（**−20 KB 前後**、段階 3 で約 −24 KB、推定）。VM は draw のたびに flash から `owned_code` へ写し（今も plan から写している）、そこで patch を当ててから今までどおり検証する。`ksn_proc_plan_run()` の「VM のコードが plan と同じか」の `memcmp` は、patch 後のコードとは一致しないので、plan の識別子（ポインタと世代）で比べる形に変える。
- **ゲスト**: §2 の最後の行（−7.1 / −8.1 KB、host）と、登録の一時的なピーク（最大 10 KB）が消える。
- **host 契約試験への影響**: `run_pocket_proc_qjs.py`（登録の引数の型と誤りの文言）、`run_pocket_proc_limits_qjs.py`（32 本の上限は slot 数なので不変）、`test_proc_plan.c`（plan の所有と構造体のコピー）、`tools/games/test_derby_host.c`（表をリンクする）、`run_megademo_app_host.py`（配列の経路のまま、不変）。追加: 未知の名前、引数の数の不一致、patch 後の検証失敗、同じ名前の多重登録。
- **変更するファイル**: `main/pocket/pocket_proc.c`、`main/ui/kasane/ksn_proc_plan.h/.c`、`ksn_procedural.c/.h`（patch つきの begin）、生成物 `main/ui/kasane/ksn_rom_plans.c/.h`（`main/CMakeLists.txt`）、`docs/api/common-api.md`、`apps/derby/derby_prog.js`・`derby_view.js`（`spec()` が名前と引数を返す）、上の試験。
- **工数**: A を済ませた上で 2〜3 日（生成器と同期検査 0.5、native 1〜1.5、試験 0.5、DERBY の移行 0.5）＋実機測定 0.5 日（推定）。
- **制約**: flash の plan は組み込みアプリにしか使えない（SD やエディタから入れたプログラムは配列の経路のまま）。plan を直すにはファームの再ビルドが要る。

## 確信の低い点

- **native の削減はすべて推定**（`sizeof` × host で測った同時の本数）。実機の空き・最大連続・ターン内の最小では測っていない。A の可変長の確保は断片化で効きが減りうる。
- ゲスト側の実機の値は host の 1.3 倍という 1 点の比（derby-pan-memory.md）からの換算。
- `prog()` だけの削減量（−3.1 KB と −1.6 KB）は表の伸長の閾値で揺れる。合計の −7.1 / −8.1 KB は安定。
- 等価性は捕獲した入力と、その摂動で見た。すべての入力での同値は証明していない（変換規則が float の結果を変えないことは §3 の規則による）。`ksn_proc_plan_run` は融合ありの経路を通したが、debug step の経路はコンパイル後の IR では比べていない（手書きと同じ VM の経路なので、比べる意味は薄い）。
- 登録のピーク（最大 10 KB）が実機のターン内最小と同じターンに来るかは測っていない。

## 再現

```
python3 tools/kasane_ir/derby_heap.py --stage3 DIR          # §2（WSL、DIR/apps/derby に 55956e9 の書き出し）
node tools/kasane_ir/derby_plans.mjs [--prog FILE] [--show NAME] [--emit OUT.c]   # §3 の表
python3 tools/kasane_ir/check_equivalence.py [--prog DIR]    # 等価性（WSL、約 1.5 分）
node tools/kasane_ir/test_kir.mjs
```

段階 3 の書き出しは Windows の git で `git show 55956e9:apps/derby/<file>` を `.cache/kasane_ir/stage3/apps/derby/` へ（WSL の git は worktree を読めないことがある）。m32 の sysroot は `.cache/kasane_megademo_app/m32sys`（無ければ `tools/vmtest/m32_sysroot.sh` が作る）。
