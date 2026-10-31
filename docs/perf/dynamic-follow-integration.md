# 動的追従核と雲接続のローカル検証

2026-10-30、基準c73d26cからのローカル変更。[契約](../api/ayatsuri-follow-core.md)、[先行机上試験](dynamic-motion-host-study.md)。本資料はhost試験であり、実機測定や物理LCDの画質受入ではない。

## 初版06bfb8aの数値と収束

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

## 高条件数の入力境界修正

06bfb8aの独立レビューでは、I=(1,1)、対角k/c=0、kc=1e18f、cc=0でprepareが有限だが誤った係数を受理する問題が報告された。Bの行和1.002529417、共通v=(1,1)が1200 tickで約20.7276、energyが1から約429.635へ増大する。通常の雲設定の発散ではなく、行列式とA⁻¹K生成の桁落ちを含む公開APIの対応範囲の欠陥として扱った。

修正では[κ∞(A)≤10⁶の対応範囲](../api/ayatsuri-follow-core.md)を明示し、減算しないdeterminant式で条件数を検査して、超過時は係数/既存stateを変更せずRANGE（JS LIMIT_EXCEEDED）とする。上限はκ×epsilon64を約2.23e-10以下に抑える保守的な数値領域で、全有限状態の物理的安定性の証明ではない。対応範囲内のB/F/G式順を維持し、既存通常条件の結果を変えない。

追加後はMinGW/ASAN/m32のO0/O2各217767検査、4976 vector、C/JS 19924成分一致。報告されたkc=1e18f、極端な慣性比/結合減衰は拒否し、準備済みoutとstateを保持して正常入力へ復旧する。隣接F32のkc=1.79999808e9は受理、1.79999821e9は拒否。受理側の共通v=1とenergy=1を1200 tick維持した。

A⁻¹K側は不均等なI=(1,2)、k=(3,7)、c=(.2,.5)、kc=2.6e9f、cc=.1fでも、展開した非桁落ちbinary64参照式と比較した。2初期条件×1200 tickの最大q差4.04e-7、v差2.36e-6、1step差2.98e-8/5.98e-8。q/v差1e-5、1step差1e-6、丸めを含むenergy増分許容1e-5の試験基準内で、任意入力への一般保証とはしない。従来のsoft/heavy厳密20秒1e-8 FAILは保持した。実QuickJSでの拒否・出力不変・正常入力への復旧と、雲2169 frameの160KiB門も追加後にPASSし、上記guest指標は同値だった。

## 06bfb8a CIの画像ハーネス修正

[Procedural workspace reuse / contracts](https://github.com/dj-oyu/cardputer-adv-pocketjs/actions/runs/41057519523/job/124068680276)ではbaseline画像ハーネスのlinkが`pocket_api_class_ready`未定義で失敗した。製品linkの失敗やMEGADEMO OOMとは別件。最小ハーネス用のweak class-owner bookkeepingとteardown時のowner忘却を追加し、同一runtimeの再入/忘却試験を行う。製品のruntime所有権処理を弱める回避はしない。修正後はbaseline/currentそれぞれのscalar/fake PIE画像試験と、baseline/current間48枚の完全RGB565一致をローカルで確認した。

## 後続: 不変データの事前変換

ユーザーのbin事前変換案は次の比較候補とする。今回の最終試験の完了条件に新形式への全面移行を加えない。160144 Bは全guestのmalloc_sizeであり、下記分類別のlive/peak内訳はまだ計測していない。差分全体から特定分類の費用を逆算しない。

| 分類 | 現状と事前変換候補 | 分類別の実測 |
| --- | --- | --- |
| 形状 | 不変のlobe位置/半径、親子情報はpacked asset候補。現在はJS descriptorからscene所有bufferへコピー | 未計測 |
| 材質 | 固有色・不変係数はpacked候補。光/視点/変形後の評価結果は動的 | 未計測 |
| メソッド | C共有核/管理objectへ移した部分とJS評価コードを分ける。dataのbin化だけではJS bytecodeを消せない | 未計測 |
| 追従state | q/v/goal/位相は可変のためROMには置かない。初期係数/初期値のみ候補。C raw型はstate16 B、prepared112 Bだがguest全費用ではない | raw型のみ確認、分類全体は未計測 |
| 一時buffer | 同期native呼出しで再利用済み。ファイル読込・検証・publishに必要な一時コピーは別途評価 | 未計測 |

binを読み込んで同じJS objectへ再展開するだけの方式と、Cが検証済みpacked dataを直接参照する方式を比較する。共有sin LUTは既にCの不変int16 tableであり、同様に不変性が明確なデータから検討する。新asset経路は未実装。

直接参照にはversion/byte order/型・精度/長さ・count・offsetのoverflow/整列を定義し、範囲を検証してから公開する。file I/OがいったんArrayBufferへcopyするか、Cが不変領域をmapできるかは未調査で、現時点でzero-copy可能と断定しない。actorと転送完了までasset所有者を保持し、読込bufferの解放・差替え・unmapでdangling参照を作らない。JS可変bufferの後借用やcandidate/committedの不変性を壊す省略はしない。失敗時は旧committed画像を保持し、生成時peak・最大単一確保・保持量・読込/検証copyを別々に測る。

追補の読取りでは、現在のJS FS readは1024 Bのstack bufferから`JS_NewUint8ArrayCopy`、Cのread_all/read_atもcaller bufferへのcopyで、mmapではないことを確認した（`main/pocket/pocket_fs.c`）。既存pet assetのEMBED_FILESとasset tableは候補だがcloud binは未対応。最初はcloudのnode/parent/初期TRS、radii/blend local lobes、材質表をversioned binから既存bufferへ直接初期化し、巨大なJS definition graphを復元しない比較を行う。既存backing storeはellipsoid264 B/group84 B、blend316+108×lobeと既にpackedなので、第一候補は生成時のdefinition graph peakであり、動的stateの大幅削減は未立証。tongueの色/opacityは可変として残す。初期化copyを認めてdispose/reloadと画素一致を測り、shared immutable参照はその後に検討する。
