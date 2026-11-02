# Shiki v2.0 の草案

2026-11-02 起草。**草案（凍結前）。数値は断りのない限り推定。** 規範の本文は凍結の時に [shiki-spec.md](shiki-spec.md) へ写す。v1.5（2026-11-02 凍結）からの変更だけをここに書く。

v2.0 で入れるもの（利用者の決定、2026-11-02: 構想の 1〜5 をすべて含める）:

| | 中身 | 節 |
| --- | --- | --- |
| 1 | 旧 API（float の plan）を廃止し、描画エンジンを Shiki だけにする | §2 |
| 2 | データ領域を番号で読み書きする命令 | §3 |
| 3 | アプリの状態をデータ領域に置く（draw の番号を式から読む） | §4 |
| 4 | 描画の並びを残し、毎フレームは入力だけ変える | §5 |
| 5 | PIE に下ろせる範囲を広げる（ベクトルの命令を仕様に入れる） | §6 |

## 0. 今回の作業から持ち込む前提

- **JS の数は整数で渡す**のが基本（[derby-integer.md](../apps/derby-integer.md)）。DERBY は v1.5 の `drawMany` を `Int32Array` で使い、JS から double を無くした。
- **Shiki に割り算の命令は入れない**（v1.2 の §2.2「実行時の割り算は 32 ビット ÷ 32 ビットに限る」と、`b = 0` の `INVALID` がフレーム全体を捨てる問題。derby-integer.md §5.2）。逆数は `LOOKUP` の表と Newton（`MUL`・`SUB`）で作る（§3.4）。
- DERBY で「投影の式 → 走者の式」の受け渡しが書けなかったのは、`FETCH`/`PUT` が `EACH` のカーソルのレコードしか触れないため（derby-integer.md §8 の R1）。§3 がこれを直す。
- 型付き配列は 1 本ごとに配列と ArrayBuffer の 2 オブジェクト（約 150 B、host の推定）かかる。DERBY は小さな配列を 10 本ほど増やして約 3 KB 増えた。§4 はこれを領域 1 つにまとめる道を作る。

## 1. 版とバイト列

- バイト列のヘッダの版のバイトを **`2`** にする（v1 は `1`）。v2 のエンジンは版 `1` のバイト列を `INVALID_ARGUMENT` で拒む。**理由:** 保存された v1 の式を使うアプリが無い（組み込みの式はビルド時に表へ焼き、JS が渡す式は試験だけ）。互換の経路を持つと、検査の経路が 2 本になる。
- 命令の番号は v1 の 0〜26 を変えない。v2 の命令は 27 から（§3・§4・§6）。試作の op 27〜42（`ksn_shiki.c` の `VEACH` など）は番号を振り直す（試作の番号は仕様ではない）。
- 決めること **D1:** 版 `1` を拒むか、読み替えて受けるか（推奨: 拒む）。

## 2. 旧 API の廃止（構想の 1）

### 2.1 今の利用者（2026-11-02、`apps/` を検索）

| アプリ | 旧 API の使い方 | 移し先 |
| --- | --- | --- |
| DERBY WATCH | 背景の `@plan` 18 本（柵・芝・スタンド・観客・距離標・ゲート・大型画面・首振りの列 `prail`・`pt`・`hl`・ダスト・ボケ・帯・地図・紙吹雪・写真・絹・`nil`）。型付きの点（大写しの馬のコマ 6 枚、HEAD ON の `hd`） | `@shiki` に書き直す。点は `i16` の領域と `EACH`・`FETCH`・`PLOT`（v1.3 の決定） |
| MEGADEMO | `proc_megademo.js`・`megademo_view.js` の plan | `@shiki` |
| BIG WAVE | `big_wave.js` の plan | `@shiki` |
| 検査用 | `proc_limits_probe.js`・`proc_multi_surface_probe.js`・`overlay_proc_multi_probe.js`・`proc_news_zoom.js` | Shiki の検査へ置き換えるか、廃止 |

### 2.2 消すもの

- `pocket.kasane.procedural`（`register`・`draw`・`unregister`）とグローバルの `gridFold`。`beginFrame`・`commit`・`resource`・`createSurface`・`grid` は `pocket.kasane.shiki` に残す（v1 で共有済み）。
- float の VM: `main/ui/kasane/ksn_procedural.c`・`ksn_proc_plan*.c`・`ksn_proc_analysis.c`・`ksn_proc_points*.c`（PIE の点の経路を含む）と、`main/pocket/pocket_proc.c` の plan の部分、`derby_runner_affine.c`（実験、既に未使用）。
- ビルドの経路: `tools/kasane_ir/lower_plans.mjs` の詰めた形式と `emit_rom_plans.mjs`（`@plan` の表）。`@shiki` の表（`emit_shiki_rom.mjs`）だけが残る。
- 効果（推定、未測定）: flash と、旧 API の `.bss`・作業領域の DRAM。数字は §8 の手順で消す前後を測る。

### 2.3 Shiki 側に要るもの

| 旧 API にあって Shiki に無いもの | v2 での扱い |
| --- | --- |
| native からの draw（`pocket.derby.ser` が `pocket_proc_draw_numbers` で plan を描く） | native の呼び出し元は `ksn_shiki_draw_fixed`（Q 形式の整数）で描く。`ser` の出す値は C の中で Q 形式に直す |
| 型付きの点（`affineQ14Points`） | v1.3 で決定済み: `i16` の領域と `EACH`・`FETCH`・`PLOT`。点の数が多い（大写しの馬で 1 コマ約 80 点）ので、§6 の `EMIT_POINTS`・`EMIT_LINES` を使う |
| plan の解析・予算（`ksn_proc_analysis.c`） | Shiki の登録時の検査（§6.1 of spec）が代わる |
| 実数の入力（`H.draw(handle, ...numbers)`） | 決めること **D2**（§2.4） |

### 2.4 実数の入力を残すか（決めること D2）

- 案 **D2-a（推奨）: 整数だけにする。** `draw(handle, ...inputs)` の入力は int32（各入力の Q 形式の `fixed`）、`drawMany` は `Int32Array` だけ。`Float64Array` と §2.4 の double の変換を消す。JS から double を無くした DERBY と揃い、境目の検査が 1 通りになる。小数を JS で持つアプリは `x * 65536 | 0` を書く（それは JS 側の double で、アプリの選択）。
- 案 D2-b: 残す（v1.5 のまま）。

### 2.5 移す順

1. DERBY の背景（`@plan` 18 本と点）。`pocket.derby.ser` の draw を `ksn_shiki_draw_fixed` に。
2. MEGADEMO。
3. BIG WAVE。
4. 検査用のアプリ。
5. 旧 API の削除（§2.2）と、関係する host の検査・CI の整理。

各段で、host の全画素の検査（`run_derby.py` などの既存の照合）と実機の fps・メモリを、移す前と比べる。

## 3. データ領域を番号で読み書きする（構想の 2）

### 3.1 命令

| `op` | 命令 | 動き | 実行時の失敗 |
| ---: | --- | --- | --- |
| 27 | `LOAD dst, a, b, aux=c \| m << 8, imm=k` | 領域 `a` のレコード `r[b]` の欄 `c`〜`c + m − 1` を `SHIFT(·, k)` して `r[dst..dst+m−1]` へ | `r[b]` が 0〜`R − 1` の外なら `INVALID` |
| 28 | `STORE dst, a, b, aux=c \| m << 8, imm=k` | 逆向き。`PUT` と同じく `SHIFT` してから要素の型の範囲へ飽和して書く | 同上。領域が書く旗（§3 of spec）を持たなければ登録で拒む |

- `aux` の下位 8 ビットが欄の位置 `c`、上位 8 ビットが個数 `m`（1〜8）。`c + m ≤ k_rec`（登録で検査）。
- `r[b]` は整数（Q0）として読む（`EACH` の件数と同じ）。
- 番地はレコード `i = r[b]` の欄 `c + j` → 領域の先頭から `(c + j) · S + i`（§5.1 of spec）。詰め物には当たらない。
- 書き込みの規則（§5.3 of spec）の 4「書き込みは `EACH` のカーソルのレコードの中だけ」を外し、「`PUT` はカーソルのレコード、`STORE` は `r[b]` のレコード」にする。それ以外（即座に見える、失敗しても戻らない）は同じ。

### 3.2 範囲外の扱い（決めること D3）

- 案 **D3-a（推奨）: `INVALID`。** 読み書きとも、範囲外は式の誤り。書き込みを端に寄せると、別のレコードを黙って壊す。
- 案 D3-b: 読みは `LOOKUP` と同じく端に寄せ、書きは `INVALID`。

### 3.3 PIE との関係

`LOAD`・`STORE` は §8.4 of spec（PIE に下ろせる `EACH`）の本体に置けない（番地がレーンごとに違う。gather/scatter は PIE に無い）。スカラーで実行する。

### 3.4 割り算の書き方（規範ではなく、変換器の手引き）

`@shiki` の `x / y`（`y` が変数）は v1 と同じく変換器が拒む。逆数が要る式は、逆数の表（`k_rec = 1` の領域、`R` 個の刻み）を `LOOKUP` で引いて初期値にし、Newton 1〜2 回（`r ← r (2 − y r)`、`MUL` 2・`SUB` 1）で詰める。精度と命令の数の手本を変換器の試験に置く（DERBY の投影の誤差 0.07 px、[derby-pan-camera-cost.md](../apps/derby-pan-camera-cost.md) §2）。

## 4. アプリの状態をデータ領域に置く（構想の 3）

### 4.1 命令

| `op` | 命令 | 動き |
| ---: | --- | --- |
| 29 | `DRAW_INDEX dst` | `r[dst]` = この draw の番号（`drawMany` の何番目か、0 から）。`draw()` から呼ばれたときは 0 |

- 1 本の式を `drawMany` で N 回呼び、各回が自分の番号のレコードを `LOAD` で読む。JS は入力を組まない（`stride = 0`）。例: DERBY の 8 頭は「状態の領域（レーンごとの位置・奥行き・色）＋同じ handle を 8 個並べた `handles`」で描ける。

### 4.2 JS の側

- 状態はアプリが `shiki.data()` で作る**少数の領域**に置き、JS は窓（型付き配列 1 本）に整数を書く。欄は `窓[c · S + j]`（`S` は §5.1 of spec の式）。`fields()` の view は作るたびにゲストのヒープを使うので、毎フレームの経路では窓を直接使う（手引き）。
- 領域の数の上限（`maxSessionDataRegions` 16）と予算（既定 48 KiB）はそのまま。
- 決めること **D4:** 窓の `S` を JS に渡す読み取り専用のプロパティ（`窓.stride` のような名前）を足すか、アプリが式で求めるか（推奨: 足さない。v1.4 で式を固定済みで、プロパティは窓ごとにゲストのメモリを使う）。

### 4.3 効果の見込み（推定）

DERBY の走者・正面の馬で、毎フレームの `bput`（1 頭 7 入力 × 2 本）と `Int32Array` の組み立てが消え、位置と奥行きを書くだけになる。型付き配列の本数は約 10 本から 2〜3 本へ。実測は移行の時に（§8）。

## 5. 描画の並びを残す（構想の 4）

### 5.1 案

- 案 **L-a（推奨）: 手引きだけ。** §4 の `DRAW_INDEX` と状態の領域を使えば、`handles` は変わらない `Uint32Array` を 1 本持ち続け、毎フレームは `drawMany(handles, null, 0)` の 1 回になる。描く順（DERBY の奥から手前）は、順番の表を領域に置き、式が `DRAW_INDEX` で表を引いてからレコードを読む（`LOAD` 2 回）。新しい API は要らない。
- 案 L-b: native のリスト。`shiki.list(handles)` が Shiki のヒープに並びを写して handle を返し、`shiki.drawList(list, from?, count?)` が描く。JS から `Uint32Array` を渡す費用（1 回の呼び出しで読み 1 回）が消えるが、それは L-a でも呼び出しに 1 回だけ。リストの寿命・`unregister` との関係・予算の規則が増える。

決めること **D5:** L-a か L-b か（推奨: L-a。L-b は L-a で測って足りないときに）。

### 5.2 途中に別の描画を挟む

DERBY は走者の間に大写しの馬（旧 API の点）を挟んでいた（`bflush` してから描く）。§2 で大写しの馬も Shiki に移れば、全部を 1 本の `drawMany` に並べられる。挟む描画が Shiki の外（画像など）に残る場合は、`drawMany` を分ける（v1.5 と同じ）。

## 6. PIE の範囲を広げる（構想の 5）

### 6.1 方針（決めること D6）

v1.5 で PIE に下ろすのは「本体全体が `.H` の列の `EACH`」の自動の下ろしだけ（§8.4 of spec）。広げ方は 2 つある（[shiki-pie.md](shiki-pie.md) §3）。

- 案 **V（推奨）: ベクトルの命令を仕様に入れる。** 試作（op 27〜42、[shiki-pie.md](shiki-pie.md) §8）を規範にする。実測で affine128 はスカラーの 4.4 倍（PIE、SoA）。書き手が 8 レーンを明示し、エンジンは命令をそのまま PIE に写す。自動の下ろしは v1.5 のまま残す（両方ある）。
- 案 A: 自動の下ろしを広げる（本体の一部の列、累算のある列）。書き手は変わらないが、下ろせるかの判定と、スカラーとのビット一致の証明が命令の組み合わせごとに要る。

### 6.2 案 V の命令（試作から）

| `op` | 命令 | 動き（要旨。詳細は試作 `ksn_shiki.c` と shiki-pie.md §8.1） |
| ---: | --- | --- |
| 30 | `VEACH a, b, aux=f` | 領域 `a` を 8 レコードのブロックで回す。端数はマスク。本体の命令は 1 ブロックに 1 回・1 ステップ |
| 31・32 | `VLOAD vd, a, b=欄`・`VSTORE vd, a, b=欄` | `i16` の領域の欄の 8 レコード（面の 128 ビット、16 B 整列） |
| 33・34 | `VSPLAT vd, a`・`VIOTA vd` | スカラーを全レーンへ・レーン番号 |
| 35〜38 | `VADD`・`VSUB`・`VMIN`・`VMAX` | 16 ビット、飽和 |
| 39〜43 | `VZACC`・`VMAC`・`VMACK`・`VACCSET`・`VOUT` | 40 ビットの蓄積器。`VOUT` のシフトは **0〜32**（試作の実機の照合で 40・63 が C モデルと食い違った、shiki-pie.md §8.2） |
| 44・45 | `EMIT_POINTS`・`EMIT_LINES` | 領域の列から native のループで点・線を描く（§2.3 の点の移行に使う） |

- 登録の検査: `VEACH` の本体はベクトルの命令と `END` だけ、ベクトルの命令は `VEACH` の外に置けない、入れ子不可（試作のまま）。
- ベクトルのレジスタの数・蓄積器の数・`@shiki` での書き方は、凍結前に決める（決めること **D7**）。
- ビット一致: C モデルと PIE の照合（実機）を、仕様の検証の要件（§10 of spec）に足す。

## 7. 決めることの一覧

| | 決めること | 推奨 |
| --- | --- | --- |
| D1 | 版 `1` のバイト列を拒むか | 拒む |
| D2 | 実数の入力（`draw` の実数、`Float64Array`）を残すか | 整数だけにする |
| D3 | `LOAD`・`STORE` の範囲外 | 読み書きとも `INVALID` |
| D4 | 窓に `S` のプロパティを足すか | 足さない |
| D5 | 並びを残す方法 | 手引きだけ（L-a） |
| D6 | PIE の広げ方 | ベクトルの命令を仕様に（V） |
| D7 | ベクトルのレジスタ・蓄積器の数、`@shiki` での書き方 | 凍結前に、試作と DERBY の背景の移行で決める |

## 8. 検証と進め方

1. この草案を SLOW の独立レビューに出し、反映する。
2. D1〜D7 を利用者が決める。
3. 実装は §2.5 の順に、各段で host の照合（全画素・oracle）と実機（fps・ゲストと native のメモリ・評価の余裕）を移す前と比べる。旧 API の削除（§2.2）の前後で flash・`.bss`・空きヒープを `tools/memlog.py --port` で測る。
4. 実機の照合: `LOAD`・`STORE`・`DRAW_INDEX` の JS API の検査（`apps/shikitest/`）、ベクトルの命令の C モデルと PIE の一致（shiki-pie.md §8.2 の形）。
5. 凍結の時に [shiki-spec.md](shiki-spec.md) を v2.0 に書き換える（§11 の移行は完了として消し、§12 の未決を更新する）。
