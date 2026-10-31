# 同一曲面の固有色領域: 最小host比較

基準`b93ae694c53a439ca20e103ca47166b26153b870`の狐v4。前の片側A/Bでは白い頬volumeを上へ増量しても球に見え、眼開口を細めると半眼になったため不採用とした。今回は[localPlane仕様](../api/material-regions.md)で、同じellipsoidの形状/法線と固有色を分離する。狐の既定assetはv4を維持し、試作はホストfixtureだけで差し替える。

## 1面の比較

[9枚の比較](../apps/material-region-surface-host.png)は単色/既存UV band/平面hard/feather/光だけ/姿勢/非一様scale/spec-only旧/新。同じ面で斜めの色境界を作り、別球の陰影を足さずにfeatherできることを確認した。[61連番のAPNG](../apps/material-region-motion-host.png)は固定世界光でbody yaw −35→+35°を動かす1回再生の診断sweep。30 samples/sは再生設定で、実機FPSではない。

全て240×135の**HOST SIMULATION**を2倍表示し、実機captureではない。模様はbody-local qに固定され、光だけでは境界が動かない。shade前の色を混合し、normalとspecを1回評価する。spec-only旧/新の全画素は一致した。RGB565量子化や低解像度の境界品質を全条件で保証するものではなく、LCDの動き/画質は未受入。

WindowsとLinuxのO0/O2で実rendererを確認。基準C rendererへ同じ旧mode0..3の20packetを渡したfull-pixel FNV32は`c66daae5`で、新版も一致した（このfixtureとcompiler条件での回帰証拠）。unit sphereの別計算oracleでhard境界の位置、回転/非一様scale、光不変、全画面/8行帯一致を検査。境界端は定色へ直接一致させ、A=1/B=18の赤成分・ambient=1/36で補間端点の1ulpが最終丸めを変える負例も回帰に残した。

## 片側狐へ接続

同じcamera/lightと4表情×yaw±35/0°、pitch±10°を比較した。

- [mask-only](../apps/fox-local-plane-mask-host.png): 既存の頭のsurfaceに斜めの白領域だけ追加。白面は連続するが、前に残るcream頬と二重曲面/白帯が目立つ。
- [mask-inset](../apps/fox-local-plane-inset-host.png): 同じmaskのまま左頬3lobeを頭内部へ退かせ、白い頭面を見せる。眼下の連続面は改善方向だが、口吻の丸い塊・対側白帯・口/顎の接合・眼窩構成は未達。[pitch/逆光](../apps/fox-local-plane-inset-aux.png)も確認した。

領域は頭ellipsoidのみ。cream-face blend自体を複数材質化したわけではない。独立画像レビューも、機能は有望だが狐造形は未達と判定した。既定モデルへ試作を採用せず、29node/8開口/12clip対象/Instance arena9192Bを維持した比較fixtureを残す。

## 費用と検証境界

mask使用時も184B packet/20entryは不変。追加inverse/atan2/normal/shade/全画面bufferは不要。hitごとのdot3乗算+2加算、offset減算、feather時の事前計算逆数との乗算・加算・clamp、中間色の3成分lerpが机上の追加。端点は直接定色を選ぶ。C loopの動的確保は追加していないが、アプリ全体のno-allocationや実機の高速化は主張しない。

| 32bit host、通常GC、cap163840B | v4基準 | material fixture |
| --- | ---: | ---: |
| 丸め後要求peak B | 138592 | 138600 |
| QuickJS会計peak B | 155104 | 155112 |
| 最大単一raw要求 B | 9199 | 9199 |
| Instance arena B | 9192 | 9192 |
| 固定blob B | 3620 | 3672 |

Modelのsection descriptorが8B増え、guest会計もこのprofileで8B増えた。blobの+52Bはcold静的データで、guestの増分へ加算しない。既定のfox/cloud blobはbyte不変。会計capとの差8728BはESP free/largestや次の確保の保証ではない。ESP native heap/BSS/stack、GC時間、描画時間分布はこのhost試験から取得していない。

material fixtureは249frame×2、48画像反復一致、全描画reference、各6回のrelease/recreate current同値、拒否0、終了guest/live0を検査する。48枚は従来42枚にsmile×3yaw×逆光±150°の6枚を追加したもの。通常fixtureの243frame/42画像と件数を混同しない。ASan/UBSan64bitも別profileで確認し、メモリ量を32bitへ換算しない。

native sectionは全件検証後publish、旧bin受理、896箇所の切詰め/不正wire、2instance共有・色A overrideと固定B・失敗時出力不変を確認。JSはnormal入力の所有、光だけでprojection更新なし、pose追従、不正作成の非公開、UV変形の明示拒否を確認。描画側は凸clipとの共用、material field変更のdamage、転送失敗repairを既存snapshot経路で検査する。

## 再現

### 未使用機能の常駐費用で見つかった回帰

最初の`de42c1a`はcloudprobeの160KiB診断fixtureで失敗した。アプリが例外を捕捉するため、uncaught例外0だけでは検出できない。host側のO(1)canaryで、presented=297、first_req=37B、first_used=163812B、cap163840B、quota拒否6/物理allocator拒否0を観測した。終了時のfreeだけではこの一時拒否を説明できない。

同じm32 binary（SHA256 `6ecb71655010fdf8c00817feaa9e3a321ab9e1b7e410bd943893e6f819dd38c9`）、同じchunks/seed/cap/通常GCでSourceAだけLF原本を差し替えた対照では、新版23153BはFAIL、基準b93の22126BはPASSだった。元の作業ファイルはfinallyで完全復元した。未使用localPlane parser/encoderの常駐を小さいoptional moduleへ分離し、baseには必要時の委譲だけを残した。native固定資産はhelperを使わない。

部分分離後はfixture2回とも297frameで完了、digest `e12e1890`、pause15、旧画像維持/通常復帰、全描画reference、終了guest0、捕捉quota/allocator拒否0でPASS。部分分離版の観測live159252B/丸め後要求peak146640Bはこのcloudprobe profileの値で、上の狐の会計peakと混ぜない。cap増加・frame毎強制GC・sleep調整・試験閾値緩和は行わない。host gateは捕捉されたOOMも明示的に拒否するようにした。

ただし部分分離でも共通SourceAに462Bの追加コードが残り、既存A/C DEVICE_SHORT比較のA側がframe48でquota拒否した（first_req328B/first_used163732B、allocator拒否0）。C側63draw×2は通り、A側が途中終了したため、この結果を画素不一致の証明とは呼ばない。最終構成では共通`character.js`をb93の22126Bとbyte一致へ戻し、材質を使う場合だけ`AyatsuriLocalPlane.scene(options)`を選ぶ。既存scene factoryをglobalに差し替えず、未使用機能の追加bytecodeを既存appへ常駐させない。native固定assetの利用方法は変わらない。

最終構成のDEVICE_SHORT比較は同じm32 binaryでA/Cとも63draw×2を完了し、126 frame rows、全描画reference、画像列一致、入力不変をPASSした。共通SourceAは基準b93とSHA256 `31287e30e968093042c5f5d5af2b33274d1532ae1e49d8a97c53d7f84bd22629` が一致する。

新機能を使わないappへ追加bytecodeを常駐させないことも、今回の性能受入に含める。通常factoryでの未対応patternの明示拒否と、namespace削除後も追加factoryで作成済みsceneが同じencoderを使うことをJS試験へ追加した。

load前後と各frameのcanaryは同じsticky flagへ合流する。loadの観測処理がcounterを消費しても拒否を見失わない。private負例では、同じm32 binaryでutility loadへ捕捉される200000Bの確保要求だけを挿入し、演技がdigest一致で完了してもgateがFAILすることを確認した。作業ソースはfinallyで完全復元し、cap/GC条件を変更していない。この負例コードを製品へ含めない。

```sh
node tools/character/test_material_region.cjs
python3 tools/character/test_material_study_inputs.py
python3 tools/kasane_contract/run_proc_ellipsoid.py
python3 tools/ayatsuri/model/run_model_contracts.py
python3 tools/kasane_contract/run_fox_app_host.py --material-study mask-inset --capture-dir .cache/material-asan
python3 tools/kasane_contract/run_fox_app_host.py --m32 --material-study mask-inset --capture-dir .cache/material-m32
```

`--material-study mask-only|mask-inset`はoffline generatorからcold fixtureを作り、host linkのprovider wrapperだけで差し替える。製品assetを書き換えず、同じJS app/native validation/Instance/snapshotを通る。入力hashは生成inc/bin/json/manifestに加え、generatorが実際に読むSourceA/blend原本まで含める。生成後の原本変更を検出する負例がある。結果は`.cache/fox-app-{m32,asan}-mask-inset/result.json`へ、compiler/flags/binary/source/画像hash・phaseを保存する。

2平面の積集合、材質ごとのroughness/厚み、任意depth/occlusionは未実装。同一面の毛色を分ける機能と、狐全体の完成・実機受入を分けて扱う。

## 次段階: blend共通面の1平面

ellipsoidの小実証に続き、既存cream-faceの最終blend depth上でcommon model-local平面を評価する新variantを追加した。[wire/単位/型の仕様](../api/material-regions.md#blendの共通面-新variant)を参照。lobeごとの単位球qで毛色を選ばず、同じnormalでalbedoだけを分ける。

[同条件の片側比較](../apps/fox-blend-region-host.png)はHOST SIMULATION。列はv4基準／形状不変の共通平面／同平面と左外端lobe1個削除。正面neutral/smileとyaw±35°、固定光の同じframeを並べた。平面だけの案はgeometry/materialのsections1..7がbyte一致。別のinset案は左3lobesの位置・大きさも変えるため、色分け単独の効果と混ぜない。既定の製品fox assetは変更していない。

| m32、cap163840B、通常GC、同249frame×2/48画像 | v4同workload | planeのみ | plane＋inset | plane＋外端1lobe削除 |
| --- | ---: | ---: | ---: | ---: |
| 丸め後要求peak B | 138600 | 138624 | 138624 | 138516 |
| QuickJS会計peak B | 155112 | 155136 | 155136 | 155028 |
| 最大単一raw要求 B | 9199 | 9223 | 9223 | 9115 |
| Instance arena B | 9192 | 9216 | 9216 | 9108 |
| cream-face lobe数 | 8 | 8 | 8 | 7 |

各profileで全描画reference、2runの48画像一致、入力不変、6回のrelease/recreate plateau、quota/allocator拒否0、終了guest0を確認。ASan/UBSanのplaneのみ案は別64bit profileで同じ249frame×2を確認し、そのRAM値を32bitへ換算しない。F32/byteの比較と見た目の受入は別である。

planeは使用nodeのarenaを24B増やす。pruned案は左外端lobe1個の108Bを削除し、差引84B減となった。これは同じhost workloadのguest会計であり、ESPのfree/largest、断片化、GC時間、実機の描画速度は未計測。固定blobやflashの増分をguest peakへ足さない。

形状を保ったplane案では、上/外側の白領域をfur色へ変えられるが、白い横帯と眼の貼付感は残る。pruned案は左端の白い張り出しを短くできる一方、主頬の横長面と丸い口吻は残る。独立画像レビューも改善が既定採用には弱いと判断したため、今回の4案は比較fixtureとして残し、既定資産には採用しない。共通面の汎用機能の成立、84Bの節約、狐の画質受入を別々に扱う。

描画試験は最終depthのbinary64独立oracle、A=B画素一致、specularのみの同一画像、旧golden、1..8lobes/切詰め、mask全fieldのdamage、snapshot/repairを検査。実QuickJSでは7引数入力の型・値・予算拒否、copy後のbuffer変更、色領域だけの更新を全描画referenceと比較する。native Modelは非一様/負scale、親子、camera変更、光変更時の領域不変、失敗評価後の復帰、閉じたModelとlive Instance、二個体をASan/UBSan/m32で検査した。

```sh
python3 tools/kasane_contract/run_fox_app_host.py --m32 --material-study unchanged --capture-dir .cache/fox-blend-unchanged
python3 tools/kasane_contract/run_fox_app_host.py --m32 --material-study blend-mask-only --capture-dir .cache/fox-blend-plane
python3 tools/kasane_contract/run_fox_app_host.py --m32 --material-study blend-mask-inset --capture-dir .cache/fox-blend-inset
python3 tools/kasane_contract/run_fox_app_host.py --m32 --material-study blend-mask-pruned --capture-dir .cache/fox-blend-pruned
```

追加のpixel処理は平面dot3乗算/3加減算、必要時のfeather係数とalbedo混合。逆変換はnode評価時に行い、pixelごとの逆行列やlobe再走査を追加しない。旧96B header/旧variant/5引数は維持し、新packetはraw末尾へdescriptor24Bを付けてから10B entryへ丸める。native submitのsnapshotコピーを削って寿命を借用へ変えることはしていない。画質と実機費用が未確認のため、速度改善済みとは主張しない。

## 白帯の根本差と次の造形試験

設定図`fox-character-sheet-volume-muzzle-v2.png`の実画素と上のhost比較を独立に見直した。現状は丸い頭へ突出した眼球、横長の白い頬cap、丸い口吻を載せる構成である。設定図は額→眼窩→鼻筋→前へ細まる口吻が連続し、眼がその面へ収まる。頬は耳側で広く、鼻側では薄い。現blendは白いgroup内だけを連続させ、頭・眼窩・口吻全体の面を共有していないため、色境界だけを動かしても独立した口ひげ感が残る。

次の候補は既存前面を置き換える小さな固定構成の3D顔面patchで、部品の追加で埋めない。額・眼窩・頬・口吻を共有し、眼開口を同じ顔面の基準点へ結ぶ。まずneutralの正面/左右35°で面のつながりをhost検証する案であり、実装・採用済みではない。既存の親子TRS、3D anchor、凸clip、固有色planeは再利用できる。一方、凹んだ眼窩や細まる顔面を制御する連続面の位置/法線更新・面上attachmentは未提供であり、単なる正のellipsoid blendだけで解決済みとはしない。一般mesh engineや全画面Z-bufferの導入を先に必須とはしない。

## SourceA の明示的 optional factory

`AyatsuriLocalPlane.scene({geometries:[AyatsuriBlend]})`から、動的なblendにも同じ1平面2色をauthoringできる。通常の`character.js`と`character_blend.js`は変更せず、非使用appに追加moduleを読み込ませない。masked blendだけが既存backing末尾に24Bを追加し、planeはgeometry/camera変更時に更新する。光だけの変更では固有色領域を再計算しない。

実QuickJSの確保失敗注入で、初稿の動的propertyコピーが例外を残したままrotationの欠けたnodeを公開する欠陥を検出した。対象fieldを明示したsnapshotへ修正し、単にpending例外を消して成功扱いにはしていない。固定版の64bit ASan/UBSan試験では113確保位置すべてがatomicに拒否され、旧packet保持・同名再試行後の全packet一致を確認した。通常factoryの対照64位置もすべて拒否した。

同試験の16 pose/camera/light条件でnative Modelとのgeometry byte一致、実C shaderの32画像対一致、plane最大絶対差0。warmの無変更1000回とpose変更1000回はallocator呼出し0。dispose済みhandleを8個保持したheader等は64bitで1432B残るがraw backingへの参照は切れ、handleも解放後のbaseline差0、runtime終了retained0を確認した。これらはhost試験であり、ESP32の速度・メモリ実測ではない。64bit sanitizer試験は160KiB容量受入とは分離する。

同じ固定版のm32実QuickJSでも16条件/32画像、113位置のatomic拒否と全packet再試行一致、warm両1000回の確保0を確認した。保持した8 tombstoneはm32では864B、解放後baseline差0。ASan版とm32版は別binaryであり、bit数により保持量が異なる。証拠は`.cache/source-plane-proof/final-{asan64,m32}-result.json`とrunner logに残す。

実際の`pocket.hone`とcandidate/committed描画経路も別harnessで検査する。`--source-plane`は12状態を2processで再生し、毎回のincremental/full-reference、色一致、pose/light/camera更新、hide/show、失敗後の旧画像保持とrepair、dispose後のcommitted画像保持と再生成一致を確認する。

固定版はこの実描画harnessも64bit sanitizerとm32の両方でPASSした。m32はcap163840B、frameごとの強制GCなし、両process終了guest0。12状態のhash列が同一で、実行中の依存source hash不変も確認した。最後のreset/invalid/recreate画像hashは`eed049309a5c9e40`。m32結果は`.cache/source-plane-render-m32/result.json`、64bit結果は対応する`source-plane-render-asan/result.json`に保存する。いずれもHOST SIMULATIONであり実機の計測ではない。
