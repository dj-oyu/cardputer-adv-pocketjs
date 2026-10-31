# 狐の顔v4: 下顔の接合と開口bufferの重複削減

基準は`d31c57baed4e5e9257c3b3ae7ee86c10ea3d433a`の[顔v3](fox-face-v3.md)。29node・8開口・12clip対象・13blend lobe・bin3620B・raw slot8496B・Instance arena9192Bを維持する。native core/binding/rendererやAYNM形式は変更しない。全身・他キャラ・2.5D画像版へ範囲を広げていない。

## 同条件で選んだ下顔

cameraは(0,0,400)、target0、zoom1.3、center(120,78)、世界光(-.5,-.4,.8)。4表情×yaw -35/0/+35度を比較する。pitch±10度と背面光±150度も別に確認した。すべて240×135の**HOST SIMULATION**を拡大した画像で、実機captureではない。

| 候補 | 変更と観察 | 判断 |
| --- | --- | --- |
| [v3](../apps/fox-face-v3-host.png) | 横長の白頬と皿状の下顎 | 比較基準 |
| [A](../apps/fox-face-v4-A.png) | 頬を薄く外側へ細め、鼻筋・顎を小さくしたが、灰色の下顎の段差が残る | 不採用 |
| [B](../apps/fox-face-v4-B.png) | 頬を高く、頭を縦長へ変更。丸い頬の膨らみと下顎の段差が強い | 不採用 |
| [A2](../apps/fox-face-v4-A2.png) | Aの顎を引き上げ、閉口の段差を減らしたが斜めの口内が張り出す | 中間案 |
| [A3 / 採用v4](../apps/fox-face-v4-host.png) | 顎の3lobeを後方から前方へつなぎ直し、口内と唇幅を縮めた | 限定的な接合改善として採用 |

各候補は同じ32bitハーネスの243frame×2、42画像の反復一致、差分/全描画一致、6回のrelease/recreateを通した。非採用案も消さず、[候補の正確な差分とasset SHA](../../tools/ayatsuri/model/studies/fox_face_v4_candidates.json)を残した。これは制作比較データであり新しいruntime形式ではない。v3 fixtureから変更nodeの各fieldを適用して再packし、4候補のbinary SHAが一致することを確認した。

採用A3はcream中央を小さく、外側lobeを細くし、鼻筋を短くする。下顎のlocal位置は(0,-3,8)/(0,-2.5,15)/(0,-2.5,20)、radiiは(10,5.5,9)/(8,4.5,7)/(6,3,5)。口内X半径8→6.5、唇8.8→6.8とし、顎の開閉に合わせた既存poseも調整する。新しい部品・clip・描画順の追加で隠す方式ではない。

独立画像レビューでは、v3より下顔の重さと皿状の露出、A2より斜めの口内の張り出しが減った。新しい漏れ・反転・部品消失は観察されなかった。一方、口髭状の白帯、平たい口、丸い橙の顎、板状の耳は残る。眼を包む連続面も未達で、設定図に十分近づいた、狐が完成した、とは扱わない。[pitch/背面光比較](../apps/fox-face-v4-pitch-backlight.png)もこの限界を含む。

## 固定データの重複を一つ除去

`AyatsuriClip.aperture`は入力を所有copyした後、未使用の`rest`と更新用`local`へ同じ座標を二重保持していた。未使用の参照を除き、最初の所有copyをそのままcurrent localに使う。初期windingは保持し、setterは別stagingへ全件copy/検証後swapする。callerのbufferを借用しない。native snapshot、画面cache、失敗atomicに必要なstagingは残す。

8開口46頂点の重複backingは552B。同じv3形状・同じ42画像・同じ寿命試験での対照結果:

| 32bit host指標 | 削除前v3 | 削除後・v3同形状 | 採用A3 |
| --- | ---: | ---: | ---: |
| 丸め後要求peak B | 140672 | 138592 | 138592 |
| QuickJS会計peak B | 157632 | 155104 | 155104 |
| 最大単一raw要求 B | 9199 | 9199 | 9199 |
| Instance arena B | 9192 | 9192 | 9192 |

低下2080B/2528Bはtyped view/buffer/property/codeを合わせた除去効果で、全量を552Bのbacking削減と呼ばない。固定binのflashサイズは変わらず、guest peakの節約へ加算しない。他のJS固定データ・一時生成・共有参照の厳密な内訳は未分離である。

## 寿命と検証

32bitは163840B cap、通常GC、frameごとの強制GCなし。QuickJS会計はこのhost profileで丸め後要求量+8B×live block数。割当callbackでpeakを追い、区間境界のO(1)counterと一致を確認する。会計上の残量8736Bは**host QuickJS capとの差**であり、ESP heapの空き・最大連続領域・次の大口確保の保証ではない。quota/allocator拒否0、終了guest/live block0。

release後のcurrentは丸め後91696B/会計101520B、再生成後134608B/150960B。各6回で同値だった。今回からrunnerは回数だけでなく、両currentが6回同値であることも明示assertする。件数を保ったまま各指標を増やす負例と、欠落例をunit testで拒否する。release中もcommitted画像を保ち、再生成後の同一画素を検査する。GC後だけ0になることを回収の代用にしていない。

ASan/UBSanの64bit別profileも243frame×2、42画像反復一致、解放・再生成PASS。189 pose sweep、無効morphのatomic拒否、入力bufferを後から変更してもcurrentが汚染されない所有権、変更のあるsetterのswapをNode試験で確認した。64bitのメモリ値を32bitへ換算していない。

```sh
node tools/character/test_ayatsuri_clip.cjs
node tools/ayatsuri/model/test_fox_face.cjs
python3 tools/character/test_fox_lifetime.py
python3 tools/kasane_contract/run_fox_app_host.py --m32 --capture-dir .cache/fox-v4-final
python3 tools/kasane_contract/run_fox_app_host.py --capture-dir .cache/fox-v4-final-asan
```

result JSONは`.cache/fox-app-{m32,asan}/result.json`へsource hash、画像hash、会計とphaseを保存する。warm loopのalloc数、GC時間、ESP free/largest/minfree、native heap/BSS/stack、実機画質/FPS、計測自身の時間費用は今回N/A。hostの画素整合と造形達成、製品linkと実機動作は別工程である。
