# DERBY 最適化の合流点と変更所有者

2026-10-20。実装者向けの固定手順。実機とmainへの適用はこの文書の作成で許可されない。

## 固定するソース

| 段階 | base/head | 扱い |
| --- | --- | --- |
| J0 baseline | PR25 `dc2c5e044429a4904e91564aa7f9cb30d7d4cbbb` | 実機試験中のheadを凍結 |
| J0 native | PR26 `3fafcadbd72ecbd0d799cbe9d29f037ff62c05d5` | J0系列の単独比較専用 |
| J0 batch | PR27 `711f94fa91c6e0d6ba7bc00def75ad686b724c7d` | J0系列の単独比較専用 |
| J1共通比較base | PR29 `b561aea1c0798feb28055a00d36681fae81ac58a`、親は上記PR25 | このDraftの最終CI/review済みheadを受領票へfull SHAで固定する |
| B DMA重なりの供給元 | PR28 `4f8855d134b8f475633b7be1decac6be577a6697`、base main `e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267` | 既定OFF、実機未受入、J1へ必要な差分だけ移植 |
| A plan再利用の供給元 | PR24 `7a9379367516340fd759ecb63264cba1e38483fd` | J1へ必要な変更だけを移植し既定OFF |
| 通常動作基盤 | PR19 `c47084d087faf3376831b821e4c03de87a3495d5` | 通常動作の実機受入待ち。J1と同じbranchとは扱わない |
| native/batchの通常OFF収容 | PR20 `49e09d05a605ea04872220d43a93e54b7647667b` | PR19受入後に再統合する別段階 |

PR25/26/27は同じ親へ直列に積まれたPRではない。26/27と25の共通祖先は
`e93500d172ef235286106fbd86b52f9a118085be`。GitHubの三点compareには共通計測変更も
出るため、表示された全diffを最適化差分として取り込まない。J1へ移す際は二点のtree差分で
native/batchの演算・binding・生成PANだけを抽出する。PR19/20/24全体の機械的mergeも行わない。

## J0 → J1

実機唯一担当は復元計測task `01a155b9-103d-73cb-a7c6-b3435c64f006`。
J0のbaseline正常起動とstrict ON完走を待ち、native/batchの比較を続ける場合もJ0系列で完結する。
J1はホスト/CIで並行準備するが、J0のflash/source/build/reset/USB/MCP jobには触れない。
J1の正常終了は詳細ON/OFF共通とし、最終guest成功return、queue drain、既存stopなし、
最後のturn deadline、session deadline、一度限りのowner出力、STOP/HOME、fatal拒否を必要とする。
J1のheadに変わったらON基準も取り直す。旧headの時間・heap・画像・完走を横流ししない。

固定fixtureは `oval-pan8-390-510-v1`、manifest
`84e0341ee1ac10c232e3008f28f8aada8823155e19229f926e633d7780a2021d`、
seed `9112A7AB`、camera8/tier1、120 completed contexts、1boot内3 dependent reps。
tail最大8steps/frame、250ms watchdog、180秒session、200秒/65536B/1024B collector上限を維持する。
詳細計測API `DD_*` とcoarse `DM_*`、fixture `DF_*` の意味はlaneから変更しない。
既存summaryと詳細stage/eventのschemaを維持し、共通owner完了recordを追加する。
verified build modeとBIN/ELF hash、flash/readback receipt、完全raw/transport receiptを照合して採否を決める。
これはinstrumentation overheadの対照であり、製品純性能やLCDの物理受入ではない。

## J2 laneと所有ファイル

| 所有者 | 専有する変更 | 共有境界・調整条件 |
| --- | --- | --- |
| 合流/終了契約担当（本task） | `main/pocket/derby_fixed_workload.*`、`tools/derby_fixed_workload_chunks.py`、`tools/games/derby_*result.py`、collector/receipt検証、`tools/record_verification.py`、`tools/verify_diagnostic_build.py`、計測CI/本書 | `main/app_session.*` と `main/main.c` の終了箇所のみ。DMA側が同じ箇所を必要とする場合は先に依頼 |
| A validated plan（PR24担当） | `main/ui/kasane/ksn_proc_plan.*`、`ksn_procedural.*`、private execution header、`main/pocket/pocket_proc.c` のplan/state/lifetime | APIはsnapshot互換、cacheのpin/generation/失敗原子性維持。既存`DD_*`境界の移動・意味変更は合流担当と調整 |
| B DMA重なり（task `01a157df`） | `main/hal/board.c` のtransfer/queue/reap、transfer bufferの所有・寿命、関連HAL header | staging buffer再利用前に完了を保証。`TX_SYNC/COPY/SUBMIT/REAP`、present成功/失敗の通知意味を変更する場合は先に合意。終了/fixture/parserは編集しない |
| C native/batch | `main/pocket/pocket_derby.c`、`tools/games/derby_runner_batch.py`、`tools/derby_runner_batch_chunks.py`、native/runner oracle | `apps/derby/derby_pan.js` は原本を直接改変せず生成。nativeとbatchが同じCファイルを触るため同時編集せず順番に担当を割り当てる |
| 合流担当が集約 | `main/CMakeLists.txt`、root `CMakeLists.txt`、`docs/README.md`、共有host harness | 各laneは必要な差分を提示し、同時に直接編集しない。専用builddir/sdkconfigを使用 |

Bの実装baseはmain `e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267`、branch
`exp/kasane-dma-overlap-20261020` に固定する。計測baseの完成を実装停止条件にしない。
既存board APIの成功returnは最終転送完了後とし、異常時にもin-flightをdrainしてからbufferを再利用する。
J1確定後に専用統合branchで計測hookを合わせ、rebase/移植後のheadを再CI・再測定する。

新しいlaneは今は追加しない。AとBは独立の主要ボトルネックを検証でき、Cは既存比較を完了できる。
各laneの実装branchは上表のbaseから先行できる。計測/合流用の専用Draftは、J1確定後にそのfull SHAへ差分を移植し、既定OFFまたは明示flag、変更所有者と依存を記す。
合流候補の条件はhostの描画入力/順序/pixel/race一致、失敗・中断再開・寿命、heap/stack予算、
同一head target link/mode proofと全CI、独立review、単独実機比較。未受入の近似演算を有効化しない。

## J3 integration

J1から `integration/derby-j3-<日付>` を作り初めはDraft。A/B/Cの順番は単独結果とmemory余裕で決める。
PR24の他実験禁止guardは現段階で維持する。組合せが必要なら各単独受入後、J3専用branchで根拠付き変更・再reviewし、lane側で黙って緩めない。
最初に全OFFを確認し、各lane単独ON、二者組合せ、最終組合せONを段階的に確認する。
各段階のtree/flags/BIN/ELF/flash/raw hashを保存し、失敗時はその段階を修正して同一head再CI/reviewする。
最終headで全CI・独立review・実機のwhole-work/render/heap/eval/stack・同一出力・正常終了を確認する。
小差は命令cache/layout交絡を分離した同一バイナリ比較などの根拠が必要。単独改善率を足し算しない。
main向けにはPR19の受入済み通常基盤へ既定OFF差分を整理し、診断ON root設定を持ち込まない。
受入前にmain mergeしない。18bootの試験計画を自動起動しない。既存Spaceページは編集しない。


## PR28をJ1へ移植する際の固定境界

供給元はPR28 head `4f8855d134b8f475633b7be1decac6be577a6697`。実装base main
`e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267` との差分20pathだけを移植候補にし、branch全体をmergeしない。
J1の計測用headはPR29 `b561aea1c0798feb28055a00d36681fae81ac58a` のまま凍結する。
この文書追補のbranchは計測baseではない。J1の最終全CI確認と、移植後headの全CI/再測定は別のgate。

API境界は以下に固定する。既存 `ksn_display_port` のlayoutを変えず、入力pixelはpresent callbackのreturn前に
copyされる。in-flightのTX/descriptorはHALが所有し、未知のcompletionでは隔離して再利用しない。

- `board_present_submit(x,y,cols,rows,pixels)` / `board_present_finish()` — HALのcopied submitと最終drain。
- `ksn_display_completion { ctx, finish }` — reentryやcore変更をしないowner task callback。
- `ksn_render_rects_complete()` → `ksn_runtime_present_complete()` → `ksn_view_host_present_complete()` →
  `pocket_kasane_present_complete()` — renderer completeへ進んだ描画経路と無damageの正常経路ではfinishしてからframe ACK/promotion。上層のinvalid引数/reentry/BUSY早期拒否は既存guardを維持し、他のpresentationのfinishを呼ばない。
- app sessionの `kasane_finish()` — 最終drainをsend elapsedへ加える。`KASANE_DMA_OVERLAP` は既定OFF。

所有の追加範囲はDMA担当が上記completion APIの宣言・伝播・retry/cancel/reset/teardownを持つ。
`main/ui/kasane/ksn_ports.h`、render/runtime/viewの対応header・実装、`main/pocket/pocket_kasane.*`、
`main/app_session.c` のdisplay adapterが共有接点になる。A担当のplan/stateファイルと同時編集しない。
renderのscope、app display adapterの計測hook、`main/CMakeLists.txt` のflag組込は合流担当が調整する。
fixture/controller/END/owner EXIT/collector/schemaはDMA laneから変更しない。

J1には既にsend callbackの `DM_SCOPE(DM_TRANSFER)`、renderer内の `DD_TX_SYNC` と
HALの `DD_TX_COPY/SUBMIT/REAP` がある。PR28をそのまま貼ると最終finishがrender bodyの外へ移り、
区分計測からdrainが落ちる。次を移植時の必須変更として扱い、単独PR28のCIを流用しない。

1. `DM_RASTER` と `DD_RENDER` の全体scopeをcomplete wrapperへ移し、finishとACKまで含める。
2. 新しい `kasane_finish()` の待ちを `DM_TRANSFER` と `DD_TX_SYNC` に含め、`sent_us` に足す。
   残待ちをrender self/JS selfへ誤分類せず、payload/copy byteをfinishで二重加算しない。
3. completion失敗をtransfer failure/present failureへ伝播し、owner完了やACKを先行させない。
   既存TX submit/reapの実call境界とfailure計数はHAL側のまま保つ。
4. raw schema/fixture/seed/capsは維持する。変更が必要な計測意味は先に合意しversion化する。

再試験は全OFFの通常回帰、DMA OFF/ONのboard1700strips・renderer100frames＋60damageケース、
shared poison/descriptor寿命、prior/final/error/timeout/未知completion隔離、retry/cancel/reset、
procedural pendingとlease/cache、repair、最終7rows/full/narrow/polling/in-place切替を含む。
J1のON/OFF固定fixture・全raw fatal/transport/END-ownerEXIT-STOP-HOME・proof負例も通す。
最終headのtarget OFF/ON link、linked mode・BIN/ELF/flash/readback/capture receipt、全CI、独立reviewが必要。

PR28単独では専用CIと関連owner/repair検証、OFF/ON target linkが成功し、親報告は5workflow/12jobs成功。
main baseで静的DIRAM増なし、ON Flash Code +56B。これはJ1とのmemory差や実機改善率ではない。
広い `tools/kasane_contract/run.sh` は32bit sysroot準備 `m32_sysroot.sh` のexit100で未完走。
未実行部分は、環境が用意できたときに完走させるか、同等範囲を通した明示的CI証拠が必要。
単独実機比較はまだで、改善率は未実測。

実機は唯一担当01a155b9が新しいJ1移植headでON基準を取り直し、DMA OFF/ONを単独比較する。
whole workは最終drain込み、bands/bytes/hash、heap/largest/eval/stack、修復、終了、物理panel整合を確認する。
小差はlayout交絡を除いた同一binary A/B等が必要。単独受入前にA/Cとの組合せ採用やmain mergeをしない。
