# descriptor不一致時の失敗計数修正候補

本体未適用・共有境界承認待ち。37830caの資料に追加する候補です。

## 原因と修正

tx_reapはSPIの戻り値でDD_TX_FAILURESを計上した後、DMA ON時にdone != &tx_pendingをESP_ERR_INVALID_STATEへ変換していました。このためSPIがESP_OKを返すNULL/別descriptorの経路だけ詳細失敗件数が0になる漏れがありました。

descriptor-accounting.patchはエラー正規化をDD_TX_FAILURESより前へ移動します。既定OFFでは正規化をコンパイルしません。tx_inflightは引き続き自descriptor返却の有無で決め、未知descriptorの場合TX/descriptorを隔離します。copy/payload/submit/reapのeventの意味、app側drain/sent_us/ACKの経路は変更しません。

DD_TX_FAILURESは失敗したreap試行ごと、coarse dm_transferはpending stripの成功/失敗を一度だけ計上します。未知descriptorの再drainが失敗しても、coarse失敗やbytesは再加算しません。回復後のfinishを過去の成功として再計上しません。

## 適用順

元のcore-prerequisite.patch → shared-boundary.patch → early-busy-guard.patchの後、このdescriptor-accounting.patchを適用します。shared-harness.patchは独立です。

追加patch内のtest_board_dma_overlap.py差分の基点はPR28実装4f8855dの同名test、test_dma_measurement.cは最初のreview資料版です。適用を簡単にする場合は、本ディレクトリtools/下の更新済み原本を本体tools/の同じ相対pathへcopyしてください。その場合test差分を重ねて適用せず、patchのHAL部分だけを使います。

## 回帰試験

- 更新harnessを未修正HALに適用すると、最初のESP_OK + NULLでDD_TX_FAILURES==before+1のassertが失敗。descriptor-before-fix.logに記録。
- 修正HALの実関数を抽出してO2/ASan+UBSanで各PASS。既存1700stripと追加3経路（OK+NULL、OK+別descriptor、error+別descriptor）を検査。
- 各経路でfailure/reap試行数、TX/pending descriptor保持、再submit/commandの進行なし、payload/copy/submit不変、自descriptor返却後の回復、再finishで追加failureなしを確認。
- 実app helper + renderer + derby_measure.cの計測harnessはDMA OFF/ON × O2/ASan+UBSanの4構成PASS（descriptor-measurement.log）。
- 追加unknown descriptor試験では2回の失敗drain後も借用を保持し、自descriptor返却で回復。coarse failure1 / success0 / bytes0、detail failures2 / reaps3、payload/copy各3840byte / submit1、sent_us111（合成時計37×3）を確認。
- 合成時計は契約試験用で、実機性能値ではありません。

target compile/linkと最新lane CIは承認後の工程です。本資料branchのCI成功や全契約PASSは主張しません。全契約harness修正は統合担当の領域として編集していません。実機操作、merge、配置は行っていません。
