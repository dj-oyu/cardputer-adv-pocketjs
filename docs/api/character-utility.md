# Character utility — 照明因果を試す縦切り実装

追加ヒントの[点描・漫画トーン・ハッチング小比較](../../tools/character/coverage-study.md)はhost専用・未採用。固有模様とpost-lighting coverageを区別して検討する。

`apps/character/character.js`。これはv0.2仕様案の全面実装ではなく、配置・階層・材質・身体座標の模様・世界の光・視点を分離して検証する実験版。最初の粗い面分割は雲の柔らかな目標品質とフレーム容量に不適合だったため、runtimeはnative `shapes.ellipsoid` を必須とし、連続曲面のcompact descriptorだけを保持する。大きなmesh配列やJS shader fallbackをdevice chunkへ含めない。実機のメモリ・fps合格や設定図の完成品質を意味しない。

## 視覚に対応する操作

```js
const scene = Character.scene({
  light: {direction: [-0.5, -0.4, 0.8], color: 0xffffff},
  camera: {position: [0, 0, 400], target: [0, 0, 0],
           zoom: 1, center: [120, 75]}
});
const body = scene.add({name: 'body', geometry: null});
const tail = scene.add({name: 'tail', parent: body,
  geometry: {kind: 'ellipsoid', radii: [12, 30, 10], columns: 6, rows: 4},
  position: [-20, 0, 0],
  material: {color: 0xd97727, ambient: 0.25, diffuse: 0.75,
    roughness: 0.8, specular: 0.05, transmission: 0.2,
    transmissionColor: 0xffb284, thickness: 0.5,
    opacity: 1, edgeSoftness: 0.04},
  pattern: {kind: 'band', axis: 'v', start: 0.7, end: 1,
            color: 0xfff0ce}
});
tail.place({rotation: [0, 0.1, 0.2], scale: [1, 0.9, 1]});
tail.material({roughness: 0.6});
tail.texture({offset: [0, 0.03], scale: [1, 1], rotation: 0});
scene.light({direction: [0.5, -0.4, 0.8]});
scene.evaluate(timeMs);
H.beginFrame(0x0043); scene.render(); H.commit();
```

`place`はposition/rotation/scaleの部分更新。値を比較して依存を自動更新する。親は同じsceneの既存handleだけを受け付け、cycleを作れない。geometry:nullは実体を描かないgroup。材質は継承しない。`visible(boolean)`は子へ伝播する。

`material`はcolor（RGB24）、ambient/diffuse/specular/roughness/transmission/thickness（0..1）、transmissionColor（RGB24）。粗さは芸術的な指数 `1+127*(1-roughness)^2` へ変換する。厚みは表面反射の粗さや鏡面係数を決めない。薄い部分のbacklightを近似する入力であり、内部散乱の物理シミュレーションではない。

光directionは表面から光へ向かう世界方向で正規化する。camera position/targetも世界座標で、現版のprojectionはorthographic。position/targetで世界の視線方向を定め、zoomは拡大率。x右、y下、z手前、モデル単位はappがピクセル相当で決める。rotationはrad、列ベクトルのT*Rz*Ry*Rx*S、world=parentWorld*local。非一様scaleの法線は逆転置で変換する。scale0やfloat32へ格納不能な値は拒否する。これはv0.2の縮退scale許容より狭い実験版制限である。

## 身体に付く模様と光の分離

ellipsoidの身体座標は単位球qに対しu=atan2(q.x,q.z)/(2π)+0.5、v=q.y*0.5+0.5。patternはband、checkerを選べる。bandはaxis(u/v),start,end,color、checkerはcolor,repeatsを持つ。UV変形は中心0.5を基準にscale→rotation→offset、wrapして模様をサンプルする。身体の輪郭や法線をtexture操作で再計算しない。

runtimeでimageを指定すると明示エラー。画像を無視してsolid描画へ落とさない。bitmap textureをanalytic descriptorで扱う所有権とnative lifetimeは今後の課題で、texture機能全体を後回しにしたという意味ではない。初期mesh実験は`tools/character/mesh_reference.cjs`に明示的に分離し、deviceにはロードしない。このhost専用参照には借用RGB565 bitmap/最近傍サンプルが残るがruntimeの対応機能として数えない。

native analytic経路は画面の2×3 affine＋center、世界normal matrix、light/view/material、固有色・模様・UV変形・厚み・opacity・edgeSoftnessを事前確保した配列へ評価する。renderが同期に借用しnative候補frameへcompact packetをコピーし、band rendererが実画素ごとに球面位置・法線・body UV・光応答・光路厚み・edge alphaを計算して合成する。`pattern`に陰影を焼き込む設計ではない。透過項は近似backlightであり、多重散乱や他部位からの影を物理計算するものではない。

host専用mesh参照はface別albedo/normalをshadeへ渡す実験記録であり、runtimeがnative capability不足を埋める経路ではない。scene.evaluateは時計だけで自動呼吸や乱数を動かさず、アニメーションの制御はappが入力する。

| 変更 | 再計算するもの | 再計算しないもの |
| --- | --- | --- |
| 更新なし / 時刻だけ | なし | geometry、法線、投影、albedo、shade |
| 葉のplace | 当該world点・法線・投影・shade | 兄弟とalbedo |
| 親のplace | 子孫のworld点・法線・投影・shade | albedo |
| 光 | 全材質のshading入力/結果 | world点・法線・投影・albedo |
| カメラ | 投影、可視面、視点依存shade | world点・法線・albedo |
| roughness等 | 当該shade | geometry、albedo |
| color/texture変形 | 当該albedoとshade | geometry、法線、投影 |

textureを変えたときも最終色は変わるが、法線や光の幾何を再生成する必要はない。native band rendererは変更のある描画帯の画素を再評価する。変更なしでもrenderはcandidateへのpacket提出を行うため、evaluateのno-opは全描画/転送0の保証ではない。帯damageの判定は既存proc側が所有する。

## データ型、借用、失敗

analytic descriptorはradii3/screen8/normalMatrix9/texture9がFloat32、surface5がUint16（RGB565二色、opacity255、thickness255、pattern ID）。material8もFloat32。pose/matrix/UV stateを含め、有形nodeは1本264 BのArrayBufferへ重複しないviewを置く。groupはpose/matrixだけ84 B。全物体に共通なorthographic light/viewをsceneで共有し、scene共通typed workは132 B。JS object/typed view/allocator/frameの費用はこれと別。各handleのplace/material/texture/visibleはshared prototypeの同じ関数で、部品ごとのclosureを作らない。groupのmaterial/texture設定はエラー。列数・行数はanalytic経路では使わず、作者がtessellationを管理する必要はない。

host専用mesh参照ではlocal/world/投影点・材質/照明がFloat32、世界法線はSNORM16（-32767..32767、方向の量子化約1/32767）、色RGB565、厚み0..255。これは初期の型比較・数値照合用でruntimeのmesh機能ではない。座標を無条件に整数化しない。

sceneがworkを所有し再利用する。native shadeは同期呼出し中だけbufferを借用しretain/copyしない。analytic ellipsoidは描画が遅延するためdescriptorをnative frameへコピーし、JS bufferをretainしない。転送準備はutilityが担う。`visit(visitor)`はhost検証向け借用・読取専用packetで、通常の作者操作にtyped offsetsを要求しない。packet/配列を保存して後から変更してはならない。

setterは入力検証後に状態を更新する。evaluate失敗後はpartial packetを描けないようrender/visitを拒否し、正常な入力でのevaluate成功後に復帰する。appの既存commit画像は触らない。これは「最後の成功workへ戻す二面atomic評価」ではなく、v0.2から簡略化した縦切りの契約。native render失敗もappはcommitしない。

renderはappがbeginFrameした候補へnative ellipsoidを提出し、frameを所有しない。描画順は作者のadd順で、一般の相互遮蔽/depth sortは未実装。native analyticのalpha合成と透過光項は別入力。実機のメモリ・時間、細い髪の様式化や雲の完成品質は別途受入が必要で、primitiveの存在だけで合格にしない。

## 検証と観測

`node tools/kasane_contract/test_character_utility.cjs` はruntimeの依存、work同一性、invalid setter未適用、失敗後の描画拒否/復帰、analytic descriptor型・容量・reuse・UV区間を検証する。初期meshの逆転置法線等は別のhost参照として照合する。`stats(out)`は累積geometryUpdates/normalUpdates/projectionUpdates/albedoUpdates/shadeUpdates/analyticUpdates、直近renderedPatches、typedBytes/copiedBytes/objectsを書く。初期参照との比較用shadeSamples/renderedFacesはruntimeでは0。analyticUpdatesをpixel shader実行回数と取り違えない。

typedBytesはこのutilityのtyped workで、JS object/配列・allocator overhead・native frame費用を含まない。copiedBytesはJS評価内のmesh/descriptor bulkコピー量で現在0。setterのscalar state書込・native frameへのdescriptorコピー・hostのsliceを含めず、全アプリのコピー0を主張しない。通常評価は新work/closureを作らない設計だが、実QuickJS heap計測前にalloc0と断定しない。

motion.jsは別chunkの`CharacterMotion.track(keys,{stride,timeColumn,interpolation,timeUnit})`と`sample(track,time,out)`。keysはplain Array/Float32Arrayを変更禁止で借用し、outへ同じ演算順で補間する。timeUnitはms/frameの明示必須で、frame入力は実時間尺の保証ではない。MEGADEMOにはmaterial engineをロードせずこの小さい共有層を使える。

`CharacterMotion.performance({track,channels,waves,gates})`はtrackの列を意味のある名前へ結び、add/multiplyの正弦waveと半開区間[from,to)のgateを重ねる。waveはchannel/amplitude/rate/phase/mode、gateはchannel/from/to/value。rateは宣言timeUnitあたりのrad。multiplyは既存track channelだけ、複数gateの重なりは加算。`performance.evaluate(time)`は再利用する借用・読取専用objectを返し、積分時計や毎回の配列生成を持たない。overflowは最後の公開結果を変えず拒否する。WAKEはpose・呼吸・揺れ・耳flickのscheduleをこれへ移し、形状生成はモデル側へ残した。full clip/layer、morph、2D補正等はこの縦切りには未実装である。

## 後続工程: HOME background sceneからの利用

ユーザーが指定した後続範囲として、ベアメタルAPIの設計・実装後に、同じ配置・材質・変形・演技のutilityをJS appとHOME背景の双方で利用できる工程を置く。現在HOMEから利用可能という意味ではなく、3体常駐や生活シミュレーション全体を実装する要求へ広げない。

実コードのHOME入口は `main/scene/scene.h` のscene_ops_t（prepare/draw/overlay）と `main/ui/shell.c` のscene table/背景frame解放。現行はCのstrip callbackであり、JS appのrealm/frameとは異なる。直接pocket名前空間を注入するだけで共有できると仮定しない。

現在のutilityは明示backendとevaluate(time)を取り、timer/入力/commitを所有しない。この分離を維持し、後段で時計、描画先、開始/停止、提示成功通知、資源のownerと寿命を環境adapterへ分ける。HOME用JS実行環境または共通評価器の方式、schema/handleの対応は実コード・メモリを確認して決める。

後段の受入は前景UIと入力応答、背景scene切替、app起動/復帰時の停止・再開・資源解放、JS/native総量と最大連続領域、strip描画予算を含む。app用ライフサイクルをHOMEへそのまま流用せず、既存scene_mem/背景解放の境界と照合する。今回はHOME実装を追加しない。

## hostのメモリ門（実機未測定）

統合中の作業ツリーで、実QuickJSとnative rendererを使う`test_character_app_host.c`のm32実行を確認した。`CHARACTER_HEAP_LIMIT=163840`、3モデル×6モード×301フレーム=5,418フレームで例外0・提示失敗0、`CHARACTER_NATIVE_PASS`。ログは`.cache/character-native-m32/cap.log`。compile_peak=96,880 B、live_max=162,036 B、追跡allocator_peak=143,864 B、host_pointer_bits=32。これらはハーネスの異なる計数であり、相互に足したりdevice総空きへ読み替えない。

shared prototypeと1本のbacking storeへの変更ではモデル部品を削っていない。typed work264 B/有形node・84 B/group・scene132 BだけでJSのlive最大を予測せず、typed view/object/closure/allocatorの実際の費用を合わせて検証した。live_maxと163,840 Bの差は1,804 Bしかなく、拡張の一般予算や実機の確保保証ではない。deviceのnative free/largest、最大単一alloc、初回frame確保とロード時の余裕は未測定のまま残す。
