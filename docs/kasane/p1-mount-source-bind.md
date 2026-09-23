# P1 mount-owned source bind（2026-09-24）

`view.bind(sourceIndex, {slotName: fieldIndex, ...})` は、現在の `mount`
がC側で登録したnative source **1件の対応表を置き換える**。indexはその
view内だけで意味を持つ0始まりの番号であり、source名や別viewへ渡せる
handleではない。C assetの初期bindingはmount時に自動購読される。

bindはsource fieldとschema slotの型、producerの`allow(consumer)`、
他sourceとのslot重複を検証する。提出中のframeがあれば`BUSY`で拒否し、
変化しないmappingは何もしない。binding objectのgetterが再入して
frameを提出した場合も、解析後に再度`BUSY`を確認して変更を拒否する。
成功時は旧・新slotをdirtyとして記録し、
次のowner stepで完全snapshotを再取得する。旧slotは最新のJS baseへ戻り、
新slotにはnative値が入る。providerが一時的に読めなくても旧表示は保持し、
失敗はowner step側で返す。bind自体は描画やproducer acquireを行わず、
hot pathにJSのUI組立てを足さない。

これは**mount-owned sourceの公開再割当**であり、任意のruntime descriptorが
system/service sourceを見つけて接続できるAPIではない。外部sourceへの
拡張には、登録者が明示的に発行する不透明capabilityと、guest reset後の
失効・provider寿命・consumer認可を先に定義する。文字列名だけによる
グローバル探索は行わない。detach/unbindも未実装なのでP1完了とはしない。

host限定2 source fixtureの実QuickJS試験では、保留中の変更拒否、field index・
型・認可・slot重複・getter再入の拒否、旧base復帰と新slotへのnative値、同一bindingの
描画skip、reset後の古いview拒否を確認。ASan/UBSanと
`-O2 -fstrict-aliasing`で0失敗、診断OFFのESP-IDF buildもPASS。
静的DIRAMは159,788 Bのまま。実機の描画時間・heap・音声共存は未測定。
