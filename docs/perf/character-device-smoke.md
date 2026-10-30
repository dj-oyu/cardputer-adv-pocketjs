# hone / ayatsuri: a30b2a2 実機smokeと画質受入計画

## 出典と対象

2026-10-30の実機担当からの報告を転記した記録。対象製品commitは`a30b2a2f8b6a21c3f1c831778f0fe1b80ee8a373`、取り込み済みmainは`7ee19cccd7e276caf15cf37756f68a26256fca32`。CIの[Character materials run 40993423920](https://github.com/dj-oyu/cardputer-adv-pocketjs/actions/runs/40993423920)はhost/firmware成功、[gate 40993423904](https://github.com/dj-oyu/cardputer-adv-pocketjs/actions/runs/40993423904)を含む72 checks成功。firmware artifact IDは12950362196、app binary 2466864 B、SHA256は`BFE100B3C5651AD7368FF82254353EB535FE0746740DBB782E1120F3270293D2`。

実機担当はbuild→flash/readback/boot ELF/catalogの一致を確認したと報告。本記録の編集担当が独立に実機を再測定した値ではない。実機run ID・生ログ/receiptの保存先・個別frameの測定窓は引継ぎに含まれていないため、後続資料で参照を追記する。CIリンクは実機receiptの代用ではない。

## 確認されたことと未受入

| 対象 | 実機担当の観測 |
| --- | --- |
| CHARACTER LAB | 3体×6modeの全18組合せ、有限run2回。BODY停止後の更新安定、切替/reset/呼吸ON-OFF、guest解放を確認 |
| MEGADEMO | 82秒で全周2回、WAKE3回。STOP/CLOSE、ログ上DEGRADE/OOM/panicなし |
| app終了/復帰 | HELLO往復後に新しいHOME_READYとHOME render-buffer取得が成功。終了時free207800 B、JS 0との報告 |
| 時間の観測範囲 | turn 7.24–20.76 ms、render 9.57–62.30 ms。個別scene/frame・採取窓・集約方法の対応が未提示のため、両rangeを対応するframeとして加算せず、percentile/ピーク/FPS保証にしない |

free207800 Bは終了時の一点で、実行中最小free、最大連続領域、最大単一確保、stack peakではない。JS 0は終了後の解放観測で、動作中のguest peakではない。hostの32bit capやmalloc_sizeと加算・比較して実機予算を導かない。

初回app準備は古いcatalogでOPEN前に拒否された。新catalogを照合し、同leaseで再準備した後に上記smokeを実施。元chain履歴はbrokenのままであり、chain全体の成功とは記録しない。終了時にlease/session解放済みとの報告。ここでは再投入・再flashを指示しない。

機能smokeと表示品質の合格は別である。物理LCD上の材質/陰影、設定図への到達、実機での転送repairは未受入。HOME復帰の成功はayatsuri HOME background adapterの実装・検証を意味しない。画像texture、相互落影、物理散乱も未実装のまま。

後続の実機担当確認では、現正式APIはcaptureとapp_sessionを同時実行できず、session終了のSTOPはguestを破棄する。app_session内capture/表示保持handoffがないため、今回のapp画像は未取得。下記の実機capture工程は現APIで実行可能とした手順ではなく後続条件である。broker改修や待ち時間による固定frame推定は行わず、独立して可能なapp時計とhost画像の整備を先行する。

## 次の判断: 画質の再現可能な比較

比較単位はcommit・binary hash・model・mode・sample tick・breathing・固定光/カメラ/背景・capture経路。論理時刻は`tick*1000/30`であり、実時間30fpsを前提にしない。同じ入力をhostとdeviceで評価し、render-bufferと物理LCD写真/動画を別の証拠として保存する。render-bufferは転送前の画素であり、LCD発色・転送・ちらつきを単独では証明しない。

### 現headでできること

1. [app操作](../../apps/character/README.md)でmodel/modeを選択し呼吸OFF、reset。BODYはtick240以後に基準姿勢を保持し、STILLも静止する。静止画像の予備確認はできるが、任意の動作途中tickを外から厳密指定・保持する機能はない。
2. hostの`run_character_app_host.py --ppm <folder>`は3体×6modeのtick `0,60,90,171,240,300`を出す。既定301frame/modeで、ファイル番号は`(model*6+mode)*301+tick`（0始まり）。`native_contact.py`はその一部だけの一覧とする。全連続frame動画を出す機能とは呼ばない。
3. 実機担当の既存の認可されたcapture経路で静止render-bufferを取得し、同時に物理LCDを撮影する。HOME用`tools/capture_home.py`はHOMEへ遷移するため、このappのcapture手順へ流用しない。capture完了を待たず次状態へ進めたり、sleep秒数だけでframeを推定しない。

### a30b2a2時点で必要だった最小修正

後続ローカル版ではapp controllerとhost固定入力captureを[実装・検証](character-capture-host.md)した。以下の提示完了/世代IDによる実機capture同期は依然未実装であり、native commit識別で置き換えていない。

- app所有の試験controllerにmodel/mode/呼吸の明示選択、整数tickへのseek、pause、1tick stepを加える。utilityにtimer/入力/capture依存を入れず、既存の純粋`sample`と評価経路を使う。
- 選択→評価→commit→提示完了→capture可能、を世代IDとsample tickで識別する。失敗時は最後の成功画像を保ち、失敗した新状態のREADYを出さない。連続要求は前の完了/取消を確認し、遅いACKを次要求の成功と誤認しない。具体的な提示完了hookは実コードと実機担当の経路を照合して決め、現APIに存在すると仮定しない。
- capture用状態表示とログにmodel/mode/tick/世代/呼吸を含め、画質比較では文字領域を固定する。保存manifestはsource SHA、binary hash、条件、画像hash、capture時刻/順序を保持する。rendererへの診断用一括書換えは避ける。
- host fixtureへ指定tickの静止/範囲連番出力を加え、deviceと同一sample列を使う。native純粋入力→同一画素、pause中の不変、seek逆順、二重要求、切替直後/終了時の未完要求、失敗/復帰を試験してから実機候補SHAを確定する。

### 受入用sample列と観察点（提案）

各3体・呼吸OFF・BODYのtick `0,60,82,105,128,150,172,195,218,240,300`を静止比較。動作はtick0–300を連続取得し、tick240以後の停止区間で模様/縁/ツヤの泳ぎや残像を確認する。全frame取得により実行速度が変わる場合は診断runとし、captureなしの性能runを分ける。

LIGHT/CAMERA/TEXTUREは各tick0–300を同じ列で比較し、入力が変わる対象以外を固定する。EXPRESSIONは半眼を保つ閉眼と復帰を確認する。人は髪のまとまったツヤ、布のマットさ、肌の局所反射と暖かな縁。狐は白い尾先のbody固定と陰影の独立、マズル/胸/尾のvolume。雲は青い柔らかな陰影、姿勢/光で変わる透過、水平な彩雲との空隙を確認する。

不連続なshade/縁、背景の汚れ、部位の順序誤り、停止後の変化はバグ候補としてframe対と入力を残す。柔らかさ/形の不足は設定図との品質差として分ける。点描/トーン/アミカケは未採用の比較候補であり、採用時には光と固有模様の分離、連続frameのちらつき/モアレ、移動/伸縮/停止を別途比較する。実測前に高速化や画質改善済みとは扱わない。
