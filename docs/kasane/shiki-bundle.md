# Shiki の draw の束ね — 固定費の内訳と、複数の handle を 1 回で描く API（2026-11-01、作業中）

[shiki-pie.md](shiki-pie.md) §1・§8 の続き。PIE の試作の後に行う、と決めた作業（ユーザー決定）。**数値は実測か推定かを必ず区別する。**

## 1. 動機と、今わかっていること

- 束ねの動機は「draw 1 回の固定費 約 100〜115 µs」（[derby-background-cost.md](derby-background-cost.md) §3）。これは**旧 API**（`pocket.kasane.procedural`、float の plan）で、算術 769 ステップの draw の時間をステップ数に回帰した**切片（推定）**で、直接は測っていない。
- 一方、Shiki のエンジンの中の空の draw（`SET` 1 つ）は**実測 264 サイクル＝1.1 µs**（[shiki-pie.md](shiki-pie.md) §8.3）。したがって、Shiki の draw の固定費の大半は、あるとすればエンジンの外（JS からネイティブへの呼び出し、引数の検査と入力の読み取り、handle とフレームの準備、フレームへの複写）にある。
- **束ねの設計の前に、Shiki の draw 1 回の費用を段ごとに実測する**（§2）。固定費が小さければ、束ねは要らないか、別の形になる。

## 2. 測り方

- アプリ `local.drawcost`（[apps/shikitest/draw_cost.js](../../apps/shikitest/draw_cost.js)、[README](../../apps/shikitest/README.md)）。JS のループ 200 回を `shikiDebug.cycles()` で包み、5 回の最小。
- 同時に `shikiDebug.drawProfile()` がネイティブの段を積む（`main/pocket/pocket_proc.h` の `pocket_shiki_draw_prof`）: `parse`（`js_draw` の引数の検査と入力の読み取り）、`lookup`（handle・フレーム・作業領域の準備）、`engine`（`ksn_shiki_draw`）、`copy`（線分をフレームへ）。`total` からこれらを引いた残りが、JS の呼び出しの費用（QuickJS の関数呼び出し、引数の受け渡し）。
- 対照: 空の JS 関数（`loop`）、ネイティブの空呼び出し（`native`、`cycles()` 自身）。

## 3. 結果（実機、`94aea92`、2026-11-01）

ログ: [logs/shiki-drawcost-20261101-94aea92.txt](logs/shiki-drawcost-20261101-94aea92.txt)。1 回あたりのサイクル（240 MHz、200 回 × 5 回の最小）。「JS 側」は `total` からネイティブの 4 段を引いた残り（計算）。

| 変種 | total | µs | parse | lookup | engine | copy | JS 側 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `loop`（空の JS 関数） | 1,120 | 4.7 | | | | | 1,120 |
| `native`（空のネイティブ呼び出し） | 1,162 | 4.8 | | | | | 1,162 |
| `draw0`（入力 0） | 2,325 | 9.7 | 494 | 107 | 270 | 77 | 1,377 |
| `draw8`（入力 8、引数で） | 7,491 | 31.2 | 1,937 | 123 | 3,378 | 77 | 1,976 |
| `draw8arr`（入力 8、配列で） | 8,789 | 36.6 | 3,661 | 123 | 3,372 | 77 | 1,556 |
| `point`（入力 2、点 1 つ） | 3,907 | 16.3 | 850 | 138 | 1,294 | 103 | 1,521 |

読み方:
- **Shiki の draw 1 回の固定費は 約 10 µs（入力 0）〜31 µs（入力 8）**（実測）。旧 API の推定 100〜115 µs の 1/3〜1/10。束ねの動機の数字は、Shiki には当てはまらない。
- **最大の項目は入力の変換**: `engine` は入力 0 で 270、入力 8 で 3,378 サイクル。差の 3,108 を 8 で割ると、入力 1 つ 約 390 サイクル（計算）。`ksn_shiki_from_double` が double の `ldexp`・`floor` を使い、S3 には double の演算器が無いので、ソフトウェアの double 演算になっているため（推定。逆アセンブルで確かめる）。
- 次は `parse`（入力 1 つ 約 180 サイクル、配列なら 約 400）。`JS_DupValue`・`JS_ToFloat64`・`JS_FreeValue` を入力ごとに通るため。
- JS からネイティブを 1 回呼ぶ費用は、JS の関数を 1 回呼ぶのとほぼ同じ（約 1,100〜1,200 サイクル、ループの分を含む）。**束ねで消えるのはこの部分と `lookup`（約 120）で、1 draw あたり 約 1,300〜2,000 サイクル（5〜8 µs）**（計算）。
