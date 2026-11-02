# Shiki v2.0 の草案

2026-11-02 起草、同日 SLOW の独立レビュー（重大 3・中 6・軽微 5、§10）を反映。**草案（凍結前）。数値は断りのない限り推定。** 規範の本文は凍結の時に [shiki-spec.md](shiki-spec.md) へ写す。v1.5（2026-11-02 凍結）からの変更だけをここに書く。

v2.0 で入れるもの（利用者の決定、2026-11-02: 構想の 1〜5 をすべて含める）:

| | 中身 | 節 |
| --- | --- | --- |
| 1 | 旧 API（float の plan）を廃止し、描画エンジンを Shiki だけにする | §2 |
| 2 | データ領域を番号で読み書きする命令 | §3 |
| 3 | アプリの状態をデータ領域に置く（draw の番号を式から読む、開始レコード） | §4 |
| 4 | 描画の並びを残し、毎フレームは状態だけ変える | §5 |
| 5 | PIE に下ろせる範囲を広げる（ベクトルの命令を仕様に入れる） | §6 |

## 0. 今回の作業から持ち込む前提

- **JS の数は整数で渡す**のが基本（[derby-integer.md](../apps/derby-integer.md)）。DERBY は v1.5 の `drawMany` を `Int32Array` で使い、JS から double を無くした。
- **Shiki に割り算の命令は入れない**（v1.2 の §2.2「実行時の割り算は 32 ビット ÷ 32 ビットに限る」と、`b = 0` の `INVALID` がフレーム全体を捨てる問題。derby-integer.md §5.2）。逆数は `LOOKUP` の表と Newton（`MUL`・`SUB`）で作る（§3.4）。
- DERBY で「投影の式 → 走者の式」の受け渡しが書けなかったのは、`FETCH`/`PUT` が `EACH` のカーソルのレコードしか触れないため（derby-integer.md §8 の R1）。§3 がこれを直す。
- 型付き配列は 1 本ごとに配列と ArrayBuffer の 2 オブジェクト（約 150 B、host の推定）かかる。DERBY は小さな配列を 10 本ほど増やして約 3 KB 増えた。§4 はこれを領域 1 つにまとめる道を作る。

## 1. 版とバイト列

- **版のバイトは `1` のまま**（レビュー M1）。v2.0 の変更は命令の追加（27 以降）と §5.3 の 4 の緩和だけで、v1 のバイト列の意味は変わらない。凍結済みの §3「版は同じバイト列が違う意味になる非互換の変更でだけ上げる」に従う（v1.2・v1.3 の命令の追加も版 `1` のまま）。マジック `'SKI1'` も変えない。
- 命令の番号は v1 の 0〜26 を変えない。v2 の命令は 27 から（§3・§4・§6）。試作の op 27〜42（`ksn_shiki.c` の `VEACH` など）は番号を振り直す（試作の番号は仕様ではない）。
- 「仕様書の版（v2.0）」と「バイト列の版（`1`）」は別に数える。旧 API を消すこと（§2）は JS API の非互換の変更なので、仕様書の版を 2 に上げる。

## 2. 旧 API の廃止（構想の 1）

### 2.1 今の呼び出し元（2026-11-02、`apps/` と `main/` を検索）

| 所 | 旧 API の使い方 | 移し先 |
| --- | --- | --- |
| DERBY の JS（`derby_view.js` など） | `H.beginFrame`・`H.resource`・`H.register`・`H.draw`・`H.commit`（`derby_view.js` の先頭で `H = V.procedural`） | `SK.*` へ書き換える。`register`・`draw` は §2.3 の移行の後 |
| DERBY の C（`pocket_derby.c`） | `side()`・`part()`・`aim()`・`wide()`・`ser()` の中の `draw()` から `pocket_proc_draw_numbers` を 8 か所で呼び、`view.hl`・`view.crowd` など旧 API の handle を持つ（レビュー R2） | すべての draw を Shiki の fixed の経路（`pocket_proc_shiki_draw_inputs`）に。`plan_handle` が Shiki の handle を受ける |
| DERBY の `@plan` 18 本 | 柵・芝・スタンド・観客・距離標・ゲート・大型画面・首振りの列 `prail`・`pt`・`hl`・ダスト・ボケ・帯・地図・紙吹雪・写真・絹・`nil` | `@shiki` に書き直す |
| DERBY の型付きの点 | 大写しの馬のコマ 6 枚（各約 80 点）、HEAD ON の `hd` | `i16` の領域と `EMIT_POINTS`・`EMIT_LINES`（§6.2、v1.3 の決定） |
| MEGADEMO | `proc_megademo.js`・`megademo_view.js` | `@shiki` |
| BIG WAVE | `big_wave.js` | `@shiki` |
| 検査用 | `proc_limits_probe.js`・`proc_multi_surface_probe.js`・`overlay_proc_multi_probe.js`・`proc_news_zoom.js`・`grid_fold_device_probe.js`（グローバルの `gridFold`）、`apps/heapprobe/derby_v0/`（旧 DERBY の複製、評価の余裕の二分探索に使う） | Shiki の検査へ置き換えるか廃止。heapprobe の DERBY は今の DERBY から作り直す |
| `pocket_shape.c` | 旧 API のフレームに直接書く（`pocket_proc_native_*`） | **残す**（フレームの部品は残る、§2.2） |

### 2.2 消すもの・残すもの

`beginFrame`・`commit`・`resource`・`createSurface`・`grid` は今も `pocket.kasane.shiki` から呼べる（`pocket_shiki_api.c` の `share()` が `procedural` の関数を写している）が、**実装は `pocket_proc.c` にある**（レビュー R1）。したがって `pocket_proc.c` は丸ごと消せない。

- **消す:** JS の `pocket.kasane.procedural` 名前空間とグローバルの `gridFold`。`pocket_proc.c` の plan の部分（plan の slot、`draw_run`、点の読み込みと結び付け、組み込み plan の登録、bgcost の計装）。float の VM（`ksn_procedural.c`・`ksn_proc_plan*.c`・`ksn_proc_analysis.c`・`ksn_proc_points*.c`）。`derby_runner_affine.c`（実験、既に未使用）。ビルドの経路の `@plan` の部分（`lower_plans.mjs` の詰めた形式と `emit_rom_plans.mjs`）。
- **残す:** フレーム（`beginFrame`・`commit`）、面（`createSurface`）、`resource`、image mode、帯の差分（band damage）、作業領域、`pocket_proc_native_*`（`pocket_shape.c` が使う）、`grid`。これらは `pocket.kasane.shiki` から直接載せる（`share()` をやめる）。
- 効果（推定、未測定）: flash と、旧 API の `.bss`・作業領域の DRAM。§8 の手順で消す前後を測る。
- 文書の更新: CLAUDE.md の「`@plan` が無いアプリは Node 不要」は、`@shiki` の組み込みの表（`emit_shiki_rom.mjs`）も Node を要るので「`@plan` か `@shiki` が無いアプリは」に直す（レビュー M6）。

### 2.3 Shiki 側に要るもの

| 旧 API にあって Shiki に無いもの | v2 での扱い |
| --- | --- |
| native からの draw（`pocket.derby` の全部） | `pocket_proc_shiki_draw_inputs`（Q 形式の整数）で描く。`ser` の内部は倍精度のまま（[derby-ser-native.md](../apps/derby-ser-native.md)）なので、C の中で §2.4 of spec と同じ丸めで Q 形式に直す |
| 型付きの点（`affineQ14Points`） | `i16` の領域と `EMIT_POINTS`・`EMIT_LINES`。大写しの馬のコマは 6 枚を 1 つの領域に並べ、開始レコードで選ぶ（§4.1 の `EACH`・`EMIT` の開始位置） |
| plan の解析・予算（`ksn_proc_analysis.c`） | Shiki の登録時の検査（§6.1 of spec）が代わる |
| 実数の入力（`H.draw(handle, ...numbers)`） | 決めること **D2**（§2.4） |

### 2.4 実数を残すか（決めること D2）

- 案 **D2-a: 整数だけにする。** `draw` の入力、`drawMany` の入力（`Int32Array` だけ）、`register` の引数（今は `const double *args` を §2.4 of spec で変換している）をすべて int32（各 Q 形式の `fixed`）にし、§2.4 of spec の double の変換を消す。**費用（レビュー M2）:** 検証の行の書式（§10.1 of spec の `ARGS`・`INPUTS` を `strtod` で読んでいる。`shiki_run.c`・`shiki_ref.mjs` の §2.4 の実装）を整数に変え、`apps/shikitest/` の検査と全 CASE を直す。DERBY・MEGADEMO・BIG WAVE の引数（色・係数）も整数で書く。
- 案 D2-b: 残す（v1.5 のまま）。境目の変換は 2 通り（実数と `Int32Array`）のまま。

**決定（2026-11-02、利用者）: D2-b、実数も残す。** 境目は v1.5 のまま、実数（§2.4 of spec の変換）と `Int32Array`（変換なし）の 2 通り。整数で書くか実数で書くかはアプリが選ぶ。

### 2.5 移す順

0. **フレームの関数を Shiki に載せ替える**（レビュー R1）: `pocket.kasane.shiki` に `beginFrame`・`commit`・`resource`・`createSurface`・`grid` を直接載せ、全アプリの `H.beginFrame` などを `SK.` に書き換える（`procedural` の名前はまだ残す）。
1. DERBY: `pocket.derby` の全 draw を Shiki に（R2）、`@plan` 18 本と点を `@shiki` に。
   - 進み（2026-11-02）: `@plan` は `nil`（型付きの点）だけを残して `@shiki` に移した。`pocket.derby` の C の描画は handle の種類で engine を選ぶ（`pocket_proc_draw_numbers`）。誤差と `@q` は [apps/derby/README.md](../../apps/derby/README.md)「背景の式もすべて Shiki」。残りは点（`i16` の領域と `EMIT_LINES`）と、実機の確認。
2. MEGADEMO。
   - 済（2026-11-02）: 24 本を `@shiki`（`apps/kasane/megademo_prog.js`）に。`eg` の `s²·noise` が Q16 を超えたので位相を差分で持つ形にした。猫の船 `sh` は i16 の窓。誤差と host・実機の結果は [megademo-story.md](../apps/megademo-story.md)「Shiki へ」。
3. BIG WAVE。
   - 済（2026-11-02）: [apps/bigwave/README.md](../../apps/bigwave/README.md)。実機はデモ画面で約 20 秒（ターン 8.3〜9.7 ms）。
4. 検査用のアプリと heapprobe の DERBY。
   - 済（2026-11-02）: `proc_limits_probe.js`・`proc_multi_surface_probe.js`・`overlay_proc_multi_probe.js`・`proc_news_zoom.js` と、float の VM を測っていた実機の probe（`ksn_procedural_device_probe.c`・`ksn_proc_compiler_device_probe.c`・`ksn_proc_limits_device_probe.c`・`ksn_megademo_device_probe.c`）を消した。D2 の 2 つの面の probe は、overlay のセッションに Shiki の heap が無いので移せない（CMake のオプションを ON にすると configure が止まる）。`KASANE_PROC_DEVICE_PROBE` も同じ（grid・pixel の probe は残るが、呼ぶ入口が要る）。heapprobe の DERBY は、旧 DERBY の複製（`apps/heapprobe/derby_v0/`）をやめ、configure の時に今の DERBY から 1 本のスクリプトを作る形に作り直した（`tools/heapprobe_derby.py`、[derby-watch.md](../apps/derby-watch.md)「heapprobe の変種」）。`grid_fold_device_probe.js` は grid（残る）の検査なので残した。
5. 旧 API の削除（§2.2）と、関係する host の検査・CI の整理。
   - 済（2026-11-02）: `pocket.kasane.procedural`、`pocket_proc.c` の plan の部分（slot は Shiki の handle だけ、float の register・draw・型付きの点・組み込み plan の表・bgcost の計装・runner affine）、float の VM（`ksn_procedural.c` は frame と帯の描画だけに）、`ksn_proc_plan*`・`ksn_proc_analysis*`・`ksn_proc_points*`、`@plan` の詰めた形式（`lower_plans.mjs` は `@shiki` を消すだけ）と `emit_rom_plans.mjs`・`pack.mjs`。`beginFrame`・`commit`・`resource`・`createSurface` は `pocket.kasane.shiki` だけに載る。`grid` と、`grid` の書き方の層 `gridFold`（グローバル）は grid の一部なので残した（§2.2 の「`gridFold` を消す」は取り消し）。
   - 実機（e7b6d28、2026-11-02、入力なし）: DERBY はデモのレースを FINISH まで（読み込み後の空き 47,640 B、ゲスト 97,556 B、ターン 4.9〜16.8 ms）、MEGADEMO は tier 2 で 1 周余り（空き 37,356 B、旧 API があった 427b308 の 36,820 B から +536 B）、BIG WAVE はデモ画面で 40 秒（空き 74,108 B、dedda13 の 73,808 B から +300 B）。いずれも失敗なし。ファームは DIRAM 180,924 B・flash 1,601,184 B（CI の memlog、削除前の値はまだ同じ形で採っていない）。ログは [logs/v2-removal-device-20261102-e7b6d28.txt](logs/v2-removal-device-20261102-e7b6d28.txt)。
   - CI: BIG WAVE を v2.0 より前の固定の版と並べて走らせる実験の 3 本（`validate-curl-section.yml`・`adopt-rock-distance.yml`・`bigwave-input-literals.yml`）は、旧版の側が動かないので外した（2026-11-02、利用者の了承。スクリプトは `tools/` に記録として残る）。

各段で、host の全画素の検査（`run_derby.py` などの既存の照合）と実機の fps・メモリを、移す前と比べる。

## 3. データ領域を番号で読み書きする（構想の 2）

### 3.1 命令

| `op` | 命令 | 動き | 実行時の失敗 |
| ---: | --- | --- | --- |
| 27 | `LOAD dst, a, b, aux=c \| m << 8, imm=k` | 領域 `a` のレコード `r[b]` の欄 `c`〜`c + m − 1` を `SHIFT(·, k)` して `r[dst..dst+m−1]` へ | `r[b]` が 0〜`R − 1` の外なら `INVALID` |
| 28 | `STORE dst, a, b, aux=c \| m << 8, imm=k` | 逆向き。`PUT` と同じく `SHIFT` してから要素の型の範囲へ飽和して書く | 同上。領域が書く旗（§3 of spec）を持たなければ登録で拒む |

- `aux` の下位 8 ビットが欄の位置 `c`、上位 8 ビットが個数 `m`（1〜8）。`c + m ≤ k_rec`、`dst + m ≤ 16`（登録で検査）。`c ≤ 7`・`m ≤ 8` なので 16 ビットに収まる。
- §3 of spec の引数の当てはめは `aux` 全体を Q0 の 1 値で置き換えるので、`c`・`m` を別々の引数にはできない（許容。どちらも定数で書く）（レビュー L1）。
- `r[b]` は整数（Q0）として読む（`EACH` の件数と同じ）。`EACH` の字句の入れ子は要らない（`FETCH` と違い、§6.1 of spec の結び付けの検査の対象外）。
- 番地はレコード `i = r[b]` の欄 `c + j` → 領域の先頭から `(c + j) · S + i`（§5.1 of spec）。詰め物には当たらない。
- §5.3 of spec の 4「書き込みは `EACH` のカーソルのレコードの中だけ」を外し、「`PUT` はカーソルのレコード、`STORE` は `r[b]` のレコード」にする。それ以外（即座に見える、失敗しても戻らない）は同じ。
- 命令ごとに 1 ステップ。

### 3.2 範囲外の扱い（決めること D3）

**決定（2026-11-02、利用者）: 読み書きとも `INVALID`。** 範囲外は式の誤り。書き込みを端に寄せると、別のレコードを黙って壊す（読みを端に寄せる案は採らなかった）。

### 3.3 PIE との関係

`LOAD`・`STORE` は自動の下ろし（§8.4 of spec）の本体に置けない（番地がレーンごとに違う。gather/scatter は PIE に無い）。§8.4 of spec の条件 1 の許す命令の一覧はそのまま（`LOAD`・`STORE`・`DRAW_INDEX` を足さない）と、凍結の時に本文で明記する（レビュー M5）。

### 3.4 割り算の書き方（規範ではなく、変換器の手引き）

`@shiki` の `x / y`（`y` が変数）は v1 と同じく変換器が拒む。逆数が要る式は、逆数の表（`k_rec = 1` の領域、`R` 個の刻み）を `LOOKUP` で引いて初期値にし、Newton 1〜2 回（`r ← r (2 − y r)`、`MUL` 2・`SUB` 1）で詰める。精度と命令の数の手本を変換器の試験に置く（DERBY の投影の誤差 0.07 px、[derby-pan-camera-cost.md](../apps/derby-pan-camera-cost.md) §2）。

## 4. アプリの状態をデータ領域に置く（構想の 3）

### 4.1 命令と変更

| `op` | 命令 | 動き |
| ---: | --- | --- |
| 29 | `DRAW_INDEX dst` | `r[dst]` = この draw の番号（`drawMany` の何番目か、0〜`maxDrawsPerCall − 1`）。`draw()` から呼ばれたときは 0。1 ステップ |

- **開始レコード（レビュー R3）:** `EACH`・`VEACH` に、`aux` の bit 1 で「`imm` のレジスタ番号の値を開始レコードにする」旗を足す（v1 の `EACH` の `aux` は bit 0 だけ使う）。`aux` は 0〜3、bit 1 が無ければ `imm` は 0、あれば `imm` は 0〜15（`imm` は整数の欄になり、引数の当てはめは Q0 だけ）。入るときに開始 `s`（0〜`R`）、次に件数（bit 0 なら `r[b]` で 0〜`R − s`、無ければ `R − s`）を 1 回ずつ読み、外なら `INVALID`。カーソルは `s` から始まる。`VEACH` は `s + 件数` が 32,767 を超えると `INVALID`（`VIOTA` のレーンの値はレコードの番号なので）。
  - **`EMIT_POINTS`・`EMIT_LINES` は旗を `dst` に置く**（実装で決めた。`aux` は色なので空きが無い）。`dst` の bit 0 が件数、bit 1 が開始。開始は `r[b]`、件数は `r[b]`（開始なし）か `r[b + 1]`（両方）。範囲の規則は `EACH` と同じ。
  - 実装（2026-11-02）: `ksn_shiki.c`、参照 `shiki_ref.mjs`（`EACH`・`LOAD`・`STORE`・`DRAW_INDEX`。ベクトルと `EMIT` は参照に無く、`test_shiki_vec.c` が確かめる）、差分の検査 `shiki_diff.mjs`、検証の行の `INDEX k`（`INPUTS` の後、`DATA` の前。無ければ 0）。これで「6 コマを 1 領域に並べ、番号で 1 コマを描く」「レーン `l` の列だけ回す」が書ける。
- **配管（レビュー M3）:** draw の番号を受ける `ksn_shiki_draw_index` を足した（`ksn_shiki_draw_fixed` は番号 0 で呼ぶ）。検証の行（§10.1 of spec）に番号の欄を足す。`drawMany` で draw `k` が失敗すると `k + 1` 以降は行われない（v1.5 のまま）ので、「draw `k` の失敗の後、`k + 1` 以降の `STORE` は起きない」を §5.3 of spec に足す。

### 4.2 JS の側

- 状態はアプリが `shiki.data()` で作る**少数の領域**に置き、JS は窓（型付き配列 1 本）に整数を書く。欄は `窓[c · S + j]`、`S = (R + 7) & ~7`（`ksn_shiki_stride` と同じ式）（レビュー L2）。`fields()` の view は作るたびにゲストのヒープを使うので、毎フレームの経路では窓を直接使う（手引き）。
- 領域の数の上限（`maxSessionDataRegions` 16）と予算（既定 48 KiB）はそのまま。
- **D4（決定、2026-11-02、利用者）: 窓に `S` のプロパティは足さない。** 式（`(R + 7) & ~7`）は v1.4 で固定済みで、プロパティは窓ごとにゲストのメモリを使う。アプリが自分で計算する。

### 4.3 効果の見込み（推定）と、DERBY で残る仕事

DERBY の走者（今は 1 頭 7 入力 × 2 本を毎フレーム `bput`）を例にすると:

- **消せる:** 入力の組み立て（`BI` の `Int32Array` への書き込み）。式が `DRAW_INDEX` でレーン番号を得て、状態の領域（位置・奥行き・色）を `LOAD` で読む。さらに `pocket.derby.lanes()` が投影の結果（今は `PO` へ書く `X`・`S`・`Y`）を状態の領域の窓へ直接書けば、JS のレーンごとの仕事は位置と奥行きを書くことだけになる（推定、未測定）。
- **残る（レビュー R3）:** 毎フレームの選択。(a) 画面外の走者を飛ばす（Shiki は座標が −480〜720 の外だと `INVALID` でフレームを捨てるので、間引きは必須）、(b) 中継で描く頭数を `FN[tier]` に絞る、(c) 奥から手前への順（毎フレーム変わる）。(a) は式の中で `REPEAT 1` と `BREAK_IF_GT` で書ける（1 条件 3〜4 命令、`run` は 42 命令なので収まる見込み）。(b)・(c) は `drawList` の順番の表（§5.1）で表す。式は順番を知らない。**「JS の仕事が 0」ではなく「レーンの状態を書くことと、並びの表の更新」**になる。
- 3 と 2 の関係: 投影の式がレーンごとの `X`・`S`・`Y` を `STORE` し、`drawList` が順番の表の順に `run`・`legs` を描き、式は `DRAW_INDEX`（リストの中の番号）→ そのレーンのレコードを `LOAD`、という流れで、DERBY の R1 の問題（投影を C に置いた理由）が Shiki の中で解ける（レビュー L5）。

## 5. 描画の並びを残す（構想の 4）

**決定（2026-11-02、利用者）: native のリスト（案 L-b）。** 手引きだけの案（不変の `Uint32Array` を `drawMany` に渡す）は採らなかった。以下は規則の草案（凍結前にレビューする）。

規則の決定（2026-11-02、利用者。すべて推奨のとおり）:

- **順と間引きはリストが持つ（案 B）。** `drawList` が順番の表（`u16` の窓）を受け、native が回す。式は順番の表を読まない（草案の「式が順番の表を `DRAW_INDEX` と `LOAD` で読み、自分で抜ける」案 A は、どの式にも同じ 3〜4 命令が要るので採らなかった）。JS は毎フレーム順番の表と `count` を書く。
- **リストは入力を持たない。** 状態はデータ領域（§4）。
- **途中の draw が失敗したら止める**（`drawMany` と同じ。後の draw は行わない）。

実装（2026-11-02）: `pocket_shiki_api.c` の `list`・`setList`・`drawList`・`releaseList`、`limits.maxLists` 8。リストの id は「枠 + 1 + 8 × 世代」で、解放した id は枠を使い回しても `CLOSED` のまま。`unregister` はリストが参照している間 `BUSY`。検査は `apps/shikitest/shiki_api_test.js`（host の QuickJS、ASan/UBSan で 75/75。実機は未実施）。

リスト自体の利点（§4 の状態の置き場とは別に）: 項目の handle の検査が作る時の 1 回になる、参照中の handle の `unregister` を `BUSY` で守れる、handles の型付き配列がゲストのヒープから消える。

### 5.1 API

| 呼び出し | 動き | 誤り |
| --- | --- | --- |
| `shiki.list(handles)` | `handles`（`Uint32Array`、長さ 1〜`maxDrawsPerCall`）を Shiki のヒープへ写し、リストの handle（正の整数）を返す。各要素は登録済みの Shiki の handle | 型・長さ: `INVALID_ARGUMENT`。未登録の handle: `CLOSED`。予算・リストの数: `LIMIT_EXCEEDED` |
| `shiki.setList(list, handles, offset?)` | リストの `offset` 番目から `handles` を上書きする（長さは変えない） | 範囲外: `INVALID_ARGUMENT`。古いリスト: `CLOSED` |
| `shiki.drawList(list, order?, count?)` | `order` を省くとリストの全部を先頭から順に描く。`order`（`shiki.data()` の `u16`・`k_rec = 1` の窓）を渡すと、`order[0]`〜`order[count − 1]` の番号の項目をその順に描く（`count` の省略は `R`）。同じ番号を 2 回描いてよい。入力は無い（`stride = 0` の `drawMany` と同じ）。`DRAW_INDEX` は**リストの中の番号**（`order` の値。`order` の中の位置ではない） | `order` が `data()` の `u16`・`k_rec = 1` の窓でない: `INVALID_ARGUMENT`。`count` が 0〜`R` の外、`order` の値がリストの長さ以上: `INVALID_ARGUMENT`（draw を 1 つも行う前に検査する）。draw の誤りは `drawMany` と同じで、`index` は `order` の中の位置 |
| `shiki.releaseList(list)` | リストを解放する | 古いリスト: `CLOSED` |

- **寿命:** リストが参照している handle の `unregister` は `BUSY`（データ領域の `release` と同じ考え方。黙って古い handle を描かない）。リストはセッションの終わりに全部解放する（`app_stop()` の reset）。
- **予算:** リストは `4 × 長さ` バイトと管理領域を Shiki のヒープから取り、`heap().used` に入る。`limits` に `maxLists`（案: 8）を足す。長さの上限は `maxDrawsPerCall`（256）。
- **割り込み:** `drawList` は `drawMany` と同じく、draw と draw の間で割り込みを確かめない（v1.5 の決定と同じ。最悪 約 1 秒）。
- **入力:** リストは入力を持たない。式は状態をデータ領域から `DRAW_INDEX` と `LOAD` で読む（§4）。入力が要る描画は `drawMany` を使う。
- **順番の表の寿命:** `order` の窓は `drawList` の間だけ読む（結び付けない）。読む時は `drawMany` の `handles` と同じく、各 draw の始め（前の draw の `PUT`・`STORE` が映る）。
- DERBY の例: 走者の `legs`・`run` を 8 頭分（16 個、項目 `2l`・`2l + 1` がレーン `l`）並べたリストを 1 本持つ。JS は奥から手前の順に、描く項目の番号を順番の表へ書き、`drawList(list, order, n)` を 1 回。式は `DRAW_INDEX >> 1` でレーンを得て状態を `LOAD` する。並びそのものを変えたいときは `setList`。

### 5.2 途中に別の描画を挟む

DERBY は走者の間に大写しの馬（今は旧 API の点）を挟む。§2 で大写しの馬も Shiki（開始レコードで選ぶ `EMIT_LINES`）に移れば、全部を 1 本の `drawMany` に並べられる。挟む描画が Shiki の外（画像など）に残る場合は、`drawMany` を分ける（v1.5 と同じ）。

## 6. PIE の範囲を広げる（構想の 5）

### 6.1 方針（決めること D6）

v1.5 で PIE に下ろすのは「本体全体が `.H` の列の `EACH`」の自動の下ろしだけ（§8.4 of spec）。広げ方は 2 つある（[shiki-pie.md](shiki-pie.md) §3）。

- 案 **V（決定、2026-11-02、利用者）: ベクトルの命令を仕様に入れる。** 試作（[shiki-pie.md](shiki-pie.md) §8）を規範にする。実測で affine128 はスカラーの 4.4 倍（PIE、SoA）。書き手が 8 レーンを明示し、エンジンは命令をそのまま PIE に写す。自動の下ろしは v1.5 のまま残す（両方ある）。
- 案 A: 自動の下ろしを広げる（本体の一部の列、累算のある列）。書き手は変わらないが、下ろせるかの判定と、スカラーとのビット一致の証明が命令の組み合わせごとに要る。

### 6.2 案 V の命令（試作から）

| `op` | 命令 | 動き（要旨。詳細は試作 `ksn_shiki.c` と shiki-pie.md §8.1） | 実行時の失敗 |
| ---: | --- | --- | --- |
| 30 | `VEACH a, b, aux=f` | 領域 `a` を 8 レコードのブロックで回す。端数はマスク。本体の命令は 1 ブロックに 1 回・1 ステップ。開始レコードは §4.1 | 件数が **32,767 を超える**と `INVALID`（`VIOTA` が 16 ビットのため。`EACH` の 65,535 と違う。レビュー M4） |
| 31・32 | `VLOAD vd, a, b=欄`・`VSTORE vd, a, b=欄` | `i16` の領域の欄の 8 レコード（面の 128 ビット、16 B 整列） | |
| 33・34 | `VSPLAT vd, a`・`VIOTA vd` | スカラーを全レーンへ（飽和）・レーン番号（ブロックの先頭のレコード番号から） | |
| 35〜38 | `VADD`・`VSUB`・`VMIN`・`VMAX` | 16 ビット、飽和 | |
| 39〜43 | `VZACC`・`VMAC`・`VMACK`・`VACCSET`・`VOUT` | 40 ビットの蓄積器（飽和）。`VOUT` は floor のシフト（**0〜32**）と sat16 で結果を出し、**蓄積器にはシフト後・sat16 前の値を書き戻す**（試作の実機の照合、shiki-pie.md §8.2 の writeback。連続する `VOUT` の意味を決めるので規範に書く。レビュー M4） | |
| 44・45 | `EMIT_POINTS`・`EMIT_LINES` | 領域の列から native のループで点・線を描く。開始レコードは §4.1 | 座標・上限。**命令全体で 1 ステップ**なので `maxSteps` は仕事量を抑えず、`maxSegments`（1,024）・`maxRasterSteps`（8,192）が門になる（§9.1 of spec の注に書く。レビュー M4） |

- 登録の検査: `VEACH` の本体はベクトルの命令と `END` だけ、ベクトルの命令は `VEACH` の外に置けない、入れ子不可（試作のまま）。
- ベクトルのレジスタは `v0`〜`v7`、蓄積器 1 つ（試作の値）。`limits` に `vectorRegisters: 8` を出す（コードで強制する値。レビュー L3）。
- `@shiki` での書き方は凍結前に決める（決めること **D7**）。利用者は書き方を任せた（2026-11-02）。縛りは **JS の文法として正しいこと**だけ（本体はビルドで消え実機の QuickJS は読まないが、エディタ・整数の lint・`findPlans` がファイルを読む）。意味は変換器（`plan_js.mjs` のパーサは自前）が決める。**スカラーの分は実装した（2026-11-02、凍結前のレビューで確定）:**

| 書き方 | 命令 | 規則 |
| --- | --- | --- |
| `const [x, y] = r.at(i);`・`r.at(i, c)` | `LOAD`（欄 `c` から名前の数だけ） | 名前は領域の Q（`for...of` と同じ）。`i` は Q0 で計算する（Q0 の名前は `i` に使った名前も Q0 になる） |
| `r.set(i, [a, b]);`・`r.set(i, [a, b], c)` | `STORE` | 値は領域の Q。書く旗の無い領域は誤り。`c` は数の literal |
| `drawIndex()` | `DRAW_INDEX` | 式の中で Q0。`const l = drawIndex();` の `l` は Q0 |
| `for (const [x] of r.from(k))`・`r.from(k).take(n)` | `EACH` の開始レコード | `k` は Q0（小数を持ちうる式は誤り） |

使わない `r.at()` の名前があっても `LOAD` は消さない（番号の検査が `INVALID` になりうるため）。検査: `test_shiki_data_compile.mjs` の `lane`・`from`（コンパイルした式と AST の評価が、状態・線分・領域の中身で一致）。

**ベクトルの分も実装した（2026-11-02、凍結前の差分レビューに出す）。** ベクトルのループの本体は直線のコードで、文 1 つ・ベクトルの式の節 1 つが命令 1 つになる（`R7`）。ベクトルの値はレーンの生の int16 で、Q の規則は付かない（Q を変えるのは `acc.out(s)` のシフトで、書き手が明示する）。

| 書き方 | 命令 | 規則 |
| --- | --- | --- |
| `for (const [x, y] of r.blocks()) { ... }`・`r.from(k)`・`r.take(n)` を `.blocks()` の前に | `VEACH`（開始レコード・件数は `EACH` と同じ）＋名前ごとに `VLOAD`（欄 0, 1, ...） | `r` は `i16`（名前を取るとき）。`take(n)` の literal は 0〜32,767。名前を取らない `for (const b of r.blocks())` も可（`b` は使えない）。入れ子・本体の中のスカラーの文（描画・`if`・`let`・代入・別のループ）は誤り |
| `r.lane(c)` | `VLOAD`（欄 `c`、ループのブロックのレーン） | `r` は `i16`、`c` は数の literal。ループの領域と別の領域でもよい（登録の検査: その領域のレコード数がループの領域以上） |
| `r.setLane(c, v);` | `VSTORE` | 書く旗の無い領域は誤り。端数のブロックはマスクの外のレーンを書かない |
| `splat(s)` | `VSPLAT` | `s` はスカラーの式を **Q0** で（名前なら Q0 の名前になる。小数は切り捨て）、int16 に飽和 |
| `iota()` | `VIOTA` | レーンのレコード番号 |
| `a + b`・`a - b`・`vmin(a, b)`・`vmax(a, b)` | `VADD`・`VSUB`（16 ビットの飽和）・`VMIN`・`VMAX` | `*`・`/`・単項の `-` はベクトルでは誤り（掛け算は蓄積器で） |
| `acc.zero();`・`acc.set(s);` | `VZACC`・`VACCSET` | `s` はスカラー Q0。`acc` は描画の間ずっと 1 つで、ブロック・ループをまたいで残る |
| `acc.mac(a, b);`・`acc.mack(a, s);` | `VMAC`・`VMACK` | 40 ビットの飽和。`s` はスカラー Q0（int16 に飽和） |
| `acc.out(s)`（式） | `VOUT` | `s` は数の literal 0〜32。蓄積器にシフト後の値を書き戻す（式に現れた順に 1 回ずつ） |
| `points(r[.from(k)][.take(n)], c);`・`lines(...)` | `EMIT_POINTS`・`EMIT_LINES` | ベクトルのループの外。`r` は `i16`、欄 2 個（線は 4 個）以上、座標の Q は領域の Q（15 以下）。`c` は定数か登録の引数。`k`・`n` は Q0、両方あると連続の 2 レジスタ |

- 変換: スカラーの被演算子（`splat`・`acc.set`・`acc.mack`）は `VEACH` の前で計算する（本体にスカラーの命令を置けないため。再計算（remat）も `VEACH` の前に置く）。ベクトルの値は本体の中だけで生きる直線のコードなので、最後に読まれた後でレジスタを返す線形の割り当てで `v0`〜`v7` に載せる。読まれない純粋な値（`VLOAD`・`VSPLAT` など）は消し、`VOUT` は蓄積器を書くので残す。同時に 9 個以上生きると誤り。
- 検査: `tools/kasane_ir/test_shiki_vector_compile.mjs`。6 本の式（affine・演算の全部・蓄積器の持ち越し・2 つの領域と `lines`・`EMIT` の開始と件数・スカラーのループの中のベクトルのループ）を、**C のエンジン**（`ksn_shiki.c` を `shiki_run.c` で、ASan・UBSan 付き）で描き、AST の評価（`shiki_fix.mjs`）と状態・線分・全領域の中身で比べる（host の参照 `shiki_ref.mjs` はベクトルの命令を持たないため）。各 200 件で全一致、誤りの 15 件は期待した文言で拒む。DERBY・BIG WAVE・MEGADEMO の `@shiki` 関数と既存の検査の式（74 本）は、変更の前と同じバイト列になる。PIE の経路（実機）では未確認（C モデルと PIE の一致は試作の検査 shiki-pie.md §8.2 による）。
- ビット一致: C モデルと PIE の照合（実機）を §10.2 of spec に足す。自動の下ろし（`.H`）とベクトルの命令が同じ領域を読む式も、スカラー・C モデル・PIE の 3 つで一致することを要件にする（レビュー M5）。

## 7. 決めることの一覧

| | 決めること | 推奨 |
| --- | --- | --- |
| D2 | 実数の入力と引数を残すか | **決定: 残す**（2026-11-02、利用者） |
| D3 | `LOAD`・`STORE` の範囲外 | **決定: 読み書きとも `INVALID`**（同） |
| D4 | 窓に `S` のプロパティを足すか | **決定: 足さない**（同） |
| D5 | 並びを残す方法 | **決定: native のリスト**（同。順番の表を受ける・入力なし・失敗で止める。§5.1、凍結前にレビュー） |
| D6 | PIE の広げ方 | **決定: ベクトルの命令を仕様に**（同） |
| D7 | `@shiki` でのベクトルの命令・`LOAD`・`STORE`・`DRAW_INDEX` の書き方 | **実装済み**（2026-11-02。スカラーは `r.at`・`r.set`・`drawIndex()`・`r.from(k)`、ベクトルは `r...blocks()`・`r.lane`・`r.setLane`・`splat`・`iota`・`acc.*`・`points`・`lines`。§6.2 の表。凍結前の差分レビューで確定） |

（初稿の D1「版 1 のバイト列を拒むか」は、版を上げないことにしたので消した。§1）

## 8. 検証と進め方

1. この草案のレビューの反映（済、§10）。D2〜D6 と §5.1 のリストの規則は決定（§7）。D7 は実装と一緒に詰め、凍結前の差分レビューに出す。
2. 実装は §2.5 の順に、各段で host の照合（全画素・oracle）と実機（fps・ゲストと native のメモリ・評価の余裕）を移す前と比べる。旧 API の削除（§2.2）の前後で flash・`.bss`・空きヒープを `tools/memlog.py --port` で測る。
3. 実機の照合: `LOAD`・`STORE`・`DRAW_INDEX`・開始レコードの JS API の検査（`apps/shikitest/`）、ベクトルの命令の C モデルと PIE の一致（shiki-pie.md §8.2 の形）。
4. 凍結の前に、実装の差分と本文を SLOW が読む（差分レビュー）。
5. 凍結の時に [shiki-spec.md](shiki-spec.md) を v2.0 に書き換える（§11 の移行は完了として消し、§12 の未決を更新する）。

## 10. レビュー（SLOW、2026-11-02）の要点と対応

| | 指摘 | 対応 |
| --- | --- | --- |
| R1（重大） | `beginFrame` などは `procedural` の実装で、Shiki は写しているだけ。DERBY は `H.beginFrame` を使う | §2.2 を「消す・残す」に分け、§2.5 に手順 0 を足した。指摘のうち「Shiki に載っていない」は誤り（`share()` で載っている） |
| R2（重大） | `pocket.derby` の C が旧 API で 8 か所描いている | §2.1・§2.3・§2.5 の 1 |
| R3（重大） | DERBY は毎フレーム間引き・頭数・コマの選択があり、`handles` 不変・JS の仕事 0 は成り立たない。`EACH`・`EMIT` はレコード 0 からしか始まらない | 開始レコードの旗（§4.1）、§4.3 の主張を弱めた |
| M1 | 版を上げるのは §3 の方針と衝突 | 版 `1` のまま（§1）、D1 を消した |
| M2 | D2-a は引数と検証の行の書式も変える | §2.4 に費用を書いた |
| M3 | `DRAW_INDEX` の配管と失敗の後の `STORE` | §4.1 |
| M4 | `VEACH` の件数の上限、`VOUT` の書き戻し、`EMIT` のステップ | §6.2 の表 |
| M5 | §8.4 の条件と、`.H` と `V*` の一致の検証 | §3.3、§6.2 |
| M6 | 移行表の抜け（heapprobe、`derby_view.js`、`pocket_shape.c`、`gridFold`、CLAUDE.md の Node の記述） | §2.1・§2.2 |
| L1〜L5 | `aux` の当てはめ、`S` の式、`vectorRegisters`、`vraw` の大きさ、§3 と §4 の関係 | §3.1・§4.2・§6.2・§4.3 |

## 11. 差分レビュー（SLOW、2026-11-02、`885fb18..f585848`）と対応

C の重大なバグは見つからず、host の検査はすべて通った（レビュー側でも走らせて確認）。止めていたのは、規範文が自分の要件（§10.1、§10.2、`limits`）を満たしていない箇所と、実機の未実施。凍結（[shiki-spec.md](shiki-spec.md) を v2.0 に書き換える）のときは、以下を本文に入れる。

**規範に書く（凍結の条件）**

- **§10.1 の例外:** 参照実装（`tools/kasane_ir/shiki_ref.mjs`）は op 0〜29 まで。op 30〜45（ベクトル・`EMIT_*`）の規範は C モデル（`ksn_shiki.c` の `vec_exec` のスカラー側）とし、独立の照合は「C モデルと PIE の実機照合（shiki-pie.md §8.2 の形）」と「`@shiki` の AST 評価（`shiki_fix.mjs`）と C の一致（`test_shiki_vector_compile.mjs`）」で行う。後者は変換器と同じ AST を読むので、バイト列の意味を独立に決める参照ではない。
- **端数ブロックのレーン:** `VEACH` の最後のブロックの `live` 未満のレーンだけがレコードに当たる。マスクの外のレーンは、`VLOAD` が 0 を入れる。`VIOTA` は外でも `ブロックの先頭のレコード番号 + レーン`（飽和）。`VSPLAT`・`VADD`/`VSUB`/`VMIN`/`VMAX`・蓄積器の命令（`VZACC`/`VMAC`/`VMACK`/`VACCSET`/`VOUT`）は 8 レーン全部で動く。`VSTORE` だけが `live` のレーンに限る。蓄積器は draw の間残るので、端数ブロックで外のレーンに溜めた値は、後のループの満杯ブロックの `VOUT`・`VSTORE` に出る（決定的）。
- **自動の下ろし（§8.4 of spec）は未実装:** エンジンの PIE は `VEACH` のベクトル命令だけ。§8.4 は「下ろしてよい」の許可で、実装したときに §10.2（スカラー・C モデル・PIE の一致）を満たす。§6.2 の「自動の下ろしは v1.5 のまま残す（両方ある）」と、末尾の「`.H` と `V*` の 3 つで一致」は、この意味に読み替える。
- **`EMIT_POINTS`/`EMIT_LINES` の `imm`:** 0〜15 の Q として各座標を `floor((x + 2^(imm-1)) / 2^imm)` で画素にする（`imm` が 0 なら x のまま）。領域の Q と一致するかは検査しない。`@shiki` は領域の Q を置く。
- **`limits.vectorRegisters: 8`** を足した（`pocket_shiki_api.c`、`dst < 8` を登録時に強制している値）。
- §9.4 に `list`・`setList`・`drawList`・`releaseList` の行を入れ、`drawList` の誤りの `index` が「`order` の中の位置」であることと、`count` を省くと `R` であることを書く。§3.1 のステップの数え方（命令ごとに 1、`EMIT_*` は命令全体で 1）と、§4.1 の `EMIT` の旗は、§4.3 の命令表 1 本にまとめる。
- §9.2 の注記: (a) USB の `'J'` 診断（manifest 無しで MEGADEMO を走らせる）は Shiki heap を取る例外で、そのとき使う大きさは直前に選ばれたアプリの manifest の `shiki_bytes`（既定 48 KiB）。(b) Shiki heap の無いセッションでも `register`・`draw` はシステム heap で通るが、`data`・`list` は Shiki heap にしか取らないので予算の誤りで失敗する（`kasane.shiki.available` が false のとき）。

**直した（コード、2026-11-02）**

- `ksn_shiki.c` の `EMIT_*` の座標の丸めを、負数の `>>`（実装定義）から `floor_shift` に揃えた（GCC の結果は同じ。`shiki_diff` 20,000 件・ベクトルの検査は通る）。
- `register` の `options.data` を読む間に、getter が先に読んだ窓を `release()` できた。読み終えた後に、窓がまだ生きていて同じメモリかを確かめ、違えば `CLOSED`。
- BIG WAVE と MEGADEMO で、`data()` で窓を作った後に `register()` が投げると窓が解放されなかった。失敗したら `release` してから投げ直す（DERBY は既にそうしている）。
- `tools/kasane_contract/run.sh` に `test_shiki_data_compile.mjs`・`test_shiki_vector_compile.mjs`・`run_pocket_shiki_api_qjs.py` を足した（誰も走らせていなかった）。

**実機（凍結の条件）:** `apps/shikitest/shiki_api_test.js`（`LOAD`/`STORE`/`DRAW_INDEX`/リスト）の実機の実行と、現在の番号（op 30〜45）での C モデルと PIE の照合を 1 回ずつ。**→ 済み（2b4dbc9、§11.1）。**

### 11.1 実機の確認と、ベースラインからの変化（2026-11-02、2b4dbc9 / f63ca75）

記録は [logs/v2-review-device-20261102-2b4dbc9.txt](logs/v2-review-device-20261102-2b4dbc9.txt)。数値はすべて実機の実測で、`KASANE_PAINT` の `turn_ms`（30 フレームの平均）。1 回ずつの走行で、DERBY はデモのレースの種が走行ごとに違う（カメラの場面で比べる）。命令キャッシュの配置でビルド間に 15% 程度は動く（CLAUDE.md）ので、それ未満の差は差と読まない。

- **ベクトル命令を既定で PIE に（2b4dbc9）:** `shikiapi` は pass 76 / fail 0。`cyclebench` の C モデルと PIE の照合は 10 件すべて一致（tail・fields2・record3・sat40 の上下・writeback・accset・shift32・unaligned・affine128）。affine128 のベクトル版は C モデル 272.2 µs → PIE 189.1 µs（−31%。f505b0e の 248.8 → 169.4 µs と同じ比）。**出荷の 3 本はベクトル命令を使っていない**（`@shiki` にベクトルの構文が無い）ので、PIE を既定にしても 3 本の時間は変わらない。下の表の 2b4dbc9 と e7b6d28 の差は走行ごとの揺れ。
- **MEGADEMO が起動しなくなっていた（直した、f63ca75）:** `act2` のコンパイルが `OOM first_req=21760`（ゲストは 160 KiB のうち 75 KB）で止まった。2b4dbc9・a87299f・同じ日の朝に動いた e7b6d28 を焼き直しても同じなので、コードではなくシステム heap の配置が変わった（再起動直後でも再現）。起動ログでは、最大の空きブロックは自動時刻同期で Wi-Fi が上がる前は 122,880 B、後は 69,632 B で、そこから Shiki heap 48 KiB をチャンクの読み込みより前に取る。`core()`（121 行の 1 関数）は解析中に pass-1 のバイトコードを 1 ブロック約 26 KB で持つ（host の `tools/vmtest/compile_peak.py`、今回最大ブロックを出すようにした）。他のチャンクはどれも 13 KB 以下。破片（`shards`）と炸裂前の部分（`ship`）を別の関数に分け、最大ブロック 26,152 → 12,140 B、コンパイルのピーク −1.6 KB。host の走行は HEAD とバイト単位で同じ（16 枚とシート）。**同じ形の失敗は他のアプリでも起きうる:** 1 関数のバイトコードが大きいと、空きの総量ではなく連続した 1 ブロックで落ちる。
- **空き heap（読み込み後の `MEM free`）:** 浮動小数の VM の時代（4392695・bd1d829）は Shiki heap を取っていない。v2 の走行は 49,152 B を取ったうえでの値。

**DERBY WATCH**（読み込み後: 空き / ゲスト）

| 場面 | 4392695（浮動小数の VM） | 6841f73（全整数、v2 直前） | e7b6d28（旧 API 削除後） | 2b4dbc9（今回） |
| --- | --- | --- | --- | --- |
| 空き / ゲスト | 70,808 / 90,064 | 47,556 / 97,684 | 47,640 / 97,556 | 48,012 / 97,460 |
| パドック | 5.37〜5.40 | 4.82〜4.91 | 4.95〜6.12 | 4.85〜4.95 |
| FIELD | 11.1 | 8.89〜9.27 | 9.3 | 8.90〜9.11 |
| WIDE 2 | 11.8〜20.9 | 9.60〜11.29 | 13.6〜16.8 | 9.52〜10.85 |
| CLOSE | 10.3〜10.4 | 8.78〜9.01 | 9.0〜11.1 | 7.81〜11.04 |
| VISION | 12.1〜13.4 | 10.07〜11.02 | 10.0〜11.1 | 10.59〜10.85 |
| WIDE 3 | 13.4〜19.0 | 10.86〜11.28 | — | 10.64〜11.12 |
| HEAD ON / FINISH | — | 7.67〜9.02 / 8.93 | 7.5〜9.7 / — | 7.54 / 7.51〜8.76 |

浮動小数の VM から、最も重い場面（WIDE 2・WIDE 3）の最悪が 20.9 → 10.9 ms、19.0 → 11.1 ms。空きは 22.8 KB 減ったが、Shiki heap 49,152 B を含んでの差なので、それ以外の使用は約 26 KB 減った（差し引きの計算）。ゲストは +7.4 KB（整数化で増え、v2 で変わらない）。

**MEGADEMO**（HEAVY、読み込み後: 空き / ゲスト）

| 場面 | bd1d829（浮動小数の VM） | 427b308（Shiki 初回） | e7b6d28 | f63ca75（今回） |
| --- | --- | --- | --- | --- |
| 空き / ゲスト | 87,604 / 102,228 | 36,820 / 102,856 | 37,356 / 102,752 | 36,872 / 102,744 |
| WAKE | — | 15.32〜15.36 | 12.1〜15.7 | 15.54〜15.69 |
| NEWS | 最重 11.7 | 4.95〜10.54 | 3.8〜10.6 | 4.39〜10.77 |
| TWIST | — | 7.63〜13.21 | 8.3〜13.8 | 7.64〜13.91 |
| ZENITH | — | 6.13〜7.33 | 6.6〜8.5 | 6.15〜7.36 |
| CORE | 最重 15.0（28.9 fps） | 8.15〜13.39 | 7.7〜13.6 | 7.82〜12.99 |
| LOST | — | 3.80 | — | 3.65〜4.84 |

CORE の最悪は 15.0 → 約 13 ms、NEWS は 11.7 → 約 10.8 ms。空きの減り 50.7 KB のうち 49,152 B が Shiki heap で、それ以外の増加は約 1.6 KB。2,597 フレーム（2.5 周）で DEGRADE・OOM・例外なし。

**BIG WAVE**（attract、Shiki 以前の実機の記録は無い）

| | dedda13（Shiki 初回） | e7b6d28 | f63ca75（今回） |
| --- | --- | --- | --- |
| 空き / ゲスト | 73,808 / 71,792 | 74,108 / 71,680 | 73,800 / 71,808 |
| turn / render | 8.29〜9.67 / 4.38〜4.55 | 8.2〜10.8 / 4.4〜6.0 | 8.24〜9.91 / 4.36〜4.77 |

60 秒で 1,791 フレーム（29.9 fps）。

**まとめ:** レビュー対応と PIE の既定化の前後（e7b6d28 → 2b4dbc9/f63ca75）で、3 本の時間と空きは揺れの範囲で変わらない。v2 の効果は浮動小数の VM からの差（DERBY の最悪 −10 ms、MEGADEMO CORE の最悪 −2 ms）で、代わりに Shiki heap 48 KiB を常に取る。PIE の効果はベクトル命令を書いたプログラムにしか出ず、出荷の 3 本にはまだ無い（自動の下ろし §8.4 も未実装）。

**後に回す（凍結を止めない）**

- DERBY の投影誤差の門: float の VM の照合と一緒に消えた（`newton_screen < 0.1`）。`pocket.derby` の C の投影の誤差を自動で縛るものが今は無い（`tools/kasane_ir/q16_error.mjs` は `@plan` 時代の分析で、検査に繋がっていない）。native の draw の入力を double で計算し直して比べる門を DERBY の host 検査に戻す。
- LINE_PATTERN の決定的な幾何の検査（重み 1:4 の 8 弦）と CUBIC の誤差の記録（§4.6 の 0.597 px）は、float の VM の試験と一緒に消えた。残るのは `shiki_diff` の乱択（C と参照の独立した実装の一致）と `test_shiki_vec.c`。
- DERBY の host 検査の再生の比較は面 0 だけ（面 1 は数えるだけ）。
- MEGADEMO の翻訳の誤差（旧い行との差 1.5e-4 px、固定小数 0.053 px）は、作業用の一回の採取（427b308 の前の旧テンプレートを読んだもの）で、検査としては残していない。BIG WAVE は `tools/games/bigwave_shiki/compare.mjs` が残っている。
- MEGADEMO の `eg` の `s * rh` は、場面が約 4 分続くと 32,768 rad（Q16 の上限）を超える。今の場面の長さなら届かない（推定）。
