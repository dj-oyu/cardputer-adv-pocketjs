# MEGADEMO WAKE のジオメトリを C へ（`pocket.shapes`）

2026-10-28。WAKE（目覚め、逆光のタマ）を 3D の点＋透視にした版（dd3bfba）は、実機で 1 フレームの JS のターンが 127〜134 ms（約 7.4 fps）だった。描画 4.3 ms・転送 7.3 ms（どちらも実測、broker の `app_session`、HEAVY）。アニメーションの量は減らさない（ユーザーの指示）ので、同じ絵を安く計算する。設計の比較は Fable の独立レビュー（候補 1〜4）にもとづく。

## どこに時間がかかっていたか（host の実測と、実機への換算）

host（quickjs-ng、`wake()` を 74 フレーム × 3 回）で 1.38〜1.45 ms/フレーム。CORE の 1 フレームが host 0.16 ms・実機 13〜25 ms なので、host → 実機は 85〜150 倍（推定の係数）。GC は無関係（上限と閾値を実機と同じにしても、GC を止めても同じ）。callgrind では 77% がバイトコードの解釈。

| 区間（`megademo_wake.js` dd3bfba） | host ms | 割合 |
| --- | ---: | ---: |
| 光の塗り（`wstrips`/`wminus`/`wspan`、1 フレームで辺を 5,078 回なめる） | 0.679 | 49% |
| 目（`tf`・`wsurf`、虹彩 − 瞳孔の塗り） | 0.296 | 21% |
| リムと耳の辺（`winside`） | 0.191 | 14% |
| 内耳・耳・ポーズ・ひげ | 残り | 16% |

JS のまま書き直す（行ごとの交点の代わりに辺の表）試作は、かえって遅かった（1.45 → 1.69 ms）。DERBY WATCH の `ser()` と同じく、解釈系の倍率が問題で、C へ移すしかない（[derby-ser-native.md](derby-ser-native.md)）。

## 何を C にしたか

芝居（POSE の表、耳・目・ひげ・頭の輪郭の点の作り方）は JS に残し、その先の重い 3 つだけを C にした（`main/pocket/pocket_shape.c`、`pocket.shapes`）。

- `project(model, points, from, count, pose)`: モデルの点（x 右、y 下、z 手前）を、yaw・pitch・透視・roll で画面の点へ。`pose` = [cos yaw, sin yaw, cos pitch, sin pitch, cos roll, sin roll, カメラの距離, 倍率, x0, y0]。
- `fill(points, plan, step, y0, y1, base, holes)`: 凸多角形（`[start, count]`）または楕円（`[cx, cy, rx, ry]`）から、穴の多角形（最大 6）を抜いた行の区間を、4 行ずつの台形（plan `tz` / `tz2`）で描く。`y0`・`y1` が `null` なら多角形の上下 ±.01。
- `outline(points, plan, start, count, closed, parts, hidden)`: 辺を `parts` 等分し、中点が隠す多角形の中にある断片を飛ばして、線（plan `ln`）で描く。画面の外の片側に収まる断片は描かない。

点は JS の `Float64Array` 2 本（モデル 146 点 × 3、画面 146 点 × 2）に置き、関数には区間の番号だけを渡す（フレームごとの配列を作らない）。描くのは `pocket_proc_draw_numbers()`（`H.draw` の後半）で、plan・VM・フレームの上限は今までどおり。

- **アプリだけの面**: `pocket.derby` と同じく、manifest の optional に `kasane.shapes` を持つセッション（MEGADEMO）にだけ入る（`app_session.c`、`app_registry.c`）。診断 `J` も入れる。capability `kasane.shapes`、limits は `maxPoints` 512・`maxHoles` 6。
- **float で計算する**: S3 は単精度の FPU を持ち、倍精度はソフトウェア。1 フレームで辺を約 5,000 回なめるので、倍精度だと 10 ms 前後（推定）かかる。ビルドは `-ffp-contract=off`（host と実機で積和を融合させない）。

## 照合（host）

`tools/kasane_contract/run_shape_oracle.py`: 置き換える前の JS（`--baseline-rev`、既定 c52af92）と今の木で WAKE の全 104 フレームを走らせ、draw ごとに plan と入力を比べる。

- **倍精度でビルドした C**（`-DPOCKET_SHAPE_DOUBLE`）: 8,100 draw すべてがビット単位で一致（PASS）。C の移植が JS の演算順まで同じことの証明。
- **単精度（実機と同じ）**: 104 フレーム中 103 で draw の数が同じ、入力の差の最大 0.00028 px。`run_megademo_app_host.py` の WAKE の 13 フレームは、JS の版と全画素一致。
- 他の試験は `pocket.shapes` を同じ C で載せる: `run_proc_megademo_scenes.py`（WAKE は 310 セグメント・ラスタ 7,085・145 draw で変わらず）、`run_proc_megademo_js.py`、`run_pocket_proc_qjs.py`、`run_megademo_app_host.py`。本物の `pocket_proc.c` を持たない試験は `shape_draw_shim.c` が draw を JS へ戻す。

## メモリ（host m32、`MEGA_HEAP_LIMIT=163840`）

| | 評価のピーク | 評価後 | 実行中の最大 |
| --- | ---: | ---: | ---: |
| JS で描いていた版（c52af92） | 134,352 | 129,816 | 145,732 |
| `pocket.shapes` | 134,188 | 130,048 | **143,764** |

型付き配列（約 3.5 KB）と消えた JS の関数がほぼ相殺し、フレームごとの配列が無くなった分、実行中の最大が 2 KB 下がった。ネイティブ側の `.bss` は 0（状態は呼び出しの間だけ）。

## 実機（2026-10-29、841e23e、broker の `app_session` 60 秒、HEAVY）

| WAKE の 30 フレーム窓 | JS のターン | 描画 | 転送 |
| --- | ---: | ---: | ---: |
| JS で描いていた版（dd3bfba） | 127.6 / 130.6 / 133.6 / 129.8 ms | 4.3〜4.7 ms | 7.3 ms |
| `pocket.shapes`（1 周目） | **21.6 / 21.5 ms** | 4.3〜4.7 ms | 7.3 ms |
| `pocket.shapes`（2 周目） | **17.1 / 22.9 ms** | 4.1〜4.7 ms | 7.3 ms |

- 30 フレームが 0.9〜1.04 秒（約 29〜30 fps。前は約 4 秒で 7.4 fps）。JS のターンは約 6 分の 1。
- 残りの約 20 ms は、C の幾何と、約 110 回の `draw`（VM の実行と、JS から呼ぶ固定費）。描画と転送は変わらない（画面全体を毎フレーム送る。8 行の帯ごとに変わった所だけ送る改善は別の項目）。
- チャンクの読み込み: wake 9,563 B、解析 34 ms（前の 12,135 B・53 ms）。評価後の空き 75,444 B（最大の連続 31,744 B、JS 115,816 B。dd3bfba は 77,028 B・31,744 B・114,616 B）。
- 約 1.5 周（WAKE を 2 回）、`DEGRADE`・エラー・OOM なし。他の場面の値は前と同じ（NEWS 7〜8 ms、TWIST 9 ms、ZENITH 7〜14 ms、CORE 11〜25 ms）。
