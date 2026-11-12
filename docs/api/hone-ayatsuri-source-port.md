# Hone/Ayatsuri: editable Source の選択移植

状態: experimental、製品モデル未採用。基点は #125 を統合した
`c6aa17ef9686bb2f00bb8ffb6cc8fec83417d74e`。
旧 #110 の全体統合ではなく、保存された
`76796a5c5ae53505de7de4f79b8e205ea1dc9c19` の
`tools/ayatsuri/asset/reduce.cjs` を選択移植する。

## 意図と境界

Hone は低レベル描画、Ayatsuri はモデル・scene・motion の共通核とし、
編集可能な Source を正本に、IR → 制約への自動縮小 → native model へ進む。
過去の狐の手調整係数へ戻すことや、既存 scene/skin/lifetime を旧実装で置き換えることはしない。

今回追加するのは offline endpoint-QEM 縮小と試験だけ。
現 main の GLB importer、IR v1/v2、明示 sidedness、static scope、material mapping、
AYNM v2 compiler をそのまま使う。旧 pipeline の v1 専用解釈、arena 推計、
reader、workflow、アプリ登録は移植していない。runtime と既定モデルは変更しない。

`reduceSurface(surface, profile)` は surface 単位に動作し入力を変更しない。
4096 頂点/8192 面、collapse 数、triangle test 数を制限する。
境界点と指定 protected vertex を保護し、endpoint を選択するため位置・法線を再生成しない。
source vertex/triangle ID を保持する。material seam をまたいで surface を統合しない。
縮小不能・非多様体 edge・不整合 winding・不正入力は失敗する。
任意の self-intersection、非多様体 vertex 全般、skin/morph の意味保存は保証しない。

`sampleErrors` は双方向の頂点と面重心の距離/法線角を比較する。
連続 surface の最大誤差、意味的対応や最終 pixel 正しさを保証するものではない。
`partition` は旧 prototype の補助 API。現行 compile 経路の分割は
`compileStatic` 側が所有し、静的共有 scope と材料を呼出側が明示する。
`parts.origins.triangle` は縮小後 IR の三角形番号であり、原本との対応は
縮小結果の `sourceTriangleIds` を経由して合成する。元の三角形と同じ面を
表す保証ではなく collapse の lineage なので、画像 oracle の意味対応とは区別する。

## 再現

```sh
node tools/ayatsuri/asset/test_reduce.cjs
node tools/ayatsuri/asset/test_import.cjs
node tools/ayatsuri/asset/compact_opaque/test_batch.cjs
node tools/ayatsuri/model/test_pack_mesh.cjs
node tools/ayatsuri/model/test_sidedness.cjs
python3 tools/check_docs.py
```

人工 GLB の 81 → 49 頂点、2 parts / AYNM 2576 bytes を再現する。
平面の標本距離誤差は 1e-12 未満。人工曲面の標本距離最大は約 0.009324、
法線角最大約 3.353°。これらは試験形状の数値で、狐の品質・native heap・FPSではない。
決定性、入力不変、境界/protected 点保持、探索打切り、lineage不正、
sparse 属性/indices 拒否、現在の single/double-sided compilation を試験する。

## 次の工程と未受入

- Source/Blender export の固定と形状受入: silhouette → clay → fixed light、多視点/表情。
- 現行 reader/render に対する縮小前後・量子化後の独立画像比較。
- 現行資源 estimator と native/guest peak の実測。古い推計値を採用しない。
- hash 付き immutable candidate publication は旧 #110 を参照し別に移植する。
- Q7 は独立の optional review。実機は並行 QuickJS-NG 2 コア作業と競合させない。

人工 fixture PASS を Blender、狐造形、実 renderer、実機の受入と呼ばない。
私有モデル・Library 原本は公開 repository に追加しない。

## 第2工程: lineage と現行 native 平面画像の回帰

`source_lineage.cjs` は現行 compiler の `parts.origins` を縮小済み IR の
`sourceTriangleIds` と合成する。非連番 ID、分割を跨ぐ全 triangle、
未知 index と sparse origin の拒否を試験する。ID は collapse survivor の由来で、
変形した三角形の面上対応や隠面の正しさを証明しない。

```sh
python3 tools/ayatsuri/asset/verify_source_native.py --asan --output-dir .cache/source-native-new
```

出力先は新規 directory のみ。入力/実装 hash と compiler/flags、画像 hash を記録する。
実際の C `ayatsuri_model` が AYNM を読み、evaluate → 所有 snapshot への copy →
actor/arena/cold bytes 解放 → `ksn_mesh_render` の順で描画する。
全画像の 1 行/8 行 scratch 一致も検査する。

人工平面の原本81頂点と縮小49頂点は yaw -35/0/+35° の各 240×135 RGB565 が完全一致。
被覆数は 5200/6400/5200 pixel。正面は原本の既知の 80×80 正方形との独立被覆比較も行う。
1 byte 改変負例は比較に失敗する。host arena はこの fixture で 5988 → 3092 bytes、
snapshot record は 4 → 2。実機メモリや高速化の主張ではない。
ASan/UBSan を使用、実行環境の制約から LSan は明示的に無効として記録する。

これは現在の Model/mesh 経路に接続した**平面の差分回帰**であり、独立の shader/depth oracle ではない。
曲面の量子化後誤差・意味的対応・遮蔽、Blender/狐造形の品質、guest/実機の計測は未完。

## 第3工程: 非曖昧な交差面の独立 color/depth 検査

同じ host probe に、ambient=1・他の照明寄与=0 の赤/青2平面を追加した。
原本単位で `z = ±0.2 * (x - 0.0125)`、scale40 の正面投影なので、
交線は画素列120と121の間にある。期待画像は描画結果や native packet を読まず、
80≤x<160、27≤y<107 の正方形内で x≥121 を赤、それ以外を青とする解析式で作る。
原本、縮小後、提出順逆転の3条件すべてが期待RGB565と全pixel一致。
被覆pixelを誤った面の色に変える負例も検出する。
これは ambient 原色の限定oracleであり、一般材質shaderを独立実装したものではない。

調査では、交線を整数画素列120に置くと縮小前後のF32深度補間が数学上の等深線で
微小に異なり、この人工fixtureでは5pixelの面選択差が観測された。
この差を許容閾値へ埋め込んで合格にはせず、等深の曖昧な境界は今回の非曖昧caseから分離する。
縮小後に等深画素の先着面まで保存できるとは主張しない。一般曲面・遮蔽対応の工程で
数値条件を含めて別に検討する必要がある。

ここで depth の検査とは前面 surface の選択（visibility/order）の検査であり、
数値 depth buffer の誤差を計測したものではない。

## Normal角度 metric の修正

F32 normal の長さ丸めを角度と誤認しないよう、標本normalと比較先normalの両方を
単位化して `atan2(|cross|, dot)` で方向差を計算する。同一のF32対角normal同士で
従来は約0.0106°と報告し得た。修正後は同一方向と実0.1rad差を回帰試験する。
geometry・縮小選択・packed bytes・native描画は変更せず、報告する角度の精度を直す。
## 次段階の曲面・実Blender synthetic export 検査

```sh
node tools/ayatsuri/asset/test_reduced_packed.cjs
blender --background --factory-startup --python tools/ayatsuri/asset/blender_grid.py -- .cache/blender-grid-new
node tools/ayatsuri/asset/verify_blender_grid.cjs .cache/blender-grid-new
```

インストール済み Blender を使う任意host検査。自動install・私有asset・実機を使わない。
出力先は新規のみで、再編集できる `source.blend` と実GLB export、元頂点/面JSONを残す。
Blender4.3.2で81頂点/128面の人工曲面をexportし、Z-up→Y-up→Hone変換と
反射後のwindingを原本JSONから検証した。最大座標差は約2.98e-9原本単位。
原本座標/面向きの改変負例は拒否。exportされたnormalの独立検算はまだ行わない。
65頂点縮小の標本距離最大約0.006595、法線角最大約1.610°、3parts/3648bytes。
Blenderからの実export経路の証拠であって、任意Blenderシーンや狐造形の受入ではない。

別の人工曲面検査では、現compilerのAYNM byteをdescriptor/vertex/indexから再読込し、
原本→縮小、縮小→packed、原本→packedを分けて比較する。
81/65/49頂点candidateの原本→packed最大標本距離は順に約2.97e-9/0.006595/0.009324。
量子化単独の最大距離は3e-9未満。原本→packedの法線角最大は約1.32e-8/1.884/3.353°。
ゼロ誤差要求・頂点破損負例を拒否する。標本点集合は分割で複製されるため、
段階間の平均値は同じ母集団の比較ではない。連続面最大・pixel・遮蔽・狐品質は未認証。
