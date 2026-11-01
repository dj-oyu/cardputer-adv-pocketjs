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

## 3. 結果

（実機の計測待ち）
