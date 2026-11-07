# 合成skinの独立scalar core

`main/ayatsuri/ayatsuri_skin.h` は、[scene/motion](scene-motion.md) の
rigid jointの次に必要なbind対応とweight変形を検査する、allocation-freeのC coreである。
このheader単体は独立数値coreであり、firmwareのcapabilityや既定appは変えない。
後続の[C描画接続](skin-render-bridge.md)がskin結果を既存Model/opaque rendererへ
同期借用で渡す。[managed JS skin評価](skin-js.md)も別sliceとして追加する。
scene facadeの自動skin制御とAYNM skin tableは未実装。公開の合成数値だけを使い、狐の資産は使わない。
描画成功・実機性能・実キャラクター完成を示す変更ではない。

## 入出力と所有権

- `ayatsuri_skin_palette(global, inverse_bind, joints, out)` は各jointの
  `global * inverse_bind` を作る。行優先3×4、列vector、binary32の保存とbinary64の
  中間計算は `ayatsuri_transform.h` と同じ。callerが親子TRSからglobalを先に作る。
  すべて同じmesh空間で渡し、mesh owner変換の二重適用を避ける。逆bindの算出、
  bind/restの照合やモデル座標系の変換をcoreは代行しない。
- `ayatsuri_skin_evaluate(palette, joints, vertices, count, out)` は最大4 influenceの
  linear blend skinning。各vertexの4個のU16 weightの和は**厳密に65535**とし、
  65535で割る。zero weightを含むすべてのjoint indexを検証する。未使用slotは
  weight=0と有効なindexを渡す。全zero・不正な和を正規化や剛体fallbackで隠さない。
  U16 indexによりjoint数は1..65536、vertex数は正でspan byte計算に収まる範囲。
  これらはデバイスに収容可能な数という意味ではない。
- inputは同期借用、outputはcaller所有。すべてのspanは有効・整列済みで呼出し中に
  変更しない。outputとinputの重複は拒否し、in-placeを許可しない。input同士の共有は
  可能。pointerの有効性や実際の確保容量はC callerの責任である。
- 全結果を一度検証してから二度目で書くため、失敗時にoutputの一部だけが更新される
  ことはない。既存の状態を保持せず、時刻・pause・seek・dirty管理もcaller所有。
  二度の計算は原子性のための選択で、速度の最適化ではない。

paletteは未参照jointも含めfiniteを要求する。position/normalもfinite、normalは
非zero（unitでなくてもよい）。不正入力は `AYATSURI_INVALID`、結果のbinary32範囲超過、
特異blendや法線計算不能は `AYATSURI_RANGE`。palette作成自体は特異行列を拒否せず、
使用vertexのblendが可逆であることを変形時に要求する。丸め規則とfast-math禁止は
transform coreと同じで、compilerの `-ffp-contract=off` が必要。

## 法線、反転、退化

positionはweightで混ぜたaffine行列で変形する。normalはその**混合後のlinear行列の
inverse-transpose**で変形して正規化する。非一様scaleやshearにposition用行列を
流用しない。負のdeterminantでも外向き法線を維持し、triangleのwindingは変更しない。
個々のjointが可逆でも混合が特異になる場合があり、そのbatch全体を拒否する。
近特異を経験的なepsilonで切り捨てる処理や姿勢の補正はない。

この法線はweightを局所的に一定とみなすaffine近似であり、weightが面上で変化する
実surfaceの微分や変形後triangleからの法線再計算ではない。joint別の逆転置法線を
weightで混ぜる方式とも異なる。LBS固有のねじり時の収縮、dual quaternion、tangent、
morph、表情、physicsは対象外。

## hostでの受入

```sh
python tools/ayatsuri/skin/run.py
python tools/ayatsuri/skin/run.py --sanitize
```

SDKや外部依存を使わず、独立した `build_skin_synthetic/` に生成する。
`CC=clang` や `--build-dir` でcompilerと出力先を変更できる。
このcloudのptrace環境ではLeakSanitizerが起動できないため、実行時のみ
`ASAN_OPTIONS=detect_leaks=0 python tools/ayatsuri/skin/run.py --sanitize` を使った。
これはAddressSanitizer/UBSanの検査であり、leak検査の成功とはしない。

検査はbind-pose identity、非自明な回転/移動を含む逆bind、既存TRSの親子global、
片jointのみの移動、1/2/4 influence、非一様scale、反転、shear、特異なweight blend、
未参照paletteのNaN、不正weight/index、nonfinite/zero normal、overflow、span重複、
不正なcount、subnormalの非zero法線、近特異だが可逆な行列、
異なる開始位置のspan重複とinverse bind/output重複、失敗後のoutput byte不変と回復を含む。120姿勢を往復する240回と各pauseを
同一入力のbyte一致で検証する。positionは「変形済みpointを混ぜる」別式と比較し、
normalは正determinantの合成平面について変形tangentの外積と独立比較する。
実rendererと同じ式を呼んだだけの画像一致試験ではないが、実surface全般の法線正解を
証明するものでもない。

hostでの `sizeof` はinput vertex 40 B、output vertex 24 B、palette joint 48 B。
従ってこれらのcaller配列は `64*vertices + 48*joints B`、globalとinverse bindも
保持する場合はさらに `96*joints B`。coreはheapを確保せず、固定サイズの局所workだけを
使う。これにmodel、JS、描画snapshot、allocator overhead、stack総量は含めない。
S3のRAM適合、stack watermark、cycles/FPSは未計測。

## 次の接続工程

AYNM v1のmeshはimmutable cold verticesのままである。後続の[C描画接続](skin-render-bridge.md)は
Cの一回の評価だけでskin結果を借用し、cold dataを変更しない。完全なasset/JS統合には
skin tableのwire契約と検証、instance所有の変形work、dirty/epochとrollback、
法線/shade/projectionの再評価、frame snapshot所有権、JS bindingとメモリ会計が必要。
別形式を暗黙追加したり毎frame Modelを作り直して接続済みとすることは避ける。
scene/motion・既存rendererの回帰検査、実機表示・ヒープ・性能検査はそれぞれ別の受入工程。
