# 同じ曲面上の2色領域: localPlane

実験版の最小実装。通常/凸clip付きellipsoidのbody-local座標と、blendの共通model-local平面で固有色を分け、形状・法線・艶・透過・alphaは共有する。別の球で白い模様を作る必要を減らす機能であり、狐造形や一般遮蔽の完成ではない。[比較結果と費用](../perf/material-region-study.md)を参照。

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

## blendの共通面: 新variant

blendは各lobeの単位球qでは分類しない。node全体のmodel-local点pに対して`f=n·p-offset`を定義し、全lobeのfront depth合成が終わった一つのsurfaceで評価する。nはunit normal、offsetとfeatherはmodel-local座標の単位。ellipsoidの単位球qの距離と同じ数値範囲とはしない。offsetは[-4096,4096]、featherは0または[1e-4,4096]。

評価時にnode TRSとcameraから`local xyz → screen x,y,depth`のアフィン変換を作り、その逆変換で`f=A*x+B*y+C*depth+D`へ写す。lightはこの変換に入らない。最終depthで得たmixによりA/Bのalbedoを選び、その後は既存の共通normal、ambient tint、艶、透過、coverageとalphaを1回だけ適用する。blend depth自体と近似normalを変更せず、normalがdepthの厳密微分になるという追加保証もしない。

旧96B headerと84B×lobe数のpacketは維持する。新EXT=20480は`[旧header96][lobes84*n][descriptor24][zero padding]`。descriptorは旧packetを10B単位へ丸める**前**のoffsetへ置く。新payloadは120+84*n B、geometry entryを含め1+ceil(payload/10) entries。最大8lobesでは792B/81entries。非整列のsegment列はmemcpyで読む。

| descriptor offset | 型/意味 |
| --- | --- |
| 0 | A,B,C,DのF32×4、有限、各絶対値≤1e6、A/B/C全zeroは禁止 |
| 16 | feather F32、0または[1e-4f,4096] |
| 20 | B色RGB565 u16 |
| 22 | reserved u16=0 |

変換済み係数はpixelとdepthの混合単位なので、その3成分の長さをunit normalとして検査しない。元の単位normalはauthoring/AYNM側で検査する。大きな係数・移動の打消しではF32の境界誤差が出るため、非常に薄い境界の精度を全許容範囲で保証しない。非有限・上限超過・逆変換不能は評価失敗として前のcommitted描画を維持する。

hone Cは`pocket_proc_native_blend_region(ctx,op,&blend,&mask)`。JS低層は既存`pocket.hone.blendEllipsoids(lobes21n,light9,material11,surface3,controls2)`の5引数を保持し、7引数の場合だけ`plane5`（A,B,C,D,featherのFloat32Array）と`color1`（B色Uint16Array）を追加する。新たな常駐JS endpoint objectは増やさない。全入力をstackへcopy/検証してから候補へappendする。低層以外の利用者はnative Modelのpattern記述を使い、画面係数やoffset管理を行わない。

今回、動的SourceA用`AyatsuriLocalPlane.scene()`はellipsoid専用のまま。blendでの高層入口はoffline Model記述とnative Instanceであり、SourceA wrapperを対応済みとはしない。2平面、任意mask、blend surfaceへの独立凸clipや別surface間depthは未実装。

## native共有Model

AYNM v1は、領域を使う場合だけrequired section8を追加する。旧readerは単色として黙認せず拒否する。従来のsection1..7だけのblobはbyte不変で使える。保存rowはLE/stride32:

| offset | 型/意味 |
| --- | --- |
| 0 | nodeId u16、重複なし |
| 2 | mode u8=4はellipsoid、5はblend、reserved u8=0 |
| 4 | unit normal3 F32 |
| 16 | offset F32 |
| 20 | feather F32 |
| 24 | B色RGB24 u32 |
| 28 | reserved u32=0 |

全rowを境界・型・値まで検証してからModelを公開する。表はcold資産としてModelが共有し、InstanceごとのJS objectへ復元しない。mode4は既存264B workへ評価し、arenaを増やさない。mode5は使用するblend nodeの既存316+108*n B slotに24Bの変換済みdescriptorだけを追加し、iteration kind3で返す。非使用nodeのslotとkind2は不変。Model descriptorは初版のsection8で8B増えるため、native化でguest会計外へ逃がす方式ではない。close/最後のInstance退役によるcold資産解放、二個体の分離、描画snapshotは[Model契約](ayatsuri-model-draft.md)を維持する。

初版のmode4/ellipsoidに加え、mode5/blendの1平面を小実証する。2平面の積集合、任意mask画像/UV、surface間depthは後続候補。各blend lobeのqへ同じ模様を貼る方式を共通surface対応と呼ばない。
