# CLOUD ACT: 喜びと驚きの読み分け

2026-10-30。基準は公開済み`55f025f2063eaf78341daeca968444cfd8bf204b`。[前反復](cloud-quality-v4.md#過去候補の判断と測定)の継ぎ目補正・大小volume・世界光への応答を維持し、今回は表情と彩雲の演技をローカルで改善する。実機は別担当の32a3336のまま、本変更の公開CI/実機検証ではない。新API方式/2.5D/画像パーツは追加していない。

## 同条件比較 — HOST SIMULATION

同じ論理時刻/入力/240×135 native host描画で比較。元RGBを変更せずAPNGへ格納し、301枚のRGB hash/CRC/順序/delayを検証。比較ラベルだけを描画領域外へ追加した。

## 演技の契約と実装

- 気づき: 2.0秒から視線が先行。驚きは2.16〜2.78秒、2.30〜2.47秒に最大。縦長の丸口とピンクの内側、広くなる目、高く上がる眉を組み合わせる。
- 喜び: 2.90〜4.14秒と5.60〜6.75秒。`expression.smile`を喜び、新しいモデル内入力`expression.surprise`を驚きとして分離し、両者の有効区間は重ねない。横に広がる上向きの口角、上がる頬、細くなる目、驚きより低く柔らかい眉で読み分ける。
- 口は既存のmouth/tongueの2面を再利用。喜びでは同じ濃色の2本の丸いstrokeで上がる口角を作り、驚きでは暗いopeningとピンクの内側へ連続変形する。色/opacityも連続入力から評価し、新しいdrawable partは増やさない。
- 彩雲は既存の屈曲と別に、モデル内の0..1入力`wispSpreadCenter/Left/Right`で広がる。中心→左右端を0/100/200msずらし、曲率に沿う位置と幅へ反映。最後の端も7.05秒で基準へ戻る。全演技はtick240以後に厳密な待機状態。

既存bodyの予備動作/重心移動/圧縮伸長/手の遅れは維持し、固定世界光やvolume色を時間で明滅させていない。全入力は時刻から純粋に評価し、非BODY modeでは新入力を0へ戻す。sleepや前frameの累積状態は使わない。

## メモリ失敗からの修正

初稿は追加部品なしでも、部位別pose record/小配列と演技コードが増え、m32/163840 B/強制GCなしでframe92に例外となった（malloc_size最大160804 B）。180000 Bは原因切り分け用の診断だけに使用し、採用上限は上げていない。

既存`handle.place()`がTRSを同期コピーする契約を使い、モデル内の作業用TRSを1組へ統合した。helperは位置XYZ・回転XYZ・倍率XYZの全9成分を毎回埋め、各nodeはscene所有のFloat32領域へコピーする。別部位の古いaxisを残さず、作業用値を後で変更しても提出済みnodeは変わらない。typed offsetやdirty flagをappへ露出する変更ではない。

修正前後の301枚は実PPMから再計算した全RGB hashが一致。表情/部品/照明を削って通したのではない。独立テストでは2nodeへ同じTRSを順に提出後、元配列を全てNaNへ変更しても各nodeの所有値・renderは不変。無効scaleを含む再利用は全成分を書き換える前に拒否し、正しい再提出で復帰できる。

## 検証結果

| 条件 | 結果 |
| --- | --- |
| cloud m32/SSE2、heap163840 B、強制GCなし、全描画reference | 1806 frames、例外/提示失敗0、repair注入・6種range失敗復帰・解放後guest0、PASS |
| 同試験の資源 | compile peak106084 B、QuickJS malloc_size最大152340 B、fixture allocator peak139540 B、最大単一要求load8316/run1571 B。compileは前候補104952 Bより増え、steady側は前候補最大156940 Bより減る。API単体の速度改善とはしない |
| 既存3体 m32、同条件 | 5418 frames、失敗0、OOM切替復帰・6種range復帰・guest0。compile peak101380 B、malloc_size最大151364 B、allocator peak137304 B、最大単一要求load10912/run1571 B |
| ASan/UBSan64 capture | 最終4条件＋BODY301枚、全描画照合、正逆seek・失敗注入後の画像保持/復帰、解放PASS。無制限heapの診断でm32門とは別 |
| 停止/原本 | tick240..300の61枚が同一RGB、first/finalの301枚が全RGB一致、native/app source hashがmanifestと一致、APNG301枚の検証PASS |
| Nodeと独立所有権試験 | app273 commits PASS。驚き/喜びの口形・色・口角/頬/眉/目、時刻の非重複と彩雲遅延、逆順評価/全TRS再利用/不正再利用後復帰を確認 |

原本/manifestは`.cache/cloud-v4-{first,final,fixed-final}/`。ログは`.cache/cloud-v4-{m32-first,m32-final,standard-m32,final,fixed-final,diagnostic-180k}.log`。native renderer/utilityは前候補から変更なし、既存binaryで変更したapp chunkを読む。guest heapの標本をnative free/largest/stackや実機FPSへ読み替えない。

独立目視では縦の驚き口→小さな上向きの笑顔→待機を読み分け、舌の垂れ/口の分離/顔比率の退行は見られなかった。彩雲の広がりと収束、前反復の柔らかな陰影も維持。笑顔は小さい線状で、設定画の開いた喜び口、細い彩雲の細部や雲の散乱には未到達。host画像列のレビューであり、物理LCD/実機再生の受入ではない。

## 過去候補の判断と測定

旧v1〜v3の反復資料はここへ集約した。比較画像/APNGは再生成物のため追跡を終了する。過去候補のソースはGit履歴に残る。以下は各候補当時の観測であり、現在headのCI・実機受入へ流用しない。

### v1: 共有前面と雲専用app

比較元は`e96d577c4e7e0cb298cda2b8a51f39350613eefa`。球ごとの積層から共有前面へ移し、梨形と周縁lobe、顔、手、彩雲、視線先行・squash・上昇・遅れ・減衰settleを組み合わせた。初版では豆/雪だるま状の主形、puff境界、小さな口、直線状の彩雲、密度/透過が未達だった。

独立ellipsoidのopacity調整だけでは内部の境界陰影が消えなかったため、[汎用の前面ブレンド](../api/blended-surfaces.md)を追加した。陰影を固定textureへ焼かず、形状/法線・世界光・視点を入力にする。ambient RGBは青い空の環境光の様式化で、方向光/透過項とは別。物理的な面光源/体積散乱の解法ではない。

新しい雲/演技/geometry codeを3体labへ全部常駐させた初稿はm32/163840 Bで初frame失敗。雲専用appへ切り分け、既存labを保持し、blendも任意moduleとした。共通utilityのforkは作らない。それでもheader費用で余裕が足りず、packed入口で通常楕円体のtyped viewを減らした。shape/部品/上限を削って通したのではない。

packedの互換viewは旧backend/visit初回だけ生成する。独立レビューで部分確保後の再試行不能を発見し、4個をlocalへ確保してから公開するよう修正。各確保点の失敗→再試行をテストした。

### 過去host観測（各候補当時）

#### v1

| 試験 | 結果 |
| --- | --- |
| standard3lab m32、163840 B、強制GCなし、全描画reference | 5418 frames、例外/提示失敗0、OOM切替後再生成・6種range復帰・guest解放後0。compile peak101380 B、malloc_size最大151364 B、fixture allocator peak137304 B、単一要求load10912/run1571 B |
| CLOUD ACT m32、同条件 | 1806 frames、全6mode、停止240/300一致、例外/提示失敗0、range復帰・解放後0。compile peak104424 B、malloc_size最大159148 B、fixture allocator peak144888 B、単一要求load8316/run1571 B |
| cloud ASan/UBSan64 capture | 4条件/逆seek/評価・render・commit注入失敗の旧画像保持と復帰・差分全描画一致・解放後0、PASS。無制限heapの診断試験 |
| BODY連続画像 | tick0..300、301枚。停止240..300の61枚は全RGB hash一致。sample位置の検証で、実機の連続表示やFPS測定ではない |
| packed/native | 旧API画素一致、整列/未整列span、padding無視、caller変更後snapshot不変、型/範囲拒否。blendの連続前面/ambient色/深度/単一alpha/packet全parameter署名/repairと既存ellipsoidをO0/O2 ASan/UBSanで照合 |
| registry/utility | 新appを末尾へ追加。registry129 checks、既存app-control、utility/Node lab、入力plan5testsを確認 |

#### v2

| 条件 | 観測 |
| --- | --- |
| m32/SSE2、heap163840 B、強制GCなし、6mode×301 | 1806 frames、例外/提示失敗0、差分と全描画reference一致、repair注入と6種native range失敗復帰PASS、終了guest0 |
| 同試験の資源 | compile peak104968 B、QuickJS malloc_size最大156956 B、fixture allocator peak143044 B、最大単一要求load8316/run1571 B |
| 3512529の同条件値 | compile peak104424 B、malloc_size最大159148 B、allocator peak144888 B。差はモデル/演技/部品構成も含み、API単体の最適化量ではない |
| 変換回帰 | stretch .84/1/1.19、頭・身体の回転あり。顔/手/各顔パーツの世界軸倍率.9、直交性、胴体のみ幅/高さ変更を確認 |
| 演技回帰 | 正逆seek、入力順独立、彩雲の段階差/反動/有限範囲、tick240/300の厳密な停止。Node app273 commits PASS |
| ASan/UBSan64 BODY capture | 301枚、逆seek/評価・render・commit失敗の画像保持と再開、全描画照合、guest解放PASS。無制限heapの診断でありm32上限の代用ではない |

#### v3

| 試験 | 結果 |
| --- | --- |
| cloud m32/SSE2、heap163840 B、強制GCなし、全描画reference | 1806 frames、例外/提示失敗0、repair注入・6種range失敗復帰PASS、終了guest0。compile peak104952 B、QuickJS malloc_size最大156940 B、fixture allocator peak143028 B、最大単一要求load8316/run1571 B |
| 既存3体 m32、同条件 | 5418 frames、失敗0、OOM切替復帰・range復帰・終了guest0。compile peak101380 B、malloc_size最大151364 B、allocator peak137304 B、最大単一要求load10912/run1571 B |
| ASan/UBSan64 capture | 最終4条件＋BODY301枚、差分/全描画照合、正逆seek・失敗注入後画像保持/復帰、解放PASS。診断は無制限heapで、m32資源門の代用ではない |
| 停止/画像原本 | tick240..300の61枚は同一RGB hash。manifest内のapp source hashは最終ソースと一致、APNG301枚のRGB/CRC/順序/delay一致 |
| native O0/O2 ASan/UBSan | 単一lobe/硬いunion×前/横/後方光の旧画像hash6個一致。cap出入りとbacklight/chord連続性、ambientのみの全画素coverage hash不変、既存snapshot/repair/band/限界試験PASS |
| native分離試験 | 固定32a3336モデル15姿勢、ASan/UBSan PASS。採用コードの全15画像は試作と同じ画素。通常ellipsoidの描画式は変更なし |
| Node | app273 commits PASS。顔/手の世界比率、回転/非一様body変形、seek/割込み/停止の既存回帰を維持 |

ログは`.cache/cloud-final-character-m32.log`、`.cache/cloud-final-dedicated-m32.log`、`.cache/cloud-final-asan.log`、`.cache/cloud-quality-{packed,motion,before,motion-final}/`。source hash/binary hash/dirty情報は各captureのmanifestにある。上記m32値はlazy互換viewの原子的公開修正後に再実行したもの。301枚も修正後のsourceで再生成して全RGB hash一致を確認した。最終head CIは別に確認する。

上記guestの値をnative free/largest/stackへ読み替えない。最大group payload768 Bをコピーするbridge/renderer/walkerとprojection cacheはnative stackを使う。構造体sizeを単純加算してstack peakとせず、実機で別測定する。

### v2: 比率保持と資源門

- actorは一様倍率と剛体移動を所有し、胴体volumeだけを伸縮。顔は胴体表面へ平行移動し、頭の回転と縦横比を保つ。浮遊手も伸縮から独立。単に顔を逆倍率にすると回転時にせん断を生むため、親から非一様scaleを除いた。
- 胴体は8つの大小lobeへ再構成。肩と裾を広げ、頭頂を幅広く奥へ寄せ、前面blendを12 world unitsへ調整。固定陰影textureを足す方式ではなく、法線/陰影/透過近似は形・世界光・視点から評価する。
- 瞳を大きくし、白い身体上で重複していた白色eye-rimを省いた。顔の可読性と通常描画の資源を両立する変更で、heap上限は据置き。
- 彩雲は5つの薄い形に弧と局所傾斜を与える。中心→左右端が0/80/160msの差で応答し、反動と収束を解析的に評価。追加入力`wispCenter/Left/Right`はモデルのローカルY差分。honeに雲固有処理を追加せず、tick240以降は全て0へ戻る。

伸長時に顔が細長くなる問題は変換契約と画像で改善。裾の広がり、彩雲の弧と左右の違いも読める。一方、滑らかな表面、丸い頭頂、青い境界筋、細い彩雲、口の表現幅は設定画へ未到達。体積密度や柔らかな散乱を再現したとはしない。

初稿の追加groupと白eye-rimを残す構成は163840 Bで初frame失敗した。胴体volumeへ直接scaleを適用して余分なgroupを除き、目の構成を見直して上記条件を通した。失敗を隠す強制GCやheap拡大は使っていない。

raw画像・manifest・ログは`.cache/cloud-v2-motion-final/`、`.cache/cloud-v2-motion-final.log`、`.cache/cloud-v2-m32-final.log`。fixtureは既存native binaryを再利用し、app chunkは実行ごとに読む。manifestに各chunkのhashとdirty状態を記録。最終headのCI・製品linkは別に確認する。host時間や静止画像から実機FPS/材質受入を推定しない。

### v3: cap境界の陰影

前面深度のsmooth-maxでは、新しい楕円体capが入る際の重みが有限でも、cap縁の高さ勾配は発散する。その法線を直接混合すると、連続したbody内部が青い筋になる。再現fixtureでは、lobe中心を54.999↔55.001へ動かした際に同じ前面pixelが旧rendererで`0x3186↔0xffff`へ変化した。

複数lobeをblendする場合だけ、陰影法線と透過chordの寄与をcap内の残余`1-q·q`で減衰させ、chordを寄与量で正規化した。修正後は上記2位置とも`0xffff`。幾何深度/輪郭/coverage/単一alpha/描画順/packet所有権は変更しない。単一lobeとblend=0は補正を通さず以前の画素を保持する。

これは[近似陰影法線の契約](../api/blended-surfaces.md)の更新で、厳密なsmooth-max深度の微分法線や体積散乱ではない。低層にcloud専用分岐は追加せず、世界光/視線と形に応答する汎用描画のまま。更新依存や資源上限を増やしていない。

この分離試験は32a3336のモデル/材質を固定した15姿勢で実行。肩/裾の筋が消え、下側の青い陰影が残るため、全体を白くするだけの変化ではない。元renderer/試作/採用コードと入力hashは`.cache/cloud-seam/`の`provenance.json`、`compare.py`、`base.log`、`current.log`で追跡できる。

## モデル・材質・彩雲

材質だけをambient .88/diffuse .28へ寄せた対照案は筋を薄くする一方、中央を平坦にしたためそのまま採用しなかった。最終案はambient .82/diffuse .38、淡い青の環境色、specular0で、形の陰影を残す。腹部の主楕円を小さくし、左右の下部volumeを少し前へ出して、同じ8lobe内で大小/前後差を作った。

彩雲は5つの既存形状を交互の高さへずらし、中心→端の変位を二次曲線で補間、局所回転もその傾きへ合わせた。新しいnodeや時刻依存の乱数は追加せず、前候補の追従時刻/収束は保持。強く曲げて羽根や触角の形へ寄せる方式は避けた。

独立目視では青い継ぎ目/縁の減少、白淡青の連続量感、顔/手の比率保持、薄い層状彩雲の改善を確認。LIGHTのみで陰影位置、CAMERAのみで形の見え方が変化する。連続301枚の選択frameと停止hashを確認したレビューで、物理LCDや実機再生の受入ではない。滑らかな雪/マシュマロ感、頭頂の丸み、単純な口、彩雲の細い繊維状の空気感はまだ不足している。

ログ/原本は`.cache/cloud-v3-{m32,standard-m32,final,native-first}.log`、`.cache/cloud-v3-final/manifest.json`。native再現は`.cache/cloud-seam/{invariants.sh,invariants-base.log,invariants-current.log,current-run.log}`と追跡済み`test_proc_blend_ellipsoids.c`。guestの標本をnative free/largest/stackに読み替えず、計算量や実機msの改善は未測定。公開CI/実機結果は本ローカル候補では未取得。

### 固定候補3512529のCI・実機報告

対象は`3512529b6f47a609b8041833f9c7e8e0bccb6ccc`。同headの72 CI checks成功、[gate run41005709035](https://github.com/dj-oyu/cardputer-adv-pocketjs/actions/runs/41005709035)と製品link成功。CI firmware artifact12955237904のbinは2,497,648 B、SHA256 `b57031e5bdcebd40cc0519995166214e6e5f1155592fb325275130d1a2acfa43`。

実機担当から2026-10-30に受領した連結flowの報告: build06:31:31、flash06:35:13、3領域readbackと起動catalog一致。CLOUD ACT約80秒で6mode/reset/呼吸/STOP/CLOSE、既存CHARACTERの人/狐/雲切替、MEGADEMO60秒（WAKE→WAKE34.404秒）、HELLO→HOME_READYを確認。lease/sessionは解放済み。本記録の編集担当が独立に再flashした結果ではない。

CLOUD/CHARACTER終了時native free207796 B、largest69632 B、JS0。これは終了時の標本であり、実行中最小値やpeakではない。CLOUDの7個の30-frame集計窓のrender値は15.57–108.52 ms。個別frame最大・JS時間・LCD転送時間・FPSへ読み替えず、処理要因は別に測る。seek/pause/stepは正式MCPに露出しておらず実機未試験、capture未取得、物理LCDの材質品質/実機repair/stack peakも未受入。後続の画質変更は別SHAで検証し、このheadの成功を流用しない。

### 固定候補32a3336のCI・実機報告

対象は`32a333698b98db490adbd63644b010d3dab5b2d0`。[同headの72 CI成功](https://github.com/dj-oyu/cardputer-adv-pocketjs/actions/runs/41025820980)と製品リンクを確認。以下は2026-10-30に実機担当から受領した結果で、本記録の編集担当が再flashした測定ではない。後続の陰影修正候補`9e1e6118ead2894f12fe1fbce101f5a8bf5ba060`の実機結果ではない。

build `hone110-build-32-approved-20261030`、flash `hone110-flow-flash-32-approved-20261030`は08:52:24成功、3領域readback。実機buildのBIN SHA256 `424d8a2dd20811044c2d2cf87142ea4adacf8ded5bd7a3c37f060d78de9119f8`、ELF `6e042b70f99461283228004dbdcc8b520670fab3f07639cda0e248ee8fa62e2c`、receipt `87c6647e803dd25aa6aa061671e70915624fa62b63e9dc1f322210496c3b0f10`。CI取得binとはbuild条件/成果物の識別が異なるため、hashを同一としない。

- `hone110-cloud-32-fresh-20261030`: 約73秒、6mode/reset/呼吸/STOP/CLOSE成功。
- `hone110-character-32-switch-20261030`: 人→狐→雲の切替成功。
- `hone110-megademo-32-smoke-20261030`: 60秒、WAKE間34.409秒。
- `hone110-home-hello-32-20261030`: HELLO後HOME_READY。終了時fresh HOME、owner/job/queue/sessionなし、uncertainty=false。

初回app admissionの`catalog_changed`はOPEN/START未送信を確認し、新bootに一致するfresh catalogで回復した。cloud終了時native free207800 B、largest69632 B、JS0。30-frame集計8窓のrender値は17.06–111.63 msで、個別frame最大/JS時間/FPS/実行中最小freeへ読み替えない。実画面captureと物理LCD画質は未確認。この結果を前候補3512529や後続9e1e611の実測と混ぜない。

## 遅いrender窓の切り分け — 基準3512529のみ

Astra担当が3512529のappとnative rendererへhost用の無変更/計数版を用意し、固定15姿勢でASan/UBSan実行。両版の全15画像のpixel FNV32は一致。STILL tick0の全描画では、予算上のlobe訪問33168、被覆hit7514、陰影評価4441、powf4192、描画した固有pixel3886、重複write552。うち7-lobe胴体はbbox4408×7訪問、被覆hit6239、陰影3420。15姿勢全体では訪問32002–35920、陰影4363–4485だった。

これは計算回数の観測で、deviceのmsではない。実機の7個の30-frame窓と同じframe/bandを対応させていないため、render15.57–108.52 msの差の原因は未特定。既存band_cyclesは背景fillとnative band描画、span_cyclesはその処理を含む外側であり加算しない。現段階でPIE採用や高速化済みとはしない。再現用helper/出典hash/集計は`.cache/cloud-workcount/{probe.py,provenance.json,counts.json,counts.csv,base.log,count.log,run.log}`。本変更の8-lobeモデルへこの回数を転用しない。

### 後続方式の未合意事項

別途相談中の画像パーツ/ワープ/陰影層による2.5Dは、着手する場合の最初の対象をキツネとするユーザー指定がある。まだ実装方式の合意ではなく、この変更へ含めていない。次の仕様検討では固有模様と陰影の分離、瞳/顎/鼻先の深度差、マスク境界と回転時の遮蔽、同じ世界光/視点での比較、JS/Cの同じ意味と所有権を検証対象として整理する。
