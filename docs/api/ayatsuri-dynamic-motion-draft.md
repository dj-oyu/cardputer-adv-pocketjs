# ayatsuri 動的演技 — 目標と持続する運動系（設計案）

2026-10-30、調査基準`c73d26cd464e35a8cf33f32cc6b77e2574adc25d`。**製品未実装・未合意の仕様案**。初稿はread-only調査と本草案。既存候補の[同head CI](https://github.com/dj-oyu/cardputer-adv-pocketjs/actions/runs/41034418770)は72 jobs成功だが、下記機能の試験結果ではない。公開・実機操作は行わない。

追補: 初稿後に[最小の机上計算/host simulation](../perf/dynamic-motion-host-study.md)、続いて[2DOF追従核の縦切り実装](ayatsuri-follow-core.md)へ進んだ。冒頭の「未実装」は初稿時点の記録であり、現在は核の一部を実装済み。一般rig/IK/制約グラフ/全actor transaction/HOMEは引き続き設計案。非対称な左右/行き戻りでの1pass費用と、immutable旧stateからの順序不変は机上資料で確認する。

## 1. 設計の中心となる二方面

**静的な目標・制約**は、「ここを見る」「ここへ手を伸ばして保つ」「姿勢を緩める」という基準を次の入力まで保持する。永久不変ではなく、途中で書き換えられる。到達点/注視点、関節可動域、保持の優先順位と許容誤差、relaxed/preciseの意図を表す。

**持続する動的運動系**は、現在位置・速度・内因性運動の状態から次の状態を生成する。応答の重さ/柔らかさ/減衰、部位間の連成、保持強度を設定し、目標誤差、外乱、呼吸、補償が軌跡を作る。各点のブレの時系列や波形を事前制作する構成にしない。乱数は生命感の主役ではなく、なくても途中目標変更・遅れ・反動・回復が成立する。

既存の純粋なtime samplerは、目標/表情の意図や診断イベントを作る補助手段として残す。`performance.evaluate(time)`を状態付きへ黙って変更しない。動的actorの`step()`と読取`sample()`は別契約にする。

## 2. 現在あるものと差分

| 現行の根拠 | 可能なこと | 不足 |
| --- | --- | --- |
| [motion.js](../../apps/character/motion.js) `track/sample/performance` | keys/wave/gateから任意時刻を純粋評価、出力再利用 | 位置・速度、目標追従、外乱、状態履歴なし |
| [cloud_motion.js](../../apps/cloud/cloud_motion.js) | 時刻をずらした解析的演技、seed不要の逆seek | 遅れは物体状態から生じない。途中retargetや外乱で演技が変わらない |
| [character.js](../../apps/character/character.js) `place/evaluate/render` | 親子TRS、材質/UV/光/視点、依存差分、同期コピーと再入拒否 | `evaluate(time)`は積分ではない。動的自由度/保持点/可動域のrig schema、IK、一般morphなし |
| [ayatsuri_transform.h](../../main/ayatsuri/ayatsuri_transform.h) | F32保存/F64中間、純粋TRS共通C核、失敗時出力不変、無確保 | 共通動力学核/scene全体のC APIなし |
| [character_blend.js](../../apps/character/character_blend.js) | 1..8個の不変lobe定義とgroup変換 | 個別lobeの実行時更新、一般形状morphなし |
| [scene.h](../../main/scene/scene.h) / [shell.c](../../main/ui/shell.c) | HOMEのC `prepare(dt)/draw(strip)/overlay` | ayatsuri adapterなし。JS appのrealmを共有できる意味ではない |

現appの`tick/30`はcommit成功に連動する診断位置。`pocket.app.onFrame`は単調timeMs/deltaMsを提供し、resume初回deltaは0（[pocket_app.c](../../main/pocket/pocket_app.c)）。これを環境adapterの時計入力として検討し、診断tickをそのまま動力学の時計にしない。

## 3. 最小セットと後続

最初は§6.1の棒/枝/保持という小fixtureで伝播近似を確認し、キャラ縦切りは**雲の胴体/頭/自由に浮く手**へ進める。目標へ追従し、途中retargetと胴体へのimpulseに反応し、注視と手先保持を補償する。共通の呼吸状態・小さな有界変動を含め、relaxed idleとprecise reach/hold、releaseを比較する。新しいgeometry primitiveや大規模物理エンジンはこの試験の前提にしない。

| 初版で判断基準にする | 後続/必要性を見て追加 |
| --- | --- |
| 値/速度を持つ追従、角度規約、固定step、途中retarget、単発外乱 | 自由3D姿勢全体のquaternion dynamics、衝突/接地/布/流体 |
| 一つのworld手先anchorと注視、角度/到達範囲、残差報告 | 任意constraint graph、多点contact、汎用全身IK |
| 共通内因性driveと部位ごとの連成、precise時の補償 | 詳細な筋肉/呼吸生理モデル、部位別のノイズ曲線制作 |
| 共通C核/JS adapter、事前確保、同一入力fixture | C scene全体、HOME接続実装（環境契約を先に分離する） |

雲の手は二骨の腕ではない。自由端点の追従試験をIK検証と呼ばない。人/狐で腕や脚が必要になった段階で、小さな二骨chain fixture（pole方向/伸び切り/到達不能/関節限界）を別に加える。

## 4. 状態・型・所有権

モデルが宣言するchannelは位置ならmodel world unit、角度rad、速度は各unit/s。時間は内部では整数step、外部clockはadapter所有。現在の座標/TRS順は[既存契約](ayatsuri-transform-core.md)を維持する。

- `intent`: 要求されたworld到達点/注視点、保持強度/priority、goalの有効状態。次の入力まで保持する。入力時にsnapshotし、callerの配列をretainしない。
- `state`: 各自由度の現在値q・速度v、前step値、呼吸/連成/有界変動の状態、step番号/seed/イベント消費位置。actorごとに独立。
- `rig`: 親子/基準TRS、channel→部位の対応、可動域、到達範囲、補償に使える自由度。作成時に検証・固定。キャラ名を低C層へ埋め込まない。
- `work/output`: 予測/制約用scratchと出力pose。事前確保・再利用。名前→狭いindexの解決は構築時、JS利用者へoffset/dirty管理を要求しない。

F32保存/F64中間を初案とし、finite/range/overflowはcommit前に検証。関節/部位indexは構築時上限に合う整数型、stepはJSで正確に表せる整数範囲をC/JSで共有し、wrapを黙認しない。具体packing/最大channel数は最小rigの実測後に固定する。仮の大容量arenaを既存160KiB予算に収まるものとして扱わない。

## 5. 追従・retarget・角度

retargetは要求goalだけを変える。q/v、呼吸位相、補償状態を初期化しない。新goalで加速度は変わり得るが、通常の目標変更で位置/速度を飛ばさない。要求された到達先gと、速度を保ってそこへ進む追従reference rを分け、rigはrへ追従する。制約段階が新しい遠方のgへ直接手先を投影してspringを迂回することは禁止。瞬間移動や速度初期化は明示`reset`だけ。外乱は最初は**速度差分Δv**として定義し、モデル単位と実物のkg/Nを混同しない。

初期C核候補は固定step hの減衰spring予測。保持goal g、ω=2π frequencyHz、減衰比ζ、内因性/外部/連成の加速度aについて、

```
v* = (v + h * (ω² * (g - q) + a)) / (1 + 2ζωh + ω²h²)
q* = q + h * v*
```

という小さな暗黙更新を比較候補とする。hは正でfinite、frequency/dampingはfiniteかつ非負、入力/中間値の範囲を宣言する。収束fixtureは正の復元周波数と到達可能な静止goalを使う。採用済み数式ではなく、retarget/反動/減衰のfixtureを見て決める。部位ごとの応答差は周波数/減衰/連成から生じさせ、遅延用の長いtarget履歴を最初から要求しない。一定係数の追従だけで自重/荷重による応答差を説明したとはせず、まず次節の簡潔な伝播近似を試す。

bounded角度は可動域へ制限し、cyclic角度は上式のg-qを最短角差へ置き換える。±πの同距離は+πを選ぶ案とする。canonical角度の表記ジャンプと物理的な速度連続を区別し、後続の描画補間も周回をまたぐ最短側へ行う。cyclicの1step回転がπ以上になる設定だけでなく、impulse/予測/拘束後の実変位も検証して拒否し、周回数を曖昧にしない。角度の速度補正はcanonical値の単純差ではなく同じ周回規約の角度差を使う。一般SO(3) dynamicsへ拡大した契約ではない。

## 6. 呼吸・ブレながら止め続ける

内因性運動は、全身に関連する低周波の状態と、それに結合する部位ごとの振幅/位相/応答差として生成する。呼吸位相/強さを持ち、活動や保持の意図で性質が滑らかに変わる最小モデルを比較する。部位ごとの完成済みsin(time)曲線を演技の主役にしない。

微小変動はseedとstepで再現する有界・連続的な状態過程を使い、共通成分と小さい部位差を持たせる。毎renderの乱数、各部位で独立した大振幅の乱数、完成poseへの無条件加算は不可。noise OFFでも追従と呼吸/補償の因果が成立することを先に確かめる。

共通driveの最小候補は、**同じ小LUTを速度・位相・振幅の異なる2〜3個のoscillatorで補間合成する**ものとする。固定stepで各位相を更新し、合成結果を根元の駆動入力へ渡す。例の`64 × int16 = 128 B`は波形本体だけで、確定サイズ/性能値ではない。各oscillatorの位相/増分/振幅、補間のwork、actor/rig/描画領域は別途必要。共有tableはimmutable、phase等はinstanceごとに所有する。

tableの末尾→先頭でも連続に補間し、位相wrap/正規化/合成振幅の上限を規定する。整数保存型は範囲/補間精度を確認して選び、運動stateまで無条件に低精度化しない。retargetで位相をresetせず、振幅や速度を活動に応じて変える場合も持続状態から滑らかに変える。seedは初期位相/状態の再現に使える。

これは完成姿勢の軌跡LUT再生ではない。各部位のstateful追従/減衰、目標誤差、外乱、補償が別に運動を決める。周期合成は厳密なPRNG列とは別で、有限状態/有限精度では最終的な繰返しや短周期化に留意する。tableサイズ/本数/線形か高次補間かは、sin/漸化式/軽量PRNG＋補間との費用・見た目比較で選ぶ。「周期だから必ず速い」とせず、今回は実装/benchを行わない。三重振り子やカオス再現は要求しない。

precise holdでは、実際の胴体の呼吸/揺れを受けた親変換から手先/注視の誤差を計算し、許された自由度で補償する。全ての揺れを消すのではなく、何を守るかをpriorityと許容誤差で指定。手先を守る間も胴体の呼吸や小さな残差は見える。保持のためにdrive振幅を弱める場合も、drive/保持強度の状態を滑らかに変え、急に全部0へ切らない。

relaxedでは保持を緩め、手を離すと保存された速度から自然に姿勢が緩む。解放時に姿勢/速度をresetしない。`pause`（状態を保存して積分停止）、`alive hold`（呼吸を含め保持）、診断の`freeze`（完全静止画像）、drive OFFの収束試験を別操作にする。収束は位置/速度の許容誤差で評価し、浮動小数点の厳密0への到達を暗黙に要求しない。

### 6.1 最初の近似: 一つの根元のブレと伝播

ユーザーの簡潔な近似案を最初の比較試験とする。**単一の根元角度の動的な揺れ → 長さによる末端の位置変位 → 柔らかい連結の遅れ/しなり/減衰 → タスクに必要な補償**。各点に独立noiseを用意しない。小角度で末端変位が長さ×角度へ近づく幾何学的拡大と、連結の運動系による応答は別に測る。先端ほど無条件に倍率を掛けるものではなく、減衰によって小さくなる場合も有効な結果とする。

同じ根元入力、同じ長さ/目標で(1)硬い棒、(2)一つの柔らかい連結を持つ枝、(3)その手先をworld目標へ保持する条件を比較。まず一つの有界な根元driveだけで、時差・反動・収束と保持補償が読めるかを確かめ、その後に雲の注視＋reachへ接続する。完成した姿勢曲線を制作する代わりに、連結の応答/減衰/可動域/保持強度を設定する。呼吸と微小変動もこの共通driveの状態へ関与させ、最終poseへ後足ししない。

この近似は負荷/筋肉/神経の忠実なモデルではない。姿勢・末端質量・重心・支点からの距離で負荷が変わり、有限強度/遅れ/小さい変動のある駆動と慣性/弾性が相互作用する、という因果は将来の検証事項として残す。共同noiseだけで負荷依存性を再現したとはしない。

必要になれば次段階の一関節＋末端質量fixtureで、実姿勢のleverによる負荷torque、慣性、駆動gain/上限/遅れを最小に加え、伸ばす/畳む、荷物なし/ありで比較する。全身inverse dynamicsや生理学的再現は要求しない。荷物を持つ/離す動的切替の連続性/impulseは別に規定し、q/vを黙ってresetしない。雲の浮く手には接続した腕の自重がないため、雲だけでこの負荷試験を済ませたことにしない。

## 7. 評価順と制約・IK・補償

各固定stepの順序案:

1. 予約した同step入力を受付順に適用し、goal/意図をsnapshot、impulseを一度だけ速度へ反映。
2. 内因性状態/連成を進め、goal誤差と合わせてspringを予測。
3. 予測した実際の親変換からworld追従reference rの誤差を求め、注視/保持/可動域を**同じ制約段階**で解く。requested goal gへ直接瞬間移動させない。
4. 解いた位置と矛盾しない速度へ補正。例えば位置修正分/hを予測速度へ反映し、禁止方向への速度を射影で除く。位置だけclampして外向き速度を残さない。
5. 全state/outputを検証して、一つのsimulation stepとしてcommit。

初版は可動域→手先reference→注視という小さな固定solve順と、許す補償自由度/固定iteration上限をモデルへ宣言する。可動域/安全域はhard、到達/注視/姿勢/styleはslackを持ち、任意priority graphは後続。到達不能な手先目標は要求goalを書き換えず、referenceとgoalそれぞれへの残差を返す。precise holdも初版は誤差内に保つtaskで、突然hard contactを有効化して速度を消すものではない。hard条件を満たすposeが作れない数値失敗はstep自体を不成立にし、到達成功と報告しない。

IKは「末端目標から関節姿勢を求める」解法であり、遅れやバネ感を自動で作るものではない。採用する場合、IK後に無関係な角度clamp/補償を重ねて到達を壊す構成を避ける。反動補償も、許された姿勢変化と実際の残差から同段階で解き、無制限な積分補償を蓄積しない。

通常retargetの速度連続と、contact/拘束切替による衝撃は別。後続contactでは拘束の有効化時刻、impulse、残差、許される速度不連続を明記する。初版の自由端点holdを物体接触solverとして宣言しない。

## 8. 時計・描画・失敗の境界

固定stepの初案は60Hz、120Hzも比較対象。数値/予算は実測前の提案で、実機60fps保証ではない。adapterが単調時計のaccumulatorを持ち、actorは時計を読まず、`step()`で1stepだけ進む。描画頻度が変わっても同stepの状態は同じ。

最小縦切りの`sample(1,out)`は最新の成功poseだけを読取り、state/RNG/イベント消費を変えない。固定stepと描画頻度は分離するが、描画補間と追加の再投影solverを初回必須にしない。

後続の`sample(alpha,out)`補間では、注視/anchorの導出poseを補間後の親変換から読取専用で再計算し、joint角の単純補間だけでholdを破らない。pose再投影は有界でsimulationへ書き戻さない。pause前にalpha=.7の像を出したのにresumeでalpha=0へ戻して表示を後退させる実装は不可。保存した描画pose/位相を引継ぐか、明示handoffで新しい補間epochを作る契約と、その連続性試験を採用前に固定する。

長い休止に巨大dtや無制限catch-upを入れない。MVPはpause/resumeでwall clockの基準だけ更新し、残余時間を保持、休止中の実時間はsimulationへ投入しない。最新成功poseを描くのでresume時に過去poseへ戻らない。稼働中にmaxSubstepsへ達した場合はadapterへ遅延状態を返し、明示的な時間再基準化を記録する。時間を黙って捨てたり、actorが勝手に走り続けたりしない。最大回数は実測して固定する。

**simulationと画像のcommitは別**。失敗したstepはq/v/前値/step/active goal/hold強度/制約mode/drive/RNG/イベントcursor/出力を全て不変にする。未消費queueは維持する。成功したstepは描画失敗で巻き戻さない。最後のcommitted画像を保持し、次の描画は最新の成功stateを使う。render再試行でimpulseを再消費しない。simulationStepとdisplayedStepを診断で区別する。

同stepの入力batchは先に全検証し、満杯/不正eventで部分受付しない。過去stepへの入力は遡及適用せず拒否。同stepは受付順、同goalの最後の設定を有効とし、impulseは順に一度ずつ適用する。実入力をnextStepへ割り当てた時点を記録し、renderのたびに別時刻へ振り直さない。失敗stepを無限に自動再試行せず、ownerが失敗batchの差替え/破棄を明示して記録する。所有task以外から直接変更しない。再入・二重開始・破棄済みhandleは明示拒否。sample中のstepやcallbackからの変更はbusyとして拒否し、失敗後はguardを解放する。

## 9. API形状の提案（現行endpointではない）

```js
const actor = AyatsuriMotion.actor({rig, stepHz:60, seed:42, capacity});
const reachPoint=[20,0,0], lookPoint=[0,-10,80], deltaV=[0,0,0];
const poseOut=rig.createPoseOutput(), statusOut={}; // One-time owned storage.
const command={targets:{rightHand:reachPoint,gaze:lookPoint},hold:holdIntent,impulse:null};
const kick={part:'torso',deltaVelocity:deltaV}; // Reuse only for a deliberate kick.
actor.enqueue(command, nextStep); // Whole batch copied atomically; hold targets.
// For a disturbance, set command.impulse=kick and enqueue once, then clear it.
// Render retries never enqueue the input again.
actor.step();
actor.sample(1, poseOut); rig.apply(poseOut, scene); // Latest accepted pose in MVP.
scene.evaluate(renderTimeMs); // Existing pure transform/material evaluation.
// The environment owns beginFrame/render/commit/ACK; actor never publishes.
actor.status(statusOut);
```

一つのenqueueが複数goal/hold/impulseの原子batchで、満杯時は全体を拒否する。別々のenqueue全体が一括になるとはしない。名前は構築時にindexへ解決し、毎更新のobject/配列/closure生成を要求しない。optional dynamics moduleとして分離し、WAKE等が既存の小さなtrack samplerを使うために動力学全体をロードしない構成を検討する。

rigの最小宣言例（schema/数値は未固定）は、`actor`にroot位置XY/roll、`face`にyaw/pitch、`floating-hand1`にlocal XYZを対応させる。gaze/rightHandのtargetはworld座標、rightHandはactor基準点`[40,20,2]`からの有限reach、gazeはhead角限界内で解き残差を返す。`holdIntent={part:'rightHand',mode:'precise',strength:1,positionTolerance:0.25}`等を構築時に作る。head角限界、reach半径、動かしてよいtorso/eye自由度とslackはmodelが具体値を宣言する。0.25は試験用提案値で、画質/実機保証ではない。汎用graphを要求せず、この小さいbindingでreference→親の逆変換→local poseの整合を先に確かめる。

共通C核の形は例として以下。型名/ABIは未固定。C/JSが別々の近似式を実装するのではなく、同じ予測・制約核を利用する。

```c
ayatsuri_follow_status ayatsuri_follow_step(
    const ayatsuri_follow_desc *desc,
    const ayatsuri_follow_state *in,
    const ayatsuri_command_span *commands,
    ayatsuri_follow_state *out,
    ayatsuri_follow_work *work);
```

descはgeometry/materialを持たず、最小rigの基準TRS/channel対応/制約を表す。Cはcallerがarenaを所有、JSはinstanceが同等のstorageを事前確保。初版はin/out/workの重複を拒否する。予測・制約の候補は破棄可能なworkへ書き、全検証後だけoutへコピーしてownerがstate bufferを交換する。失敗時workは不定でもin/out不変。異なるbufferのinstanceは独立、同期借用後にpointerをretainしない。statusも新設案であり、既存TRSのenumを拡張済みとしない。入力不正/範囲/容量/過去入力/busy/破棄済み/制約数値失敗を区別し、到達不能の有効poseは成功＋残差で表す。C scene全体の完成やHOME実装を意味しない。

通常のstep/sample/goal更新でallocatorを呼ばないことを要件とする。初期化/明示snapshot/exportは別に計測し、エラーobject生成も通常成功経路と分離。JS bridgeの変換/TypedArray view生成/iterator等も実QuickJSで測り、Cだけalloc0で合格にしない。

## 10. 顔モーフ・身体rig・honeの不足

現在の雲は口/頬/眉/瞳の既存楕円体をTRS/materialで動かしている。顔の輪郭の対応点をmorphする機能、skin weight、局所deformer、拘束付きblend shapeはない。身体は親子TRSとbodyだけの非一様scaleで、一般の骨格skinではない。

動的目標追従の最初の試験には新hone primitiveは不要。手/頭/rootを現在のhandleへ反映できる。一方、輪郭表情を進める場合は次を別に検討する:

- bodyの局所変形: 固定lobe数のposition/radiiを更新するutility setter。元データは作成時コピーで現在は不変だが、native packetには更新結果を表せる。全入力検証→一括更新、正半径、依存伝播/法線再計算、旧新damageとsnapshotを契約化する必要がある。C rendererの新方式が必須という意味ではない。
- 顔輪郭: 固定対応点/同一topologyのbase＋deltaと複数weight、眼/口の開閉と表情の複合補正。geometry変更なしの単純TRSと区別する。weight競合/範囲/補正順を定義する必要がある。
- honeの既存project/fill/outlineで局所3D輪郭を投影する小試験は可能。ただし凸polygon/flat色、1024点、1polygon256頂点、穴6個等の既存制約がある。一般の凹面分割、滑らかな陰影付きの眼瞼/口内mask、任意lit surfaceを提供済みとはしない。必要になれば固定容量の表面/法線更新契約を別途設計する。

tex/2.5Dへの方式変更ではなく、現在3D rigで何を動かせるかと、局所形状を変えるための不足を分けた一覧である。大きな骨格/mesh engineを今のMVPへ一括導入しない。

## 11. 再現・JS/C一致・最小合格試験

再現単位はseedだけでなく、rig/config版、初期q/v/drive状態、fixedHz、step付きgoal/hold/impulse列、pause/resume/rebase列。動的seekは初期状態からの再実行または明示checkpointが必要で、任意時刻を一発評価する既存samplerと区別する。

同一backend/build/profileの再実行は同じstepのstate一致を要求。JS/Cは同じevent境界・目標/拘束の意味・許容誤差で合格を判断する。異なるlibm/target間のtrig/反復解まで無条件bit一致とはしない。共通核/no fast-math/FMA方針とF32丸めを揃え、許容誤差はfixtureに数値で固定してから実装を受け入れる。

最小試験は**注視＋reach → 移動中retarget → 胴体impulse → 到達/保持 → release**。途中pause/resumeを挟み、同じstep入力を異なる描画cadenceで再生する。時刻列は診断用入力であり、完成した姿勢軌跡を期待値にしない。

その前の伝播fixtureは、同じ単一根元driveで硬い棒/柔らかい枝/手先保持を比較し、長さだけ変えた幾何変位、連結だけ変えた遅れ/減衰/反動、保持による残差/補償を別々に記録する。信号生成の候補ごとの費用比較は実装段階に回し、この仕様作業ではbenchしない。

| 検査 | 合格条件 |
| --- | --- |
| 途中retarget/角度境界 | 位置/速度がresetされず、角度は短い側。拘束切替impulseとは区別 |
| 外乱/到達不能/制約 | finite/可動域内、goalは保持、残差と制限状態を報告。矛盾したposeを成功扱いしない |
| 全drive OFF | 呼吸/変動を止め、静止goalへ位置/速度誤差が宣言したεp/εv/εangle以内へ収束し、所定step窓で維持 |
| noise OFF・呼吸ON | 身体は動き続けても、注視/手先保持の残差が宣言誤差内に収まる。全q/vの0は要求しない |
| alive hold/精密保持 | 呼吸中も手先/注視の宣言誤差以内。on時のstate/速度/driveが有界で、release時に緩む |
| 描画cadence/失敗 | 15/30/60等の描画、render欠落/拒否でも同stepのsimulation一致、impulse一度。最後の画像は保持 |
| pause/freeze/replay | 休止時間の一括投入なし、freeze画像不変、同じ初期state/seed/step入力で再現 |
| 所有権/資源 | 多instance分離、再入拒否、不正batch/失敗stepは全state不変。warm step/sampleのalloc0をC/実QuickJS双方で計測 |

ε/収束step数・有界幅・最大substep・channel/constraint容量は**ドッグフーディングで数値を決める項目**。試験前にmodel unitとangle単位で宣言し、目視だけ/epsilon後付けでPASSにしない。hostのJS peak/native free/largest/stack/単一確保、pixel/band負荷とdevice時間は別々に観測。現行c73d26cの160KiB実績を新actor容量の保証にしない。

## 12. 仕様を固める順番と残る判断

まずstate/goal/step/失敗/所有権を固定し、小さなscalar追従C核と単一driveの棒/枝/保持というJS/C同義fixtureを作る工程を提案。次に雲の注視＋自由端点holdと呼吸/外乱、relaxed/precise/releaseをドッグフーディングし、必要な制約だけ追加する。負荷モデル/IK/形状morph/HOME adapterは各々の実需要を確認して進める。

HOMEは将来adapterが同じcontroller/rig契約を使い、C prepareで固定step進行、strip draw中は同じsnapshotを読む形を検討する。drawの帯数でstepを進めない。app開始/scene変更時の停止・世代・state保持か破棄か・資源解放をadapterが所有し、scene_memの解放可能scratchへ持続stateを置いたままにしない。前景UI/入力応答/予算の検証前に共用可能とはしない。

レビューで必要な判断は少数に絞る: (1) 棒/枝/保持から雲へ進める最小境界、(2) spring予測と精密保持の許容誤差/solverを小fixtureで選ぶこと、(3) relaxed/preciseの見え方と呼吸/補償の配分。今回の草案でこれらを実装済み/合意済みとはしない。
