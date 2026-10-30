# AyatsuriClip: 3D anchorを持つ凸開口の最小adapter

実験版JS utility。低層は[hone/shapesのclipped ellipsoid](shapes.md#ellipsoidclippedpackeddata-byteoffset-lighting-xy)。同じカメラと親子TRSで3D開口を投影し、解析的なglobe/iris/pupilなどを切る。画像部品、一般mesh、Z buffer、物理的な切断面は提供しない。

動的JS作成には`scene(options)`、固定native資産には`nativeScene({model,light,camera})`を使う。後者は`AyatsuriNative`を読み込み、SourceAの`character.js`/blend moduleを必要としない。どちらも同じaperture/morph/EMPTY/evaluate/render/disposeの意味を持つ。

```js
const scene = AyatsuriClip.scene({backend: pocket.hone, light, camera});
const eye = scene.add({geometry: {kind: 'ellipsoid', radii: [8,10,3]}});
const opening = scene.aperture(eye, restXYZ); // Float32Array、3〜8点
opening.bind(eye);
opening.setVertices(morphedXYZ);
scene.evaluate(timeMs);
scene.render(); // beginFrame/commitと時計はapp所有
```

rest/morphedXYZはprojection楕円体の**単位球local座標**。globeのradiiと同じparent/world変換を経た既存screen8で投影する。一般の独立したworld point APIではない。projection handleのradii/同一性と現private layoutは変更しない。globeをsocketに固定し、視線はiris/pupilを動かすモデルから始める。

`bind(target)`は同じsceneの通常楕円体だけを登録し、複数targetへ開口を共有できる。`setVertices`は全点をstagingへコピーして有限・strict convex・rest windingを検査する。無効な変更の後はrenderを拒否し、次の有効morphとevaluateで復帰する。旧native committed画像の維持にはappが失敗candidateをcommitしないことが必要。

`setEmpty(null|'closed'|'pose')`は明示状態。closedはモデルの完全閉眼、poseはモデルが可視域外と判断した状態、nullは通常投影。縮退/反転/凹化を黙ってEMPTYへ直さない。不可視でもlocal morphの妥当性を検査する。nativeは両windingを許す一方、このadapterはrest方向と初回投影方向を保ち、対応するカメラ角度域をモデル側で制限する。

evaluateは既存scene評価→必要な全開口の投影/検査→全件成功後publish。カメラ/親子pose/morph変更はclipへ伝播する。光だけの変更でscreenが不変なら再投影せず、陰影評価は既存sceneに任せる。offsetやdirty flagの手動管理はcallerへ要求しない。

初期化時にlocal/staging/projected/screen cacheを確保し、更新時には既存bufferを使う。typed bufferの寿命はscene所有、native呼出時には同期snapshotされる。disposeは冪等で、保持apertureをtombstoneにし不要なscene/work参照を切る。warm loopの実alloc数、wrapperのguest費用とnative packet増分は別に計測する。新clipped packetは従来より1枚あたり7entry/70B増え、EMPTYは0append。

動的JS作成pathは`character.js`のprivate handle/node/264B work layoutとの実装上の結合を持つ。これはJS authorへoffsetを公開する意味ではなく、実験adapter内部の互換条件。固定native pathは下節の共通C評価を使う。追加AA、一般depth/遮蔽、view correctiveの自動生成、任意角度/負scaleの保証はない。

## 固定native資産の利用

```js
const asset = pocket.ayatsuri.modelBuiltin('fox-face');
const scene = AyatsuriClip.nativeScene({model: asset, light, camera});
const globe = scene.node('sclera-1');
const opening = scene.aperture(globe, restXYZ);
opening.bind(globe).bind(scene.node('iris-1')).bind(scene.node('pupil-1'));
scene.node('fox-head').place({rotation: [0, yaw, 0]});
scene.evaluate(timeMs);
scene.render();
scene.dispose(); // 個体とutility buffer。modelは借用
asset.dispose(); // 所有者が最後にclose
```

native pathは`node(name)`/`handle(id)`で固定部品を参照し、新しい部品をaddしない。初回evaluate以降はaperture/bind登録を固定する。bind時の一時的なtyped table再確保と、毎frameの再利用を区別する。readProjectionの同期copy32B/開口と、提出時のnumeric snapshotは残る。

setup時に勝手なevaluateは行わず、geometry種別と可視性の検査は最初の明示evaluate/submitで確定する。bound projection/targetは可視ellipsoidに限定する。hiddenなbound node/祖先は拒否されるため、閉眼や可視域外は`setEmpty`を使う。全rowのactive座標を検査し、未使用座標はpacketへ持ち込まずゼロ化する。アプリ所有のbegin/commitは一度だけ行う。

このpathは既存`AyatsuriNative`のprivate instance/n/readyへ依存する実験adapterだが、C work pointerは受け取らない。計算は既存の共通C評価器を使う。C利用者も同じnative iteratorとhone snapshot境界を使える一方、aperture authoring stateの汎用C facadeは未実装である。JS/Cの全framework機能が完成したという意味ではない。
