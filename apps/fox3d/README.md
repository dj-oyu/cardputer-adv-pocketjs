# FOX FACE: 狐3Dの顔の縦切り

`local.fox3d` / typed catalog名`FOX FACE`。画像パーツ版ではなく、解析的な3D頭・鼻口吻・顎・眼を使う造形試作。1キャラの完成や全身・尾・胸毛の完成を意味しない。既存CHARACTER LABは比較用に保持する。2.5D画像パーツ方式は3体の3D版の後続で、このappには含めない。

目標は既存の`fox-character-sheet-volume-muzzle-v2.png`（Libraryの元設定図、repoへ複製しない）。焼込み陰影を固有色へ取り込まず、橙/creamの固有色と世界光に応じる陰影・鼻眼のツヤ・薄い耳の透過近似を分ける。

## 操作と再現

- LEFT/RIGHT: yawを35度ずつ変更（−35..35度）。
- UP: neutral/smile/surprise/jawOpenを順送り。
- DOWN: 世界光だけを0/60/−60度へ移動。
- ENTER: 表情・姿勢・光・app tickを初期化。

正面/左右35度、pitch±10度が初回の検証範囲。大角度の横顔は対応済みとしない。app所有の`FoxFaceLab.select(expression,yawDegrees,pitchDegrees,lightYawDegrees)`はhost比較用の明示入力であり、frameworkの時計APIではない。範囲外入力は部分適用しない。`read(out)`は呼出元の出力へ書き、active中の再入を拒否する。

frameは入力→model更新→scene.evaluate→beginFrame→scene.render→commitの同期順。sleepや待ち時間に依存しない。顔方向を動かす試験と、顔を固定して光だけ動かす試験を分離する。ここでの固定表情の比較は、動的目標追従や生物的演技の完成を主張するものではない。

## 形状とclip

眼のglobe/iris/pupilは同じ3D開口を投影した凸clipを共用する。clipで白目が瞼の外へ漏れる範囲を制限しても、頭・頬による一般のdepth遮蔽は実装されない。耳の輪郭clipも解析曲面の切抜きであり、閉じた三角錐や一般meshを提供するものではない。最終画素で遠側眼・鼻口吻・頬・顎の前後関係を別に確認する。

utilityはoptionalな`apps/lib/ayatsuri_clip.js`。既存`character.js`のprivate packed layoutへ依存する実験adapterで、一般的なmesh/UV/skin/morph基盤の完成とはしない。実装と制約は[API文書](../../docs/api/ayatsuri-clip.md)を参照。

固定29nodeは[既存AYNM資産](assets/README.md)としてnative側が直接読み、個体Instanceの可変poseとJSの演技・開口指示を分ける。旧25nodeのIDを保ち、内耳・耳毛4nodeを追加した。形状/材質の大きなJS定義graphへ戻さず、同一Instance内の元の順序で提出する。native投影の読み取りも呼出元の固定bufferへ同期copyし、Cの可変pointerをJSへ貸し出さない。

全JSの25node版と、native7node/JS19nodeへ部分分離した版はいずれも163840Bのquotaを超えた（allocator拒否0）。後者を遅延生成にしても初frameの眼生成で失敗した。それを実機の物理heap不足や、画像bin一般の非有効性とは呼ばない。全25nodeを固定native資産にした版では、SourceA/blend JS moduleと固定定義graphの復元を省く。native arenaも同じguest capへ課金され、生bufferや描画snapshotがゼロになる方式ではない。

## host検証と未達

`python3 tools/kasane_contract/run_fox_app_host.py --m32 --capture-dir .cache/fox-capture`で実entry/chunksを実QuickJS/native rendererへ渡す。32bit profileでは163840B制限、通常GC、frameごとの強制GCなし。64bitの既定実行はASan/UBSan用で、同じメモリ量を意味しない。

4表情×3yawの12画像、pitch端点、連続yaw、光のみの移動、停止同一画像、無効入力の不変性、操作reset、incremental/full-reference、runtime解放を確認する。画像は**HOST SIMULATION**と表示し、実機captureや実機FPSへ読み替えない。試験結果は`.cache/fox-app-{m32,asan}/result.json`とログに保存する。

造形受入は試験のPASSと別。設定図に対する頭/耳/頬のシルエット、眼の突出、喜びと驚きの可読性、鼻口吻のdepth、接合や白目の漏れを画像で評価する。全身・尾/胸毛の柔らかさ、実機画質・実機時間/stackは後続の未完項目。

起動失敗も含む容量の比較条件と証拠の限界は[host検証記録](../../docs/perf/fox-native-face-study.md)を参照。

現在の造形は[顔v2](../../docs/perf/fox-face-v2.md)。app所有selectのlightYawは−180..180度へ広げ、左右150度の背面光を比較する。通常キーの光切替は0/60/−60度を維持する。hostは4表情×yawの12枚、24枚のpitch比較、6枚の背面光を含む237frameを2回実行し、capture指定時には42枚すべての長さと反復hashを確認する。

眼の方向補正は`model.update({viewYaw})`のカメラ相対yawから決まる。省略時は固定正面カメラを前提にbody yawを使う。新しいカメラ配置へ組み込む側は相対角を渡す。一般の自動depth遮蔽ではなく、対応角度内での作者による露出補正である。
