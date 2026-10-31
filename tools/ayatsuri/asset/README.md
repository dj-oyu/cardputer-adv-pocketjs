# Source asset pipeline prototype

[設計案](../../../docs/api/ayatsuri-asset-pipeline-draft.md)の最初の人工fixture用実装。
**狐の造形、Blender互換、最終描画品質を受入済みという意味ではない。**
製品API・既定モデルを変更せず、生成物は `.cache` 内の未採用candidateとして扱う。

```powershell
node tools/ayatsuri/asset/test_import.cjs
node tools/ayatsuri/asset/test_pipeline.cjs .cache/asset-pipeline/synthetic-v1
node tools/ayatsuri/asset/pipeline.cjs .cache/asset-pipeline/synthetic-v1/source.glb .cache/asset-pipeline/synthetic-v1/profile.json .cache/asset-pipeline/new-candidate
```

最後の出力directoryは存在しないものを指定する。既存candidateは上書きしない。
directoryを排他的に確保し、immutable filesを書き、全fileの長さ/SHA256を記した
`COMPLETE.json`を最後に書く。利用側は `readCandidate()` でmarkerと全hashを検証する。
途中でI/Oが失敗したdirectoryは保存するが、markerなし/不整合は利用不可。
Windows OneDriveでのdirectory rename失敗をsleep/retryで隠さず、このcommit境界を使う。
人工sourceは81頂点の平面gridで、GLBに加え編集可能なpositions/normals/indicesのJSONを保存する。
これは高自由度sourceの読み込み・縮小・分割・pack境界の試験であり、旧狐の係数調整を再開するものではない。
平面に加えて曲面gridの正例も生成する（`curved-*`）。source→最終packの観測距離最大
0.009324 source metres、normal角最大3.353°、9viewのsilhouette IoUは1。
この正例のprofileは距離.01/normal8°/IoU.97で事前指定し、厳しい平面profileでの曲面拒否負例も残す。
これらは人工曲面の縮小誤差であり、狐の造形や実rendererの品質ではない。

## 現在通る経路

`import_glb.cjs` はGLB2の静的single sceneをF64 JS-number IRへ取り込む。
world transformとglTF→Hone基底を一度bakeし、逆転置normalと反射のwindingを処理する。
default値・未対応拒否・byte/count/decode上限は `test_import.cjs` とexported DEFAULT_LIMITSを参照。
baseColorとPBR係数は原本情報として保持し、RGB565へ自動変換しない。
明示profileのmaterial ID→Hone係数/色mappingを必須とする。

`reduce.cjs` はendpoint QEM。面から計算したquadric誤差を最小化するedge collapse候補を
決定的なcost/vertex ID順で調べ、境界vertexを固定し、反転・縮退・重複・nonmanifold・link条件を検査する。
境界/材質seamを保護しても、任意形状のself-intersectionまで防ぐ実装ではない。
初版はmodel全体4096頂点/8192面以下、collapse回数とtriangle検査回数にも上限がある。
入力が大き過ぎる場合は明示FAILし、無制限に探索しない。wall timeoutで採用binを出さない。
自動法線再計算や手動係数補正で形状誤差を隠さず、選択endpointのsource normalを保持する。

`partition()` が32頂点/64面へ分割し、共有境界のposition/normalを変更せず複製する。
これはtopology-preserving最適partitionではなく、stable triangle順のgreedy prototype。
partitionによる固定workslot増加もprofile資源gateへ含める。
AYNMは既存 `pack_model.cjs` のcompileを使い、既存reader形式を拡張しない。

## 実装済みgateと未完

geometry/normal比較は双方向の全vertexと全face centroid sample。
nearest triangleへのbarycentric対応で距離とnormal角を測る。
sampleは等重み（面積重みではない）で、連続surface最大誤差を保証しない。
hard creaseや別surfaceをまたぐ正しい意味対応の比較は未実装なので、人工fixture以外の受入に使わない。
surfaceごとにshape/normal誤差gate、pack後のF32属性再読込gateを通す。
段階別誤差だけでなく、source→最終decodedの合成誤差も同じ閾値で検査する。
正射影silhouetteは整数pixel中心の全triangle coverageの和集合をsource/packedで比較する。
Float64・両面・epsilon付きbarycentric coverageであり、nativeのF32/backface/fill規則そのものではない。
pack後はdescriptorの頂点/面数とsection11のindex列も実byteから読み直して比較する。
これは正面face選択やdepthを解決する最終renderer oracleではない。

資源はcold、全face可視としたsegment上限、24 B/nodeを用いた保守的arena見積り、
**profile全view中のbbox pixel訪問最大**をgateする。view sampling外は保証しない。
scene単位のguest peak・snapshot/frame資源は実測が必要。
Modelのraw arena値は下記の実C probeと照合する。

```sh
python3 tools/ayatsuri/asset/verify_native.py .cache/asset-pipeline/synthetic-v1/candidate.aynm --asan --output .cache/asset-pipeline/synthetic-v1/native-64.json
# Only when a prepared 32-bit toolchain already exists:
M32_SYSROOT=/path/to/prepared/sysroot python3 tools/ayatsuri/asset/verify_native.py .cache/asset-pipeline/synthetic-v1/candidate.aynm --m32 --asan --output .cache/asset-pipeline/synthetic-v1/native-32.json
```

probeは本物のC reader/arena estimator/init/disposeを使用し、全truncated prefixとbad magic拒否、
失敗出力不変、cold-copy再読込を検査する。renderer・QuickJS・装置は含まない。
自動install/sysroot setupを行わない。C reader PASSを最終画像PASSと呼ばない。

`report.json` のstatusは常に `generated-experimental-not-accepted`。
Blender export損失、native最終RGB565/Gouraud色差、同semantic内のdepth/原本surface対応を含む遮蔽、
source自体の狐造形、guest peak、実機は未受入項目として明示される。
表情morph/skin/UVtexture/一般材質graph/両面指定など未対応を黙って除去せず拒否する。
実Blender原本によるexport/roundtrip受入を人工GLBの合格で代用しない。

## 次の最小工程

まず実Blender exportと編集可能な狐sourceの眼窩→頬→口吻を整える。
source自体の造形レビュー後、同じreduction profileでnative画像の色/depth/対応ゲートを追加する。
sourceを固定した比較で、原本・縮小・量子化・native描画の誤差を分離する。
形状が美術的に不合格なら、縮小誤差PASSでも製品候補へ採用しない。
