# ドキュメントの入口

導入・操作・アプリ開発のための資料と、設計判断・過去の計測記録を目的別に探す。
数値は対象 commit・image・workload に限る。host の検証、実機の測定、推定を読み分ける。

## はじめに読む

| 目的 | 入口 |
| --- | --- |
| プロジェクトと基本操作 | [プロジェクト README](../README.md)、[ホーム UI](scenes/home-ui.md) |
| 初回取得・依存関係 | [clean checkout の依存一覧](platform/clean-checkout-dependencies.md) |
| 環境構築・ビルド | [Windows / EIM 開発環境](platform/build-environment.md) |
| テストを実行する | [テストコマンド](platform/test-commands.md)、[作業時の規則](../CLAUDE.md) |
| JS アプリを書く | [共通 API](api/common-api.md)、[ファイルシステム API](api/filesystem-api.md)、[Kasane](kasane/README.md) |
| 日本語入力・PC 連携を使う | [SKK 日本語入力](apps/japanese-input.md)、[Pet Companion](apps/pet-companion.md)、[app-control v1](api/app-control-v1.md) |

## 分野別の索引

| 分野 | 内容 |
| --- | --- |
| [API](api/README.md) | `pocket.*` の仕様、capability、未実装の面 |
| [基盤・開発環境](platform/README.md) | 取得、ビルド、テスト、ハードウェア、起動・終了・入力 |
| [ホームと背景](scenes/README.md) | XMB、設定、FLOWER・SOLAR SAIL・LEVEL WAVE |
| [アプリと機能](apps/README.md) | 操作・機能設計、DERBY・MEGADEMO・音声の実装と比較記録 |
| [VM](vm/README.md) | QuickJS / FreeRTOS の仕様・設計、L0〜L3 の記録・台帳 |
| [性能と描画](perf/README.md) | PIE、Flash、画像変換、測定方法と実験記録 |
| [Kasane](kasane/README.md) | 描画の契約、判断台帳、検証ゲート、動的描画 |
| [過去の調査・実験・提案](archive/README.md) | 移動先一覧、旧手順・重複の扱い、比較未完の理由 |

## 文書の状態と更新方法

- **仕様**は公開契約。実装済み範囲と未実装の面を明記する。
- **設計**は判断と理由。素案・未接続・既定 OFF・未受入を実装完了と混同しない。
- **記録**は対象 source と当時の結果。旧手順や訂正前の解釈を現行仕様として使わない。
- **backlog / roadmap** は未解決の項目。各分野の索引から辿り、完了項目の根拠は元資料に残す。

新しい文書は分野別の索引へ追加する。過去の記録は削除せず `archive/` へ保存するか、コードから参照される場合は元の場所に残して記録と明示する。
API の節番号・設計の決定番号はコードから参照されるので動かさない。移動時はリポジトリ全体を検索して参照を修復する。
ゲスト JS のソースサイズはヒープへ影響するため、文書整理のために JS コメントを変更しない。上流パッチも原本を保つ。
旧パスの互換案内と保存先は [archive の索引](archive/README.md) に記す。

文書・索引を変えたら、リポジトリルートで `python tools/check_docs.py` と `git diff --check` を実行する。
ローカル Markdown リンク・画像・見出しアンカーと索引到達性を検査する。外部 URL の到達性や実機の挙動はこの検査の対象外。
