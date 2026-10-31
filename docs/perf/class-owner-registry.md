# native class owner: A終了後のC起動assert

[性能文書](README.md) / [CLOUD MEMORY fixture](cloud-memory-fixture.md)

対象基準は診断791c95df。狐顔面の未採用host prototypeと通常OFF復帰差分は別保存し、この修正候補は障害時と同じTRACE/phase-only ON/ONを保つ。新候補のflash・実機再試験はまだ行っていない。

## 実機報告と観測範囲

親の実機担当報告では791のflashは2026-10-31 05:55:11成功、3領域readback一致。同boot `5794774345283764502`で次の順序だった。原本`hone110-791-device-result-20261031.json`、`hone110-791-memory-measurements-20261031.json`、`hone110-791-class-owner-handoff-20261031.json`はこちらで未取得のため、原ログを独立再照合した数値とはしない。

| 順序 | 報告された結果 |
| --- | --- |
| A: local.cloudmem gen2 / protocol run1 | 同guestでcold/warm各63draw完走、START→COMPLETE 4.189/4.082秒 |
| 109018ms | APP_STOPPED、JS0 |
| 109022ms | local.player |
| C: 126532ms / gen4 / protocol run2 | loadとModel作成まで到達 |
| 130112ms付近 | create.model後、`pocket_api.c:562`の`class_owner_count < 16` assertで再起動 |

Cはscene/63draw未到達。新bootへ送られたwarm要求はstale_bootで拒否され未実行。その後のHELLO1→HOMEは正常との報告。これをCの成功や画質受入には数えない。run時間は初期化・待機callbackも含み、63で割って描画FPSとは呼ばない。

| Aの報告点 | guest gu (B) | ESP free (B) | largest (B) |
| --- | ---: | ---: | ---: |
| steady | 125668 | 38488 | 29696 |
| complete | 90656 | 70512 | 24576 |
| warm側の報告点 | 90688 | 70500 | 未取得 |

Aのallocator usable highは118728B、session minimumは31360B、cache heldは3072B。guはQuickJS会計、highはallocator callbackのusable highwater、minimum/largestはESP heap、cache heldはguest会計外であり、同じ量ではない。Cはloaded時gu61496B/free99660Bだけで、A/Cの同phase比較ではない。この実機結果からnative化の節約率や性能差は算出しない。

## 原因: runtime数でなくdistinct owner集合の16枠制約

旧helperは、各surfaceのstatic `JSRuntime *`変数の**アドレス**を16枠の表へ記録する。`pocket_api_reset()`は各owner値をNULLにするが、表とcountを保持して同じstaticを重複登録しない。したがって同じ集合の反復だけではcountは増えず、guest heap leakやrealm数16という意味ではない。プロセス中に初めて使う別surfaceが増えると、過去のguestが終了していても枠を消費する。

現製品のhelperを使うstatic ownerは24種類。

| 登録元 | 数 |
| --- | ---: |
| pocket API hub/token | 2 |
| Kasane通常class | 10 |
| Kasane source capability（遅延登録） | 1 |
| follow actor/drive | 2 |
| Model/Instance | 2 |
| FS / storage / text / workspace / music | 各1 |
| net lease/response | 2 |
| 合計 | 24 |

Aの最小集合14に過去の別ownerが1つ加わった15→CのModelが16→Instanceが17という順は、create.model完了後のassertと整合する。ただし実機で増えたownerの名前/count自体はログがなく、FS・storage・music・source capabilityのいずれだったかは断定しない。単にreset時countを0にしても、同runtimeで24種類を使う場合の16枠不足は残る。

hostが見逃した理由も別である。app hostは全ハードウェアsurfaceをinstallせず、Model専用testは小さい独自表を持っていた。`tools/hostshim/pocket_api_stub.c`は16超過をassertせず追跡から黙って省略し、製品と契約が異なった。

## 修正: static descriptorのactive list

`pocket_class_registry.h`を製品・hostで共有し、各classがzero初期化されたdescriptorを持つ。descriptorはruntimeとnextの2pointerで、登録時の追加heap確保も固定owner枠もない。

- 初使用時にdescriptor自身をO(1)でlink。同runtime再使用ではclass IDを保持し、二重linkしない。
- 別runtimeで再登録するときにだけIDを0へ戻し、そのruntimeからIDを取得する。
- resetは登録された全descriptorのruntime/nextをNULLへ戻してdetachする。過去のownerアドレスを表へ蓄積しない。
- resetでclass IDは消さない。API reset後から旧runtime破棄まで、finalizerが従来のIDでopaque値を読める必要がある。
- FS単独resetは`pocket_api_class_forget()`で安全にunlinkする。直接runtime fieldだけをNULLにしてlistを壊さない。

単一active guest、JS owner task、descriptorがreset/forgetまで生存するという内部C契約。静的class IDを共有する複数runtimeの同時使用を可能にする変更ではない。JS公開APIやshapes packet契約は変更しない。内部C helperのowner引数はdescriptor型へ移行し、製品の全24箇所を再compileする。

host shim、proc weak fixture、Model単独fixtureも同じheader実装を使う。hostだけの容量・黙殺分岐を残さない。クラス生成/プロトタイプ失敗のretryは各surfaceの既存責務で、managed Model/followの`JS_IsRegisteredClass`検査も維持する。

## 回帰と証拠

`python3 tools/pocket/run_class_registry.py`は旧791のhelperとresetの正確なfragmentを固定SHAで保持する。Git履歴の有無に依存せず、修正版も製品wrapper/resetと共有headerを実際にcompileする。無関係なpromise停止依存だけをstubにする。

- 旧コードの1 history条件: Model完了markerの後、Instance17でassertを再現。2 history条件でModel17になる別負例も区別する。
- 全24 owner、256同session、16ずつ使う256歴史アドレス、同runtimeのID保持、同address runtime再利用、個別forget/head/middle/tail、反復reset。
- 実QuickJSでは24 class×20 runtime、reset後480 finalizerが完了。
- source内の全descriptor宣言を収集し、製品ledger24との不一致を拒否する。class追加時にcoverageが黙って古くならない。

これは製品owner集合を使うregistry契約試験と実QuickJS class試験であり、radio/FSなど全物理surfaceを動かす試験ではない。別に実Model/Instanceのconstructorを24個の既存QuickJS class後に実行し、reset→FreeContext→FreeRuntimeの本番順で破棄・OOM復帰を検査する。

`run_class_session_order.py`は実CLOUD MEMORYのA→C→A→Cを同processの4 runtimeで実行し、各sessionで63draw×2、全frame reference、終了guest0を確認する。製品API基盤や全hardware namespaceをinstallするものではなく、full24契約試験と補完関係にある。m32は160KiB、64bit sanitizerは容量受入と分離し、frameごとの強制GCは加えない。

証拠保存先は`.cache/class-owner-regression/`、`.cache/class-session-order/{asan64,m32}/`、`.cache/ayatsuri-model-contracts/{asan64,m32}/`。新候補の実機でA→Cと全phaseのメモリを再測定するまで、実機修復済みとはしない。

## 静的費用

同じ診断構成の旧791 ELF/mapとローカル修正preflightを比較した。owner関連BSS symbolは164→196B（+32B）。24 descriptorとregistry headだけで、登録数に応じたheap確保はない。全DIRAMのmap集計は180052→180068B（+16B）で、symbol単体差とは別の集計である。配置/整列を含む全体差をowner payloadだけの増減と混同せず、ESPの動的free/largestやstack peakの実測へ読み替えない。

修正候補は通常版ではなくON/ON診断版として識別する。別保存した通常OFF復帰差分・狐prototypeをこの障害修正へ混ぜず、実機確認候補と次の製品headをSHAで分ける。

## ローカル修正候補の検証結果

| 検査 | 結果 | 条件・限界 |
|---|---|---|
| 製品helper/reset抽出＋旧版負例 | PASS | ASan/UBSan。旧Instance17 assert、新24/256/reset/forgetを分離 |
| 実Model/Instance＋先行24 class | PASS | ASan/UBSanとm32。4 runtime、192 OOM位置、35注入失敗、最終retained 0 |
| 実A→C→A→C | PASS | 各profile 2 process、各504成功draw/668全参照比較、各session guest 0。m32は163840 B、64bit sanitizerは無制限 |
| 製品診断構成リンク | PASS | ローカルpreflight。TRACE ON / PHASE_ONLY ON。実機再試験ではない |
| 修正版の実機A/C | 未実施 | 791のC失敗を修復後の成功へ読み替えない |

Character materials CIへregistry試験と実session順序試験（sanitizer/m32）を追加する。生成binary・関連source hash・反復protocol同値のguardを成果物へ残す。CI完了は公開headと対応付けて別途報告し、旧headの成功を流用しない。
