# 共有条件の追加確認

この資料は4c359e03の補足です。本体未適用。承認対象は以下の追加ガードを含めて評価してください。

## early BUSY

renderer APIの直接呼出しでcoreがbuildingのとき、KSN_BUSY後にもfinishを1回呼ぶことを再現しました（early-busy-probe.txt、期待0/実際1）。view/runtimeのpresenting拒否は先行returnしますが、下位rendererにも所有権確認が必要でした。

shared-boundary.patchまで適用した後、early-busy-guard.patchを適用してください。prepared ticketが0なら今回の呼出しはdisplay workを所有していないためfinishせずreturnします。provider/send失敗後はprepared ticketがあり、最初のエラーを保持してdrainする既存経路を維持します。

early-busy-test.patchは元のDMA renderer testに適用する補足です。invalid入力およびbuilder BUSYでfinish回数が増えないことを確認します。元testはPR28実装4f8855dのtools/kasane_contract/test_dma_overlap.c、runnerはtools/test_kasane_dma_overlap.pyです。

proposal copyでrenderer O2/ASan+UBSanの両構成PASS、計測harness DMA OFF/ON × O2/ASan+UBSanの4構成もPASS。lane/共有本体sourceは変更していません。

## 全契約exit 1の別環境での確認結果

親threadからWindows読取結果を受領: run39379940014 (head6e4b855c)はDMA比較前、check_equivalence.py:74 instrumentの「derby_view.js: draw line moved」でassert。検索対象const dr=(n,a)=>...H.draw(live[n],a)と実際の9引数wrapperが不一致。lowerは@planなしで素通し。関係6fileはbase e0fe98a8と同一blobとの照合結果。

これは既存base側capture instrumentation追従漏れを示しますが、base再実行は未実施です。全契約PASSとも、base再実行で回帰否定済みとも扱いません。共通harness修正は統合担当へ依頼済みで、このlaneでは重複編集しません。こちらのZIP取得はproxy503で停止したままです。
