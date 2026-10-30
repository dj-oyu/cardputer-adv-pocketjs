# 通常 main への DMA 再提案（2026-10-27）

> **既定 ON（2026-10-30）**: MEGADEMO の実機 A/B（末尾の節）で send が約 5 ms/frame 減り、turn・render・heap は不変だったので、PR #111 で既定を ON にした。OFF は `-D KASANE_DMA_OVERLAP=OFF` で残る。以下の「既定 OFF」は当時の記述。
>
> **比較の状態（2026-10-29）**: source-selected ON 比較の #99 は比較未完で closed / unmerged。既定 ON の実機優位は未確認で、性能不良を理由とする不採用ではない。[終了理由と再比較の条件](../archive/proposals/dma-source-on-pr99.md)。既定 OFF を維持する。


#37 head `578d552384c992cb852822d40e2635e7b317d498` から DMA の copied submit/finish、
ACK 前 completion、descriptor 所有権、window 管理、単一 3,840 B TX payload を抽出。
通常 main `45a06e0` 上で `KASANE_DMA_OVERLAP` は既定 OFF。
描画 buffer/port ABI と失敗時 repair 契約を保持する。
単一 TX payload は OFF/native UI にも適用され、従来の native copy/DMA overlap は失われる。
その時間影響は未測定であり、実機受入が必要。3,840 B BSS 差は layout の事実であり guest 空き増加の実測ではない。

DD 診断 hook/列挙と transport fault/counter tests は保存した。
旧 fixed workload/telemetry/測定出力・typed proof/manifest consumer の全体はこの提案に接続しない。
旧診断 ON 構成、定量的な受入値、過去 artifact は元 #37 の固定 SHA に保存する。
以下は元候補の履歴であり、この再提案の測定結果ではない。

# Kasaneの既存DMAと次band描画の重ね合わせ

## 対象と合流点

現laneのbaseはJ1/PR29 `b561aea1c0798feb28055a00d36681fae81ac58a`（tree `a46db1efcf7121ac9e82c647e00fe4faf251f5ad`）。供給元はmain `e0fe98a8` 基点のPR28実装 `4f8855d1` だが、main全体やPR28 branchはmergeしていない。`KASANE_DMA_OVERLAP` は既定OFF。第2core、worker、framebuffer追加、queue拡大、描画計算の変更は含まない。PR19/20、PR24 validated plan reuse、PR25/26/27 controller splitには依存しない。fixture/controller/正常終了ログschema/collectorは変更しない。

統合担当が共有scope/adapter/CMake/OFF harnessを許容後、専用J1 laneへ移植した。PR28は `6e4b855c` で凍結したまま。J0/J1既存headを更新しない。J1の診断既定ONと全最適化OFFをbaselineにし、単独OFF/ONと組合せ比較を分ける。PR30の共通capture harness修正は別工程で、このlaneには含めない。

参照調査を実際に読み、baseのコードに照合した。

- [最適化調査](https://chatgpt.com/space/page_0ff002295eb08191ac548083270a6316)
- [実行経路・メモリ地図](https://chatgpt.com/space/page_6ad66406ab908191a16824e3e9b49560)

## 重ねる作業

現行boardにはDMA、TX二重buffer、PIE copy+byte swap、連続RAMWRが既にある。Kasaneの同期wrapperだけが各submitの直後にもreapし、次band raster開始前に待っていた。

ON時はband nを既存TXへcopyしてqueueし、shared stripでband n+1をrasterする。次submit前に先行結果を回収し、frame最後にfinishする。copy・byte swap・SPI command順・overlay順は保持する。fullwidth連続RAMWRと狭rectの再windowは既存board実装に委ねる。先行errorはcommandが処理を始める前に返す。

最初の準備と最後のdrain、command境界の待機は残る。共有RAM帯域やISR/schedulingとの競合もあり、改善率は未実測。OFF/ON別buildには命令配置の交絡も残るため、小さい改善を確定する段階では同一binary内比較をJ1の測定契約に追加する必要がある。host testの回数やtarget compile成功を性能測定とは呼ばない。

## 公開APIと所有権

既定OFFでは従来のrenderer/pocket入口をそのままコンパイルし、追加completion API本体はON時だけ含める。J1既存の診断OFF `.text` 比較を維持する。

既存 `ksn_display_port` のlayoutは不変。別の `ksn_display_completion { ctx, finish }` を渡す追加APIを用意する。

- `board_present_submit(x,y,cols,rows,pixels)` / `board_present_finish()` は実験ON時のみ。
- `ksn_render_rects_complete` は通常背景と任意backdropを扱う。
- `ksn_view_host_present_complete` → `ksn_runtime_present_complete` → `pocket_kasane_present_complete` がcompletionを明示的に伝播する。
- 既存APIは残り、既存port initializerやctxを差し替えるprocedural port wrapperへのfield追加は不要。

present callbackは入力pixelsを戻り後に保持しない。shared stripは直後から再利用可能。DMAが参照するのはboard所有のTXとstatic descriptorのみ。追加pixel buffer/heap/workerなし。既存容量はshared 3840 B、TX2本7680 B、inflight descriptor1本。

完了はESP-IDF `spi_device_get_trans_result` のdescriptor返却で判断する。`ESP_ERR_INVALID_STATE`でも当該descriptorが返ればDMA失敗完了なので所有権を解放する。descriptor未返却ならerrorでもTXとdescriptorを保持する。timeoutはfinish内で再待機し、それ以外の未返却errorは新submit・panel commandを停止し、次finishで返却を確認するまでboard資源を再利用しない。アプリのpixelやframeへのborrowはない。永続的なdriver無応答からの復帰は保証せず、既存 `portMAX_DELAY` の制約が残る。

## ACK・異常・終了

renderer本体はprepared ticketを返し、外側でfinishしてから `ksn_core_presented` を呼ぶ。damageなし経路も同じ順序。view resolve、procedural/grid/video/pixel promotionはこの後に進む。

provider/raster/send失敗でもfinishを実行し、先行errorを保持する。最終finish失敗はACKせず、ticketをrepair/retry/cancel可能な状態に保つ。prepare前のinvalid/BUSY拒否では今回の表示workを所有せず、finishもACKもしない。prepared/render経路ではviewがfinish中もpresenting=trueを保ち、再入やapp resetをBUSYにする。empty fast pathのfinishにはこのBUSY保証はなく、全経路でcompletion契約の再入・変更禁止が前提。

STOP/HOMEはownerのpresent復帰後に処理される。通常の復帰時にはdrain済み。返却不明errorでも保持するのはboard lifetimeのstatic TX/descriptorだけで、guest終了時に解放するbufferではない。終了ログ契約を変更しない。

DM_RASTER/DD_RENDERはbodyではなくcomplete wrapperに置き、最終drainとACKを含む。中間drainは既存送信scope、最終finishはDM_TRANSFER/DD_TX_SYNCを一度だけ開き、待機を `display_state.sent_us` に一度加算する。coarse成功件数/bytesはqueue受理時でなくdrain成功時に確定し、pending capture属性もsubmit時に保持する。最終7行失敗時は成功16件/61440byte、失敗1件。再finishでcoarse件数/bytesを二重計上しない。HAL詳細failureは各失敗reap試行を数え、ESP_OKでもNULL/別descriptorならエラー正規化後に一度計上する。payload/copy/submit/reapの意味は維持する。

重なった実時間を除去したCPU self-timeを新設しているわけではない。全体workを主指標にし、render/send各elapsedをSPI純通信時間やCPU使用率と同一視しない。

## 再現可能なhost検証

実機接続不要。

```sh
python3 tools/test_board_present_sync.py
python3 tools/test_board_dma_overlap.py
python3 tools/test_kasane_dma_overlap.py
python3 tools/test_dma_completion_owners.py
python3 tools/test_dma_measurement.py
bash tools/kasane_contract/run.sh
```

board試験は実ソースのreap/command/submit/finish/full/rect関数をfault-injected SPIへlinkする。1700 stripの遅延読出し、shared poison、TX内容/descriptor寿命、full/narrow、最終7行、queue拒否、descriptor返却ありDMA失敗、timeout後完了、返却なしerrorと再submit拒否、retry/cancel/exit、既存polling/in-place-swap切替、固定buffer容量を検証。

renderer試験は同期と遅延copyの100 frame全画素oracle、60件の狭/full damage、通常/zero-damage ACK、provider/send/finish failure、retry/cancel/resetをO2/strict-aliasingとASan/UBSanで検証する。

owner試験は既存のruntime/pocket end-to-end契約の期待値を保持し、新completion API経由で実行する。runtime finish中の未ACKとshutdown BUSY、cache/lease/allocation上限、実QuickJSとnative procedural promotion、repair/teardownを検証する。panelの最終copyはfinishへ遅延する。

CI `kasane-dma-overlap.yml` はexact headのhost契約とESP-IDF v6.0.1 target DMA OFF/ON × detail OFF/ON compile/linkを実行し、ELF/map/sdkconfig/size/SHA、linked mode proof、compiler stack evidenceを保存する。既定OFFでbuildしたCMakeCacheにOFF、ONにはONが記録される。compileは実画面の正しさや性能を保証しない。

## クラウドtarget検証記録

旧main基点PR28実装の記録（J1移植版の数値ではない）: ESP-IDF v6.0.1で既定OFFとONの両方をcompile/link、flash budget checkに成功。同一sdkconfig。OFF/ONともDIRAM 172668 B、.bss 79168 B。ONのFlash CodeはOFF比+56 Bで、静的DRAM増加なし。target ELFのsharedは3840 B、TXは7680 B、descriptorは40 B。build間の配置差を含むので、このサイズ差を性能差に換算しない。

独立レビューでACK順序、descriptor所有権、timeout/error、互換性に実装上のブロッカーなし。補強したowner契約とruntime allocation上限はhost検証の対象。実機性能とglass整合は未受入。

## 実機担当への手順

唯一の実機担当01a155b9へ引き渡す。クラウドlaneはhardware MCP job、USB、flash、reset、資源取得、server変更を行わない。merge/配置もしない。

1. J1確定baseで、他の性能flagを同じOFF条件に固定したDMA OFF/ONを用意。head、全flag、sdkconfig、ELF/map/bin SHAを保存する。
2. 共通controllerとfixture/seed/camera/固定窓・正常終了契約を保持。独立bootを統計単位に実行順を交互化し、最終drain込みworkを測る。
3. bytes/bands/hash、heap free/largest、stack、END/STOP/HOME、captureなし実画面を確認。狭rect/full混在、最終7行、途中failure→repairも含める。
4. 単独受入後、専用integration branchでPR24/native/batch等と組合せる。組合せだけの結果をDMA単独効果としない。

MISO未配線のため、host panel oracleとboard_captureはglassへの電気的到達・表示完了を保証しない。

## MEGADEMO での実機 A/B（2026-10-30）

PR #111 の head `13e5d3e`（既定 OFF）と、同じソースで既定だけを ON にした probe（`7b5a95e`、`main/CMakeLists.txt` の `option` を ON にし、`add_compile_definitions` を `NOT CMAKE_BUILD_EARLY_EXPANSION` で囲んだだけ）を、broker 経由で同じ機体に焼き、MEGADEMO を最初から通しで 1 回ずつ流した（各条件 1 boot、交互化なし。上の手順 2 の統計受入ではない）。`KASANE_PAINT` の 30 frame 窓で読む。

| 実測 | OFF | ON |
| --- | --- | --- |
| `send_ms`（大半の場面） | 6.5〜7.4 ms | 1.4〜3.3 ms |
| CORE の最重窓の間隔 | 1.041 s（約 28.8 fps） | 0.991 s（約 30 fps） |
| 同じ窓の turn / render / send | 15.23 / 10.17 / 6.24 ms | 15.36 / 10.08 / 1.51 ms |
| 評価後の空き / JS | 88,292 / 102,204 B | 88,292 / 102,204 B |

- turn と render は変わらず、転送待ちだけが 1 frame あたり約 5 ms 消える。全場面で同じ傾向。
- 両条件とも error・DEGRADE なし。目視で破綻なし（MISO 未配線なので glass の正しさはソフトでは確かめられない）。
- 単独 boot・1 回ずつの比較なので、15% 未満の差は主張しない（命令キャッシュの配置差）。send の 5 ms はそれを大きく超える。

この記録を根拠に、同じ PR #111 で既定を ON にした（`main/CMakeLists.txt`）。
