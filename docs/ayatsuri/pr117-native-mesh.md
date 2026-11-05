# Native mesh on the Shiki frame path

Base: main merge `51f5febb753e3f0d0950043af0b579119d6fa0f0` (#117 included).
Native implementation source: preserved `4a4d4cf77f6c336971eb271324b34a32b4c2aa01`.
This is an independent integration branch; neither main nor PR117 is modified.

## #122 統合（2026-11-05）

公開main `3ccd8fdef955987e87f55f7076a19adbc177d9d3` を、公開mesh
`b10976a3beb6bc8c963bcc7650b4ab6d4fc285fc` にローカル統合。
mainのLCD driver、4行strip、read_rows、range/carry、O2設定を保持する。
native packetは帯外でも論理entry全体を読み飛ばし、carryにはheaderの位置だけを記録する。
rangeの両端は論理entry境界。truncationは失敗し、payloadを線として描かない。
packet scratchは関数境界に隔離し、通常のShiki range walkに持ち込まない。

imageのread_rows、scaled spanの帯cache、backdropは同じ入口からpainter→depthを描く。
depth snapshotは描画対象frameに対応するcandidate/committedを選択する。
workspaceの契約は1行または8行のままで、4行stripと最終3行も内部の行ループで描ける。
depth-only damage、全submit rollback、image IO retry/ACK、backdrop修復を保持する。
原本・画像・狐データ・private rendererの統合は含まない。

再現（既存のESP-IDF 6.0.1環境、Node 16以上、初期化済み依存を使用）:

```sh
python tools/kasane_contract/run_mesh_depth.py --opt 2
bash tools/kasane_contract/run.sh
python tools/pocket/run_class_registry.py
python tools/kasane_contract/run_pocket_proc_turn_qjs.py
python tools/kasane_contract/run_pocket_shiki_api_qjs.py
python tools/test_board_half.py
idf.py -B build_main122 -D SDKCONFIG="$PWD/build_main122/sdkconfig" build size
```

新規回帰はGouraudの帯外payload、sub-range、4+3行、切れたpacketの拒否と、
image IO retry中のread_rows/span/backdrop一致を含む。
正式buildの静的DIRAMは181,200 B、残160,560 B。これはruntime heap peakや実機fitではない。
生成命令で共通render_bandのown frameは112 B、native ext_entryは896 B。
これらは個別frameであり、呼出し鎖peakやcycle測定ではない。
PR作成・merge・実機操作は行わない。

`pocket.kasane.shiki.beginMeshFrame(color, bytes, work, rows[, surface])`
opens an explicit opaque shared-depth scope. Limits remain 65536 bytes,
8000000 work, 256 records, rows1 or8. Use ordinary `shiki.beginFrame` for
Shiki line plans and native painter packets. Mixing a Shiki draw or painter
packet into a mesh scope rejects. The removed float procedural VM, its JS
namespace and its full-frame scratch are not restored.

Model/Instance evaluation, Hone shade/Gouraud (including 7/8-argument API),
Ayatsuri targets and the explicit material-sidedness pipeline come from4a4d.
Kind5 uses one copied face-side record per nonempty logical mesh. No models,
images, exports, geometry or fixed character assets are included.

The current short frame allocation, direct Shiki tail writes, image_band_buf,
plan-dispose on detach/reset, and JIT behavior remain on the PR117 path.
Depth candidate/committed payloads use separately bounded native ownership;
mark/rollback includes their bytes, records and work. Commit keeps both copies;
ACK swaps them. Image IO failure retains the candidate for retry; backdrop IO
failure repairs from old committed state. Mesh/depth changes invalidate damage
conservatively, including depth-only edits and transitions back to painter.
Reset releases owned storage. The existing class registry is extended with the
validated owner-list implementation to support Model/Instance without a fixed
sixteen-owner assertion; all class users migrate consistently.

CMake includes model/visibility/compact/depth sources and preserves F32 order
with -ffp-contract=off. PR117 security settings, sdkconfig.defaults, partition
layout and workflows are unchanged. Inherited JIT memory-protection settings
require their own product decision; this change does not add security overrides.

## Host checks

```
python3 tools/kasane_contract/run_mesh_depth.py --opt 2
python3 tools/pocket/run_class_registry.py
python3 tools/kasane_contract/run_pocket_proc_turn_qjs.py
python3 tools/kasane_contract/run_pocket_shiki_api_qjs.py
```

The mesh runner uses actual C/QuickJS with ASan/UBSan: import/batch/pack, core
mirrors/sided normals, pinned old codec bytes, side-record RGB/depth equality,
Gouraud/visibility, both submit methods, partial limit rollback, OOM, reset,
image retry/ACK, backdrop repair, depth-only damage and the existing current
Shiki frame suite. It caches unchanged QuickJS objects; no downloads occur.
Broader suites are listed separately and only count as verified when run.
Historical a426 paired legacy captures and private character/Blender comparison
are separate evidence. Synthetic success is not a character quality percentage,
firmware runtime peak, device-fit or hardware/JIT performance claim.
