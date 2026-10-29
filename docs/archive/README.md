# 過去の調査・実験・提案記録

[文書トップ](../README.md)

ここは測定条件・不採用理由・比較未完を保存する入口。古い数値や旧手順を現在の仕様として使わない。移動は削除や不採用判定を意味しない。

## 保存した資料

| 旧パス | 保存先 |
| --- | --- |
| `docs/flower-fixed-point-pipeline.md` | [flower-fixed-point-pipeline.md](perf/flower-fixed-point-pipeline.md) |
| `docs/flower-optimisation-options.md` | [flower-optimisation-options.md](perf/flower-optimisation-options.md) |
| `docs/perf/builtins-census.md` | [builtins-census.md](perf/builtins-census.md) |
| `docs/perf/compiler-builtins-cgu04.md` | [compiler-builtins-cgu04.md](perf/compiler-builtins-cgu04.md) |
| `docs/perf/compiler-builtins.md` | [compiler-builtins.md](perf/compiler-builtins.md) |
| `docs/perf/flash-removals.md` | [flash-removals.md](perf/flash-removals.md) |
| `docs/perf/kasane-image-transform-recon.md` | [kasane-image-transform-recon.md](perf/kasane-image-transform-recon.md) |
| `docs/perf/kasane-opt-integration.md` | [kasane-opt-integration.md](perf/kasane-opt-integration.md) |
| `docs/perf/kasane-consolidation.md` | [kasane-consolidation.md](perf/kasane-consolidation.md) |
| `docs/perf/kasane-grid-sampling-host.md` | [kasane-grid-sampling-host.md](perf/kasane-grid-sampling-host.md) |
| `docs/perf/libgcc-64bit-division.md` | [libgcc-64bit-division.md](perf/libgcc-64bit-division.md) |
| `docs/perf/system-pie-survey.md` | [system-pie-survey.md](perf/system-pie-survey.md) |
| `docs/perf/trig-lut.md` | [trig-lut.md](perf/trig-lut.md) |
| `docs/kasane/pie-stall-host-report.md` | [pie-stall-host-report.md](kasane/pie-stall-host-report.md) |
| `docs/kasane/proc-pie-stall-options.md` | [proc-pie-stall-options.md](kasane/proc-pie-stall-options.md) |
| `docs/apps/sd-async-experiment-20260923.md` | [sd-async-experiment-20260923.md](apps/sd-async-experiment-20260923.md) |
| `docs/platform/keyboard-startup-probe.md` | [keyboard-startup-probe.md](platform/keyboard-startup-probe.md) |

## 参照を保って元の場所に残す記録

C・ゲスト JS から参照する資料は、実装やゲストのソースサイズを変えないため元のパスを保つ。各分野の索引で仕様・設計と記録を区別する。

- [VM の計測結果・台帳](../vm/README.md)（L0〜L3、割り当て、OOM、キャッシュ、旧設計と実装後の追記）
- [描画・性能の計測記録](../perf/README.md)（FLOWER、PIE、Kasane の最適化）
- [アプリの設計と記録](../apps/README.md)（DERBY、MEGADEMO、音声。比較中・既定 OFF も含む）
- [Kasane の設計と検証](../kasane/README.md)（初期 IR、D0〜D6、機械可読の旧構想）
- [基盤の提案・障害記録](../platform/README.md)（USB console、TLS の未送信草稿）

## 重複・旧手順

- FLOWER 装飾光線の正本は [perf/flower-decor-cost.md](../perf/flower-decor-cost.md)。ルート版にだけあった55行（固定点平方根と profile gate の結果）も統合し、[旧パス](../flower-decor-cost.md) は移動案内として維持。
- 固定点 pipeline の FPU 解釈は後続実測で訂正。保存先冒頭から FPU・平方根の再測定へ案内する。
- 古い branch、build directory、COM port、測定値は再現条件の一部であり、初回導入には [現在の開発環境](../platform/build-environment.md) を使う。
- [findings.md](findings.md) は2026-09-15に削除された旧記録の要約。原文は Git 履歴に残る。今回の整理では記録を削除していない。

## 比較未完の提案

- [DMA source-selected ON 比較（#99）](proposals/dma-source-on-pr99.md): closed / unmerged。実機優位が未確認で比較未完。性能不良を理由とする不採用ではない。
