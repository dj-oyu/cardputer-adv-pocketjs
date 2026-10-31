# 高自由度sourceからHone資産へのコンパイル設計案

状態: **制作方針は合意済み、以下の形式・アルゴリズム・閾値は設計案**。2026-10-31。
実装基準は `03f24827ae02421fc3d4bdd897425e5e9a19a43b`。
この文書の初版追加時点で、新importer・自動縮小器・Blender実行経路は未実装。
現在の最小実装範囲は、synthetic GLB→IR→endpoint QEM縮小→32/64 partition→既存AYNMと、
native reader/arena検証である。着手範囲と完了実績を区別し、実Blender取り込みと
native画像の品質ゲートを、この範囲への着手だけで達成済みとはしない。
既定FOX FACE / FOX SHEET、製品API、実機023559fを変更しない。

## 1. 制作入力と実行形式を分ける

ユーザーの決定は、高自由度の形状を原本とし、自動近似・縮小と誤差検証を経て
Hone用binを作ること、最終的にBlenderで作ったモデルを取り込めることである。
手動で多項式係数を調整して狐の造形を追い込む工程は終了する。
過去の係数生成・小mesh比較はrendererの比較資料として保存し、完成sourceとは扱わない。

```text
Blender .blend（制作原本、任意の高密度mesh/表情/材質）
  → 固定版Blender exporter / GLB（入力adapter）
  → 正規化IR + 原本対応表（高密度のまま、損失とunsupportedを列挙）
  → 自動縮小候補 + 誤差/資源評価
  → 制約内のmesh/解析形状 + 自動partition
  → 既存AYNM packer → C readerで再読込 → native画像/資源検証
  → 採用候補bin + report + reproducibility manifest
```

低層の32頂点・64面を制作側に押し付けない。近似式に出力を限定せず、
低密度mesh、複数patch、既存の楕円体/blendから品質と総費用で選ぶ。
初回は**静的mesh backend**を成立させる。近似式を採用する場合も、
既存形式にoffline tessellationするのか、新runtime evaluatorを追加するのかを区別する。
後者は別途ABI・資源・失敗契約のレビューが必要で、初回の前提にしない。

固定topology/形状/材質はnative共有Model、個体のpose・速度・可視性・許可された差分はInstance、
JS/C ayatsuriは演技目標と明示load/disposeを所有する。動的JS作成経路も残す。
全体mesh化、全体native化、汎用DCC、画像2.5D版、全面Z-bufferの実装は本工程の範囲外。

## 2. Blender取り込みと入力profile

第一adapter案は **glTF 2.0 GLB**。`.blend`を独自parseしない。
Blender本体の固定版と標準exporterでGLBを出す。自動化する場合はheadless Blenderから
同じadapterを呼ぶ。独自exporterが必要になっても、出口を同じIRにして縮小器を分岐させない。
Blender importerの完了条件には、実際の`.blend`→GLB→IR→binの通過を含める。
手書きGLBが読めることだけでは達成としない。

glTFは右手系で+Yが上。Blender exporterは三角形化やattribute境界での頂点分割を行い得る。
このためimport後の頂点番号をBlender編集頂点と同一視しない。
出典: [Khronos glTF 2.0仕様](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html)、
[Blender glTF exporter文書](https://docs.blender.org/manual/en/3.3/addons/import_export/scene_gltf2.html)。
後者は機能確認に使用した3.3版であり、採用Blender版を意味しない。
実装時に利用可能なBlender版・exporter設定を固定し、その版で再確認する。

最小profile `fox-static-v1` は選択した頭部mesh、階層、明示指定のopaque材質、
頂点normal、意味付けした眼/鼻/口/毛根anchorを入力とする。
sourceの高密度制限はruntimeと独立したoffline safety capとして設定する。
入力byte数、accessor/mesh/vertex/index数、展開後byte数、処理時間の上限をmanifestに必須化し、
上限なしに全体を展開しない。人工fixture開始用の暫定offline cap案はGLB64 MiB、
展開256 MiB、100万頂点/200万三角形、10000 nodesとする（製品側予算とは無関係）。
実sourceで必要性とparserの最大使用量を確認して固定する。時間上限も実装時に必須設定とし、
無制限の探索を行わず `SEARCH_BUDGET_EXCEEDED` で候補/reportを残す。

| 入力要素 | 初回compileの扱い |
| --- | --- |
| triangle mesh、固定TRS、opaque base color、頂点normal | 直接IRへ。出力精度は検証する |
| quad/ngon、curve、modifier | Blender側の明示的評価・三角形化。設定と原本対応を記録 |
| 負scale、非一様scale、固定matrix/shear | 静的geometryへbake。normalは逆転置、反射時のwinding変換を検証 |
| material graph、PBRのmetal/roughness等 | 明示mapping profileで近似できるものだけ。未指定・表現不能は拒否 |
| UV、画像texture、mesh alpha、glTFの光学的transmission/volume/refraction | IRには保持可能にするが現Hone meshへそのまま出力不可。初版は拒否 |
| skin、bone animation、shape key/morph | IRと対応情報の設計対象。初版runtime出力は拒否、黙ってrestだけにしない |
| armature/modifier等の一時刻の静的評価 | ユーザーが選んだstatic-bake profileだけ許可。失う可動性をreport |
| compression extension、未知required extension、外部network URI | 初版拒否。任意スクリプト・式を実行しない |

GLB chunk境界、bufferView/accessorのstride・offset・count・型・component・indexを
overflow-safeに検証する。sparse等は対応実装と試験がない限り拒否する。
NaN/Inf、ゼロ面積、範囲外index、未定義normal、階層cycleも拒否。
外部fileを後で許す場合は入力root内に制限し、absolute/親参照/path traversalを拒否する。
unsupported一覧はnode/primitive/material/attribute位置付きで返す。
既存Honeには厚みU8と薄さ/背面光の近似項がある。これはglTFの光学的transmission互換ではなく、
使用する場合は別の明示mapping profileとして検証する。

## 3. IR契約（案、実行ABIではない）

IRはsource由来の形状をF64で保持し、最終pack時のF32化と区別する。
座標単位はmetre、角度radian。sourceの単位補正・軸変換・asset→Hone scaleはmanifestに一度だけ記録する。
軸変換の初期案はglTFからHoneへ `C=diag(1,-1,1)`、
`p_hone=assetScale*C*p_gltf` とし、cameraの向きとは別に扱う。
反射を含むためtriangleのwindingを一度反転しnormalもCで写す。
親matrixは同じ基底で変換し、root scaleの適用位置を一か所に固定する。
採否はrendererの正面判定を含むbasis fixtureで確定し、
反射を伴う変換でnormalだけ反転したりwindingを二重反転したりしない。
sourceとreducedの誤差比較には同じ変換・cameraを用い、各候補を再fitして誤差を隠さない。

```text
SceneIR {
  version, sourceHash, adapterVersion, exportSettings, basis, metresPerUnit,
  nodes: {stableId, sourcePath, parentId, restTransform, partId}[],
  surfaces: {stableId, positions, cornerNormals, triangles,
             cornerUVs?, materialIds, protectedEdges, anchors,
             morphTargets?, sourceCorrespondence}[],
  materials: {stableId, sourceProperties, explicitHoneMapping?}[],
  deformationDomain, supportedViewDomain, diagnostics
}
```

normal/UV/materialの不連続はcorner attributeで保持し、同じpositionでも安易にweldしない。
日本語を含むsource名は保持し、AYNM用一意ASCII名とstable IDの対応表を生成する。
左右対称は要求しない。rest形状と変形delta、camera依存correctiveは別の項目にする。
眼窩・瞼loop・目頭目尻・口角・顎支点・鼻口吻接合・頬毛根・意図したcreaseを
保護featureとして入力できる。compilerは生成meshへのbarycentric対応/複製対応を出力する。
asset内の前後順/clip/attachmentの意味をmaterial IDへ押し込まない。

将来morphでは全targetに同じcollapse mapと共有境界を適用する。
表情ごとに別々に最適化してindex対応を失う経路は採らない。
restだけでなくblink/smile/surprise/jawOpenと指定組合せで、反転・縮退・接合・normal・clipを検証する。
対象domain外の任意組合せは保証しない。初版でmorphを実行できるとは記載しない。

## 4. 自動縮小・表現選択

初回候補はconstrained edge-collapse/QEM系の静的mesh縮小。
形状距離の二乗和だけでなく、normal・材質境界・保護feature・投影輪郭の誤差を制約にする。
QEM目的関数が小さいこと自体を品質PASSとしない。独立のsource比較器を最終判定に使う。
固定のcandidate順、edge tie-break、seed、演算精度を規定する。
上限へ達するまで無条件にcollapseせず、許容誤差を越える操作は拒否する。

1. source IRを検証し、未対応と保護境界を確定。
2. 複数の縮小率で候補生成。meshはcomponent/材質seam/attachmentを維持。
3. view/表情domainで誤差と遮蔽を検証。
4. 出力patchへ自動分割し、複製vertex・固定slot費用を含め資源を再評価。
5. pack→C再読込→native描画でF32/RGB565/分割による追加損失を再測定。
6. 品質と予算を両方満たす候補のうち、総費用が小さいものを選ぶ。

解析形状fitは後続候補。係数はsourceからsolverで生成し、手動調整を制作条件にしない。
式の残差・輪郭・normalを同じゲートへ通し、meshより費用が大きい/品質不足なら選ばない。
複数patchの境界position/normal/material値は共通sourceから作る。
partitionごとにnormalを再平均して継ぎ目を作らない。材質の固有色と照明は分離し、
模様をnormal/light由来で移動させたり、固定陰影をalbedoへ焼き込んで代用したりしない。
初回material mappingは明示したopaque単色primitiveを基本とし、境界vertexを複製しても
位置と滑らかなnormalの対応を維持する。二色maskへまとめる候補は境界誤差ゲートへ通す。
glTFのbase colorの色空間とHoneのRGB565値を同一と仮定せず、mappingの変換・量子化順を固定する。
任意の頂点色やmaterial graphを現meshの二色maskだけで再現できるとは扱わない。

## 5. 現行出力の制約と遮蔽

次は基準headで**実装済みの上限**。新pipelineの希望値ではない。
根拠: [native mesh仕様](ayatsuri-native-mesh.md)、
`tools/ayatsuri/model/pack_model.cjs`、`main/ayatsuri/ayatsuri_model.c`、
`main/ui/kasane/ksn_procedural.c`。

| 項目 | 現行契約 |
| --- | --- |
| model | 1～256 nodes、parent先行、名前一意ASCII 1～127 bytes |
| cold | JS packer最大512 KiB、C reader最大1 MiB。小さい方をcompiler上限にする |
| 各mesh | 3～32 vertices、1～64 triangles、hole最大2個・各3～8点 |
| attribute | finite F32、XYZ/mask ±4096、unit normal、local U16 index |
| 色/陰影 | RGB565二色+scalar mask、Gouraud。mesh UV/alpha/skin/vertex morph未対応 |
| visibility | mesh内centroid-depthはheuristic、mesh間はnode/packet順、一般Z-bufferなし |
| 更新 | mesh TRS/visibility。材質変更等の非対応操作は拒否 |

32/64に入るだけでは成功でない。交差patch、循環する前後順、三角形内で切り替わるdepthは
重心sortで一般には解決しない。sourceのnearest-depth/semantic-ID oracleとnativeの最終画像を比較する。
問題が出る場合は局所分割・明示clip・可動域制限を候補とし、費用込みで再検証する。
解けなければ `OCCLUSION_UNSUPPORTED` で失敗。都合の悪いfaceを黙って消さない。
offline比較器にZ-bufferを使っても、製品側に実装されたことにはならない。

対応viewは初回yaw ±35° / pitch ±10°を候補とする。正射影の現runtimeに合わせ、
遠近投影で作った評価画像をnativeの正射影と直接比較しない。
samplingした角度で合うことと連続した全域の保証は区別し、境界付近・遮蔽変化点は細分する。

## 6. 総資源予算と失敗契約

rendererの上限とは別に、model cold / instance arena / 全guest peak / native frame / flash / 描画費用の
profileを入力必須とする。単に面数を最小化せず、分割で増える固定費とoverdrawも評価する。
現行実装の費用式は次の通り（将来変更時はC estimatorを正とする）。

```text
meshSlot(V) = align8(84 + 1040 + 12*V)
instanceBytes = align8(sizeof(model_node_state)*N) + sum(meshSlot and other slots)
meshSegments = 1 + ceil((8 + 16*V + 6*Tvisible + 68*H) / 10)
frameSegments <= app resource maxSegments <= 1024
sum(screen-clipped triangle bbox pixel area) <= 65535
```

arenaは `ayatsuri_model_instance_bytes()` でpack済み資産から実測する。
packet上限は可視face最多のviewでも検査する。bbox評価pixel/part×band訪問/overdrawもreportする。
assetファイルのbyte数が160 KiB guest capにそのまま加算されるとは限らず、static flash直読と
owned copyを分ける。Instanceはnativeでも同guest allocator/capに会計される。

初回は既存profileの160 KiB（163840 B）を維持する。cold/arena/flashの新しい総上限は
実sourceと同head測定後に選ぶため未決。未記入なら採用判定を出さない。
参考として03f2482の旧形状試験はcold11912 B、arena27436 B、m32 guest peak91604 Bだったが、
新sourceの保証や全アプリ共通予算へ転用しない。
二個体共有、load/createの一時peak、JS演技/動的state、最大単一確保、candidate/committed frame、
BSS/stack、linked flash、dispose/recreateを別々に測る。host会計残量をESP空き連続heapと呼ばない。

Modelはimmutable、Instanceは個体別。publish snapshot/candidate rollbackは現行を維持する。
load/検証/確保の失敗で現在のcommitted表示を壊さない。全target検証後に更新する。
共有中のModelは保護し、最後のInstance退役と明示closeで不要RAMを回収する。
描画packetのsnapshotがcoldを参照しない場合に架空のframe待ちを追加しない。
bin生成は一時出力へ行い、全ゲート成功後に完成manifestを公開する。
失敗candidateはreportとして保存し、既存採用binを上書きしない。

## 7. 誤差と受入（数値は未実測の初期提案）

原画はsource自身の造形レビューに使用する。設定図はcamera未校正・投影不整合を含むため、
原画対縮小meshを定量縮小誤差と呼ばない。
比較はsource→reduced→packed decode→nativeを分離し、camera・scale・pose・seedを固定する。
image resizeはしない。240×135のnative pixelで評価し、拡大nearest画像は説明用とする。

| 指標 | 初期閾値案、実測前にprofileへ固定 |
| --- | --- |
| silhouette | IoU ≥ .97、双方向輪郭距離p95 ≤1 px / max ≤2 px |
| 眼端・眼中心・鼻先・口角・顎・毛根 | 投影RMS ≤.5 px / max ≤1 px、左右/近遠側を分離 |
| surface distance | source顔幅で正規化した双方向距離p95 ≤.5% / max ≤1.5% |
| 保護眼窩/鼻口吻surface | max ≤.5% face width |
| normal | 平均角度 ≤3° / p95 ≤8° / max ≤20°、指定creaseは別検査 |
| material boundary | p95 ≤.5 px / max ≤1 px、領域欠落/混同0 |
| 遮蔽 | protected eye/lid/muzzleの非tie semantic-ID誤り0 px、全体誤り≤.1% face pixels |
| 同semantic内の可視depth | source顔幅で正規化した絶対view-depth差p95 ≤.5% / max ≤1.5%、保護域max ≤.5% |
| 最終RGB565出力 | 同対応画素の各R5/G6/B5成分差: 平均 ≤.5 level / p95 ≤1 level / max ≤2 levels |

geometry近接tieの判定epsilonはscene scaleからprofileに明示し、誤りpixelを恣意的に除外しない。
毛先消失・薄い接続・穴の閉塞はIoU平均で隠れ得るためsemantic region別の最大誤差も出す。
指定crease・材質境界と滑らかな法線の継ぎ目を区別する。
距離の連続最大を証明できない近似samplingは「観測最大」と記す。
閾値超過時はFAIL。結果を見て黙って閾値/asset上限を緩めない。

### 7.1 同じsemantic-ID内の前後誤りも検出する

semantic-IDの一致だけでは、同じ頭部の裏側・別の折返し面を誤って描いた場合を検出できない。
sourceの画素中心rayに対する最前面について、semantic-IDに加えsource triangle ID、barycentric位置、
view-depth、可視normalを保存する。reducedにも最前面のdepthと、縮小対応表から得るsource側の
surface対応を保存する。両者の同ID可視画素で符号付きdepth差と絶対差を計測し、
近すぎる面と遠すぎる面の両方を検出する。単に同semantic内のどこかの最寄点を探して
隠れた面へ対応を付け、可視面の誤りを小さい距離へ置き換えない。

sourceのみ/reducedのみの画素や異ID画素は、このdepth統計から外れてもsilhouette/遮蔽ゲートには
必ず残す。縮小でtriangle番号が変わるため番号一致は要求せず、原本対応と可視surfaceの距離を
別々に記録する。rayが同IDの複数面と交差する負例を用意し、同IDでも前後を誤ればFAILすることを
確認する。tie epsilonは投影・scale・F32量子化から事前固定し、depth差の上限より小さくする。
保護域の内部対応を定義できないprofileは、depthゲートを成功とせず未検証とする。

### 7.2 色空間・量子化・陰影誤差を別に測る

最終表示の初期合否基準は、表の**RGB565成分levelの絶対差**を候補とする。
R/Bは0..31、Gは0..63で測り、8bit化した見かけの差やreference値で割った相対誤差へ置き換えない。
これにより黒で除算不能になったり、白と黒で違う許容率を暗黙適用したりしない。
この閾値も未実測の提案であり、黒・白・中間灰・低輝度の彩色・強いhighlight・材質境界のfixtureで
固定してから候補を評価する。source/reducedで可視IDが一致する画素を対象とし、除外された
境界/欠落画素は7.1と既存のsilhouette/遮蔽ゲートで独立に落とす。

linear RGBでのRMSE/最大差は補助指標として出せるが、glTFのlinear base color、Honeへ渡す値、
rendererの計算色空間、transfer function、clampとRGB565丸めの順をprofileに明記した場合に限る。
RGB565を展開した値を無条件にlinear光量と呼ばない。linear RGBの小さい平均誤差を理由に、
最終RGB565での境界・highlight・暗部の誤差を合格にしない。

同じHone材質mapping・camera・光で、高自由度sourceとreducedを別々に評価して、
Gouraud補間による陰影損失を測る。さらにreducedのpack前、F32/RGB565化したdecode後、
実native描画を分け、partition境界の複製、補間、丸め、描画順による追加差を報告する。
shapeを補うためのnormal変更・albedoへの固定陰影焼込みは、形状誤差の解消とは扱わない。
明示した材質近似そのものの損失と、縮小による損失も別に記録する。

### 7.3 Blender export損失と縮小損失を分離する

比較は `.blend`の評価済みsource→GLB/IR、GLB/IR→reduced、reduced→packed decode→nativeの
段階ごとに行う。exportの三角形化、modifier/static-bake、normal/attribute分割、材質mappingで
失うものをexport reportへ出し、その損失を縮小器の誤差に混ぜない。
原本・export設定・各中間物のhashと対応表を保持する。

synthetic GLB→IRのidentity対照が通っても、Blender→GLBが損失なく通った証明ではない。
実Blender/exporterからの比較が未実施ならexport損失は未測定とし、縮小側の数値が良好でも
Blender import完了を宣言しない。実native画像の比較がなければ最終RGB565/Gouraudゲートも未検証である。

最小view集合はyaw 0/±15/±35 × pitch 0/±10、別の中間角をholdoutにする。
unlit silhouette、gray clay、normal可視化、albedo-only/semantic-ID、固定世界光3方向を出す。
固定光でposeだけ/固定poseで光だけを変える。光の差を材質境界やshape誤差へ混ぜない。
将来morphは各endpointと指定複合域の検証を追加する。restのみ成功ならrest対応と記す。

## 8. 最小縦断試作の順序と着手用interface案

```text
blender -b fox-source.blend --python export_hone_glb.py -- export.json
python tools/ayatsuri/asset/import_glb.py source.glb --profile fox-static.json --out scene.ir
python tools/ayatsuri/asset/reduce.py scene.ir --profile fox-static.json --out candidate/
node tools/ayatsuri/model/pack_model.cjs ...  # 既存compile(manifest)をadapterから呼ぶ
python tools/ayatsuri/asset/verify.py source.ir candidate/ --native --out report/
```

**上記は提案interfaceで、既存コマンドとして実行してはならない。**
新規実装は `tools/ayatsuri/asset/` に限定し、runtime ABIや既定狐を初段で変更しない。

| 段階 | 小実装と合格条件 |
| --- | --- |
| P0 入力準備 | 実Blender sourceの頭部、眼窩/頬/口吻を共有する面、材質境界/anchorを制作。原画との正面/斜め形状を先にレビュー |
| P1 adapter | 固定Blender版の実export→GLB→IR。人工fixtureで軸/normal/反射/材質seam/範囲外accessor/unsupported拒否を試験。原本対応を確認 |
| P2 縮小 | 同一sourceを最低2予算で自動edge collapse、保護featureと対応表維持。縮小しないidentity対照と既知劣化のFAIL対照も通す |
| P3 pack | 自動32/64 partition→既存compile→C reader→native画像。F32/RGB565/境界複製/遮蔽/全資源ゲートを閉じる |
| P4 狐比較 | 原画/source/旧023/新source縮小を同条件で提示。source造形合格と縮小誤差合格を別々に判定 |
| P5 製品候補 | m32 160 KiB、OOM/寿命/二個体共有/recreate/旧packet回帰/製品link/同head CI。実機は別候補として担当へ引渡す |

最初の実sourceは顔面の静的neutralだけに絞る。眼窩→頬→口吻の面と固有色境界を
縮小しても保持できることを確認し、次に笑顔等へ進む。人工fixtureを狐造形完成と呼ばない。
式fit、PIE、texture/skin/morph runtime追加、全身、2.5Dには初段を依存させない。

実装内部案は `import_glb(bytes, profile) -> SceneIR`、
`reduce(ir, constraints, budgets) -> Candidate + Correspondence`、
`partition(candidate, backend_limits) -> PackedManifest`、
`verify(source, decoded, domain) -> Metrics + Failures` とする。
各関数は入力を破壊せず、failed resultに採用可能binを付けない。
stable ID順に決定的に出力し、集約順・同値edge選択・乱数seedを固定する。
同一toolchain/profile/sourceから2回生成してbyte一致を試験する。
異なるOS/版のbit一致は未保証とし、版差はmanifestに残す。

reportはsource/GLB/IR/outputのSHA256、Blender/exporter/compiler版と設定、seed、
許可view/表情域、unsupportedとbake損失、近似候補の採否理由、誤差分布/最大位置、
頂点/面/複製/patch数、cold/arena/packet推定と同head実測、画像/diffを必須とする。
成功判定は `generated` / `reduction_verified` / `source_art_accepted` /
`host_runtime_verified` / `device_verified` に分ける。bin生成成功だけで完成扱いしない。

## 9. 現在の不足と次の判断

このcheckoutには利用可能な`.blend`/GLBの高自由度狐sourceは見つかっていない。
設定PNGは存在し実見済みだが、3D原本の代わりにはならない。
限定した読み取り探索では、PATH、Program Files/Program Files (x86)/ユーザーProgramsの
Blender名directoryと既知Blender Foundation場所、HKCU/HKLM/WOW6432のuninstall登録に
Blenderを発見できなかった。task-2内のcheckout（.git/node_modules/build/cacheを除く）と
親directoryの深さ2、既知cacheの深さ3までの`.blend`/`.blend1`/GLB/glTF、対象名のOBJにも
高自由度狐原本を発見していない。cacheのOBJはfox/character/source/model/mesh名を深さ2まで
調べた限定結果であり、全ディスクや全cacheに存在しないとの断定ではない。
Blender起動・install・実exportは未実行。
インストールや装置操作をせず、まずP1のparser/basis人工fixtureとP0のsource制作を分けて進められる。
現最小実装のsynthetic GLB、endpoint QEM、partition、AYNM reader/arenaの検証結果は個別に記録し、
実Blender export、最終native画像、同headの全資源/実機ゲートへ流用しない。

次に固定するのは、(1) 実sourceとBlender版/export設定、(2) 最小opaque材質mapping、
(3) 同sourceに適用する品質閾値と総cold/arena/packet予算である。
数値閾値はここでは提案、既存renderer制約とBlender import必須は前提として区別する。
旧係数試験の結果はsource品質や本pipeline実装済みの証拠へ転用しない。

## 10. 人工fixtureの初回実装（仕様全体の受入ではない）

後続作業で[試作コードと実行手順](../../tools/ayatsuri/asset/README.md)を追加した。
GLB2→IR、境界固定endpoint QEM、決定的32/64分割、既存AYNM compile、
sampled geometry/normal・silhouetteと資源gate、C reader/arena probeを実装。
未知attribute/optional extension、両面指定、nontriangle、skin/morph等は初版では拒否する。
glTFのmode既定はTRIANGLES、alpha既定はOPAQUE、doubleSided既定はfalseとして扱う。
nonmanifold辺と共有辺の不整合windingを縮小前に拒否する。

初期入力は編集可能なmesh配列JSONと同じ人工GLBの平面gridで、狐原本ではない。
81頂点/128面→49頂点/64面→2mesh、cold2584 B、segment上限134。
64bit ASan/UBSanの実C probeでarena3092 B（node state24 B）、全2584切断prefix拒否、
初期pose/再読込/dispose成功を確認した。これはQuickJS/実機のメモリpeakではない。
ローカルm32は既存 `/tmp/m32sys` の準備条件不足でcompile前に停止し、未測定。
新しいsoftwareやsysrootは導入していない。

距離/normalはsourceとreduced双方の全vertex・全face centroidを等重みsampleし、
nearest triangleのbarycentric normalと比較する。面積重みや意味対応を持つ連続最大ではない。
source→最終F32 decodedまでの合成誤差も検査する。
silhouetteは全triangle和集合の整数pixel sampleで、nativeの背面選択/遮蔽を証明しない。
資源は指定全viewの最大訪問数で判定する。決定的なcollapse数/triangle検査数を上限とし、
timeoutや予算超過で採用binは出さない。

CLIは入力GLB/profileを上限付きで読み、生成先を排他的に確保する。
全出力hash付きCOMPLETE manifestを最後に生成し、未完/不整合候補をreaderが拒否する。
既存出力を上書きせず、失敗途中のcandidateを製品へ登録しない。
statusは常に `generated-experimental-not-accepted` とし、実Blender export損失、
native最終色/depth/原本対応、狐造形、guest peak、実機は未検証のまま残す。
これらが未完であるため、人工fixtureの数値PASSを縮小器一般や狐の品質合格へ拡大しない。
