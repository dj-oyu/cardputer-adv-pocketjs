# `pocket.shapes` — 平面のベクター描画

手続き面（`pocket.kasane.procedural`）の、組み立て中のフレームへ、塗りと線を C で描く。点は呼び出し側の型付き配列に置き、番号で指す（フレームごとに配列を作らない）。設計の経緯は [megademo-shapes.md](../apps/megademo-shapes.md)、実装は `main/pocket/pocket_shape.c`、契約の試験は `tools/kasane_contract/run_pocket_shape.py`。

## 使えるアプリ

manifest の `required` か `optional` に `kasane.shapes` があるセッションにだけ入る（共通 API ではない。`pocket.derby` と同じ）。`pocket.capabilities.get('kasane.shapes')` の limits:

| 名前 | 値 | 意味 |
| --- | ---: | --- |
| `maxPoints` | 1024 | 1 つの配列で指せる点の数 |
| `maxPolygonPoints` | 256 | 1 つの多角形の頂点の数 |
| `maxHoles` | 6 | 1 回の塗りで抜く穴、または輪郭を隠す多角形の数 |

## 点と多角形

- **点の配列**は `Float32Array`（書き込み可）だけ。計算は単精度なので、`Float64Array` はメモリが倍になるだけで精度は上がらない（渡すと `INVALID_ARGUMENT`）。2 次元の点は `x, y` の 2 つ、3 次元の点は `x, y, z` の 3 つで 1 点。点 `i` は配列の `2i`（3 次元は `3i`）から。
- **多角形**は `[start, count]`: 点 `start` から `count` 個（3..256）。凸多角形として扱う（回りの向きはどちらでもよい）。凸でないと、各行の左端から右端までが塗られる。
- **複数の多角形**は `[start, count, start, count, ...]` と平たく並べる。
- 頂点に有限でない値（`NaN`、`Infinity`）を含む多角形は**空**として扱う（塗る形なら何も描かず、穴・隠す形なら無視する）。`project()` がカメラの後ろの点に `NaN` を書くので、そのまま渡してよい。
- 座標は画面のピクセル（x 右 0..239、y 下 0..134）。行 `y` はその整数の高さで形を切った区間。

## `project(matrix, source, target[, from[, count]])`

3 次元の点を画面へ写す。`matrix` は 3×4 の射影行列（行優先の 12 個の数。配列か `Float32Array`）`[ax, ay, az, at, bx, by, bz, bt, wx, wy, wz, wt]` で、

```
w = wx·x + wy·y + wz·z + wt
X = (ax·x + ay·y + az·z + at) / w
Y = (bx·x + by·y + bz·z + bt) / w
```

- `source` は 3 次元の点の配列、`target` は 2 次元の点の配列。`source` の点 `i` が `target` の点 `i` になる（同じ番号）。
- `from`（既定 0）から `count` 点（既定 `source` の残り全部）。
- `w <= 0`（カメラの面か後ろ）の点は `NaN, NaN` を書く。
- 回転・移動・拡大・透視の合成は呼び出し側が行列にする。例（MEGADEMO の WAKE）: yaw・pitch で回し、距離 `D` のカメラで透視し（`w = D − z`）、画面で roll と倍率を掛けて `(X0, Y0)` に置く。

## `fill(points, shape, holes, color[, options])`

形の内側を、行ごとの横線で塗る。

- `shape`: 多角形 `[start, count]`、または軸に沿った楕円 `[cx, cy, rx, ry]`（半径は正）。
- `holes`: 抜く多角形（最大 6）。`[]` か `undefined` で穴なし。
- `color`: RGB565（0..65535）。
- `options`（省略可）: `{top, bottom, step}`
  - `top`・`bottom`: 塗る行の範囲を、形の上下からさらに狭める（有限の数）。
  - `step`: `1`（毎行、既定）か `2`（偶数の行だけ。網点の半透明）。
- 行 `y`（`ceil(上端)`..`floor(下端)`、画面の 0..134 に切る）ごとに、形の区間から穴の区間を引き、残った区間を画面の 0..239 に切って、両端を四捨五入した横線にする。幅 0 の区間（頂点の行、楕円の先端）は描かない。穴の縁の列は塗る側に残る。

## `outline(points, start, count, color[, options])`

点 `start` から `count` 個（2..256）を順につないだ線。

- `options`（省略可）: `{closed, pieces, hidden}`
  - `closed`: 最後の点から最初の点へも引く（既定 `false`）。
  - `pieces`: 各辺を等分する数 1..8（既定 1）。
  - `hidden`: 隠す多角形（最大 6）。断片の中点がその中にあれば描かない（形の内側を通る輪郭を消す）。
- 端に有限でない点を持つ辺は描かない。断片は画面（0..239 × 0..134）に切ってから端を四捨五入する（画面の外の長さはラスタの費用にならない）。

## エラーと上限

| code | とき |
| --- | --- |
| `INVALID_ARGUMENT` | 引数の形が違う（行列が 12 個の有限の数でない、点が書き込み可の `Float32Array` でない、多角形の頂点数・穴の数・色・`step`・`pieces` が範囲外、オプションが object でない） |
| `LIMIT_EXCEEDED` | 多角形や点の範囲が配列の終わりを越える。または描いている途中でフレームの上限（面の線分数、ラスタ 65,535 step）に達した |
| `BUSY` | `beginFrame()` の前、または前のフレームが上限で捨てられた後（`draw()` と同じ）、または他の手続き面の呼び出しの最中 |

- **上限で止まった呼び出しは何も残さない**（その呼び出しで足した線分はフレームから取り除く）。フレームは `draw()` が上限で失敗したときと同じく捨てられ、次は `beginFrame()` から。
- `fill()` と `outline()` の線分は `draw()` の線分と同じフレームに、呼んだ順に並ぶ。plan も VM も使わない。

## 費用（実機、MEGADEMO の WAKE）

1 フレームに `project` 2 回・`fill` 4〜6 回・`outline` 13 回。plan を通していた前の版で JS のターン約 22 ms（同じ絵を JS で描くと約 130 ms）。数値の内訳は [megademo-shapes.md](../apps/megademo-shapes.md)。
