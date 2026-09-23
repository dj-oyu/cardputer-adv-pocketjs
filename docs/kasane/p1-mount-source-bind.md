# P1 source capability / bind（2026-09-24）

`view.bind(sourceIndex, {slotName: fieldIndex, ...})` は、現在の `mount`
がC側で登録したnative source **1件の対応表を置き換える**。indexはその
view内だけで意味を持つ0始まりの番号であり、source名や別viewへ渡せる
handleではない。C assetの初期bindingはmount時に自動購読される。

別のC serviceは`pocket_kasane_source_capability(ctx,registry,handle)`で
session-scopedの不透明JS capabilityを明示的に発行できる。
`view.bind(capability,{slotName:fieldIndex,...})`はruntime descriptorを含む
任意のmounted schemaから、そのservice registryを購読する。同じcapabilityを
再bindすると対応表を置き換え、初回bindだけで固定上限4購読の領域を1回確保する。
sourceを使わないmountはその領域を持たない。複数の内部・外部registryも
同じbundleで合成し、payloadはlease中だけ借用する。

bindはsource fieldとschema slotの型、producerの`allow(consumer)`、
他sourceとのslot重複を検証する。提出中のframeがあれば`BUSY`で拒否し、
変化しないmappingは何もしない。binding objectのgetterが再入して
frameを提出した場合も、解析後に再度`BUSY`を確認して変更を拒否する。
成功時は旧・新slotをdirtyとして記録し、
次のowner stepで完全snapshotを再取得する。旧slotは最新のJS baseへ戻り、
新slotにはnative値が入る。providerが一時的に読めなくても旧表示は保持し、
失敗はowner step側で返す。bind自体は描画やproducer acquireを行わず、
hot pathにJSのUI組立てを足さない。

`view.unbind(sourceIndexOrCapability)`は提出中のframeがある場合`BUSY`で拒否し、
それ以外は該当subscriptionを外して旧slotをdirtyにする。次のowner stepで
最新JS baseを表示し、最後の外部subscriptionならlazy領域を解放する。
同じsourceの再bindも可能。serviceがhandleを登録解除・世代更新した場合は、
次の取得で古いsubscriptionを自動的に外し、他sourceを維持してbaseへ復帰する。
提出中ならまずそのframeを表示・確定してから再取得・切り離す。

capabilityは文字列名で探索しない。plain objectは偽造できず、reset後の
旧capabilityは同じregistry/handleに再発行しても失効したまま。C serviceは
registryとproviderを`pocket_kasane_reset()`まで有効に保つ契約である。
`ksn_source_unregister`したhandleへの新規bindは拒否する。古いcapability
recordは新しい世代へ再利用できるがserialは再利用しない。
実際のsystem/serviceがcapabilityを発行する配線、別task producerの
固定snapshotとの接続、実機gateが残り、P1完了ではない。

host限定2 source fixtureの実QuickJS試験では、保留中の変更拒否、field index・
型・認可・slot重複・getter再入の拒否、旧base復帰と新slotへのnative値、同一bindingの
描画skip、reset後の古いview拒否を確認。ASan/UBSanと
`-O2 -fstrict-aliasing`で0失敗、診断OFFのESP-IDF buildもPASS。
静的DIRAMは159,788 Bのまま。実機の描画時間・heap・音声共存は未測定。

外部2 registryと内部2＋外部1 sourceの実QuickJS試験では、独立revision、
後続source失敗時の全pin解放、復旧、JS baseのoverride、期限到来時のbase復帰、
再有効化、認可、偽造拒否、lazy割当の上限とOOM時の無変更、
reset後の旧capability拒否、静的/外部unbindと再bind、後続sourceを保持した
登録解除時のbase復帰、世代更新を繰り返してもcapability枠が枯渇しないことを確認。
source更新を提出した後・表示前にserviceが登録解除した場合も、coreが所有済みの
提出frameを先に表示し、その次のowner stepでbaseへ復帰することを確認。
ASan/UBSan・`-O2 -fstrict-aliasing`で0失敗、診断OFF製品ビルドもPASS。
