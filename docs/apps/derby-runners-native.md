# DERBY WATCH: 走者の描画を `pocket.derby.runners` に下ろす（api-reorg P4b）

2026-11-13、ブランチ `p4b-derby`（`claude/eager-fermi-9ax7h8` の `2ef6480` から）。[api-reorg.md](../kasane/api-reorg.md) §5 の 7・§6 P4b（§7.3 で実施が決まった）と、P4 の DERBY の項（flush ごとの `subarray` をやめる）をまとめて行った。

**結論:** 走者（`legs`・`run`）と HEAD ON の正面の馬（`frl`・`frb`）の 1 フレーム分の仕事 — 投影・間引き・奥から手前の並べ替え・入力の組み立て・`drawMany` — を、`pocket.derby` の 1 関数 `runners()` に移した。**Shiki の状態の領域と `drawList`（shiki-v2.md §4・§5）は使わなかった**（§4 の比較）。r5sim の DERBY のレースの場面で 1 ターン −8.8〜−18.3%（WIDE −19.4k、VISION −26.1k カウント）、確保 78 → 66 回/ターン、ゲストの `heap_used` −3.4 KB（いずれも r5sim の実測）。host の全検査で画素は全フレーム一致（CSV のフレームのハッシュ、§6）。実機は未計測で、同じバイナリの中の A/B（§7）で親が測る。

## 1. 何に時間を使っていたか（r5sim、実測）

[api-reorg-perf.md](../kasane/logs/api-reorg-perf.md) の `derby` 負荷（デモのレース 1 本、`R5SIM_ICOUNT=shift=2,sleep=off`、同じチェックアウトで前後を比べる）。場面ごとの行に、ターンの内訳を足した（`tools/r5sim/main/r5sim.c`: `js` は `frame()` 全体（中の native 呼び出しを含む）、`shiki` はその中の Shiki の draw の native の全部（`pocket_shiki_turn` の `total_cy`）、`end` は `pocket_kasane_end_turn`、`draws` は Shiki の draw の数）。

走者の経路を落とした 2 つの変種で、経路の重さを測った（カウント/ターン、`2ef6480`）:

| 場面 | 元 | E2: `drawMany` を呼ばない（`subarray` は作る） | E1: `rin()` が何もしない | 走者の Shiki 以外（元 − E1 − Shiki の draw の差） |
| --- | ---: | ---: | ---: | ---: |
| wide | 220,734 | 205,073 | 193,870 | 約 18.2k（Shiki の draw 8.7k は別） |
| field | 123,404 | 107,848 | 96,465 | 約 18.3k |
| vision | 142,654 | 120,932 | 103,866 | 約 26.4k（中継の走者を含む） |
| finish | 113,312 | 97,806 | 87,067 | 約 17.6k |
| headon | 94,343 | 84,328 | —（HEAD ON は `rin()` を通らない） | `drawMany` の呼び出しだけで約 2.8k（元 − E2 − Shiki の差）。`bput` 16 回の JS は E1・E2 では消えない |

- 走者 1 頭は Shiki の draw 2 本で、16 本の Shiki の native は約 8.7k カウント（`shiki` の差）。**それ以外の約 18k（`rin`・`bput` の JS 約 11k、`drawMany` の呼び出しと `subarray` 約 7k）は、絵に関係のない JS と境目の仕事だった。** WIDE で 1 ターンの 8%、FIELD・FINISH で 15%、VISION で 19%。
- WIDE のほかの約 166k（Shiki の 36k を除く）は、首振りの列（`pocket.derby.ser`、C）・HUD の文字列・`at()`・監督で、P4b の範囲外（§8）。パドック（走者 1 頭）でも 1 ターン 45k ある。

## 2. 設計

```js
// derby_view.js。CO: 毛色 0..7、勝負服 8..15（enter('pad') で F から）。
S.runners(xs, lz, order, CO, hA, hB, mode, close, mask, from)  // -1 か、大写しの馬の order の中の位置
```

| mode | どこから | 間引き | 並べ替え | 1 頭の draw |
| --- | --- | --- | --- | --- |
| 0 横のカメラ（`course()`） | `side()` のカメラ（大型画面の中継なら `K`） | `mask` のレーンだけ（中継は `FN[tier]` 頭、JS がビットにする）。中継は顔の中に丸ごと、ほかは X が −160〜400 px | しない（`order` = `zo`、`at()` が並べた） | `legs`（X, S, Y, d, 毛色, 0, 0）、`run`（…, 勝負服, 勝負服 ^ 0x8410） |
| 1 首振り（`pan()`） | `aim()` のカメラ（`pose()`・`pj()`） | カメラの前で X が −160〜400 px | 奥行きの降順に**安定な挿入で並べ、`zo` に書き戻す**（JS の `zo.sort` と同じ結果。次のフレームの `at()` の sort がこの順から始まるので、書き戻さないと同点の順が変わる） | 同上 |
| 2 HEAD ON（`paint()`） | 決勝線の先の固定カメラ | しない | `order`（= `ro`）を後ろから | `frl`、`frb`（x, y, s, d, 毛色, 勝負服） |

- **入力は整数のまま**（`drawMany` の `Int32Array` と同じ。§2.4 の変換が無い）。`pocket_shiki_frame.c` に `pocket_proc_draw_fixed()` を足した（`pocket_proc_shiki_draw_inputs()` の薄い入口。`DRAW_INDEX` には呼び出しの中の draw の番号を渡す。今の 4 本の式は読まない）。
- **大写しの馬（CLOSE）は JS に残した。** `g0`〜`g5` の点列と `silk` の 2 本で、レーンの順の途中に入る。`runners()` は大写しのレーンに来たら描かずにその位置を返し、JS が 2 本を描いて次の位置から呼び直す（1 フレーム 2 回まで）。順序（奥の走者 → 大写し → 手前の走者）は前と同じ。
- **投影は 1 か所。** `lanes()` の中身を `lane()`、`head()` の中身を `head()`（C の static 関数）に出し、`lanes()`・`head()`（A/B の旧経路と host が使う）と `runners()` が同じ関数を通る。
- `plan` のハンドルが 0（未登録）なら描かない（旧 `rin()` の `!live.run || !live.legs`）。描画の失敗は `drawMany` と同じく例外で止まり、`FRAMEFAIL` になる。
- 状態を持たない（`.bss` 0 B、ゲストの値も持たない）。`app_stop()` の reset に足すものは無い。スタックは約 0.2 KB（`p[8][4]` など）。

## 3. JS に残るもの

```js
function runners(xs, pan, close, K) {
  if (AB) return abRun(xs, pan, close, K);
  let m = 255;
  if (K) for (let i = m = 0; i < FN[tier]; ++i) m |= 1 << ro[i];
  for (let i = 0; (i = S.runners(xs, lz, zo, CO, live.legs, live.run, pan, close, m, i)) >= 0; ++i) { /* 大写しの 2 本 */ }
}
```

`BH`・`BI`・`PO`・`HO`・`bput`・`bflush`・`rin` は本体から消え、A/B の `derby_ab.js` に移った。DERBY の本体は `drawMany` も `subarray` も使わなくなったので、**P4 の DERBY の項（`drawMany(BH, BI, bs, bn)` への書き換え）は不要になった。** P3 の `drawMany` の `count` を待つ必要も、そのための native の追加も無い。

## 4. 選ばなかった案

| 案 | 中身 | 採らなかった理由 |
| --- | --- | --- |
| A. Shiki の状態の領域と `drawList`（api-reorg §5 の 7 の原案、shiki-v2.md §4.3・§5.1） | 走者の `X`・`S`・`Y`・`d`・色を `i32` の領域に、`lanes()` がそこへ書く。式 `run`・`legs`・`frl`・`frb` を `drawIndex()` と `r.at()`（`DRAW_INDEX`・`LOAD`）で書き直す。JS は奥から手前の順番の表（`u16` の窓）と `count` を書いて `drawList` 1 回 | (1) **JS のレーンごとの仕事が残る**: 間引き（画面外・中継の `FN[tier]`）と順番の表の書き込みは JS（shiki-v2.md §4.3 の「残る」）。消えるのは `bput` と `drawMany` の組み立ての分で、§1 の 18k のうち 1/2〜2/3 と見る（推定、作っていない）。B は 1 回の native 呼び出しだけが残る。(2) **組み込みの式を 4 本書き直す**: 画素の一致を式の側で保つ必要があり、`LOAD` の分だけ draw ごとのステップが増える。(3) **Shiki の heap**: 領域（8 レーン × 7 欄 × 4 B = 224 B ＋管理）、リスト（16 × 4 B）、順番の表を、最も厳しい窓（HEAD ON・FINISH、最大の空き塊 2,492 B）の中で常に持つ。B は Shiki の heap を使わない |
| C. P4 だけ（`drawMany` の `count`） | `subarray` 2 個をやめる | P3 の API が要る。§1 の 18k のうち `subarray` の確保の分（数百〜千数百カウント、推定）しか消えない |
| D. 走者の draw だけを C に（投影・間引き・並べ替えは JS） | `drawRunner(l, X, S, Y, d)` のような 1 頭 1 呼び出し | 呼び出しが O(頭数) のまま（api-reorg §2 の G1）。境目の 1 回が約 1,100〜1,400 サイクルなので 8 頭で約 1.5k カウント残り、JS のループと `PO` の読み出しも残る |
| E. `at()` の `zo.sort` も native に | 毎フレームの比較関数の呼び出しが消える | 走者の描画の経路の外（`photo()` が `zo` を写す）。効果は測っていない。次の候補（§8） |

B を選んだのは、[derby-ser-native.md](derby-ser-native.md) と同じ理由: DERBY の走者の投影はもともと `pocket.derby`（C）にあり、JS は C の出力を読み直して C の入力に詰め直していただけだったから。境目を「1 フレーム 1〜2 回」にすると、JS に残るのは呼び出しと大写しの 2 本になる。

## 5. 結果（r5sim、実測）

`2ef6480` と同じチェックアウト、同じ r5sim の像の作り方（r5sim.c の内訳の行を足した後）で、前後とも 1 回ずつ（`sleep=off` では同じ像の 2 回が全部同じ数になる。api-reorg-perf.md §2）。

| 場面 | ターン | 前 | 後 | 差 | 確保/ターン 前 → 後 |
| --- | ---: | ---: | ---: | ---: | --- |
| wait（パドック） | 61 | 45,314 | 45,283 | −0.1% | 82 → 78 |
| pad | 72 | 43,875 | 43,227 | −1.5% | 79 → 75 |
| gate | 41 | 93,370 | 77,714 | −16.8% | 88 → 78 |
| wide | 759 | 220,734 | 201,364 | **−8.8%** | 83 → 70 |
| field | 145 | 123,404 | 107,169 | **−13.2%** | 75 → 65 |
| close | 98 | 104,117 | 92,718 | −10.9% | 85 → 71 |
| vision | 146 | 142,654 | 116,535 | **−18.3%** | 98 → 77 |
| headon | 53 | 94,343 | 80,103 | −15.1% | 72 → 66 |
| finish | 103 | 113,312 | 96,649 | **−14.7%** | 73 → 63 |
| photo | 111 | 67,830 | 55,458 | −18.2% | 64 → 54 |
| res | 152 | 51,875 | 51,272 | −1.2% | 40 → 36 |
| 全体（61〜1773） | 1,713 | 150,175 | 134,636 | −10.3% | 78 → 66 |

- Shiki の draw の数と `shiki` のカウントは全場面で同じ（例: wide 39 本・36,336 → 39 本・36,344）。減ったのは JS と境目だけ。
- `heap_used`（1773 ターン後のゲスト）: 104,220 → 100,780 B（−3,440 B）。
- **A/B の旧経路（`AB = 1`、`derby_ab.js` を入口で読む像）**: wide 220,760、field 123,861、vision 143,553、headon 94,354（前とほぼ同じ。+0〜0.6% は `runners()` → `abRun()` の 1 段と `AB` の判定）。同じバイナリで旧経路が前と同じ費用で走るので、実機の A/B の差は経路の差と読める。
- r5sim は Shiki の engine を C のモデルで走らせ、flash の待ちが無い（api-reorg.md §6.1）。実機の `turn_ms` への換算はしない。

## 6. 正しさとメモリ（host）

- **画素:** `python3 tools/games/run_derby.py`（64 bit、ASan/UBSan）の全部の実行で `DERBY_HOST PASS` が前後とも 19 回、14 本の実行のフレームごとの CSV（`frames_*.csv`。各フレームの画素のハッシュ `fh`、draw・線分・ステップの数）が**前後でバイト単位で同じ**（MID 14,519 フレーム、LIGHT/HEAVY・director だけ・首振りだけ・画面の移動・ser の oracle を含む。楕円のデモのレースも入る）。着順・時計・オッズの検査も同じ。
- **A/B の旧経路も同じ画素:** 新しいバイナリで `DERBY_JS="pocket.app.load('ab');AB=1"` の MID の CSV が、前の CSV とバイト単位で同じ。
- host の検査は `pocket.derby` の新しい draw を `pocket_proc_draw_fixed_hook`（`tools/games/test_derby_host.c`）で受けて、`drawMany` の `Int32Array` と同じく再生する。
- **ゲストヒープ（`run_derby.py --m32 --heap-limit 163840`、実機と同じ 8 B の JSValue と 4 B のポインタ、TLSF の数え方、MID の 1 本）:**

| | 前（`2ef6480`） | 後 | 差 | 後、旧経路を読み込んだ（`ab`） |
| --- | ---: | ---: | ---: | ---: |
| 評価の前 | 45,104 | 45,320 | +216 | 45,320 |
| 評価中のピーク | 129,000 | 125,964 | **−3,036** | 125,964 |
| 評価の後 | 111,820 | 108,972 | −2,848 | 108,972 |
| GC 後の生きている量の最大 | 143,292 | 138,072 | **−5,220** | 145,428 |
| 実行全体のピーク | 130,232 | 125,964 | −4,268 | 132,096 |

  評価の前の +216 B はチャンクの表の 1 行（`ab`）など。評価のピークが下がるので、実機の評価の余裕（基準 約 28.9 KB）は広がる側、ゲスト最大（基準 134.1〜134.8 KB）は下がる側と見る（推定。実機で確かめる）。**A/B で旧経路を読み込むと、GC 後の最大は前より +2.1 KB（host m32）になる**。実機のゲスト最大に足すと約 137 KB（推定）で、140,000 B の線の内側だが余裕は減る。

- **Shiki の heap:** 全実行で「used max 31,140、largest free min 17,556（of 49,152）」が前後で同じ（64 bit host。m32 は 30,516 / 18,204 で同じ）。`runners()` は Shiki の heap から何も取らない（案 A の領域・リストが要らない）ので、実機の HEAD ON・FINISH の窓の最大の空き塊（基準 2,492 B）も変わらない見込み（推定。`SHIKI_TURN` の `shk=` で確かめる）。
- **ファーム（`idf.py size`、別のビルドディレクトリと `-DSDKCONFIG=`）:** Flash Code 1,694,808 → 1,696,196（+1,388 B、`runners()`・`lane()`・`head()`・`pocket_proc_draw_fixed()`）、`.rodata` 783,444 → 785,092（+1,648 B、主に埋め込みの `derby_ab.js` 2,014 B の分と、JS の短くなった分の差）、DIRAM 181,316 → 181,316（`.bss`・`.data`・`.iram0.text` とも同じ）。IRAM には何も置いていない。

## 7. 実機の A/B（同じバイナリ）

**2026-11-13 に退役した**（利用者の決定。[derby-ser-division.md](derby-ser-division.md) §10）: `derby_ab.js` とチャンク `ab` を消し、up は `ser()` の整数／倍精度の切り替え（`DERBY AB INT`／`DERBY AB DOUBLE`）になった。以下はこの A/B があった間の記録。

- **切り替え:** broker の `action: up`（`frame(b)` の `b & 16`）。最初の 1 回で `derby_ab.js`（旧経路、チャンク `ab`）を読み込み、以後押すたびに `AB` を反転する。ログに `DERBY AB JS`（旧）／`DERBY AB NATIVE`（新）が出る。既定は新（NATIVE）。
- この up は**デモを止めるキーに数えない**（`attract()` に渡す前に落とす）。だから「DERBY を開く → 15 秒待ってデモのレースを始めさせる → レースの途中で up を押す」で、同じレースの中で新旧を切り替えられる。読み込みに失敗したら `DERBY ABFAIL ...` を出して何もしない（アプリは止まらない）。
- 物理キーの `;` も up（`keymap.c` の nav）なので同じく切り替わるが、`K.down()` に出るのでデモは止まる。
- 旧経路のチャンクは読み込んだ後ずっと残る（ゲスト約 1〜2 KB、推定）。評価の余裕とゲスト最大の比較は、up を押していない状態で行う。

## 8. 残り（この段の外）

- WIDE の 1 ターンの残り（r5sim で約 165k、Shiki を除く）の内訳は分けていない。候補は HUD の文字列（毎ターンの連結と比較）、`at()` の `zo.sort` と `mx.apply`、`attract()` の `K.down().join()` と 9 回の `K.pressed`。パドックでも 45k ある。
- 実機の `turn_ms`・評価余裕・ゲスト最大・`SHIKI_TURN` の `shk=` は、親が A/B で測る（§7）。
- 実機の重い CAM WIDE 2（draws 約 50・`pat=` 約 40 で 11.5 ms）の原因は JS ではなく `ser()`（C）の倍精度の計算だった（r5sim の内訳、[derby-ser-cost.md](derby-ser-cost.md)）。
