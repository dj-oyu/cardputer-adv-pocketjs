# 動的追従核と雲接続のローカル検証

2026-10-30、基準c73d26cからのローカル変更。[契約](../api/ayatsuri-follow-core.md)、[先行机上試験](dynamic-motion-host-study.md)。本資料はhost試験であり、実機測定や物理LCDの画質受入ではない。

## 数値と収束

`tools/ayatsuri/run_follow.py` のMinGW、Linux ASAN/UBSAN、m32 SSE2各O0/O2は206,943検査、4,968出力vectorが一致。Node参照の19,888成分も符号付き0を含め同環境で一致。異なるlibm/全CPUのbit一致を保証するものではない。

F32格納/F64中間の核と、丸めないF64直接連立解との最大差。4構成に対し、1step q差7.451e-9 rad、v差4.229e-8 rad/s。1,200step q差3.581e-8、v差1.974e-7、35+35 model unitのtip差1.963e-6。24,000step q差6.057e-8、v差2.171e-7、tip差3.406e-6。これは数値参照差で、実物の運動との誤差ではない。

実用probeは試験前に両q誤差≤1e-3 radかつ両速度≤1e-3 rad/sを60step維持、20秒以内と宣言。drive OFF、初期q=(.2,-.1)、v=(.1,-.2)、goal=0、60Hz。基準は駆動1.2Hz/ζ=.7、結合1.8Hz/ζ=.7、柔は結合.9Hz/ζ=.35。周波数は係数の設計値で固有振動数の測定ではない。

| 構成 | 末端慣性 | 60step維持完了 | 20秒の最大q残差 | 厳密1e-8 rad |
| --- | ---: | ---: | ---: | --- |
| 基準 | 1 | 3.2833秒 | 1.152e-22 | PASS |
| 基準 | 2 | 4.7167秒 | 8.078e-15 | PASS |
| 柔 | 1 | 4.5833秒 | 6.624e-15 | PASS |
| 柔 | 2 | 7.9667秒 | 1.060e-8 | FAIL |

先行studyの16秒速度FAILも削除しない。今回の実用基準の成功を、厳密条件の成功へ読み替えない。4構成の無励振4,800stepエネルギー非増加も確認した。

## 更新費用と所有権

C核のwarm20,000回でmalloc/calloc/realloc/free呼出し0。実QuickJSではcompose/composeNode各1,000回、raw followStep/followDrive各1,200回、managedのtyped retarget→step→sampleを1,200回それぞれguest allocator呼出し0。各メソッドの初回呼出しは準備区間へ分離。初回のsampleを含めると5回の確保があり、この初回込みのFAILとwarm区間を区別する。managed drive.stepのnumber返却、JSの数値boxing、モデル/表情更新、ログ、描画は別。アプリ全体の無確保を示すものではない。

preparedは112 B、config32 B、q/v state16 B。逆行列はprepare時だけ。step数式本体は14乗算/14加減算で、有限性/範囲検査、LUT、呼出し、座標計算、描画を含めない。native橋渡しはstack上へ短いpacketをsnapshotし成功結果だけ戻す。このコピーは借用寿命をcall境界に閉じるため残す。JSオブジェクトやview、stack最大、native free/largestはこのraw型サイズに含めない。PIE、多コア、デバイス時間の優位は未測定。

## 雲でのドッグフーディング

root/頭、左右の手、彩雲中央/左端、右端/haloの4pair。異なる係数・慣性とheld intentを持ち、共通root driveから前のaccepted root誤差/速度を末端へ伝える。pair内は相互連成だがpair間は一方向の視覚カスケード。一般の全身力学やIKではない。

BODY intentは診断用step境界で切り替える目標値。運動軌跡自体をLUTやtimecodeへ記録しない。顔・伸縮など補助表現は従来の解析samplerが残り、すべての演技を動的化したとはしない。breathing OFFは駆動振幅0、論理step進行中の位相は進む。同時刻の再描画/pauseは位相もq/vも進めない。

Nodeの同step描画cadence、seed再現、retarget q/v保持、J/I、外乱後の履歴seek拒否、再入、失敗後復帰がPASS。雲adapterのdrive OFF残差は10秒で彩雲.001588 model unit/速度.008055 unit/s、20秒で6.71e-12/3.32e-11（host参照）。8秒でsnapして旧版の画素完全停止試験へ合わせない。旧pure samplerの試験は維持し、動的経路の収束/同時刻停止を別検査にした。

最初の160KiB試験は初回生成でnull例外となりFAIL。失敗時compile peak125356 B、allocator peak147936 Bは成功時の容量保証ではない。緩和した220000 B条件では初版のlive最大190156 Bを確認したが、受入には使用しなかった。状態をguest課金されるnative objectへ移し、JS descriptorをnativeで検証、診断を任意ロードへ分離、同期借用できる作業bufferを共有。groupの材質専用field、identity親ノードを削り、pose/matrixのtyped viewを1本へ統合した。モデル構築と演技状態の準備も分けた。

最終ローカル試験は**163840 Bのまま、強制GCなし**でPASS。下記は32bit TLSFモデルのguest指標で、native free/largestや実機メモリではない。

| app | 検証frame | compile peak | live最大(malloc_size) | allocator peak | 最大要求 load/run | 解放後 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 動的雲 | 2169 (6mode×301＋駆動ON363) | 115588 B | 160144 B | 146896 B | 8512 / 1571 B | 0 B |
| 既存3体 | 5418 | 105792 B | 151016 B | 137204 B | 10912 / 1571 B | 0 B |

全frameのincremental/full描画一致、転送失敗repair、native範囲外からの復帰、既存3体の切替失敗からの復帰もPASS。capacityの余裕はこのfixtureに限り、任意モデルの保証ではない。MEGADEMOの160KiB比較も別途実行し、最終公開headの結果はCIと照合する。

画像は `.cache/dynamic-cloud-packed/` のHOST SIMULATION。固定tick0,60,90,120,177,240,300の4mode計28枚、native damage/full reference、pause/seek/失敗再試行、realm解放がPASS。全28枚は容量修正前 `.cache/dynamic-cloud-capture/` とbyte一致。眼の親子統合では602frameの送信用packetの33成分に最大7.63e-6の差（F32結合順序）があり、全frameのpacket bit一致とは呼ばない。固定tick28枚のnative画素一致を確認した範囲で見た目を維持した。

`dynamic_debug`はactor生成前の明示ロードでstatus/reset/impulseを同じcore状態へ付加する。本番ではロードしない。compact poseは従来の顔/補助形状169388値・表情10812 objectと一致、production/診断あり経路も301frame一致。物理LCD、実機の動的演技・時間・native peak/stackは未確認。
