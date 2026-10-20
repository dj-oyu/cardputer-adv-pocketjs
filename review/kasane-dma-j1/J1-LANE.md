# J1 DMA lane 最終引き渡し

共有境界を統合担当が許容し、descriptor不一致の失敗計数修正を指示・許容した後、J1専用laneへ適用済みです。本review branchの旧patch群はレビュー履歴です。最新実装はDraft PR31 https://github.com/dj-oyu/cardputer-adv-pocketjs/pull/31 、head `b7d257adf8326c41bc982852c072acf64e3fbee5`、tree `3665aebdd654201d7d9f8598e20d98ae8c92ab1b`。依存baseはPR29/J1 `b561aea1c0798feb28055a00d36681fae81ac58a`。既存J0/J1/PR28/mainは変更していません。

## 確定した境界

最終drain+ACKはDM_RASTER/DD_RENDER内。最終待機はDM_TRANSFER/DD_TX_SYNCとsent_usへ一度だけ加算。成功件数/bytesはdrain成功後に確定、strip件数維持、再finishで重複なし。HAL copy/payload/submit/reapの意味を保持。drain失敗時はACK/promotionせず、既存正常終了契約で失敗を扱います。early BUSY/prepare前拒否では別presentationをfinishしません。NULL/別descriptorは失敗計上前にエラー正規化し、隔離と再利用禁止を維持します。

DMA OFFは旧renderer/pocket経路をコンパイルし、追加completion API実装はONのみ。既存の診断OFF 5 C objectsの.text完全一致契約を維持。DMA ON/detail OFFにもno-op detail macrosのheaderを含め、4構成をtargetで検証しました。fixture/controller/collector/終了schema、PR30共通harnessは編集していません。

## 検証結果

最新headの6 workflow / 14 jobすべてsuccess（evidence/ci-final.json）。DMA専用run39389261682はhost1 + DMA OFF/ON × detail OFF/ONのtarget4。全targetでexact head compile/link、linked mode proof、receipt、stack/size、ELF/BIN/map/config/SHAを保存済み。4 artifactのID/digest/linkも同JSONに収録しています。

実board1700strip、NULL/foreign descriptor負例、隔離/再submit拒否/回復、最終7行、copy寿命をO2/ASan+UBSanでPASS。未修正HALで新負例が失敗することを確認済み。renderer100frame全画素oracle+60damage、実QuickJS owner、実adapter/renderer/計測のDMA OFF/ON4構成もPASS。fixture detail OFF/ON各3反復は120context/4975draw、input986b2e6e027af00d/proc c1b0fdbde748d0de/panel cec4b765a2794b14で一致。独立レビューは8a883c5全体とb7追加includeを確認し、未解決指摘なし。

evidence/verification-off.json / verification-on.json はローカルESP-IDF6.0.1、最新clean headのreceipt。evidence/target-comparison.jsonはpristine J1とDMA OFF/ONの同一sdkconfig比較（診断既定ON）。DIRAM178572B/bss85072Bは全build一致、Flash Codeはbase/OFF1558472B、ON1558804B（+332B）。shared3840B/TX7680B/descriptor40Bは同じ。present_frame240→256B、ON render body1360B + completion wrapper80Bは個別関数compiler stackで、実行時high-waterや全call-chain余裕の実測ではありません。全証拠のhardware_tested=false。

## 別工程と残工程

全契約capture instrumentationの修正はPR30 head3a0b0888070e4e316e546bfdbb4f6bdfd95545f9の担当。別工程run39385450215はsuccessを読取確認済みですが、PR31とPR30の合流全契約は未実施です。PR31のCI成功と混同しません。

実機担当01a155b9のみがJ1 baselineを再確立し、単独DMA OFF/ONを同一fixture/flag/configで比較します。head/ELF/BIN/config/hashを保存し、独立bootを単位に順序を交互化、最終drain込みwork・bytes/bands/hash・glass・STOP/HOME/END・実測heap/stackを確認。ホスト合成時計を実機性能とは扱いません。組合せ評価は単独受入後に分離します。

クラウドlaneは実機MCP/USB/flash/reset/実機資源取得/server変更・merge・配置を行っていません。権限/認証拡大もしていません。
