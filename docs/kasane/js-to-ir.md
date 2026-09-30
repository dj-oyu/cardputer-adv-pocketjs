# JS 風の言語 → 手続き型 IR のコンパイラと、「IR を flash に置き id で登録」の見積もり

2026-09-30、ブランチ `vm/ir-compiler`（`vm/main` 909decd から）。DERBY WATCH の首振りカメラ（段階 3、`vm/pan-camera` 55956e9、マージしない）が実機の DRAM に収まらない件（`vm/pan-camera` の docs/apps/derby-pan-memory.md、[derby-pan-camera-cost.md](../apps/derby-pan-camera-cost.md)）への、ユーザーの提案「JS → IR コンパイラをチェックするのはどう」の調査と試作。**出荷コード（`apps/`・`main/`）は変えていない。** 試作は [`tools/kasane_ir/`](../../tools/kasane_ir/)。

数値の区別: **host 実測** は WSL の `-m32`（実機と同じ 8 B の JSValue、確保は実機の TLSF の長さで課金）。**ビルド確認** は ESP-IDF のツールチェーンで作ったオブジェクトの `nm`/`size`。**推定** は `sizeof` × 個数などの計算。実機は使っていない。

## 結論

- **(i) コンパイラは作る価値がある。ただし DRAM を減らすのはコンパイラではなく、IR をゲストに持たせないこと。** 手書きの IR はすでにほぼ最適で、コンパイラは静的な命令数を 467 → 453（−3.0%）、DERBY の実入力での実行ステップを −1.6% しか減らさない（confetti だけ −7.5%）。価値は保守性と誤りの防止: 名前と式で書ける、16 レジスタ・64 命令・CUBIC の r0..r7 をビルド時に機械が守る、死んだ命令を見つける（crowd の `I8,0`）、式マクロと展開で同じ式の繰り返し（首振りのニュートン法 4 段）を 1 行にできる。コンパイラはビルド時（host）に走らせるもので、実機のゲストでは走らせない（走らせればゲストのヒープを食う）。
- **(ii) 「IR を flash に置き id で登録」はゲスト側で約 7〜8 KB（host、評価後）効き、native 側の plan の常駐を直せばさらに 11〜24 KB（推定）効く。DERBY の不足（3〜11 KB）を紙の上では超える。** native を触らずに文字列を詰める案（§2.1）は、ゲストで −1.6〜2.5 KB（host）にとどまる。内訳:
  - ゲスト: plan の文字列 13 本と `prog()` を持たないと、評価後のヒープが **−7,108 B**（基準）/ **−8,104 B**（段階 3）、評価のピークが **−5,600 / −8,068 B**（host 実測）。実機では host の約 1.3 倍になった前例がある（derby-pan-memory.md）ので **−9〜10.5 KB 前後**（推定）。
  - 登録の一時的なピーク: 今は 1 本の登録で `prog()` が **3.1〜10.0 KB** を一時的に確保し、配列の配列 **2.1〜7.1 KB** を `register()` が返るまで保つ（host 実測、命令 1 つで約 119 B）。id 登録では引数の配列（8 個以下の数）だけになる。
  - native: **plan は命令数に依らず 1 本 872 B**（`code[64]` 固定、Xtensa のオブジェクトで確認）で、DERBY は同時に **25 本**（段階 3 で **30 本**）を持つ（host 実測）: **21.8 KB / 26.2 KB**。命令数ぶんだけ確保すれば **−11.0 / −12.0 KB**、flash の IR を指せば **−20 / −24 KB**（どちらも推定）。
- **(iii) 優先順位**: ① native の plan を命令数ぶんだけ確保する（API 不変、アプリの変更なし、推定 −11〜12 KB。可変長の確保による断片化は実機で見る）→ ② 組み込みアプリ用の「flash の plan を id で登録」（API 追加、ゲスト −7〜8 KB host、native はさらに約 −9〜12 KB 推定）と、その表を作るコンパイラ → ③ コンパイラを plan を書く標準の道具にする（②の生成器として入れば追加費用は小さい）。①は ②の前提ではないが、②を入れても JS から配列で登録する経路（保存したプログラム、MEGADEMO）が残るので、①は単独で意味がある。

## 1. 現状（コードで確認）

**JS → IR のコンパイラ・アセンブラ・逆アセンブラはリポジトリに無い。** あるのは 1 命令 1 文字のテキストを配列にする 10 行の `prog()` で、DERBY（[derby_prog.js](../../apps/derby/derby_prog.js)）、MEGADEMO（[proc_megademo.js](../../apps/kasane/proc_megademo.js)、`U` の短縮つき）、BIG WAVE（[big_wave.js](../../apps/bigwave/big_wave.js)）がそれぞれ写しを持つ。文字 → op の表引きと `$n` の置換だけで、検査は native の登録に任せている。[grid_fold.js](../../apps/kasane/grid_fold.js) は symbolic な式 builder だが、対象は型付きメモリ IR（`ksn_proc_grid`）で、この float VM ではない。

**登録時コンパイラ（D3a）はソース言語を持たない。** 入力は数値の行（`[op,dst,a,b,value,color]`）で、`pocket_proc.c` の `register_impl()` が読んで `ksn_proc_plan_prepare()`（[ksn_proc_plan.c](../../main/ui/kasane/ksn_proc_plan.c)）に渡す。prepare がするのは、IR を plan の `code[64]` へ写す、[ksn_proc_analysis.c](../../main/ui/kasane/ksn_proc_analysis.c) で有界 CFG・def-use・生存・失敗条件を取る、同じ基本ブロックで依存のある隣り合った算術 2 命令に融合の印（`fused_at`）を付ける、の 3 つだけ。命令の移動・削除・レジスタの割り当ては無い（「No code is moved or elided」）。PIE の選択は型付き Q14 点列（`affineQ14Points`）と grid の IR に対してで、この float IR には掛からない。

**命令セット**（[ksn_procedural.h](../../main/ui/kasane/ksn_procedural.h)、15 op）: `SET INPUT ADD MUL SIN REPEAT END MOVE PLOT LINE REPEAT_REG BREAK_IF_GT PLOT_COLOR_REG LINE_COLOR_REG CUBIC`。除算・減算・比較分岐・sqrt・floor・レジスタ間の MOV は無い（減算は `×−1` と加算、コピーは `+0`）。`CUBIC` は r0..r7 の 4 点を読む（dst=0 固定、分割 1..64）。制約: レジスタ 16、入力 8、1 plan 64 命令、ループの深さ 8、`REPEAT` 1..255（0 は検証で拒否）、`REPEAT_REG` 0..255 の整数（0 は本体を飛ばす）、1 draw の step 10,000・線分 1,024・ラスタ 8,192、座標 −480..720。`draw()` のたびに `begin` がプログラムを VM へ写して検証し直し、**レジスタは毎回 +0 から始まる**（`pocket_proc.c` は状態を持ち越す `begin_state` を使わない）。算術の結果が非有限、`REPEAT_REG`・色レジスタが範囲外の整数でない、座標が範囲外、のどれでも draw 全体が失敗する。

**メモリ**: `ksn_proc_inst` 12 B、`ksn_proc_plan` **872 B**（`code[64]` 768 B ＋ `fused_at[64]` ＋点列の係数）。Xtensa の `-Os` オブジェクトの `nm -S` で 0x368 = 872 B、m32 host でも同じ（ビルド確認）。plan は `calloc(1, sizeof *plan)` なので、1 命令の plan（DERBY の点列用 `S0,0` が 7 本）も 872 B を取る。VM（996 B）とフレームは共有で、plan の数に比例しない。

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

### 2.1 文字列を詰める案（native を触らない代替、`pack.mjs`）

ユーザーの問い「IR と 1:1 のテキストだから膨らんでいるのでは」への答え。**膨らんではいるが、詰めても効くのは 1.5〜2.5 KB で、flash 案（7.1〜8.1 KB）の 3 分の 1 以下。**

[`pack.mjs`](../../tools/kasane_ir/pack.mjs) が `derby_prog.js` の `T` を Latin-1 の詰めた文字列（QuickJS では 1 文字 1 バイト）に置き換え、`prog()` を同じ行を返す復号器に替える。全 plan で復号した行が今の `prog()` と一致することを確かめてから書き出す（行が同じなので、登録される IR も同じ）。

- **byte 形**: op 1 バイト、欄 1 つに 1 バイト（0..199 はそのまま、`$n`、16/24 bit、負号、小数は桁の文字列）。
- **nibble 形**: op と第 1 レジスタで 1 バイト、残りのレジスタは 2 つで 1 バイト（ユーザー案のとおり、レジスタは 4 bit、色は 2 バイト）。
- **共通の前置きのマクロ**: 先頭の `I0,0 I1,1 …` を 1 バイトにする。

| 形 | 基準 13 本（`T.map` の 8 行込み） | 段階 3 込み 17 本 | 評価後のヒープ差（基準 / 段階 3） | 評価のピーク差 |
| --- | ---: | ---: | ---: | ---: |
| 今の文字列 | 3,182 字 | 4,337 字 | — | — |
| byte 形（マクロあり） | 1,673 B | 2,216 B | −2,780 / −1,920 | −1,308 / −1,920 |
| nibble 形（マクロあり） | **1,080 B** | **1,376 B** | **−1,604 / −2,464** | **−1,604 / −2,464** |
| nibble 形（マクロなし） | 1,136 B | — | −1,552 / −2,364 | −1,552 / −2,364 |
| 参考: flash の plan（文字列も `prog()` も無い） | 0 | 0 | −7,108 / −8,104 | −5,600 / −8,068 |

（host m32 実測。ヒープ差は §2 と同じく表の伸長の閾値で ±1.5 KB 揺れる。byte 形が nibble 形より多く減って見える基準の値はその揺れ）

- **(a) 文字列の常駐**: atom は今 24,794 B。nibble 形で 22,885 B（**−1,909 B**）、byte 形で −1,347 B。plan の文字列が占める atom は今 約 3,525 B（flash 形で消える量）なので、nibble 形の文字列の常駐は **約 1.6 KB**。
- **(b) 復号器の常駐**: 関数と bytecode は今の `prog()` より **+436 B**。`prog()` 自体が約 600 B（関数の構造体 313 B、bytecode 283 B）なので、復号器は **約 1.0 KB**。
- **(c) 差し引き**: **−1.6 KB（基準）/ −2.5 KB（段階 3）**。段階 3 の plan 4 本（1.6 KB 相当の文字列）の分は −0.9 KB。
- **マクロと共有は効かない**: 先頭の入力のマクロは 56 B（ヒープで約 50 B）。13 本すべてを 1 本の deflate で圧縮しても、さらに 145 B（段階 3 で 239 B）しか縮まず、JS の展開器の方が大きい。短い並び（`V0,1`、`Q4` など）は 1〜2 バイトになった後で、共有して節約できる量が残らない。
- **登録のピークは下がる**: 復号器は `split` の一時的な文字列を作らないので、fr の登録のピークは 9,556 → 7,024 B、pt は 10,020 → 7,456 B（配列の配列はほぼ同じで残る）。flash 形では引数の配列だけになる。
- **再登録**: plan は場面ごとに `want()`/`drop()` で登録と解除を繰り返すので、文字列（詰めた形でも）は保ち続ける必要があり、使ったら捨てることはできない。flash 形だけが、この常駐も無くす。
- **判断**: 詰めた形は native を触らずに入る唯一の案だが、DERBY の不足（3〜11 KB）には足りない。flash 形か native の plan の縮小（§4 A）が要る。先に入れるなら、復号器が 1 つで済むので他のアプリ（MEGADEMO・BIG WAVE）にも使える点は利点。

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

## 5. plan を普通の JS の関数で書く（2026-09-30、ブランチ `vm/ir-js`）

ユーザーの指示「plan を別言語（`.kjs`）ではなく、アプリの JS ファイルの中の普通の JS の関数として書く」の実装と、出荷ソースから関数本体を消す変換の設計・試作。**`apps/` と CMake は変えていない**（`apps/derby` は derby-trim が別ブランチで編集中。組み込みはそのマージ後）。数値は host 実測。

### 5.1 流れ

1. **書く**: アプリの `.js` に、JSDoc の `@plan` を付けた関数を置く。`inputs:` の名前が draw の入力 0..7（この順）、関数の引数が登録時の引数（今の `$0`, `$1`, …）。
2. **host で走らせる**: [`plan_js.mjs`](../../tools/kasane_ir/plan_js.mjs) の `reference()` が、`move/line/plot/cubic` を線分を記録する偽物にして、その関数を Node で実行する。VM と同じ規則（座標の丸めと範囲、色の範囲、1,024 線分・8,192 ラスタ）で結果を返す。
3. **ビルド時に消す**: [`lower_plans.mjs`](../../tools/kasane_ir/lower_plans.mjs) が `@plan` 関数を `const f = '<詰めた IR>';`（§2.1 の nibble 形）に置き換え、`@planDecoder` の印の付いた関数を復号器に置き換える。`--ids APP` なら `const f = 'APP.name';`（§4 B の flash の plan。native の API はまだ無い）。
4. **実機**: 置き換えた後のソースだけが埋め込まれ、今と同じく `prog()` が行を作って `register()` する。

```js
/** @plan rail inputs: x0, dx, top, ground, posts, mid, colour */
function rail() {
  let x = x0;
  const end = posts * dx + x;
  move(x, top); line(end, top, colour);
  move(x, mid); line(end, mid, colour);
  for (let j0 = 0; j0 < posts; j0++) {
    move(x, top); line(x, ground, colour);
    x += dx;
  }
}
```

### 5.2 JS の部分集合と float32

| 規則 | 許すもの |
| --- | --- |
| R1 文 | `let`/`const`、`= += -= *=`、`for (let i = 0; i < n; i++)`、`if (a > b) break;`、`move/line/plot/cubic(...)` |
| R2 式 | 数値、名前、`+ - *`、単項 `-`、定数での `/`、`sin()` か `Math.sin()`、`Math.PI`、括弧 |
| R3 ループ | `for (let i = 0; i < n; i++)`（`++i`・`i += 1` も可）。`n` は本体で変わらない名前だけ、`i` は代入しない。`while`・`do`・`continue` は不可 |
| R4 名前 | `inputs:` の名前、引数、使う前に宣言したローカル。外側の名前を隠す宣言と `const` への代入は不可 |
| R5 使えないもの | 文字列、配列、オブジェクト、他の関数呼び出し、三項演算子、`if`/`for` の外の比較、`%`・`**`・ビット演算 |
| R6 break | `if (a > b) break;` か `if (a < b) break;`（中括弧可）を `for` の中でだけ。`>=`・`else` は不可（VM には BREAK_IF_GT しかない） |

外れるとコンパイルエラーになり、ファイル・plan・行・規則を出す。例: `derby_prog.js: t: line 4: the count reads n, which the body changes [R3: ...]`。パーサは依存なし（acorn は使わない）。[`test_plan_js.mjs`](../../tools/kasane_ir/test_plan_js.mjs) が 21 通りの違反と、受け付ける形（`Math.sin`、`Math.PI`、本体で読むループ変数、中括弧の break、引数の色と回数、0 回のループ）を確かめる。

- **意味の対応**: `for` は `REPEAT`（回数が定数か引数）か `REPEAT_REG`（式）。本体がループ変数を読むときだけ、その変数を 0 から 1 ずつ増やす命令を足す（`break` では増やさない。JS の `i++` と同じ）。定数 0 回のループは消す。何も描かない plan は `S0,0` 1 命令（`register()` は 1 命令以上を要求する。DERBY の点列用の plan）。
- **JS と VM が違うところ（コンパイラは直さない）**: ループ回数が整数でない・範囲外（`REPEAT_REG` は 0..255、定数・引数は 1..255）なら VM は draw ごと失敗するが、JS は切り上げた回数だけ回る。計算の途中が非有限なら VM は失敗する。`/` は定数でだけで、2 の冪以外は掛け算に直すので厳密ではない（警告が出る）。配列の表引き（`SILK[k]`）は書けないので、定数に焼き込むか引数で渡す（DERBY の `map` は焼き込んだ）。
- **float32 の基準**: `reference()` は既定で、関数の各演算を `Math.fround` で丸め、非有限で失敗し、ループ回数の検査も VM と同じにした JS を実行する（パーサの AST から生成した JS。関数の文面をそのまま double で走らせる形も持ち、参考として数える）。`sin` は `Math.sin` を float32 に丸めた値で、host の glibc の `sinf` と全件一致した。実機の newlib の `sinf` と、Xtensa の GCC が積和を 1 命令（`MADD.S`）に縮約するかは見ていない（CUBIC の C の式に効きうる）。

### 5.3 検証（host 実測）

- **IR**: 13 本を JS の関数にした [`plans_js/derby_plans.js`](../../tools/kasane_ir/plans_js/derby_plans.js)（`.kjs` から変換。rail・turf・crowd・runner・conf は名前つき、残りは逆コンパイルの名前 `r8`・`in0` のまま）が、13 本とも `.kjs` と**同じ IR** になった（`derby_plans.mjs --js`）。`.kjs` は移行の間は残す。
- **JS の関数 = VM**（`python3 tools/kasane_ir/check_equivalence.py --js tools/kasane_ir/plans_js/derby_plans.js`）: 捕獲した入力 6,971 本と摂動した入力 6,000 本で、float32 の JS の結果と、コンパイルした IR を実物の `ksn_procedural.c` で走らせた結果（状態・線分・色・ラスタ step）が**全件一致**（`JS REFERENCE PASS`）。摂動で VM が失敗した 1,032 本も、同じ種類の失敗で一致した。関数の文面を double のまま走らせると、捕獲した入力で 29 本（crowd・runner・pole・conf）、摂動で 176 本が 1 画素以上ずれる（単精度の VM との差で、誤りではない）。手書きの IR とコンパイル後の比較（§3）も同じ実行で `EQUIVALENCE PASS`。
- **DERBY 全体**（`python3 tools/kasane_ir/check_lowered.py`）: `apps/derby` のコピーを JS の関数の形に移し（[`migrate_derby.mjs`](../../tools/kasane_ir/migrate_derby.mjs)）、`lower_plans.mjs` で出荷形にして host の全台本を LIGHT・MID・HEAVY で走らせた。元の `apps/derby` と比べて、フレームごとの描画の統計（線分、ラスタ、draw 数、同時の plan 数、点）と着順が**同一**（`LOWERED PASS`）。ハーネス自身の画素照合も通る。違うのは VM の実行ステップだけで −0.6〜−1.1%。
- **大きさ**: 関数の形のソースは plan の部分が 6,896 B（コメントと名前込み）、出荷形は詰めた IR 1,109 B（14 本、`still` を含む）。評価後のゲストヒープは 108,244 → 106,692 B（**−1,552 B**、§2.1 の nibble 形と同じ効き）、評価のピークは 125,712 → 125,632 B（−80 B）。基準は vm/main bd6328e の `apps/derby`（§2 の 909decd から q24 段階 1・2 が入り、基準値が約 1.1 KB 上がった）。

### 5.4 ビルドへの組み込みの設計（未実装）

- **どこで変換するか**: `make_app_chunks.py`（configure 時）が、`@plan` を含むチャンクを見つけたら、そのファイルを「変換後の生成物」に差し替えた `APP_CHUNK_FILES` を書く。変換そのものはビルド時の `add_custom_command`（`OUTPUT ${CMAKE_BINARY_DIR}/generated/apps/<app>/<file>`、`DEPENDS` に元ファイルと `tools/kasane_ir/*.mjs`）で行う。configure 時に変換すると、plan を直すたびに再 configure が要る（今のチャンクは、編集では再 configure が要らない）。生成物は元と同じ base name にするので、埋め込みのシンボル（`_binary_derby_prog_js_start`）とチャンク表は変わらない。
- **Node**: IDF の環境には無い（この機体の PATH には Volta の Node v24.19.0 がある）。`find_program(POCKET_NODE node)` を、`@plan` を含むチャンクがあるときだけ必須にし、無ければ「tools/kasane_ir は Node が要る」で configure を止める。代案: 変換器を Python に移す（コンパイラ全体で約 1,000 行）か、生成物をコミットして host の検査で同期を見る（`check_flash.py` の流儀）。
- **入口のファイル**: `EMBED_TXTFILES` で埋め込む入口（`derby_watch.js`、今は plan を持たない）に plan を書く場合は、`DERBY_WATCH_SOURCE` と同じく生成物のパスへ差し替える。
- **host のツールも同じ変換を通す**: `tools/games/test_derby_host.c` は cwd の `apps/derby/chunks.txt` を読むので、`DERBY_APP_DIR`（チャンク表と入口の場所）を足し、`tools/games/run_derby.py` は `lower_plans.mjs` で `.cache/derby_host/lowered/apps/derby` を作ってからそこを渡す。`check_equivalence.py`（捕獲）、`derby_heap.py`、`check_lowered.py` も同じ生成物を読む。`derby_plans.mjs` は plan を `T` の文字列ではなく `@plan` の関数から読む（`--js` の経路を既定に）。`tools/games/bgcost`・`pancost`、`apps/heapprobe/derby_v0` は凍結したコピーなので変えない。
- **変更するファイル**（derby-trim のマージ後）: `tools/make_app_chunks.py`、`main/CMakeLists.txt`、`tools/hostshim/app_chunks_host.c` または `tools/games/test_derby_host.c`（`DERBY_APP_DIR`）、`tools/games/run_derby.py`、`tools/kasane_ir/derby_plans.mjs`・`derby_heap.py`・`check_equivalence.py`、`apps/derby/derby_prog.js`（`migrate_derby.mjs` の出力を手で整える: 名前の無い 8 本に名前、コメント）、`apps/derby/README.md`、`docs/platform/test-commands.md`（host の検査の手順）。

### 5.5 未実装・未確認

- CMake への組み込み、`apps/derby` の書き換え、host ハーネスの `DERBY_APP_DIR`（§5.4、derby-trim の後）。
- id 形（`--ids`）は生成だけで、受け取る native の API（§4 B）が無いので走らない。
- 段階 3 の 4 本（`pt` など）は JS の関数に移していない。`.kjs` の式マクロ（`function` の式）と `unroll` は JS の部分集合に入れていない（JS では入れ子の関数と展開を書くことになる）。
- 出荷形に plan の前のコメントが残る（評価のピークには効かなかった: −80 B の差は大半がソースの縮み）。消すなら変換で落とす。
- 実機での確認（`sinf` の実装差、`MADD.S` の縮約、評価後ヒープ）。

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
node tools/kasane_ir/test_plan_js.mjs                           # §5.2 の規則
python3 tools/kasane_ir/check_equivalence.py --js tools/kasane_ir/plans_js/derby_plans.js   # §5.3（WSL）
python3 tools/kasane_ir/check_lowered.py                        # §5.3 DERBY 全体（WSL）
node tools/kasane_ir/pack.mjs IN.js OUT.js [--nibble] [--no-macro]   # §2.1 の詰めた形（derby_heap.py が呼ぶ）
```

段階 3 の書き出しは Windows の git で `git show 55956e9:apps/derby/<file>` を `.cache/kasane_ir/stage3/apps/derby/` へ（WSL の git は worktree を読めないことがある）。m32 の sysroot は `.cache/kasane_megademo_app/m32sys`（無ければ `tools/vmtest/m32_sysroot.sh` が作る）。
