# 狐の顔v3: 部品を増やさない眼の改善と寿命計測

比較基準は`3cb5d03d39ca7e49c0747732e3b87c4f8ab07556`の[顔v2](fox-face-v2.md)。29node、8開口、12clip対象、bin3620B、raw slot8496B、Instance arena9192Bを維持した。固定資産はflashであり、guest peakの増減へ足し合わせない。native core/binding/rendererと共通scene/clip APIは変更していない。

## 眼と画像

眼球の高さ9.2→8.5、Z半径1.5→1.1、虹彩のX半径4.8→5.8とし、虹彩・瞳・上瞼も約.25奥へ移した。開口の下中央.9→.76、下肩.45→.4で、縦長の白い輪を減らす。眼窩位置、瞳の大きさ、耳・頬・口・顎はv2を維持する。smileのEMPTY、surpriseの開口増加、カメラ相対yawの遠側眼補正も維持する。

cameraは位置(0,0,400)、target0、zoom1.3、center(120,78)、世界光(-.5,-.4,.8)。[v2の12枚](../apps/fox-face-v2-host.png)と[v3の12枚](../apps/fox-face-v3-host.png)は同じ4表情×yaw -35/0/+35度。[v3のpitch/背面光](../apps/fox-face-v3-pitch-backlight.png)も保存した。すべて240×135の**HOST SIMULATION**を2倍に拡大したもので、実機captureではない。

独立画像レビューでは、白輪と突出感が減り、虹彩に面積ができてneutral/jawOpenが穏やかになったと確認した。4表情の区別と、掲載yaw±35°/pitch±10°の可視性を維持し、新しい漏れ・反転・眼消失は見られない。一方、横長のcream頬、皿状の下顎、丸い頭部、板状の耳によるハムスター状の印象は残る。虹彩をさらに拡大するだけでは、眼を包む瞼・頬の連続面は作れない。狐全体や自然な演技の完成とはしない。

## 明示releaseと再生成

app所有の`FoxFaceLab.release()`はbusy時拒否・冪等。モデル参照を切り、scene→assetの順で閉じる。表示resource R、preset、光、camera、tickは保持し、次のframeでtransactionalに再生成する。frameworkの新しい共通unload APIや、表示resource全体の解放ではない。

hostはrelease後、再生成前にcommitted packetから全描画し、旧画素を保つことを確認する。その後の再生成画素も一致させ、これを6回繰り返す。packetはsnapshotなので、架空のframe待ちやsleepは追加しない。通常GCを使い、強制GCは呼ばない。runtime終了後はguest使用量と観測live blockが0になる。

## 測定した内訳

観測はhost allocatorの固定counterと区間境界の`JS_GetMemoryCounters`で行う。heap全走査は行わない。丸め後要求量は4B丸め/最小12Bのlive合計で、host malloc headerを含まない。QuickJS会計はこのprofileの`丸め後要求量+8B×live block数`。割当callbackごとにpeakを追い、各区間のO(1)会計値と一致を検査する。実機ESP heap、largest free、断片化、native realloc内部の一時二重確保を測るものではない。

32bit・163840B cap、通常GC、同じ243frameを2runtimeで実行した最終値:

| 区間境界 | 丸め後要求current B | QuickJS会計current B |
| --- | ---: | ---: |
| runtime/context | 25200 | 27392 |
| API導入後 | 27284 | 29708 |
| entry/load VM frame退役後 | 85432 | 94112 |
| actor生成・初回present後 | 136424 | 153176 |
| 42比較画像後/steady | 136460 | 153220 |
| actor release後（6回とも同値） | 91720 | 101544 |
| 再生成後（6回とも同値） | 136688 | 153488 |

全区間の丸め後要求peakは140672B、最大単一要求9199B、QuickJS会計peakは157632B。ホスト会計capまで6208Bで、quota/allocator拒否0。会計の式は各観測で一致し、終了guest/live blockは0。6208BはESPの最大連続freeや実機の余裕を意味しない。

初回actor区間の増分には、native arena/metadata、JS handle、typed target/clip buffer、評価/提出を含む。9192B arenaは構造値と実際の最大単一要求9199B（整列余裕7B）が対応するが、残りをJS property/bytecode/closure等へ厳密分離してはいない。release後もappコード・表示resource等が残る。残留分を漏れや断片化と断定しない。

`FOX_LOAD`はmodule別current/cumulative peak/累積割当を記録するが、観測用`JS_ToCString(name)`の一時値を含む。`alloc_calls/alloc_bytes`は入力fixtureのJS_Evalも含む累積で、reallocは新しい要求長全体を数える。steady no-allocationや実機のGC時間の証拠にはしない。native heap/BSS/stackの動的内訳は今回N/A。

## 小さな参照重複の削除

native model controllerに用途のない`parts`配列があり、既存handleを重複保持していた。repo内の利用箇所がないことを確認して除去した。これはapp固有controllerで、ayatsuri共通handle APIは変えていない。同じv3・243frame/6再生成での比較:

| 項目 | 削除前 | 削除後 |
| --- | ---: | ---: |
| 丸め後要求peak B | 141092 | 140672 |
| QuickJS会計peak B | 158084 | 157632 |
| 42画像 | 全一致 | 全一致 |

420B/452Bの低下は、この配列・生成loop・返却propertyを合わせた除去効果で、全てを配列backing storeのサイズと呼ばない。v2の要求peak140120Bに対しては、明示releaseを持つappコードと追加寿命試験も変わっており、眼の変更だけの増減とはしない。64bit ASan/UBSanも243frame×2 PASS、要求peak199276B/会計peak216220B、拒否0・終了guest0（capなし）。

## 再実行と残る範囲

```sh
node tools/character/test_ayatsuri_clip.cjs
node tools/ayatsuri/model/test_fox_face.cjs
python3 tools/kasane_contract/run_fox_app_host.py --m32 --capture-dir .cache/fox-v3-final
python3 tools/kasane_contract/run_fox_app_host.py --capture-dir .cache/fox-v3-final-asan
```

結果JSONは`.cache/fox-app-{m32,asan}/result.json`にsource hash、42画像hash、phase、会計検証を保存する。観測自身の時間費用、実機メモリ/画質/FPS、全身・他2体・2.5Dは未受入。次の造形課題は眼を包む面と口角・顎の接合であり、部品の追加だけで解決したとは扱わない。
