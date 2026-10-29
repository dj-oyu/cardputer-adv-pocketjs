# `pocket.shapes` — 平面のベクター描画

手続き面（`pocket.kasane.procedural`）の、組み立て中のフレームへ、塗りと線を C で描く。点は呼び出し側の型付き配列に置き、番号で指す（フレームごとに配列を作らない）。設計の経緯は [megademo-shapes.md](../apps/megademo-shapes.md)、実装は `main/pocket/pocket_shape.c`、契約の試験は `tools/kasane_contract/run_pocket_shape.py`。

## 使えるアプリ

manifest の `required` か `optional` に `kasane.shapes` があるセッションにだけ入る（共通 API ではない。`pocket.derby` と同じ）。`pocket.capabilities.get('kasane.shapes')` の limits:

| 名前 | 値 | 意味 |
| --- | ---: | --- |
| `maxPoints` | 1024 | 1 つの配列で指せる点の数 |
| `maxPolygonPoints` | 256 | 1 つの多角形の頂点の数 |
| `maxHoles` | 6 | 1 回の塗りで抜く穴、または輪郭を隠す多角形の数 |
| `maxShadeSamples` | 1024 | `shade` 1 回の配列の標本数 |

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

## `shade(normals, albedo, thickness, lighting, material, output[, from[, count]])`

材質と照明の小さな標本カーネル。**実装中の拡張**であり、実機性能と最終的な3体の見た目は未検証。画像・frame・描画順・物体名は所有せず、標本ごとの色だけを計算する。既存の `project/fill/outline` の意味は変えない。

| 引数 | 型と長さ | 意味 |
| --- | --- | --- |
| normals | Int16Array、3n | 世界座標の法線xyz。SNORM16、各成分−32767..32767。−32768とゼロベクトルは拒否。方向を単位化して使う |
| albedo | Uint16Array、n | 物体に属する固有色/模様、RGB565 |
| thickness | Uint8Array、n | 0は薄い、255は厚いというモデルの透過減衰係数。距離の物理単位ではない |
| lighting | Float32Array、9 | 光へ向く方向xyz、観測者へ向く方向xyz、光RGB。方向各成分−1..1（非ゼロ、単位化）、RGBは0..1 |
| material | Float32Array、8 | ambient,diffuse,specular,shininess,transmission,透過色RGB。shininessは1..128、他は0..1 |
| output | 書込み可Uint16Array、n | 出力RGB565。入力とは別配列、またはalbedoと完全に同一のview |

nは0..1024。fromは既定0、countは既定残り全部。いずれも整数で範囲を越えるとLIMIT_EXCEEDED。他の型/長さ/値/重複bufferの違反はINVALID_ARGUMENT。requested rangeの法線を全て検証してから書くので、遅い標本に不正値があってもoutputは変わらない。範囲外標本は読まず書かない（lighting/materialと配列形の検証は毎回行う）。

全bufferは同期呼出しの間だけ借り、保持・コピーしない。typed viewと値を読み終えてから計算し、その間JS/callbackを実行しない。outputはalbedoの同一viewだけin-placeを許し、他入力とのbyte領域の重複、albedoとの部分重複は拒否する。出力への書込は2×count byteで、返り値はundefined。frameを開く必要はない。

Nは単位法線、Lは光方向、Vは視点方向、Hはnormalize(L+V)。front=max(0,N·L)、back=max(0,−N·L)、thin=1−thickness/255。L+Vがゼロならspec=0、ほかはfront>0のときだけspec=max(0,N·H)^shininess。

各RGB成分の式は次のとおり。baseはRGB565の各成分を31/63/31で割った値。

```
C = base*(ambient + diffuse*front*lightRGB)
  + lightRGB*(specular*spec + transmission*thin*back*transmitRGB)
```

Cを0..1へclampし、31/63/31を掛けて四捨五入する。これは表示RGB上の様式化近似で、線形色空間PBRや物理的内部散乱ではない。厚みは裏側光の減衰だけを変え、表面ツヤの強さを厚みから決めない。world法線の逆転置変換、テクスチャUVから固有色への評価、光/視点変更に応じた再評価範囲はutilityの責務。実際の背景の透過合成や落影はこのカーネルだけでは実現しない。

法線の量子化幅は1/32767（約0.0000305）。位置/変換を整数化するAPIではない。法線再正規化の後に照明を計算する。標本数以外の動的確保はなく、常駐モデルcacheも持たない。既存のWAKE計測をこの追加カーネルのfps保証へ流用しない。

## `ellipsoid(screen, normalMatrix, lighting, material, surface, texture)`

滑らかな材質と透過合成を検証する、**実装中の解析的パッチ**。投影された単位球を走査帯の各画素で評価する。三角形の平面塗りへ分割せず、全画面bufferも持たない。既存の `fill/outline` と同じ候補frame/順序/上限を使い、appがbeginFrame/commitする。

| 引数 | 型/長さ | 内容 |
| --- | --- | --- |
| screen | Float32Array/8 | Ax,Ay,Az,cx,Bx,By,Bz,cy。単位球のlocal点qを画面x=A·q+cx、y=B·q+cyへ写す直交affine投影 |
| normalMatrix | Float32Array/9 | local球面法線を世界法線へ写す行優先3×3。モデル線形変換の逆転置 |
| lighting/material | Float32Array/9、8 | shadeと同じ方向・光色・材質の契約 |
| surface | Uint16Array/5 | colorA565、colorB565、opacity0..255、thickness0..255、pattern0..3 |
| texture | Float32Array/9 | uScale,uShear,uOffset,vShear,vScale,vOffset,edgeSoftness,bandStart,bandEnd |

screen各成分は有限±4096、normalMatrixは有限±1,000,000で非特異。投影の2行は独立（Gram行列のdet > 1e−6×対角積）。texture affineの6値は有限±4096、softness/start/endは0..1、start<=end。不正型/長さ/値/近特異投影はINVALID_ARGUMENT。offscreenとopacity0は有効な空描画。

球面の身体UVはu=atan2(qx,qz)/(2π)+0.5、v=qy/2+0.5。textureの2×3 affineを掛ける。pattern0はAのみ、1はUの小数部が[start,end]ならB、2はVで同判定、3はfloor(U)+floor(V)の奇数cellならB。画像textureはこのprimitiveでは未対応で、未知patternを黙って無視せず拒否する。模様は球体local座標に固定し、照明と独立に評価する。

screenの2行の擬似逆行列で各画素から球内の点を求め、2行の外積方向にsqrt(1−r²)だけ進めて表面を復元する。normalMatrixと世界視線で前面を選ぶため、負scaleの反転でも背面を描かない。透過減衰はshadeのthinの代わりに `1−thickness/255×sqrt(1−r²)` を使い、光路の長さの近似とする。表面ツヤとは独立で、物理的subsurface/PBRではない。

alphaはopacityにedge coverageを掛け、現在の走査帯の下地へRGB565成分ごとに合成する。coverageは `min(1,(1−r²)/max(edgeSoftness,2/min(radiusX,radiusY)))`。softness0でも輪郭の約1pxを滑らかにする。softnessを増すと柔らかな縁になる。真の体積多重散乱、光の落影、透明物体の自動depth sortは未対応。draw順はapp/utilityが決める。

呼出時に184Bの数値packetをcandidateへコピーして凍結する（詰物もゼロ化）。1パッチは既存frameの20entry=200Bを使い、clipped AABBの画素数をraster予算へ保守的に課金する。フレームごとのJS bufferコピー0とは異なり、この所有権確定のコピーは必要。caller配列を後で変えてもcommit済み画像は変わらない。render帯でpacketをstackへ読み出すが、継続参照・動的確保・全画面cacheはない。packet全byteを帯damage署名へ含めるため、光/材質/模様だけの変更も再描画される。

上限失敗はLIMIT_EXCEEDED、候補frame破棄、呼出の部分結果なし、次beginFrameが必要。実機のrender時間/heap/画質は未検証。ホスト契約試験は `tools/kasane_contract/run_proc_ellipsoid.py` の実renderer O0/O2・全画面/8行帯一致、alpha/光/視点/UV/負scale/特異入力/切れたpacketを扱う。
