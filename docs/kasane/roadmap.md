# Kasane の残タスク — 再評価用

これは時系列の開発日誌ではない。各行に「目標」「実装」「実測」「残る判定」を分ける。詳細な数値と証拠の適用範囲は[verification.md](verification.md)、不採用理由は[decisions.md](decisions.md)。縮約した5文書だけを入力にしたAstraレビューを反映した。これは**資料の整合性と次の判定方法の提案**であり、コードや実機の独立再検証ではない。**ゲートの妥当性**と**実装の欠陥**を別々に評価し、過去の固定ゲートを事後変更しない。次回以降の試験条件を改訂するなら、改訂前に対象workload、独立boot数、試験時間/frame数、許容揺らぎ、全runの扱いを決める。

| 領域 | 目標 | 現在 | 残る判定 |
| --- | --- | --- | --- |
| P0 | 比較可能な正しさ/性能基準 | **基準確立済み**。hello、musicの固定ゲートと注入repair試験を定義。 | 最新候補の不合格はP0基準の未完成を意味しない。将来試験の統計手順は別作業。 |
| P1 | アプリ非依存 source と音声共存 | generic bind/typed snapshot/固定pool、decoder配置・heap順序対策を実装。host shimでarena/複数producer・readerを確認。実機の2コア同時arena初回確保は独立boot×2で重複確保の片方を解放し、全mutex回収後backing解放。確保失敗注入→fallback/回復、pool全pin→5回skip→release後再利用も確認。output sourceは停止未確認で旧serviceを保持・boot中BUSY、playback sourceは旧pin中BUSY・release後に回収。wall sourceも借用中の文字不変・旧pin中BUSYをhostで確認し、実機の通常分更新は21画素変化・領域外0。最新の全probe OFF製品imageはbuild/容量と20秒music→Hello smokeを通過。 | 30 Hz可視表示の製品要否、実heap全体のOOM、停止timeout/孤児leaseの実機再現、別task同時publishは別途判定する。 |
| P2 | dirty-nodeで変わった所だけ解決 | **24-nodeの定義済みworkloadでは達成**。依存表と差分解決、CPU利益、画素、注入修復を確認。 | 他画面への適用範囲は別に判断。overlay全画面転送、実GRAM、物理SPI故障をP2の直接合格条件へ混ぜない。 |
| P3 | **native text**公開済みpayload→各core destinationを≤1 copy | **限定したtext経路では達成**。実機1 destination、host複数destination、lease/非表示/pool枯渇を確認。 | 他source型を必須範囲にするなら型と操作を指定。複数destinationの実機は必要性に応じて選ぶ。seek可能音源はP3 copyよりP5統合の課題。 |
| P4 | producer原データ→描画まで全経路≤1 copy | 部分的clone/JS一時copyを削減したが厳格目標は未達。copy/変換/転送の操作的定義と既存計数器の重複範囲を固定。実機45秒のnative textはsource→core 46回/368 B、text bank clone・render scratch copy 0。実music 45秒でもtext clone/render scratch/decode copyは0だが、view decodeが13,359回/708,027 B（約498 B/frame）、命令・metadata等の構造copyが残る。同一診断imageの独立boot×2でview decodeは空括弧差引き154〜155 cycles/回、約5.8〜5.9 µs/frame。QuickJS実装とhost allocator試験ではASCII→C文字列はallocation 0、日本語は1回。旧UTF-8計数はASCIIもcopyに含める表示だったため要求量へ分離した。単純slot COWはborrow pointer契約と2bank予算で不成立。 | 現musicのview decode単独は危険な所有権変更を正当化しない。payload lineageと日本語変換・他の構造copyの実機CPU寄与を測り、利益上限から候補を絞る。小さければ厳格目標は未達・保留。必要な候補だけRAM、断片化、repair、安全性、描画p99で比較。 |
| P5 | 音楽と通常アプリを製品経路に統合 | 通常アプリ5ソースをmountへ移行したが、現行imageに埋め込まれるのはhello・imucal・pet・companionの4件（bridgeは未登録）。deskclock/music overlayもnative mount。music source-only、限定light PATCH、通知・pause・低heap・切替の複合試験まで進んだ。位相分類ありの45秒×2ではsend最大は両runとも通常frame（5,302/5,291 µs）、注入451/455 µs、復旧4,551/4,846 µs。位相分類を分離・OFFにした同条件の独立boot×2でもsend最大5,336/5,358 µs、draw最大9,510/9,415 µs、音声障害0で固定線を通過。静止music helpの注入修復はSPI直前の32,400画素が独立boot×2で一致。全probe OFFの製品imageも45秒music→Hello切替のsmokeを通過（decoder fault/IO ERROR=0）。旧5,506 µsは分類不能の不合格として保持。 | 故障注入付き診断imageの時間値を全probe OFF製品imageの性能と呼ばない。duration有無の表示はhostで判定済み。実SDの既知duration曲・SD抜去・物理GRAMは別の音声/SD/物理QAへ分け、Kasane v1のために全条件の直積を要求しない。全計画PATCHは現状不採用。 |
| PIE text | binary maskを追加copyなしで8 lane処理し、画素と描画時間を悪化させない | 0/255 port宣言、PIEカーネル、整列/ゼロskip、全256 maskの命令シミュレーションとhost画素A/B。整列日本語診断では描画p99 4,095→3,327〜3,455 µs、全画素一致。実music 45秒の同一binary・独立boot ABBAは音声障害0だが、overlay compute p50はOFF 3,839 / 3,839、ON 3,967 / 3,967 µsで利益なし。既定OFF。 | 現musicでは採用しない。高速化対象を明確化して小ブロックのPIE呼出し等を調べ、改善案があれば画素・音声・性能を再判定する。現時点で製品既定ONにしない。 |
| Framework全体 | 新アプリがKasane C core変更なしで安全にmountできる | 汎用schema/slot/descriptorと低レベルescape hatchはある。通常5ソース（うちbridgeは未登録）が例となる。 | v1範囲は下表で固定済み。動的`mount({version:1,...})`の汎用性と失敗原子性をhost契約に対応づけ、未試験の組合せだけ補う。cache/modal、native home/picker/editor、生成器、効果を暗黙の完了条件にしない。 |

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
| **必須: APP** | メニュー登録済みの hello・imucal・pet・companion の native `mount`、slot更新・入力反映、APP終了/再mountでのlease安全性。表示内容の意味と永続化は各アプリが所有する。bridgeはソース移行済みだがimage未収録で、実機必須集合には入れない。 | hostのKasane契約/O2・sanitizer、実機の非破壊的な代表入力と故障回復6種、hello 180更新の`verification.md`固定線。6姿勢校正、Pet育成値の保存、Companion alarm/wake設定は**アプリ機能QA**であり、Kasane coreの合格条件ではない。APP終了の一般契約はhost teardownで確認し、Pet保存値を変える実機BackはKasane判定のために要求しない。 |
| **必須: overlay** | deskclockの起動・時計snapshot・終了と、musicのSD選曲→再生→pause/resume→終了→Hello切替、再生中のSYSTEM通知post/clear、表示失敗後のrepair。`pocket.overlay`がregion/入力、Kasaneが表示。duration有無による表示分岐も対象だが、実SDの既知duration曲の調達は必須にしない。 | 既知/未知durationの可視状態は`tools/test_pocket_kasane.c`のhost統合試験、実SDは承認済み未知duration曲で音声共存・UIを判定。deskclockは背景・領域外画素と更新/終了。musicは音声障害0、正しい表示・入力、有限時間のrepair、固定45秒 P0/16 KiB複合線（`verification.md`）。診断imageと全probe OFF製品imageを分け、**旧send max 5,506 µs不合格は消さない**。新runの固定線超過はそのrunを不合格として保持し、原因か撤回判断を要する。 |
| **必須: 汎用source/SYSTEM** | 世代・pin付きsnapshot、dirty購読、APP/SYSTEM二層、通知優先とAPP終了後のSYSTEM保持。producerはUI待ちせず、有界BUSY/OOMを返す。 | hostの複数reader/全pin/解放/再利用、実機の通常通知・music共存・低heap。UAF、lease破壊、音声障害、無限待ちは即不合格。未再現の真のheap OOM・停止timeoutは「合格済み」に含めない。 |
| **研究（v1の阻止条件ではない）** | 33 ms可視native source、producer原データから描画まで全経路≤1 copy、binary text PIE、cache/instance、非overlay modal、group opacity・dither等の出荷アプリ未使用効果、制作時schema/生成器。実装/APIやhost試験があっても製品能力を意味しない。 | それぞれ別の画素・CPU・RAM・音声ゲートを先に定義してから採否を決める。33 msを15 Hzに読み替えず、P4の厳格目標も削除しない。overlay modal は現行契約でUNSUPPORTED。 |
| **Kasane FW v1 の対象外** | native home/picker/editorへの移植、frosted backdrop・raster cache・native flex・汎用blur/affine/3D、物理GRAM読戻し保証。既存shell/editorを削除する意味ではない。 | 別の製品要求・容量予算・実機ゲートが決まるまで、Kasane v1の完了をこれらで判定しない。 |

この範囲の**現在の状態は未合格**。hostや診断imageが通った項目と、製品imageで未実施の関所を混同しない。直近の `553f0fd` 統合版（既定YIELD=y、RELOC/OOMPROBE=n）はVM Test262 7,501 pass/194既知fail/退行0、静的DIRAM 158,892 B、実機hello中 free 139,600 B・largest 98,304 B、5周smokeと20秒music→Hello smokeを通した。2026-09-25の同一ソースの追加確認は下表。4メニューアプリの起動・初期frame、deskclockの起動・継続表示・終了、imucal/companion/petの**非破壊的な代表更新・入力**は確認した。Petは保存値を変えないためBackせずにresetした。6姿勢校正/育成値保存/timer設定と実SDの既知duration曲は未確認だが、上表の責務境界によりKasane v1の阻止条件にはしない。20秒製品smokeを時間分布・repair・pauseの合格へ外挿しない。

| 追加の統合確認 | imageと実行 | 結果と主張の境界 |
| --- | --- | --- |
| host契約 | 同一ソースの `tools/kasane_contract/run.sh` | ASan/UBSan、O2 strict aliasingを含め全項目exit 0。実task/実SPIの代用ではない。 |
| P0 Hello | 診断image SHA256 `583762BE…`、P0/lowheap/repair/notice ON。`tools/kasane_p0_hello_run.py`、`.cache/kasane-v1-merge-gate-20260925/hello` | 180更新・181 frame。turn p99 383、render p99 1279、send p99 895 µs、送信最大7496 µs、heap min116980 B、stack free23692 B。`verification.md` のHello固定線を1 bootで通過。 |
| P0 music複合 | 同じ診断image、`tools/sd_async_compare_run.py`、`music-repair2` と `music-notice`（前記cache配下）、各独立boot・45秒track02・20秒後2秒pause・16 KiB hold・最後Helloへ切替 | repair注入は17帯/64800 Bで39 ms後復旧。noticeはPOST/CLEAR・COMPOSITED 1/0。双方でdecoder fault/underrun/IO ERROR=0、描画/送信/UI frame/heap/stackの低heap固定線を1 bootずつ通過。noticeのdraw max9966 µs、UI p99 12287 µsは上限に近い。正常heapのfloorを低heap runへ要求しない。旧send max5506 µs不合格は残す。 |
| 製品経路の画面 | 全probe OFF image SHA256 `B5D5D68D…`、`tools/app_mount_device_test.py` と `tools/overlay_device_test.py`、前記cacheの `apps-default` / `overlays-default` | hello・imucal・companion・petを起動し各135行LCD capture、hello入力でCOUNT 1。deskclockとmusicは各135行capture、約6秒の継続表示と終了、設定値2への復元。音声再生と時間分布をこのrunから主張しない。 |
| 通常アプリ代表入力 | 同じ全probe OFF image、拡張した `tools/app_mount_device_test.py`、`.cache/kasane-v1-app-input-20260925-r2` と `-r3`（独立boot×2） | imucalはセンサー表示行の変化と再mount、companionは右で別ページ・左で元の見出しへ復帰、petは右で操作/選択表示が変化。各135行LCD capture。最初の「10秒以内にIMUCAL_SAMPLE 1」条件は端末の静止度に依存して不成立だったため、FW回帰ゲートにはしない。PetはEnter/Backを送らずresetし、保存値は変更しない。 |

送信 `max≤5,500 µs` は現時点では**回帰防止の固定線**であって、製品の応答期限としての意味は未確定。新たな製品期限を採用するなら、変更前にworkload・独立boot数・時間/frame数・p99とmaxの役割・全runの扱いを決め、旧不合格を残す。通常の再生/画素/lease/音声の失敗は統計的な許容揺らぎにしない。source鮮度は既存の約1 Hz telemetry を必須とし、33 ms表示は要求が確定するまで研究側とする。

## ここからの実行順序

以下は**新しい計測の依頼リストではない**。まず既存の証拠で閉じ、コードの欠落が見つかった箇所だけ直す。各作業は完了・保留・不合格のいずれかで終える。「実機でまだ全故障を見ていない」を無期限の作業にしない。

| 順番 / owner | 実作業と成果物 | 完了条件・実機を使う条件 |
| --- | --- | --- |
| **K1 source寿命 — 完了** / `ksn_source_pool`、`pocket_*_source`、mutex arena | APP停止、reader pin、unregister拒否、次session open、pool全pin、arena初回競合・確保拒否を既存試験と照合。clock sourceのpin中reset→新規BUSY→解放後再作成が未試験だったため実QuickJS host試験を追加した。registryはUI owner task、別task producerは同期済みpoolを使う契約を`ksn_source.h`に明記した。 | 下のK1証拠表によりv1のfail-closed契約を確認。実行時コードは変更なし、COM3再計測なし。真の全heap OOMや強制task停止timeoutは未再現として残し、合格済みとは呼ばない。source実装を変更する場合だけhost全契約＋影響する実機代表条件を再実行。 |
| **K2 汎用mount境界** / `ksn_schema*`、`pocket_kasane` | 既存`test_pocket_kasane.c`のアプリ名を知らない`mount({version:1,...})`、slot型/容量/不正入力のatomic拒否、source購読、APP/SYSTEM分離を対応表で確認する。欠けた組合せだけhost testを追加し、coreにアプリ別分岐を足さない。 | 新しい定義をcore変更なしでmount→更新→disposeでき、不正入力は部分反映せず、SYSTEM確定状態はAPP teardown後も残る。既存4アプリ・overlayの実機画面と代表入力を利用。schema/renderer実装を変えた場合に限り画素一致・Hello P0・該当画面の実機smokeを再実行。 |
| **K3 v1判定の凍結** / `docs/kasane` | K1/K2の合否、image SHA/flags/スクリプト/ログを1枚の証拠表に結び、各行を**hostのみ・診断実機・全probe OFF**に分類する。旧send 5,506 µs不合格も独立行で保持する。 | 必須行に未判定がなく、新しい既知不具合もないならKasane FW v1を完了とする。既存のHello 180更新、music 45秒複合、画面capture、音声0障害を再利用。現行コードのまま「もう一回測る」はしない。変更後に必要な固定ゲートだけ実行し、どのrunも超過したらそのrunを不合格として残す。 |
| **K4 研究の分離** / P4・PIE | v1の完了判定とは別に、全経路1-copy・可視33 ms source・text PIEの採否を個別issue化する。P4はpayload lineageと残copyのCPU/RAM利益上限を先に定義し、PIEは現music既定OFFを維持する。 | 利益上限が小さければ「未達・保留」で終了。所有権・bank数を増やす実装は、利益、UAF安全性、画素、音声、heap、p99の事前比較条件が揃うまで着手しない。製品v1をこれらの研究待ちにしない。 |

**別トラック（Kasane coreの阻止条件ではない）:** IMUの6姿勢校正・保存、Pet育成値の保存、Companion timer/wake永続化、実SDでの既知duration曲、SD抜去後の製品復旧仕様。これらをアプリ/音声/SDのQAとして扱い、表示契約に関係する分岐はhostの既存試験を使う。物理GRAM読戻し、非overlay modal、native home/picker/editor、未採用効果もv1へ戻さない。

K1の証拠境界（2026-09-25）: pool全pin・2 producer/2 reader・再利用は`test_source_pool`、APP/SYSTEMのpartial IO・100回teardownは`test_app_teardown`、arenaの同時activate・予約中deactivate・確保拒否は`test_mutex_arena`。実QuickJS/ASan/UBSan `tools/build_kasane_test.sh` → `.cache/test-pocket-kasane-k1` は`PASS: 0 failure(s)`で、output sourceの停止未確認→boot中BUSY、playback sourceの旧pin→BUSY→解放後再open、wall sourceの旧pin中reset→BUSY→解放後再openを含む。意図的なboot寿命保持があるためこのhost実行のLeakSanitizerだけをOFFとし、ASan/UBSanはON。実機のarena 2 core競合・確保失敗注入・pool全pin＋45秒MP3・16 KiB低heap＋音声は`verification.md`の既存ログを使い、全heap OOM/実task停止timeoutへ外挿しない。

旧repair runのsend 5,506 µsは分類不能の不合格として保持する。後続の独立boot×2と今回の統合診断runが固定線を通過した事実で旧runを書き換えない。`max≤5,500 µs`を製品応答期限へ昇格させる場合は、変更**前**にworkload・独立boot数・時間/frame数・p99とmaxの扱いを決める。通常の画素不一致・音声fault・lease破壊は許容揺らぎにしない。
