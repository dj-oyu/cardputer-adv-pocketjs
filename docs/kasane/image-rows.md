# Shiki の画像を帯へ直接書く（read_rows）と横線の塗り（2026-11-04）

不透明・等倍の画像を、32 画素ずつの読み出しではなく、帯の行をまとめて Kasane の帯へ直接書く経路。STRESS TEST の描画の時間の内訳を追う中で入れた（[apps/stress/README.md](../../apps/stress/README.md)）。数値は断りが無ければ実機の実測。

## 何を変えたか

それまで、Shiki の画像（`pocket_shiki_frame.c` の `image_span`）は次の順で帯へ届いていた。

1. 8 行の帯のバッファを背景色で塗り、`ksn_proc_render_band` でラスタする（`image_band_buf`）。
2. Kasane が 32 画素ごとに `image_read` → `image_span` を呼び、帯のバッファから scratch へ写し、alpha を 255 で埋める。
3. scratch から Kasane の帯へもう一度写す（`ksn_render.c` の `KSN_IMAGE`）。

全画面の画像では 1 フレームに約 1,000 回の呼び出しと、画素の 2 回の写しが要る。

- **`ksn_image_port.read_rows`（`ksn_ports.h`）:** 不透明の口だけが持てる任意の関数。元画像の行 `[y, y+rows)`・列 `[x, x+count)` を、`dst` へ行の幅 `stride` で直接書く。結果は `read_span` と同じ画素。`KSN_UNSUPPORTED` を返すと、Kasane は今までの 32 画素ずつの経路に戻る。
- **core の別表（`ksn_core.c`）:** `read_rows` は `read_span` の関数をキーにした 2 枠の表に置く。core の画像の表は予算（`KSN_CORE_STORAGE_BYTES`）に入っていて、64 ビットのホストでは余裕が 48 B しかないため、表そのものは大きくしない。
- **`ksn_render.c`:** 等倍（`image_stretch_identity`）・不透明・不透明度 255・回転なし・stretch map なしの画像は、帯の部分を `read_rows` に一度で頼む（`g_ksn_image_rows`、既定 1）。
- **`pocket_shiki_frame.c` の `image_rows`:** 全幅の行だけを受ける（それ以外は `UNSUPPORTED`）。`pocket_proc_backdrop` と同じく、背景色で塗って（2 画素ずつの書き込み、`may_alias`）、その場で `ksn_proc_render_band` を走らせる。帯のバッファ・32 画素の写し・alpha の埋めが無くなる。
- **覆われた帯の背景を塗らない（`g_ksn_cover_skip_fill`、既定 1）:** 帯の全体（行と、変化した列）を不透明の APP の画像が覆うときは、背景色で先に塗らない。

## 正しさ

- host: STRESS の 900 フレームの画面のダイジェストが、スイッチ 4 通りすべてで `de4a66c5`（`KSN_IMAGE_ROWS` / `KSN_COVER_SKIP_FILL` の環境変数、`tools/test_stress_app.c`）。新しい経路が実際に使われていることは、帯のラスタの書き込み先がヒープの帯のバッファではなく Kasane の帯になることで確かめた（`--wrap=ksn_proc_render_band`）。
- host: Shiki の画像の口の検査（`run_pocket_proc_qjs.py`、24 フレームの RGB565 の完全一致・面 2 枚・修復・古い資源）、`tools/kasane_contract/run.sh`（`GAMES_M32=0`）、BIG WAVE・DERBY・MEGADEMO・LCD CATCH のホストの検査がすべて PASS。

## 実機の A/B（fdab2f5、2026-11-04）

同じバイナリで 30 フレームの窓ごとに、「両方有効 → 行の経路だけ無効 → 塗りの省略だけ無効」を回した（`POCKET_PROC_ROWS_AB`、計測用のビルドだけ。`35ac5fe`→`5ff20b1` で戻した）。STRESS の LV1、入力なし、約 50 秒、最初のコンパイルの窓を除く。ログは [logs/stress-image-rows-ab-20261104-fdab2f5.txt](logs/stress-image-rows-ab-20261104-fdab2f5.txt)。

| 条件 | 窓 | `render_ms` 中央値（範囲） | `send_ms` |
| --- | ---: | --- | --- |
| 従来（`image_rows=0`） | 16 | 11.80（11.34〜12.19） | 約 0.90 |
| 行の経路 + 塗りの省略 | 14 | 9.34（9.19〜9.61） | 約 1.53 |
| 行の経路のみ（`cover_skip_fill=0`） | 15 | 9.41（9.04〜9.79） | 約 1.55 |

- **行の経路で描画は 2.46 ms 短くなった（−21%）。** 送出の待ちが約 0.63 ms 増えたので、描画と送出の合計では約 12.7 → 10.9 ms（−1.8 ms）。
- **送出の待ちが増えた理由（推定）:** 帯の描画が速くなり、前の帯の DMA の完了を待つ時間が表に出た。転送用のバッファは 1 本（`tx_buf`、issue #118）なので、前の帯の送信が終わるまで次の帯を写せない。SPI は 80 MHz（`board.c:397`）で、全画面 64,800 B の転送は理論で約 6.5 ms（推定）。全画面を毎フレーム送る限り、描画と送出の合計はこの値より下がらない。
- **背景の塗りの省略は、差が見えなかった**（中央値 9.34 と 9.41 ms、窓の揺れと区別できない）。`fill565` は PIE の塗りで、もともと速い。害も無く、画素も同じなので有効のまま残した。

## 横線を塗る（1a、e07efdf）

帯のラスタ（`ksn_shiki_frame.c`）で、横線（`y0 == y1`）を 1 画素ずつ歩かず、x の範囲を画面に一度だけ切り詰めて 32 ビットの書き込みで 2 画素ずつ塗る（`g_ksn_proc_hspan`、既定 1）。模様の線を含むフレームの経路（`plain_band`）にも入れた。横の Bresenham の線は、その行の両端の間をちょうど塗るので、画素は同じ。STRESS では帯のラスタの歩みの 76%（1 フレーム 22,270 歩のうち 17,031 画素、host の数え）が横線だった。

- 正しさ（host）: 無作為のフレーム（横線が半分、端は −480〜720、点、帯の始まりと高さを全部、両方のループ、帯の前後に見張りの画素）で新旧が一致（`tools/kasane_contract/test_procedural_hspan.c`、107,421 帯、64 ビット ASan と m32 の O2。わざと 1 画素ずらした版は捕まえる）。STRESS の画面のダイジェストも新旧で同じ。`run.sh` 一式も PASS。
- 実機（8efd336 = e07efdf + `POCKET_PROC_HSPAN_AB`、STRESS LV1、窓ごとに交互）。ログは [logs/stress-hspan-ab-20261104-8efd336.txt](logs/stress-hspan-ab-20261104-8efd336.txt)。

| 条件 | 窓 | `render_ms` 中央値（範囲） | `send_ms` 中央値 | 合計 |
| --- | ---: | --- | ---: | ---: |
| 横線を塗る（`hspan=1`） | 23 | 7.38（7.08〜7.60） | 2.90 | 10.34 |
| 1 画素ずつ（`hspan=0`） | 23 | 9.58（9.30〜9.95） | 1.43 | 11.00 |

- **描画は 2.20 ms 短くなった（−23%）。** 見込み（既存の単価で 1.0〜1.5 ms）より大きい。
- **ただし送出の待ちが 1.47 ms 増え、1 フレームの合計は −0.66 ms にとどまった。** 描画が LCD の DMA より速くなり、CPU が前の帯の送信の完了を待つ時間に置き換わった（推定）。転送用のバッファは 1 本（`tx_buf`、issue #118）。描画と送出の合計 10.3 ms は、全画面の転送の理論値約 6.5 ms（80 MHz の SPI、推定）よりまだ大きいので、帯ごとに CPU と DMA が交互に待つ形になっている可能性がある（推定、未測定）。
- **ここから先、帯のラスタをさらに速くしても、1 フレームの短縮はわずか**（送出の待ちに置き換わる）。次に効くのは転送の重ね方（転送用のバッファを 2 本にして、描画と送信を本当に並べる、issue #118）か、送る量を減らすこと。
