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

統合head `891b65271cfc98e372fa4b999c119f4bf2348797` のmesh/Gouraud
ASan/UBSan、Shiki API（75/75）、turn、class registry、board半帯、STRESS
（900 frames）はPASS。firmwareのproject_versionも `891b652` と照合した。
総合contract scriptのC描画・画像・repair・resource等は通過後、MEGADEMO harnessの
初回cache作成漏れで停止した。baseline書込み前のmkdirを補い、
`python tools/kasane_contract/run_megademo_app_host.py` を再実行して200 frames PASS。
後段のgame/m32等を含む総合script全体の完走とは扱わない。
同一headのGitHub Actions runは照合時0件。PRを作ってCIを起動する操作は行わない。

### 追加検証（a44b8ac、2026-11-05）

前段の「後段未実行」は次の結果で更新する。対象は公開head
`a44b8ac5838fd18f62be00b0ce37b7237a4e5720`。production codeの追加修正はない。

- `GAMES_M32=0 bash tools/kasane_contract/run.sh`: entry pointがexit 0。
  MEGADEMO、frame/turn、grid fold/resize/stream、video、LCD CATCH、DERBY
  （camera条件・series oracle・odds校正）、BIG WAVE、末尾のprojection/PIE model/
  counter/UTF-8/C++ header検査まで実行した。
- JITの3検査はtoolchainを `~/.espressif` だけで探すためrunner内ではskip。
  既存のESP-IDF 6.0.1用Xtensa toolchainのassemblerパスを
  `gen_encoding.assembler(path)` に渡す独立orchestrationで補完した。
  encoder 3,590件、vector 11 sections、scalarの45 shipped programsがPASS。
  scalar random 5,000 programsはaggregate内の結果を利用し、補完では繰り返さない。
- m32は未実行。使用する既存cacheにsysrootがなく、最小 `cc -m32` linkも
  `Scrt1.o`、`crti.o`、`-lgcc` 等の欠落で失敗した。helperは不足時にpackage取得を行うため
  呼ばず、追加導入を行っていない。非m32でのexit 0をm32合格と読み替えない。
- fresh contextの独立内部reviewがmain `3ccd8fde` と旧mesh `b10976a3` の実diffを確認。
  確定修正指摘0件。packet cursor/carry、read_rows/span/backdrop、depth damage、
  rollback/retry/ACK/reset、build source/FP設定を調査した。
  別作業領域のASan/UBSan O2試験もPASS。独立syntheticではheader offset
  0/1/31/32/63/64/127/255/511/1000、1〜7行分割、全payload切断位置を確認した。

これはhost検証とpublic code reviewであり、実機動作、m32 layoutでのguest peak、
実機stack/heap peak、cycle/FPS、private modelの表示品質は未確認。
Actions runとcheck-runsはa44b8acで各0件。branchへのpushで動くworkflowはなく、
PRやCI設定の変更による起動は行わない。

### Hostの描画・メモリ追加測定

実機へ進む前の公開synthetic検証として、次を追加した。production codeはed880931
（a44b8acと同一）のまま。private modelや別rendererは使用しない。

```sh
python tools/kasane_contract/run_mesh_memory.py
```

実際のC/QuickJS frame APIで、imageのrow/span/backdrop全画面一致、4/8行と最終3/7行、
色・背景を変えた3cycleのimage IO retry/ACK、別modeのbackdrop IO後の旧committed修復を
検査する。各workspaceでASan/UBSan/LSan成功、reset/runtime破棄後のtracked liveは0。
計測はリンクしたobjectのmalloc/calloc/realloc/free要求を追跡する。

| 64-bit hostの実測 | 1行workspace | 8行workspace |
| --- | ---: | ---: |
| tracked requested peak | 69,152 B | 75,872 B |
| 最後のbackdrop phaseでのtracked live | 64,596 B | 71,316 B |
| snapshot capacity合計 | 2,048 B | 2,048 B |
| depth workspace | 960 B | 7,680 B |
| metadata（host sizeof） | 204 B | 204 B |
| guest memory_used / malloc_size（その時点） | 35,841 / 43,312 B | 35,841 / 43,312 B |
| 全破棄後のtracked live / blocks | 0 / 0 | 0 / 0 |

peakはguest/runtime・native・fixtureの追跡対象要求byteであり、共有libc/sanitizer内部、
allocator overhead、stack、static領域（計測ledgerも含む）を含まない。
guestの表の値はpeakではない。snapshot/workspaceはtracked合計の内数で、重ねて足さない。
S3の静的DIRAM 181,200 Bとは別の測定で、host値と合算して実機fitとはしない。
追加probeは独立read-only reviewも受け、同一色による検出漏れとmode混在の試験不備を
修正後、未解決指摘0件。production修正は不要だった。

クラウドm32の環境準備はAPT設定読取りのOS Permission deniedで停止中。
取得・導入やm32 jobは始めていない。これを迂回せず、m32 layoutの描画・メモリ検証を残す。
性能実機ownerへのready-to-flash指示は保留し、現時点ではread-only readiness/recovery
確認に留める。公開syntheticからの将来の性能測定も、host結果と分離して記録する。

後続の専用公開workloadは [MESH BENCH](../../apps/meshbench/README.md)。
既存のModel/Instanceとapp登録だけを使い、privateデータ・別rendererやbroker API変更を要しない。

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
