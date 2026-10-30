# hone の連続前面ブレンドとpacked楕円体

雲の造形を一つの柔らかなvolumeへ近づけるために追加した汎用描画primitive。キャラクター名・演技・時計は持たない。既存`ellipsoid`の描画式/署名と`shapes`互換aliasは保持する。ayatsuri C scene全体の実装を意味せず、C側はhone描画、共有ayatsuri C核は引き続き親子TRSである。

## blendEllipsoids

```js
pocket.hone.blendEllipsoids(lobes, light, material, surface, controls);
```

| 引数 | 型と意味 |
| --- | --- |
| lobes | Float32Array、21*n要素、n=1..8。各rowはscreen8、world normal matrix9、depth4 |
| light | Float32Array(9)、世界の光方向3・視線方向3・光RGB3。既存ellipsoidと同じ |
| material | Float32Array(11)、既存material8とambient RGB3（0..1）。ambient係数へ環境色を掛け、直接光/透過色から分離 |
| surface | Uint16Array(3)、固有色RGB565・opacity0..255・厚み係数0..255 |
| controls | Float32Array(2)、blend幅0..128世界単位・edgeSoftness0..1 |

screen8は既存楕円体の2×4投影、normal9はworld逆転置。depth4はカメラへ向かう単位方向とworldの各半径basisとの内積3個、および同方向と(world中心-camera target)の内積。screen/depthの絶対値は4096以下、normalは1e6以下かつ非縮退。全値はfinite、light/materialの既存制約も検査する。

複数の解析的前面深度をsmooth-maxし、深度勾配に対応する法線を混合して一度だけ照明/alpha合成する。blend=0は前方の面を選択し、同深度は決定的な順序で扱う。輪郭は元の投影楕円の和集合を越えず、隙間を埋めるsurfaceや一般の3D booleanではない。小さい周縁lobeを主volumeより奥へ置くなど、形の設計も必要。

透過項は混合した正規化chordによる厚み近似。体積散乱/多重透過/相互落影/全sceneのdepth bufferはない。group内の材質は共通で、個別の色・UV patternは未対応。模様が必要な従来形状は既存ellipsoidを使う。

候補frameへ96+84*n Bをsnapshot（最大768 B、geometry entryを含む最大78 compact entries）。caller bufferは同期借用だけで、呼出後の変更・解放は候補に影響しない。未整列のcompact entry列はmemcpyで読む。全描画parameterがband署名に入り、旧/新boundsのdamageと失敗転送後repairを既存のcandidate/committed契約に従わせる。

clipしたgroup bbox面積×nを、既存surfaceの累積65535 raster budgetへ公開前に加算する。packet/資源制約違反は候補への部分書込み前に拒否し、最後のcommitted画像を変えない。これは実機速度保証ではなく仕事量の上限。native stack/最大単一確保はguest heapと別に測る。

## ellipsoidPacked

```js
pocket.hone.ellipsoidPacked(data, byteOffset, light);
```

`data`はArrayBuffer、`byteOffset`は0..UINT32_MAXの整数、`light`は既存Float32Array(9)。offsetから148 Bのspanが必要。offset自体は未整列でもmemcpyで読み、live pointerを保持しない。

| offset（span先頭から） | 要素 |
| --- | --- |
| 0 | Float32 screen8 |
| 32 | Float32 normal9 |
| 68 | Float32 material8 |
| 100 | Float32 texture9 |
| 136 | Uint16 surface5 |
| 146 | padding2、無視してnative snapshotはzero初期化 |

fieldごとに既存ellipsoidのvalidationを行い、同じ184 B snapshotと描画経路へ渡す。旧6引数APIと同画素であり、別のshade実装ではない。コピー0という意味ではなく、utilityが通常描画のためだけに複数のtyped view headerを保持する費用を避ける。

## ayatsuriでの利用

```js
// appが任意moduleをロードし、scene単位で導入する。
const scene=Ayatsuri.scene({backend:pocket.hone,geometries:[AyatsuriBlend]});
scene.add({name:'volume',geometry:{kind:'blendEllipsoids',blend:6,lobes:[
  {position:[0,12,0],radii:[28,33,19]},
  {position:[-2,-15,0],radii:[25,32,18]}
]},material:{color:0xf5fbff,ambient:.8,ambientColor:0xaecff4,diffuse:.55}});
```

`character_blend.js`は任意module。高層は位置・半径・材質だけを指定し、row offsetやdirty管理を要求しない。形状定義は作成時コピー、以後不変。group poseで親子配置/回転/非一様scaleを行い、カメラ変更はscreen/depth、光だけの変更は照明だけへ伝播する。sceneはdescriptorを保持しdisposeで解放する。`pocket.app.load`はrealm内で一度だけなので、繰り返し生成するappは必要なfactory参照を所有し、削除したglobalを再loadで復元できると仮定しない。

標準楕円体の通常native経路はpacked入口を優先する。互換backend/visit用のradii/screen/normal/texture viewは初回利用時だけ生成して再利用し、borrowed viewの寿命は従来通り。viewの追加費用と生のtyped byte数は異なる。新しい雲の動機・画像・受入状態は[CLOUD ACT](../../apps/cloud/README.md)を参照。
