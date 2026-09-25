# Kasane の残タスク — 再評価用

これは時系列の開発日誌ではない。各行に「目標」「実装」「実測」「残る判定」を分ける。詳細な数値と証拠の適用範囲は[verification.md](verification.md)、不採用理由は[decisions.md](decisions.md)。縮約した5文書だけを入力にしたAstraレビューを反映した。これは**資料の整合性と次の判定方法の提案**であり、コードや実機の独立再検証ではない。**ゲートの妥当性**と**実装の欠陥**を別々に評価し、過去の固定ゲートを事後変更しない。次回以降の試験条件を改訂するなら、改訂前に対象workload、独立boot数、試験時間/frame数、許容揺らぎ、全runの扱いを決める。

| 領域 | 目標 | 現在 | 残る判定 |
| --- | --- | --- | --- |
| P0 | 比較可能な正しさ/性能基準 | **基準確立済み**。hello、musicの固定ゲートと注入repair試験を定義。 | 最新候補の不合格はP0基準の未完成を意味しない。将来試験の統計手順は別作業。 |
| P1 | アプリ非依存 source と音声共存 | generic bind/typed snapshot/固定pool、decoder配置・heap順序対策を実装。host shimでarena/複数producer・readerを確認。実機の2コア同時arena初回確保は独立boot×2で重複確保の片方を解放し、全mutex回収後backing解放。確保失敗注入→fallback/回復、pool全pin→5回skip→release後再利用も確認。output sourceは停止未確認で旧serviceを保持・boot中BUSY、playback sourceは旧pin中BUSY・release後に回収。wall sourceも借用中の文字不変・旧pin中BUSYをhostで確認し、実機の通常分更新は21画素変化・領域外0。最新の全probe OFF製品imageはbuild/容量と20秒music→Hello smokeを通過。 | 30 Hz可視表示の製品要否、実heap全体のOOM、停止timeout/孤児leaseの実機再現、別task同時publishは別途判定する。 |
| P2 | dirty-nodeで変わった所だけ解決 | **24-nodeの定義済みworkloadでは達成**。依存表と差分解決、CPU利益、画素、注入修復を確認。 | 他画面への適用範囲は別に判断。overlay全画面転送、実GRAM、物理SPI故障をP2の直接合格条件へ混ぜない。 |
| P3 | **native text**公開済みpayload→各core destinationを≤1 copy | **限定したtext経路では達成**。実機1 destination、host複数destination、lease/非表示/pool枯渇を確認。 | 他source型を必須範囲にするなら型と操作を指定。複数destinationの実機は必要性に応じて選ぶ。seek可能音源はP3 copyよりP5統合の課題。 |
| P4 | producer原データ→描画まで全経路≤1 copy | 部分的clone/JS一時copyを削減したが厳格目標は未達。copy/変換/転送の操作的定義と既存計数器の重複範囲を固定。実機45秒のnative textはsource→core 46回/368 B、text bank clone・render scratch copy 0。実music 45秒でもtext clone/render scratch/decode copyは0だが、view decodeが13,359回/708,027 B（約498 B/frame）、命令・metadata等の構造copyが残る。同一診断imageの独立boot×2でview decodeは空括弧差引き154〜155 cycles/回、約5.8〜5.9 µs/frame。QuickJS実装とhost allocator試験ではASCII→C文字列はallocation 0、日本語は1回。旧UTF-8計数はASCIIもcopyに含める表示だったため要求量へ分離した。単純slot COWはborrow pointer契約と2bank予算で不成立。 | 現musicのview decode単独は危険な所有権変更を正当化しない。payload lineageと日本語変換・他の構造copyの実機CPU寄与を測り、利益上限から候補を絞る。小さければ厳格目標は未達・保留。必要な候補だけRAM、断片化、repair、安全性、描画p99で比較。 |
| P5 | 音楽と通常アプリを製品経路に統合 | 通常アプリ5ソースをmountへ移行したが、現行imageに埋め込まれるのはhello・imucal・pet・companionの4件（bridgeは未登録）。deskclock/music overlayもnative mount。music source-only、限定light PATCH、通知・pause・低heap・切替の複合試験まで進んだ。位相分類ありの45秒×2ではsend最大は両runとも通常frame（5,302/5,291 µs）、注入451/455 µs、復旧4,551/4,846 µs。位相分類を分離・OFFにした同条件の独立boot×2でもsend最大5,336/5,358 µs、draw最大9,510/9,415 µs、音声障害0で固定線を通過。静止music helpの注入修復はSPI直前の32,400画素が独立boot×2で一致。全probe OFFの製品imageも45秒music→Hello切替のsmokeを通過（decoder fault/IO ERROR=0）。旧5,506 µsは分類不能の不合格として保持。 | 故障注入付き診断imageの時間値を全probe OFF製品imageの性能と呼ばない。既知duration・SD抜去・物理GRAM等はリスクごとの代表条件を選び、全条件の直積を要求しない。全計画PATCHは現状不採用。 |
| PIE text | binary maskを追加copyなしで8 lane処理し、画素と描画時間を悪化させない | 0/255 port宣言、PIEカーネル、整列/ゼロskip、全256 maskの命令シミュレーションとhost画素A/B。整列日本語診断では描画p99 4,095→3,327〜3,455 µs、全画素一致。実music 45秒の同一binary・独立boot ABBAは音声障害0だが、overlay compute p50はOFF 3,839 / 3,839、ON 3,967 / 3,967 µsで利益なし。既定OFF。 | 現musicでは採用しない。高速化対象を明確化して小ブロックのPIE呼出し等を調べ、改善案があれば画素・音声・性能を再判定する。現時点で製品既定ONにしない。 |
| Framework全体 | 新アプリがKasane C core変更なしで安全にmountできる | 汎用schema/slot/descriptorと低レベルescape hatchはある。通常5ソース（うちbridgeは未登録）が例となる。 | v1で必要なcache/modal、native home/picker/editor、制作スキーマ/生成器、効果を**必須/研究/対象外**に分類。全部を暗黙の完了条件にしない。 |

## 直近の再現セット

詳細な数値・旧不合格の扱いは[verification.md](verification.md)。下表の診断imageと全probe OFF imageを同じ性能試験として比較しない。

| 判定 | image SHA-256先頭・条件 | 入口と証拠 | 結論の範囲 |
| --- | --- | --- | --- |
| P1異常停止の安全側 | 全probe OFF `7E5294D0`。hostでは停止未確認と旧playback leaseを模擬 | `tools/build_kasane_test.sh`→実QuickJS O2統合試験、`.cache/kasane-product-playback-lease-smoke-20260925` | output sourceはboot中BUSY、playback sourceはpin中BUSY・解放後再開。実機では20秒通常再生→Hello正常。実機停止timeout/孤児leaseは未再現。 |
| P1 wall不変snapshot | P0診断 `7CD8A196`、全probe OFF `F7C4F2FB` | `tools/kasane_wall_source_device.py`、`.cache/kasane-wall-source-immutable-20260925` | hostで複数pin中の更新保留、実機で1分後21画素変化・領域外0。孤児leaseの実機再現ではない。 |
| P4 view decode CPU | `26D7A956`、P0とdecode cycle ON、copy計数OFF | `tools/sd_async_compare_run.py`、`.cache/kasane-p4-decode-cycles-20260925` と `-run2` | 独立boot×2、空括弧差引き約5.8〜5.9 µs/frame。P4全copyの上限ではない。 |
| P5音声共存修復 | `048D9F91`、P0/低heap/故障注入ON、send位相OFF | `tools/sd_async_compare_run.py`、`.cache/kasane-p5-gate-no-phase-20260925` | 独立boot×2、固定線と音声障害0。故障注入のない製品性能ではない。 |
| P5修復画素 | 同じ`048D9F91`、静止help・音声なし | `tools/kasane_overlay_repair_pixels.py`、`.cache/kasane-p5-repair-pixels-20260925` と `-run2` | 独立boot×2、各32,400画素差分0。SPI直前でありGRAM読戻しではない。 |
| 全probe OFF通常経路 | 最新`F7C4F2FB`、build・実機smoke済み | `tools/sd_async_compare_run.py --product-smoke`、`.cache/kasane-product-wall-lease-smoke-20260925` | 20秒music→Helloの機能確認。時間分布・pause・修復は主張しない。 |

## Kasane FW v1 の判定範囲（2026-09-25 固定）

これは **Kasane FW の統合判定**であり、端末全体の出荷承認ではない。既にある機能をすべて「必須」にしない。以下の必須範囲は現行の組込みアプリと、利用者が実際に通る画面・操作から選んだ。新しいアプリへ一般化する契約は `architecture.md` のままで、個別アプリ名を core に入れない。従来の P0 固定ゲートは変更しない。

| 分類 | 範囲と理由 | 判定・撤回条件 |
| --- | --- | --- |
| **必須: APP** | メニュー登録済みの hello・imucal・pet・companion の native `mount`、値更新、入力、終了・再起動。表示内容の意味は各アプリが所有する。bridgeはソース移行済みだがimage未収録で、実機必須集合には入れない。 | host の Kasane 契約/O2・sanitizer、実機の4アプリ起動→更新→終了、故障回復6種。hello 180更新は `verification.md` の P0 画素/時間/heap/stack 線。失敗または元画面の破壊は統合を止める。 |
| **必須: overlay** | deskclockの起動・時計snapshot・終了と、musicのSD選曲→再生→pause/resume→終了→Hello切替、再生中のSYSTEM通知post/clear、表示失敗後のrepair。`pocket.overlay` がregion/入力、Kasaneが表示。曲の既知/未知durationは各1例を選び、未用意なら未検証とする。 | deskclockは背景・領域外画素と更新/終了の代表試験。musicは音声障害0、正しい表示・入力、有限時間でのrepair、固定した45秒 P0/16 KiB複合線（`verification.md`）。診断imageと全probe OFF製品imageの結果を分ける。**旧send max 5,506 µs不合格は消さない**。今後のどの実機runでも固定線を超えたらそのrunは不合格として保持し、原因か撤回判断を要する。 |
| **必須: 汎用source/SYSTEM** | 世代・pin付きsnapshot、dirty購読、APP/SYSTEM二層、通知優先とAPP終了後のSYSTEM保持。producerはUI待ちせず、有界BUSY/OOMを返す。 | hostの複数reader/全pin/解放/再利用、実機の通常通知・music共存・低heap。UAF、lease破壊、音声障害、無限待ちは即不合格。未再現の真のheap OOM・停止timeoutは「合格済み」に含めない。 |
| **研究（v1の阻止条件ではない）** | 33 ms可視native source、producer原データから描画まで全経路≤1 copy、binary text PIE、cache/instance、非overlay modal、group opacity・dither等の出荷アプリ未使用効果、制作時schema/生成器。実装/APIやhost試験があっても製品能力を意味しない。 | それぞれ別の画素・CPU・RAM・音声ゲートを先に定義してから採否を決める。33 msを15 Hzに読み替えず、P4の厳格目標も削除しない。overlay modal は現行契約でUNSUPPORTED。 |
| **Kasane FW v1 の対象外** | native home/picker/editorへの移植、frosted backdrop・raster cache・native flex・汎用blur/affine/3D、物理GRAM読戻し保証。既存shell/editorを削除する意味ではない。 | 別の製品要求・容量予算・実機ゲートが決まるまで、Kasane v1の完了をこれらで判定しない。 |

この範囲の**現在の状態は未合格**。hostや診断imageが通った項目と、製品imageで未実施の関所を混同しない。直近の `553f0fd` 統合版（既定YIELD=y、RELOC/OOMPROBE=n）はVM Test262 7,501 pass/194既知fail/退行0、静的DIRAM 158,892 B、実機hello中 free 139,600 B・largest 98,304 B、5周smokeと20秒music→Hello smokeを通した。2026-09-25の同一ソースの追加確認は下表。4メニューアプリの起動・初期frame、deskclockの起動・継続表示・終了、imucal/companion/petの**非破壊的な代表更新・入力**は確認した。6姿勢校正と保存、Petの保存を伴う操作と正常終了、Companionのtimer/wake永続化、既知duration曲は未確認。Petは保存値を変えないためBackせずにresetした。20秒製品smokeを時間分布・repair・pauseの合格へ外挿しない。

| 追加の統合確認 | imageと実行 | 結果と主張の境界 |
| --- | --- | --- |
| host契約 | 同一ソースの `tools/kasane_contract/run.sh` | ASan/UBSan、O2 strict aliasingを含め全項目exit 0。実task/実SPIの代用ではない。 |
| P0 Hello | 診断image SHA256 `583762BE…`、P0/lowheap/repair/notice ON。`tools/kasane_p0_hello_run.py`、`.cache/kasane-v1-merge-gate-20260925/hello` | 180更新・181 frame。turn p99 383、render p99 1279、send p99 895 µs、送信最大7496 µs、heap min116980 B、stack free23692 B。`verification.md` のHello固定線を1 bootで通過。 |
| P0 music複合 | 同じ診断image、`tools/sd_async_compare_run.py`、`music-repair2` と `music-notice`（前記cache配下）、各独立boot・45秒track02・20秒後2秒pause・16 KiB hold・最後Helloへ切替 | repair注入は17帯/64800 Bで39 ms後復旧。noticeはPOST/CLEAR・COMPOSITED 1/0。双方でdecoder fault/underrun/IO ERROR=0、描画/送信/UI frame/heap/stackの低heap固定線を1 bootずつ通過。noticeのdraw max9966 µs、UI p99 12287 µsは上限に近い。正常heapのfloorを低heap runへ要求しない。旧send max5506 µs不合格は残す。 |
| 製品経路の画面 | 全probe OFF image SHA256 `B5D5D68D…`、`tools/app_mount_device_test.py` と `tools/overlay_device_test.py`、前記cacheの `apps-default` / `overlays-default` | hello・imucal・companion・petを起動し各135行LCD capture、hello入力でCOUNT 1。deskclockとmusicは各135行capture、約6秒の継続表示と終了、設定値2への復元。音声再生と時間分布をこのrunから主張しない。 |
| 通常アプリ代表入力 | 同じ全probe OFF image、拡張した `tools/app_mount_device_test.py`、`.cache/kasane-v1-app-input-20260925-r2` と `-r3`（独立boot×2） | imucalはセンサー表示行の変化と再mount、companionは右で別ページ・左で元の見出しへ復帰、petは右で操作/選択表示が変化。各135行LCD capture。最初の「10秒以内にIMUCAL_SAMPLE 1」条件は端末の静止度に依存して不成立だったため、FW回帰ゲートにはしない。PetはEnter/Backを送らずresetし、保存値は変更しない。 |

送信 `max≤5,500 µs` は現時点では**回帰防止の固定線**であって、製品の応答期限としての意味は未確定。新たな製品期限を採用するなら、変更前にworkload・独立boot数・時間/frame数・p99とmaxの役割・全runの扱いを決め、旧不合格を残す。通常の再生/画素/lease/音声の失敗は統計的な許容揺らぎにしない。source鮮度は既存の約1 Hz telemetry を必須とし、33 ms表示は要求が確定するまで研究側とする。

## Astraレビュー後の最小順序（v1範囲固定後）

1. **v1対象を固定する — 完了（上表）。** 必須/研究/対象外を分け、既存の固定ゲートと旧不合格を保持した。新規の製品要件が出たら分類と関所を事前改訂する。
2. **repairの単発超過を切り分ける。** 旧5,506 µsのframeは既存ログでは特定不可。追加した独立boot×2の分類では最大値は通常frame、注入・復旧はそれより短い。この事実から旧runを再分類しない。位相分類をOFFにした独立boot×2の同条件は事前固定線を全run通過した。旧5,506 µsは不合格のまま残す。なお故障注入には診断buildが必須なので、この測定を全probe OFF製品imageの性能に外挿しない。38 msのrepair所要時間は製品許容値未設定の参考値とする。
3. **source/arenaの安全性穴を閉じる。** 真の並行activate、pool全pin、OOM、release/APP終了をhost・必要な実task条件へ分ける。有界BUSY/OOM、deadlock/UAFなし、lease破壊なし、復旧後に再利用可能、音声障害なしを合格条件にする。
4. **v1代表回帰をまとめる。** mount→更新→終了、music既知/未知duration、通知、pause/resume、低heap、repairから故障影響の大きい組合せを事前選定。SD抜去は製品で約束する復旧動作を定義してから追加。通常動作は音声障害0、画素/論理状態一致、既存性能線を確認する。故障注入時は事前定義したエラー通知・停止/復帰・UI応答と有界repairを判定し、意図したIO ERRORを通常動作の音声障害と混同しない。全アプリ×全曲×全故障の直積は要求しない。
5. **P4を別の研究判定で区切る。** copyの操作的定義を先に決める。文字生成、UTF-8 materialize、schema保有、bank clone、render decode/scratch、LCD送信の各操作について「データ複製」「変換」「転送」のどれを数えるか、各回数/bytes/CPU寄与を記録する。現行RAM余裕と実害を測り、改善上限が小さければ未達・保留とする。第3bankや新lease実装を自動的に必須にはしない。

レビュー時点で不足している材料は、v1必須機能一覧、send max 5,500 µsの製品上の意味、現在のcore/arena/stack余裕、sourceの鮮度要求と省略許可、cache/modal/効果の製品対応表、現行image/flags/スクリプト/結果を結ぶ短い証拠manifestである。資料から決められない項目を「実装済み」や「出荷阻止」と推測しない。

## 優先順位を決めるための質問

1. 「描画を劣化させない」はどの製品workloadか。通常musicの全画面送信をP2 dirty-nodeの成否に含めない。音声・低heap・通知・修復の同時条件を、頻度と故障影響に応じて最小の代表集合にする。
2. 33 ms可視 source は製品要件か研究要件か。約1 Hzの数値snapshotと15/30 Hzのアニメーションは同一要求ではない。頻度を下げて見かけ上合格としない一方、不要な30 Hz更新も課さない。
3. P4全経路1-copyを、RAM・複雑さ・p99を悪化させても追うか。まず残copyの発生頻度/bytes/所要時間を測り、性能上の利益の上限を示す。目標を残すことと製品出荷を止めることは別の判断。
4. send max単発6 µs超過の扱いをどう一般化するか。今回の不合格は維持し、今後は分位点・繰返し・worstの役割を事前に再定義する。音声fault、画素不一致、repair破壊は統計的揺らぎとして許容しない。
5. System/cache/modal/将来効果をどこまでKasane FW v1の完了条件にするか。実装済みAPI、実機検証済み能力、制作時の構想を混ぜない。

ロードマップの更新時は各タスクに owner、合格条件、ホスト試験、実機条件、予算増分、撤回条件を付ける。数値がない提案は「速くなる」「0 copyになる」と断言しない。
