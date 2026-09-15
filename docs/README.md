# docs 索引

このディレクトリの入口。**いま動いている主線は3本**（VM の高速化、PIE による描画の高速化、デザインシステム Kasane）で、それぞれにディレクトリがある。仕様・設計・実測・履歴を同じ場所に混ぜないため、各行に種別を付けた。

- **仕様**: いま守る契約。コードが従う。
- **設計**: 実装前・実装中の方針と判断。決定と未決を書く。
- **記録**: 実測・調査の結果。書いた時点の値で、更新しない。
- **履歴**: 過去の構成の記録。現在の構成の根拠には使わない。

新しい文書は下のどれかのディレクトリに置き、この表に1行足す。ファイル名は動かさない（本文や C のコメントが名前で参照している）。

## 主線1: VM の高速化 — [`vm/`](vm/)

QuickJS を FreeRTOS 上で中断・再開できる実行基盤に作り替える（L0〜L5）。ブランチ運用は `vm-branching.md`。

| 文書 | 種別 | 中身 |
| --- | --- | --- |
| [quickjs-freertos-vm-spec.md](vm/quickjs-freertos-vm-spec.md) | 仕様 | L0〜L5 の全体仕様。§14.6 はまだ直していない不具合の一覧 |
| [vm-branching.md](vm/vm-branching.md) | 仕様 | `vm/*` ブランチの運用 |
| [vm-L2-design.md](vm/vm-L2-design.md) | 設計 | L2（移動しない VM スタックと中断・再開）。決定表 D1〜、整列（§3.3）、計測 |
| [vm-L1-design.md](vm/vm-L1-design.md) | 設計 | L1（ジョブ境界の実行制御と起床） |
| [task-allocation-facade.md](vm/task-allocation-facade.md) | 設計 | FreeRTOS タスクの配置ファサード（`standalone/fp_ticket`、未接続） |
| [vm-L0-report.md](vm/vm-L0-report.md) | 記録 | L0 の実機計測（ターン内訳、ヒープ、ジョブ単価） |
| [vm-L1-report.md](vm/vm-L1-report.md) | 記録 | L1 の実装と実測 |
| [vm-l1-clock.md](vm/vm-l1-clock.md) | 記録 | 時計読み出しのコスト |
| [vm-l1-tuning.md](vm/vm-l1-tuning.md) | 記録 | スケジューラ定数の調律 |
| [vm-ledger/](vm/vm-ledger/) | 記録 | QuickJS 内部の台帳 01〜07（呼び出し経路、フレームへの生ポインタ、ジョブと割り込み、opcode チェックポイント、メモリ確保、アロケータ比較、セグメント検査） |

## 主線2: PIE と描画の高速化 — [`perf/`](perf/)

ESP32-S3 の PIE（SIMD）で描画カーネルを書く知見と、FLOWER 背景の性能作業。PIE 命令の一次情報（TRM 抽出、実機で確かめた命令の意味とストール）は別リポジトリ [esp32s3-hw-mcp](https://github.com/dj-oyu/esp32s3-hw-mcp) にある。

| 文書 | 種別 | 中身 |
| --- | --- | --- |
| [pie-simd.md](perf/pie-simd.md) | 設計・記録 | PIE カーネルの書き方と、実機で当たった・外れた数字。§10 がカーネルを書く前のチェックリスト、§11 が次の高速化に持っていく知見 |
| [flower-perf-handoff.md](perf/flower-perf-handoff.md) | 記録 | FLOWER の性能作業の引き継ぎ（2026-09-09 時点） |
| [flower-decor-cost.md](perf/flower-decor-cost.md) | 記録 | 装飾光線のコスト見積もり（ホスト） |

検証の道具は [`tools/pie/README.md`](../tools/pie/README.md)。

## 主線3: デザインシステム Kasane — [`kasane/`](kasane/)

描画・QuickJS・PocketJS に依存しない C の基盤と、その上のデザインシステム。**開発は `vm/design-contracts` ブランチで進んでいる。** このブランチにあるのは 2026-09-14 時点の写しで、最新はそちら。

| 文書 | 種別 | 中身 |
| --- | --- | --- |
| [design-system.md](kasane/design-system.md) | 仕様 | デザインシステム仕様 |
| [design-schema.md](kasane/design-schema.md) | 仕様 | デザイン定義スキーマ（[JSON Schema](kasane/design-schema.json)、[例](kasane/design-example.json)） |
| [design-system-pet.md](kasane/design-system-pet.md) | 仕様 | ペットへの適用 |
| [system-runtime.md](kasane/system-runtime.md) | 設計 | 時計・電源・通知の共通ランタイム |
| [module-boundaries.md](kasane/module-boundaries.md) | 設計 | モジュール境界と依存の向き |

`vm/design-contracts` にだけある文書（`kasane-roadmap.md`、`kasane-progress.md`、`kasane-astra-plan.md`、`design-api.md`、`design-composition.md`、`design-contract-review.md`、`design-device-probe.md`）は、そのブランチを取り込むときにこのディレクトリへ移す。

## JS API — [`api/`](api/)

| 文書 | 種別 | 中身 |
| --- | --- | --- |
| [common-api.md](api/common-api.md) | 仕様 | 共通 JS API `pocket.*`。`main/pocket/` はこれの実装 |
| [filesystem-api.md](api/filesystem-api.md) | 仕様 | ファイルシステム API |

## 基盤と環境 — [`platform/`](platform/)

| 文書 | 種別 | 中身 |
| --- | --- | --- |
| [hardware-constraints.md](platform/hardware-constraints.md) | 仕様 | ハードウェア仕様と開発上の制約 |
| [build-environment.md](platform/build-environment.md) | 仕様 | Windows / EIM の開発環境とビルド手順 |
| [architecture.md](platform/architecture.md) | 設計 | プラットフォーム設計（2026-09-06 時点の実装を反映） |
| [idf-tls-txbuffer-report.md](platform/idf-tls-txbuffer-report.md) | 記録 | ESP-IDF の TLS 送信バッファの二重計上（上流への報告草稿、未送信） |

## ホームと背景 — [`scenes/`](scenes/)

| 文書 | 種別 | 中身 |
| --- | --- | --- |
| [home-ui.md](scenes/home-ui.md) | 仕様 | ホーム UI |
| [xmb-settings-sound.md](scenes/xmb-settings-sound.md) | 仕様 | XMB のカテゴリ・設定・操作音 |
| [flower-home.md](scenes/flower-home.md) | 設計 | 花・木漏れ日・雨滴の背景 |
| [flower-shots-guide.md](scenes/flower-shots-guide.md) | 仕様 | FLOWER のショット定義 |
| [flower-transitions.md](scenes/flower-transitions.md) | 設計 | FLOWER のシーン遷移 |
| [flower-decor-rays.md](scenes/flower-decor-rays.md) | 設計 | 装飾光線 |
| [solar-sail.md](scenes/solar-sail.md) | 設計 | SOLAR SAIL 背景 |

## アプリと機能 — [`apps/`](apps/)

| 文書 | 種別 | 中身 |
| --- | --- | --- |
| [japanese-input.md](apps/japanese-input.md) | 設計 | 日本語入力（SKK 移植） |
| [player-overlay.md](apps/player-overlay.md) | 設計 | ホーム画面のプレイヤー（残作業の一覧） |
| [pet-companion.md](apps/pet-companion.md) | 仕様 | Pet Companion |
| [pet-asset-design.md](apps/pet-asset-design.md) | 設計 | ペット画像の省容量化 |
| [pet-review.md](apps/pet-review.md) | 記録 | ペット2アプリのレビューと残りの修正（2026-09-07） |
| [mp3-implementation.md](apps/mp3-implementation.md) | 記録 | MP3 実装と実測 |
| [opus-feasibility.md](apps/opus-feasibility.md) | 記録 | Opus 復号の実現性と実測 |
| [tutorial-review.md](apps/tutorial-review.md) | 設計 | チュートリアル見直し（予約、着手条件つき） |

## 履歴 — [`archive/`](archive/)

過去の構成の記録。数字も構成も当時のもので、現在の根拠には使わない。

| 文書 | 中身 |
| --- | --- |
| [milestone-01.md](archive/milestone-01.md) | M1 の受け入れ計画 |
| [firmware-m1.md](archive/firmware-m1.md) | M1 ファームの実装と実機検証 |
| [implementation-audit.md](archive/implementation-audit.md) | 2026-09-06 の実装・文書整合性レビュー |
| [xmb-research.md](archive/xmb-research.md) | PSP XMB の再調査と初版ホームの記録 |
| [background-experiments.md](archive/background-experiments.md) | 初期のホーム背景の比較 |
| [depth-and-motion.md](archive/depth-and-motion.md) | 被写界深度・傾き連動の試作 |

## 古いパスをわざと残した場所

2026-09-15 にディレクトリへ分けたとき、`docs/<名前>` の参照はリポジトリ全体で新しいパスに書き換えた。次の2種類だけは書き換えていない。ファイル名は変えていないので、名前で検索すれば見つかる。

- **JS のソース**（`apps/**/*.js`、`tools/vmtest/**/*.js`）。ゲストは起動時にソースを解析するので、バイト数がそのままヒープを使う。OOM の境界を突くテストは、ヒープの余りがバイト単位で動くと結果が変わる（`vm/vm-L2-design.md` に記録がある）。コメントのパスのためにそこを動かさない。
- **上流へ出すパッチ**（`reports/upstream/*.patch`）。レビューした内容のまま保つ。

## 既知の切れた参照

どのブランチの履歴にも存在しない文書を、コードのコメントが参照している。書いた文書が別の場所にあるのか、書かれなかったのかは未確認。

- `docs/skk-ime-design.md` — `components/ime_core/include/ime_core.h`、`components/skk_core/include/skk_core.h`
- `docs/keyboard-ime-unification.md` — `components/skk_core/include/skk_core.h`
