# J1 DMA単独比較の検証条件

2026-10-20。専用検証branch `integration/derby-j1-dma-contract-20261020` の契約。
この文書の作成やCI成功はmain merge・実機配置の許可ではない。実機操作は唯一担当の別工程とする。

## 固定した供給元と合流範囲

| 役割 | full commit | 取り込む範囲 |
| --- | --- | --- |
| J1単独基準、PR29 | `b561aea1c0798feb28055a00d36681fae81ac58a` | controller/fixture/owner正常終了を含む比較基準 |
| J1 DMA、PR31 | `b7d257adf8326c41bc982852c072acf64e3fbee5` | 製品コードはこのtreeをそのまま保持 |
| 共通capture修正、PR30 | `3a0b0888070e4e316e546bfdbb4f6bdfd95545f9` | capture module・回帰test・全契約script・CIの4ファイルのみ |

PR30の親mainをmergeしない。PR31の製品treeへ共通テストを移植し、CIの対象だけをJ1基準と
今回のcombined exact headへ変更する。追加は検証CIと本書/索引。元PR29/30/31、J0、mainのheadは変更しない。
新しいheadのfull SHA/treeはPRと各CI receiptに固定する。source branch名だけで画像を採用しない。

## 同じ新headのクラウドgate

- 全契約CIは `GAMES_M32=1` で省略せず実行。J1基準には同じ3ファイルのtest overlayのみを適用し、
  source/harness SHA・overlay tree/patch/hashを保存する。combined jobは今回のexact headをcheckoutし、
  overlay後treeがHEAD treeと一致することも確認する。既存画素/IR比較の期待値は変更しない。
- targetは `KASANE_DMA_OVERLAP=OFF/ON` × `POCKET_DERBY_DETAIL_MEASURE=OFF/ON` の4構成。
  全jobが今回のPR headをcheckoutし、builddirとsdkconfigを各job内で分離する。
- 既存mandatory diagnostic proofに加え、DMA cache選択とlinked submit/finish symbolsを照合し、
  `dma-mode-proof.json` にsource full SHA、両mode、cache hash、actual BIN/ELF bytes/SHA-256を保存する。
  これは既存collector/owner schemaを変えるものではない。
- descriptor未返却・遅延失敗・重複finish・capture寿命・early BUSY・retry/repair/teardownと、
  入力/画素/fixture/正常終了のhost gateを同じheadで確認する。全CI後も独立reviewを必要とする。

PR31の既存CI、PR30の旧main/旧DMA全契約成功を新headへ流用しない。DIRAM差0、ON Flash Code +332 Bという
PR31の値も参照値であり、今回の4構成のartifactから改めて確認する。host時間は実機性能ではない。

## 実機比較の対照

各行でdetail mode Mを固定する。M=OFFとM=ONは別の対照系列として扱い、混ぜて改善率を計算しない。

| セル | 製品source | DMA | 目的 |
| --- | --- | --- | --- |
| B0 | J1単独 `b561aea1…` | OFF | 新しい経路でJ1基準を取り直す |
| B1 | 今回のcombined exact head | OFF | 合流/配置/計測基盤の対照。B0の結果を無条件に代用しない |
| D1 | B1と同じcombined exact head | ON | DMAだけを変えた単独比較 |

B1→D1ではDMA以外の全flag、sdkconfig、固定seed、fixture、camera/tier、tail上限、watchdog/session、
coarse/詳細modeと終了契約を同じにする。PR24のplan再利用、native/batch、近似演算など別laneは追加しない。
detail OFFもcoarse/fixtureは残るため、製品純性能とは呼ばない。

実行前に唯一担当が各actual source/build/flash/capture receipt、全flag、linked mode、BIN/ELF hash、
検証済みrecoveryと正式実行経路を確認する。CIの4構成成立からMCPのDMA ON selectorや配置済み画像を推定しない。
本branchはMCP設定・登録・job・USB・flash・resetを変更/起動しない。

統計単位は独立boot。各boot内の3repは従属として保持し、独立sample数へ足さない。B1/D1は実行順を
交互化し、必要なboot数は実機担当と別途固定する。18boot等の大きな計画を自動起動しない。

主指標は最終drainを含むwhole work。render/send elapsed、copy/submit/reap、strip/band/bytes、
入力/手続き画素/panel hash、heap free/largest/eval余裕、stack、repair、正常終了と物理表示を併記する。
DM_RASTER/DD_RENDERはfinish/ACKまで、待ちはDM_TRANSFER/DD_TX_SYNCとsent_usへ一度だけ含める。
queue受理をcoarse成功へ先行計上せず、NULL/別descriptor等の失敗は正規化後一度だけ数える。

fixtureは `oval-pan8-390-510-v1`、120 completed contexts、同じ固定workを維持する。
最終7行・full/narrow/polling/in-place切替、途中失敗→repairも確認し、raw fatal/gap/reset/refusalを拒否する。
END→owner EXIT→STOP→HOMEの契約を保つ。wire名 `DERBY_DETAIL_EXIT` からdetail modeを推定しない。

小差は命令cache/layout交絡を除いた同一binary A/B等の追加根拠が必要で、別binaryの小差だけでは
DMA効果と断定しない。既存copy/transfer elapsedはowner経過時間であり、SPI純通信時間やCPU使用率と同じではない。
MISO未配線のため、software panel hashだけでglass到達を受入済みにしない。

単独受入と残gateが揃ってから、親が次の適用/組合せを判断する。この検証branchからmain mergeしない。

## 隔離した型付きDMA proof案

別branch `proof/derby-dma-typed-mode-20261020` だけにarchive-safe追加proofを置く。
元のJ1/PR31/合流headは書き換えず、既存の診断proofも保持する。runtime C/JSや終了wireは変えない。
既存linked関数でmodeを判別できるため、新たなFW常駐markerは追加しない。

新laneの要求は `dma_mode: "OFF"|"ON"` と `detail_mode: "OFF"|"ON"` を別々の必須enumとする。
省略・null・boolean・AUTO・未知値を拒否し、自由なCMake flagやsource URLを受け取らない。
sourceはregistryが保持する承認済みfull SHAに固定する。新entry/headの受理は別承認が必要で、
既存J1専用entryへDMAや新headを暗黙に追加しない。J1のdetail proofをDMA証明に流用しない。

`tools/verify_dma_diagnostic_build.py --build <builddir> --expect-dma OFF|ON --expect-detail OFF|ON`
は `dma-diagnostic-mode-proof.json` を生成する。CMakeの追加ALL targetは両要求値を正規化して明示する。

| フィールド | 固定した意味 |
| --- | --- |
| `proof_contract` / `schema_version` | `derby-dma-diagnostic-v1` / `1`。既存detail proofとは別契約 |
| `requested_axes` / `verified_axes` | `dma_overlap`・`detail_measurement` の両enum。要求・cache・実ELFの一致時だけ出力 |
| `dma_overlap.symbol_contract` | `board-present-submit-finish-v1` |
| `dma_overlap.native_symbols` | ONでは実ELFの `board_present_submit` と `board_present_finish` の両方、OFFでは両方不在。片方だけは拒否 |
| `dma_overlap.cmake_cache_sha256` | 両axisを検査したactual cacheのSHA-256。cache欠落/重複/不一致も拒否 |
| `diagnostic` | 既存診断proof全体。fixture/seed/linked detail値/owner終了/IDF/BIN-ELF組を引き続き検証 |
| `artifacts` | nested diagnosticと同じactual BIN/ELF bytes・SHA-256 |
| `hardware_tested` | `false` |

MCPが新laneを扱う場合は、要求enum、registry固定head、archive/source receipt、build manifest、
このproofの両axisとBIN/ELF hashを一致させ、prepare/commit/durable結果と後続flash/captureへ保持する。
既存schemaだけ、mode文字列だけ、CI artifactだけ、同じhashのlatest記録だけでは受理しない。
cache/ELF拒否はbuild失敗として扱い、登録済みJ1へのfallbackやガード緩和をしない。
producer追加だけで旧consumerの受理を防げるわけではない。MCP/consumer側の新契約必須gateは未実装・未受入として残す。

この案と隔離host/CIはMCP実装済み・runtime新head許可・実機性能受入を意味しない。
新headの実登録/配備/実機操作は実施しない。B0/B1/D1、独立boot、固定fixture、物理表示と余裕の条件は上記を維持する。
