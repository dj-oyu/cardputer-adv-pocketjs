# 任意appから使うnative sceneと明示時刻motion

`apps/lib/ayatsuri_model.js` の `AyatsuriNative.scene` と
`apps/lib/ayatsuri_motion.js` の `AyatsuriMotion` は、PR #110の公開汎用コードを
現在のmanaged Model/Instanceへ接続したoptionalなJS層である。CLOUD/狐の資産、
旧動的character engine、モデルの組込みloaderは取り込まない。既定appやHOMEへ
自動ロードせず、必要なappだけがchunkとして選択する。

## 組込み方と責務

既に登録されているappの `chunks.txt` に、例えば次を加える。
`tools/make_app_chunks.py` は相対パスを解決し、既存buildがchunk tableへ登録する。

```text
app local.myapp
native-scene ../lib/ayatsuri_model.js
motion ../lib/ayatsuri_motion.js
```

appは `display.kasane`、`kasane.shiki`、`kasane.shapes` の通常のcapabilityを必要とする。
`pocket.app.load('native-scene')` と `pocket.app.load('motion')` をsetup中に一度ずつ
呼び、app所有の検証済みAYNM ArrayBufferを使う。任意ファイルの読込み・認可を
libraryが代行する機能ではない。

```js
pocket.app.load('native-scene');
pocket.app.load('motion');
const model = pocket.ayatsuri.modelOpen(modelBuffer);
const scene = AyatsuriNative.scene({model,
  camera: {position:[0,0,400], target:[0,0,0], zoom:1, center:[120,67]},
  light: {direction:[0,0,1], color:0xffffff}});
const joint = scene.node('leftJoint'); // Bind names once during setup.
const track = AyatsuriMotion.track(new Float32Array([
  0,0, 100,.4, 350,-.6, 800,0
]), {stride:2, timeUnit:'ms'});
const playback = AyatsuriMotion.performance({track,channels:['time','angle']});
const rotation=[0,0,0], patch={rotation};
const P=pocket.kasane.shiki;
function draw(timeMs) { // Called by the app's clock/input/lifecycle adapter.
  const pose=playback.evaluate(timeMs); // Borrowed reusable output.
  rotation[2]=pose.angle; joint.place(patch);
  scene.evaluate(timeMs);
  P.beginMeshFrame(0x0011,4096,200000,1);
  scene.render(); P.commit(); // Presentation ACK still belongs to the app/runtime.
}
function releaseSources() {
  scene.dispose(); model.dispose(); // Pending frame snapshots remain owned by Shiki.
}
```

callerは表示先image viewの設定、時計、入力、pause/seek、begin/commit、提示成功と
失敗後の再試行、app/scene終了を所有する。`scene.evaluate(time)` は時刻を検証するが、
motionを自動進行させない。停止時はcallerが同じ時刻を再使用するか、target更新を止める。
内部timer、積分時計、隠れた乱数、対称動作の仮定を持たない。

上のdepth scopeはopaque mesh専用である。解析楕円体/blendを含むModelは
`P.beginFrame` のpainter経路を選ぶ。異種packetをdepth scopeで黙って描画しない。
HOMEのC `scene_ops_t` adapter、別realmやapp間のnative handle共有は未実装であり、
このJS層を名前空間へ注入するだけでは対応済みにならない。

## state、失敗、解放

handleの `place({position,rotation,scale})`、`visible(boolean)`、対応kindの
`material({color,opacity})` とsceneの `camera` / `light` を用いる。scaleは非zero、
cameraはorthographic、色はRGB24。未対応のmesh材質overrideは拒否される。
入力には副作用のない通常のdata object/Array/TypedArrayを使う。内部のdirty maskや
stride、private bufferをcallerが直接変更する契約ではない。

Modelはimmutable cold dataを共有し、sceneは独立native instanceと再利用するtyped
workを所有する。handleとsceneのmethodは共有prototypeに置き、部品ごとのclosureを
作らない。`node(name)` は一度解決したhandleをscene内のnull-prototype tableへcacheする。
Modelをcloseした後も既に解決済みの名前/handleと既存instanceは使えるが、新規の名前
解決と新instance作成は拒否される。bindingをsetupで済ませてからModelをcloseする。
sceneは借用Modelを勝手にcloseしない。

不正setterは既存targetとready状態を変えない。core評価失敗後はrenderを拒否し、
正常なcamera/targetによる再評価で復帰する。native submitの失敗ではcandidateを
commitせず、app側でnew beginから作り直す。[frameの責務](character-lifecycle.md)も参照。
stats等のcallback中は同sceneへの再入/disposeを拒否し、throw後もbusy guardを解除する。

`dispose()` は冪等で、scene所有のinstanceとtyped work参照を切る。保持された古い
handleは操作を拒否する。Model.close後の最後のinstance解放でcold copyも同期的に
detachされる。frame snapshotは別ownerなので、sourceのdisposeを提示ACKまで遅延させる
必要はない。scene解放はShikiのcommitted画像/frame領域の解放ではなく、それらは既存
runtimeのreset/teardownに従う。

motionのkeysは変更禁止で借用し、`timeUnit` は `ms` または `frame` を明示する。
linear/smooth/step補間、名前付きchannel、add/multiplyのwave、半開区間gateを持つ。
wave/gate/channel宣言は構築時にsnapshotし、返却結果は次の成功evaluateで更新される
borrowed objectである。履歴が必要ならcallerが値をコピーする。overflow時は前回の
公開結果を変えずに拒否する。skin、physics、clip/layer/morph全体の実装ではない。

## 再現とメモリ会計

```sh
node tools/kasane_contract/test_scene_motion.cjs
python tools/kasane_contract/run_scene_motion.py
python tools/kasane_contract/run_meshbench_app.py
```

hostシナリオは公開の5node/3group/2quad/4triangleだけを生成する。親子TRSの
rigid joint、左右非対称の回転/移動、逆seek、pause、可視性、非一様/反転scaleを
同じQuickJS上の独立target構築と比較する。referenceは同じnative TRS/rendererを
共有するため、JS/C行列式の独立数学的検証やBlender再現率ではない。

4/8行×row/span×片面/明示両面材質の8条件で、3回の生成・破棄と各120 scene frameを検査する。
正scaleの非一様変形を比較し、反転scaleは既存片面meshの背面除去をnegative caseとして
保持する。別の明示両面入力では反転後も可視であることを検査する。片面の既存windingを
黙って書き換える機能や、左右反転を自動でrig動作へ置き換える機能ではない。
遅延4行transportはhalf-stripを借用し、8行はcopy control。IO失敗/retry/ACK、
source全解放後のpending frame描画、保持中tombstone、constructor OOM後の
GC前instance数0と復帰、最終requested ledger全解放を確認する。

facade typed workはsceneあたり `49*N+132 B`（N=5で377 B）。JS object/view、
名前cache、motion配列、native arena/cold blob、frame snapshotは別費用であり、
typed workだけでheap予算を主張しない。testは2 sceneと直接比較用actorも持ち、
cold976 B（片面）/1072 B（左part明示両面）、arena/actor、guest現在値、linked malloc/calloc/reallocの要求live/peak、
snapshot/workspaceを分けて表示する。cold/arenaとtyped workはguestとrequested合計の
内数なので重ねて足さない。

warm allocationは再利用patchと `JS_Call` を使い、frameごとの `JS_Eval` parseを除いた
scene/motion/submit/present範囲を計数する。実 `pocket_app_load.c` と既存host chunk tableで
chunkロードと冪等性も検査する。namespaceはeager host shimで、製品lazy登録の総メモリと
同じではない。host chunk file buffer/name/pathの要求bytesも別記してtotalへ含める
（製品の同sourceはimmutable flash）。hostの静的panel・比較用buffer・ledger、
allocator overhead、共有library内の確保、stack、target全体は含まない。64bit hostの
結果を32bit ABIやS3 RAM適合、cycles/FPSへ外挿しない。

## 未完の実キャラクター工程

狐3Dの実モデル描画とリギングは未完である。この合成成功を完成・画質向上として
扱わない。このsliceは公開の合成rigid-joint入力だけを対象とし、狐の原本・派生geometry・
画像を使用しない。

今回のjointは親子TRSで剛体partを動かす基盤までである。狐へのarmature/骨配置、
bind/rest対応、weight/skinning、変形法線、morph/表情、実モデルの遮蔽・姿勢・メモリ
検証は未実装/未確認。固定小数点/PIEの独立作業も統合していない。これらは別々の
受入工程として扱う。

公開の合成入力については、独立した[skin scalar core](skin-core.md)でbind paletteと
4 influence変形のhost検査を追加し、後続の[C描画接続](skin-render-bridge.md)で
既存Model/opaque rendererへの同期借用を検査する。JS sceneのskin APIとAYNM skin tableは
未実装であり、上記の実キャラクター工程の完了を意味しない。
