# J1 DMA共有境界のレビュー資料

これは適用承認用の資料commitです。本体ソース、既存PR28/J0/J1のheadには適用していません。

基点: PR29 J1 `b561aea1c0798feb28055a00d36681fae81ac58a` (tree `a46db1efcf7121ac9e82c647e00fe4faf251f5ad`)。
供給元: PR28実装 `4f8855d134b8f475633b7be1decac6be577a6697`。main全体のmergeではありません。PR28は `6e4b855cb1105077d6e203eaf6368e62cb8cf6a1` で凍結中。

## レビュー対象と適用順

1. `core-prerequisite.patch`: HAL、completion API、renderer/runtime/view/pocketの所有権と終了処理。J1そのものに適用する前提差分。
2. `shared-boundary.patch`: 上記適用後への差分。main/CMakeLists.txtの既定OFF flag、main/app_session.cの送信adapter、ksn_render.cの計測scope移動。
3. `shared-harness.patch`: tools/test_derby_fixed_integration.pyで既定OFFの#ifdef/#ifndefだけを明示的に正規化する2行。schema/fixture/期待結果は変更しません。
4. `tools/` の2ファイルは提案を検証した追加harnessの原本。本体tools/へ同じ相対pathで配置し、共有patch適用後に `python3 tools/test_dma_measurement.py` を実行します。本資料branch上での実行は想定していません。

共有編集はintegration ownerの確認待ちです。このbranchをmergeせず、合意した差分だけを別J1 DMA laneへ適用します。

## 完了と計測の契約

- 既存DMAとTX bufferを使い、submitはreturn前にTXへcopy。前回転送の完了前にTX/descriptorを再利用しません。
- render bodyからcomplete wrapperへDM_RASTER/DD_RENDERを移し、最終finishとACKの順序を含めます。
- kasane_drainはscope/sent_usを持ちません。pending bytes/captureを完了時に一度だけ計上します。
- 中間drainは既存送信scope内、最終finishはDM_TRANSFER/DD_TX_SYNCを一度だけ開き、sent_usを一度だけ追加します。
- 最終7行が遅延失敗した場合、成功は16回/61440byte、失敗1回。queue時に成功17回/64800byteを先行計上しません。
- 最終drainでpayload/copy bytesを再加算しません。最初の失敗を保持し、失敗時にはACK/native promotionしません。
- controller/schema/fixture/正常終了契約は変更しません。第2コア化、全面描画方式変更、buffer増設はありません。

## 検証済みの範囲

- 提案copyで実adapter helper + renderer + derby_measure.cをリンクするfake clock harness: DMA OFF/ON × O2/ASan+UBSanの4構成PASS。
- 最終待機scope、late failure、前回完了失敗、queue拒否、capture lifetime、繰り返しfinish、self時間/byte重複なしを確認。
- 狭いOFF正規化を適用した比較は凍結済みapp/mainソースと一致。
- HALの実source 1700strips、rendererの100frame+60damage、実QuickJS owner completion試験PASS。
- J1既存completion 2/2、fixed integration 7/7 PASS。fixture OFF/detail ON各3rep: input986b2e6e027af00d/proc c1b0fdbde748d0de/panel cec4b765a2794b14、120context/4975drawsで一致。
- 独立レビューで最終失敗計上とunused capture guardを修正後、4構成とOFF比較を再確認。既知の未解決指摘なし。
- pristine J1のtarget compile/linkのみ完了。移植laneのtarget OFF/ON、最新head CIは共有編集承認後です。
- 元PR28の実装target/既存5workflowは成功。追加full-contract workflowは公式m32準備成功後exit1、全契約成功とは扱いません。run39379940014/artifact12244942939。ZIP取得はproxy CONNECT503で停止し、失敗箇所とbase/回帰分類は未確定です。

## 残工程

共有編集の合意後にJ1 DMA laneへ適用し、host契約・target OFF/ON・CI・独立レビューを新headで完了します。実機は担当01a155b9のみ。J1 baselineを再確立してDMA単独OFF/ONの同一workloadを比較し、最終drainを含む全体時間、raster/self、transfer、copy、submit/reap、heap/stack、frame hash、end/owner契約を保存します。ホストfake-clock値を実機性能とは呼びません。merge/配置は行いません。
