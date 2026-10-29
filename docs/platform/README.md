# 基盤・開発環境の文書

[文書トップ](../README.md) / [過去の調査・実験](../archive/README.md)

仕様・設計にも将来案や当時の値が含まれる。採用範囲・既定値・未確認条件は本文の状態表示を優先する。

## 仕様・設計・作業の入口

| 文書 | 種別 | 内容 |
| --- | --- | --- |
| [architecture.md](architecture.md) | 設計 | 現在のディレクトリ構成、責務、アプリの状態遷移、起動と終了 |
| [hardware-constraints.md](hardware-constraints.md) | 仕様 | ハードウェア仕様と開発上の制約（RAM 表、配線、UI ノード数の崖） |
| [build-environment.md](build-environment.md) | 仕様 | Windows / EIM の開発環境とビルド手順 |
| [test-commands.md](test-commands.md) | 仕様 | 実機テストとホスト側テスト（PIE 3層、WSL のみのもの）のコマンド一覧。守る規則は CLAUDE.md |
| [native-app-control.md](native-app-control.md) | 仕様 | 通常 main への #47 再提案と USB console 前提。 |
| [wifi-autostart.md](wifi-autostart.md) | 設計 | Wi-Fi の自動起動: ホームのアイドルで一過性の時刻同期（アプリ優先で中断）、参照カウントの接続サービス `net_service`、常駐の背景サービスへの方針候補と実機で測る項目 |
| [keystate.md](keystate.md) | 設計 | 物理キーの押下集合（HAL の keystate）と `pocket.input.keys`: 入力経路と消費者の一覧、ライフサイクル、却下案、実機で測る同時押し・ゴースト・FIFO あふれ |
| [backlog.md](backlog.md) | backlog | srcstore とエディタの保存まわりの不具合2件（コードで再現確認済み）、入力キュー、Docs 機能 |
| [clean-checkout-dependencies.md](clean-checkout-dependencies.md) | 設計・記録 | 適用範囲と実装状態は本文冒頭を参照。 |

## 計測・調査・実験の記録

| 文書 | 種別 | 内容 |
| --- | --- | --- |
| [usb-console-home.md](usb-console-home.md) | 修正・検証記録 | USB 送信と HOME 復帰の統合修正。本文の固定 base に対する記録。 |
| [idf-tls-txbuffer-report.md](idf-tls-txbuffer-report.md) | 記録 | ESP-IDF の TLS 送信バッファの二重計上（上流への報告草稿、未送信） |
