# 実装laneへの移植後の証拠

共有境界を統合担当が許容し、descriptor不一致の失敗計数修正を指示・許容した後、J1専用laneへ適用済みです。本review branchの旧patch群はレビューの履歴です。実装の現物はDraft PR31 https://github.com/dj-oyu/cardputer-adv-pocketjs/pull/31 、head `8a883c516720bfdc662c029f473f2e53e53c4e5f`、tree `ec8b3410358030cf6a32c83811ab78d206060d81`。baseはPR29/J1 `b561aea1c0798feb28055a00d36681fae81ac58a` です。

旧patch群からの補足: J1既存の診断OFF 5 C objectsの.text完全一致契約を保つため、追加completion API実装をDMA ON時だけコンパイルします。OFFは旧renderer/pocket経路を保持。test driverはONを明示します。最終headで既存生成/OFF object比較、renderer O2/sanitizers、計測4構成を独立レビューで再実行してPASS。未解決指摘なし。

evidence/verification-off.json と verification-on.json はローカルESP-IDF6.0.1のexact clean head receipt。linked mode proof・fixture/owner contractを検証し、ELF/BIN/map/config等のhashを記録。hardware_tested=falseです。
evidence/target-comparison.json はpristine J1とDMA OFF/ONのサイズ・buffer symbol・個別関数compiler stack比較。同一sdkconfig、診断既定ON条件。DIRAM178572B、bss85072Bは3buildで一致。Flash Codeはbase/OFF1558472B、ON1558804B（+332B）。shared3840B、TX7680B、descriptor40Bは同じ。present_frameは240→256B、ON render body1360B+completion wrapper80B。これは個別関数の静的compiler stackで、実行時high-waterや全call-chain余裕の実測ではありません。

evidence/fixture-pair.json はhost detail OFF/ON各3反復の機能比較。host合成時計/経過時間を実機性能とは扱いません。3反復の各windowは120contexts/4975draws、input986b2e6e027af00d/proc c1b0fdbde748d0de/panel cec4b765a2794b14で同一。

最新head CIは別途PR31で監視中: DMA39388485053、detail39388484935、fixed39388485272、HOME39388485388。完了前の全PASS主張はしません。PR30共通capture instrumentation修正はこのlaneへ取り込まず、別工程で扱います。
実機baseline/単独DMA比較、glass/STOP/HOME/実測heap・stackは実機担当01a155b9の工程。既存J0/J1/PR28/main更新・実機操作・merge・配置はしていません。
