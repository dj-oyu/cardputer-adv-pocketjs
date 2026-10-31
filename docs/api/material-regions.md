# 同じ曲面上の2色領域: localPlane

実験版の最小実装。通常/凸clip付きellipsoidのbody-local座標で固有色を分け、形状・法線・艶・透過・alphaは共有する。別の球で白い模様を作る必要を減らす機能であり、狐造形や一般遮蔽の完成ではない。[比較結果と費用](../perf/material-region-study.md)を参照。

## authoring

動的JS作成ではoptional module `apps/lib/ayatsuri_local_plane.js`をロードし、`AyatsuriLocalPlane.scene(options)`を明示的に使う。通常の`Ayatsuri.scene`を書き換えるmoduleではない。appのchunksへ`regions ../lib/ayatsuri_local_plane.js`を登録し、必要時に`pocket.app.load('regions')`する例になる。既存band/checkerや領域を使わないappはこのmoduleを読み込まない。固定native assetもJS helperを必要としない。

専用factoryは元のsceneと同じ配置/材質/光/時計/描画の入口を持ち、同じ面への色領域適用を引き受ける。作成済みpatternは所有normalと共有encoderを保持し、global helperの置換/削除で意味を変えない。scene.disposeはhelperの作業参照も切る。現在はSourceAのprivate264B layoutを使う実験adapterで、任意geometry/clip adapterの自動合成を保証しない。

```js
const scene = AyatsuriLocalPlane.scene(options);
const cheek = scene.add({
  name: 'cheek',
  geometry: {kind: 'ellipsoid', radii: [26, 23, 18]},
  material: {color: 0xe47b30, roughness: 0.94, specular: 0.025},
  pattern: {kind: 'localPlane', normal: [-0.45, 0.86, 0.24],
            offset: 0.45, feather: 0.12, color: 0xffe8bb}
});
cheek.place({rotation: [0, yaw, 0], scale: [1, 0.9, 1]});
scene.light({direction: lightDirection});
scene.evaluate(timeMs);
scene.render(); // beginFrame/commit and clock belong to the app.
```

`normal`は有限F32化可能な3成分で、0 vectorを拒否し、authoring時に正規化する。`offset`は正規化後の単位球local座標での距離、有限[-2,2]、既定0。`feather`は境界の全幅、0または[0.0001,2]、既定0。`color`はRGB24のB色、material.colorがA色。入力normalは所有copyし、その後callerが変更しても影響しない。初版は作成時指定のみで、`texture()`によるUV変形は拒否する。色Aのmaterial更新とposeは既存入口を使い、固定B色の動的overrideは追加しない。

前面の復元点qでd=n·q−offsetを計算する。feather=0はd>=0でB、それ以外A。正ならmix=clamp(0.5+d/feather,0,1)。端点はA/Bを直接選び、中間だけRGB565の各成分を混合してから既存のshadeを1回行う。端点をA+(B−A)で再計算し、丸めが変わることを避ける。

qはworld normalやlightではなく元のunit sphere座標なので、模様は物体とともに動き、非一様scaleにも追従する。light変更では模様座標を変えず、shadeだけ変わる。新しい任意mesh morphや物理的な皮膚/毛の散乱を保証する機能ではない。

## hone / C packet

既存184B `ksn_proc_ellipsoid`のsurface[4]=4を使う。通常とclippedが同じカーネルを通る。新しいpacket variantや全画面bufferは追加しない。

| field | mode4の意味 |
| --- | --- |
| surface | A565, B565, opacity0..255, thickness0..255, 4 |
| texture[0..2] | F32 unit normal、encoded成分のlength²をdoubleで計算し1±0.0001 |
| texture[3] | offset [-2,2] |
| texture[4] | feather 0または[1e-4f,2] |
| texture[5..8] | 数値0, edgeSoftness0..1, 数値0, 数値1 |

未使用0は数値として検査し、符号付き0を区別しない。normalはhone側で黙って正規化しない。旧mode0..3のUV意味・validation・計算を維持する。無効型/値/近特異投影は従来のINVALID_ARGUMENT。candidate/snapshot/rollback/repair/帯damageの所有権契約も既存どおり。全packet byteをdamage署名へ含むため、境界やB色の変更も描画更新される。

maskのnormal/featherは法線場やroughnessを切り替える値ではない。共通のnormal・材質で1回shadeするので、模様の境界へ偽の凸凹や別highlightを追加しない。RGB565での様式化近似であり線形色空間PBRではない。

## native共有Model

AYNM v1は、領域を使う場合だけrequired section8を追加する。旧readerは単色として黙認せず拒否する。従来のsection1..7だけのblobはbyte不変で使える。保存rowはLE/stride32:

| offset | 型/意味 |
| --- | --- |
| 0 | nodeId u16、ellipsoidだけ、重複なし |
| 2 | mode u8=4、reserved u8=0 |
| 4 | unit normal3 F32 |
| 16 | offset F32 |
| 20 | feather F32 |
| 24 | B色RGB24 u32 |
| 28 | reserved u32=0 |

全rowを境界・型・値まで検証してからModelを公開する。表はcold資産としてModelが共有し、InstanceごとのJS objectへ復元しない。既存264B workへ評価し、Instance arenaを増やさない。Model descriptorは8B増えるため、native化でguest会計外へ逃がす方式ではない。close/最後のInstance退役によるcold資産解放、二個体の分離、描画snapshotは[Model契約](ayatsuri-model-draft.md)を維持する。

初版は1平面・ellipsoidのみ。2平面の積集合、blendの共通local領域、任意mask画像/UV、surface間depthは後続候補。各blend lobeのqへ同じ模様を貼る方式を共通surface対応と呼ばない。
