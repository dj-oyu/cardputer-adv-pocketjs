# docs 索引

このディレクトリの入口。**いま動いている主線は3本**（VM の高速化、PIE による描画の高速化、デザインシステム Kasane）で、それぞれにディレクトリがある。

- **仕様**: いま守る契約。コードが従う。
- **設計**: 実装済み・実装中の方針と判断。決定と未決を書く。
- **記録**: 実測・調査の結果。測った時点の値と出所（日付・commit・実測/推定の別）を持つ。
- **backlog**: 開いている作業だけ。終わった項目は消す。各ディレクトリに1つ。

新しい文書は該当ディレクトリに置き、この表に1行足す。ファイル名は動かさない（本文や C のコメントが名前と節番号で参照している）。節番号を変えたら `git grep -n "<ファイル名>"` で参照元を直す。

## 主線1: VM の高速化 — [`vm/`](vm/)

QuickJS を FreeRTOS 上で中断・再開できる実行基盤に作り替える（L0〜L5）。

| 文書 | 種別 | 中身 |
| --- | --- | --- |
| [quickjs-freertos-vm-spec.md](vm/quickjs-freertos-vm-spec.md) | 仕様 | L0〜L5 の全体仕様。各段の現状と、実装で確定した値 |
| [vm-branching.md](vm/vm-branching.md) | 仕様 | `vm/*` ブランチとワークツリーの運用 |
| [vm-L1-design.md](vm/vm-L1-design.md) | 設計 | L1: ジョブ境界の実行制御と起床 |
| [vm-L2-design.md](vm/vm-L2-design.md) | 設計 | L2: 移動しない VM スタックと中断・再開。決定表 D1〜D43、整列（§2.2） |
| [vm-tco-design.md](vm/vm-tco-design.md) | 設計（下書き） | 末尾呼び出し最適化。未決・未実装 |
| [task-allocation-facade.md](vm/task-allocation-facade.md) | 設計 | FreeRTOS タスクの配置ファサード（`standalone/fp_ticket`、未接続） |
| [vm-L0-report.md](vm/vm-L0-report.md) | 記録 | L0 の実機計測（ターン内訳、ヒープ、ジョブ単価） |
| [vm-L1-report.md](vm/vm-L1-report.md) | 記録 | L1 の実測。時計読み出しのコスト（§8.7）、コア移行（§8.8）、スケジューラ定数の調律（§10） |
| [vm-L2-results.md](vm/vm-L2-results.md) | 記録 | L2 の実測と関所の結果（段ごと、host/device の別つき） |
| [vm-ledger/](vm/vm-ledger/) | 記録 | QuickJS 内部の台帳 01〜07（呼び出し経路、フレームへの生ポインタ、ジョブと割り込み、opcode チェックポイント、メモリ確保、アロケータ比較、セグメント検査） |
| [backlog.md](vm/backlog.md) | backlog | L2 の未完了条件、L1 の範囲外として残った決定、VM とは独立の不具合（GC 閾値、確保ヘッダ 12B など） |

## 主線2: PIE と描画の高速化 — [`perf/`](perf/)

ESP32-S3 の PIE（SIMD）と、このコアでのスカラーコードの最適化。PIE 命令の一次情報（TRM 抽出、実機で確かめた命令の意味とストール）は別リポジトリ [esp32s3-hw-mcp](https://github.com/dj-oyu/esp32s3-hw-mcp) にある。

| 文書 | 種別 | 中身 |
| --- | --- | --- |
| [pie-simd.md](perf/pie-simd.md) | 設計・記録 | PIE の性質（§1）、コストモデル（§2）、スカラーコードの値段（§3）、何を最適化するかの決め方（§4）、正確性（§5）、測定方法（§6）、出荷済みカーネルと実測（§7）、チェックリスト（§8） |
| [backlog.md](perf/backlog.md) | backlog | 未着手の性能候補（テキストのマスク合成、MP3 FIR、整数平方根、装飾光線の PIE 化など） |
| [kasane-pet-row-cache.md](perf/kasane-pet-row-cache.md) | 記録 | PET 画像 provider の重複復号を 64 行キャッシュで消した（切替つき、8,208 B、−70.4%） |

作業ブランチの装飾光線コストと部分別計測は[flower-decor-cost.md](perf/flower-decor-cost.md)に保持する。
検証の道具は [`tools/pie/README.md`](../tools/pie/README.md)。

## 主線3: デザインシステム Kasane — [`kasane/`](kasane/)

描画・QuickJS・PocketJS に依存しない C の基盤と、その上のデザインシステム。**開発は `vm/design-contracts` ブランチで進んでいる。** 2026-09-15のvm/main同期で、このブランチの最新仕様・実装記録をこのディレクトリへ統合した。

| 文書 | 種別 | 中身 |
| --- | --- | --- |
| [design-system.md](kasane/design-system.md) | 仕様 | デザインシステム仕様 |
| [design-schema.md](kasane/design-schema.md) | 仕様 | デザイン定義スキーマ（[JSON Schema](kasane/design-schema.json)、[例](kasane/design-example.json)） |
| [design-system-pet.md](kasane/design-system-pet.md) | 仕様 | ペットへの適用 |
| [system-runtime.md](kasane/system-runtime.md) | 設計 | 時計・電源・通知の共通ランタイム |
| [module-boundaries.md](kasane/module-boundaries.md) | 設計 | モジュール境界と依存の向き |
| [system-runtime-migration.md](kasane/system-runtime-migration.md) | 設計 | pet_hub から時計・通知・タイマー・鳴動を取り出す手順（S1〜S4、Kasane の checkpoint との対応） |

| 文書 | 種別 | 中身 |
| --- | --- | --- |
| [kasane-astra-plan.md](kasane/kasane-astra-plan.md) | 設計 | 実装順序とcheckpointごとの検証・commit/push条件 |
| [kasane-progress.md](kasane/kasane-progress.md) | 記録 | 実装結果、検証、実機未確認事項 |
| [kasane-roadmap.md](kasane/kasane-roadmap.md) | 設計 | 機能カバレッジ、Taffy移行、動画・3Dの将来設計 |
| [design-api.md](kasane/design-api.md) | 仕様 | 利用APIとowner境界 |
| [design-composition.md](kasane/design-composition.md) | 仕様 | 重なり、透過、cache、modal、効果 |
| [design-contract-review.md](kasane/design-contract-review.md) | 記録 | 公開契約の評価 |
| [design-device-probe.md](kasane/design-device-probe.md) | 記録 | 実機診断と計測 |

## JS API — [`api/`](api/)

| 文書 | 種別 | 中身 |
| --- | --- | --- |
| [common-api.md](api/common-api.md) | 仕様 | 共通 JS API `pocket.*`。冒頭に節ごとの実装状況（BLE 以外は実装済み）。`main/pocket/` はこれの実装 |
| [filesystem-api.md](api/filesystem-api.md) | 仕様 | `pocket.fs`。`app:` / `assets:` / `sd:` の3ボリュームとも実装済み |
| [backlog.md](api/backlog.md) | backlog | BLE、Opus の採否、SD の帯域、PC bridge の wire 形式など |

## 基盤と環境 — [`platform/`](platform/)

| 文書 | 種別 | 中身 |
| --- | --- | --- |
| [architecture.md](platform/architecture.md) | 設計 | 現在のディレクトリ構成、責務、アプリの状態遷移、起動と終了 |
| [hardware-constraints.md](platform/hardware-constraints.md) | 仕様 | ハードウェア仕様と開発上の制約（RAM 表、配線、UI ノード数の崖） |
| [build-environment.md](platform/build-environment.md) | 仕様 | Windows / EIM の開発環境とビルド手順 |
| [idf-tls-txbuffer-report.md](platform/idf-tls-txbuffer-report.md) | 記録 | ESP-IDF の TLS 送信バッファの二重計上（上流への報告草稿、未送信） |
| [backlog.md](platform/backlog.md) | backlog | srcstore とエディタの保存まわりの不具合2件（コードで再現確認済み）、入力キュー、Docs 機能 |

## ホームと背景 — [`scenes/`](scenes/)

| 文書 | 種別 | 中身 |
| --- | --- | --- |
| [home-ui.md](scenes/home-ui.md) | 仕様 | ホーム UI: XMB 操作、Apps / Settings、背景の選択、遷移、エラー画面 |
| [xmb-settings-sound.md](scenes/xmb-settings-sound.md) | 仕様 | XMB の設定項目と操作音（起動時に焼き込む効果音テーブル） |
| [flower.md](scenes/flower.md) | 設計・仕様 | FLOWER 背景: 花の種、カメラとショット、森・光・雨、シーン遷移、装飾光線、メモリ |
| [solar-sail.md](scenes/solar-sail.md) | 設計 | SOLAR SAIL 背景: 惑星と衛星の軌道モデル、時刻同期の境界 |

## アプリと機能 — [`apps/`](apps/)

| 文書 | 種別 | 中身 |
| --- | --- | --- |
| [japanese-input.md](apps/japanese-input.md) | 仕様 | SKK 日本語入力と `pocket.input.text` |
| [player-overlay.md](apps/player-overlay.md) | 設計 | ホーム画面プレイヤー: オーバーレイの仕組みと費用の管理 |
| [pet-companion.md](apps/pet-companion.md) | 仕様 | Pet Companion: PC 連携、API、実装メモ |
| [pet-asset-design.md](apps/pet-asset-design.md) | 設計 | ペット画像の省容量化（PPT2 形式） |
| [mp3-implementation.md](apps/mp3-implementation.md) | 記録 | MP3 実装と実測 |
| [opus-feasibility.md](apps/opus-feasibility.md) | 記録 | Opus 復号の実現性調査と、実装後の答え合わせ |
| [backlog.md](apps/backlog.md) | backlog | チュートリアルの見直し、オーバーレイの残り、日本語入力の残り |

## 過去の知見 — [`archive/`](archive/)

| 文書 | 種別 | 中身 |
| --- | --- | --- |
| [findings.md](archive/findings.md) | 記録 | 削除した過去の記録から残した知見（M1 の受け入れ結果、背景描画の最適化手順、傾き連動と被写界深度で退けた案、XMB の設計根拠） |

過去の文書そのものは git の履歴にある（2026-09-15 の整理で削除）。

## 古いパスをわざと残した場所

`docs/<名前>` の参照はリポジトリ全体で新しいパスに書き換えた。次の2種類だけはパスを書き換えていない。ファイル名は変えていないので、名前で検索すれば見つかる。

- **JS のソース**（`apps/**/*.js`、`tools/vmtest/**/*.js`）。ゲストは起動時にソースを解析するので、バイト数がそのままヒープを使う。OOM の境界を突くテストは、ヒープの余りがバイト単位で動くと結果が変わる。節番号の書き換えは同じバイト長で行った。
- **上流へ出すパッチ**（`reports/upstream/*.patch`）。レビューした内容のまま保つ。
