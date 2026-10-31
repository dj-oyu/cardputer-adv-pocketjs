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

分離後はfixture2回とも297frameで完了、digest `e12e1890`、pause15、旧画像維持/通常復帰、全描画reference、終了guest0、捕捉quota/allocator拒否0でPASS。観測live159252B/丸め後要求peak146640Bはこのcloudprobe profileの値で、上の狐の会計peakと混ぜない。cap増加・frame毎強制GC・sleep調整・試験閾値緩和は行わない。host gateは捕捉されたOOMも明示的に拒否するようにした。

新機能を使わないappへ追加bytecodeを常駐させないことも、今回の性能受入に含める。helper未読込の明示拒否と、namespace削除後も作成済みsceneが同じencoderを使うことをJS試験へ追加した。

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

2平面の積集合、blendの共通local座標、材質ごとのroughness/厚み、任意depth/occlusionは未実装。同一面の毛色を分ける機能と、狐全体の完成・実機受入を分けて扱う。
