# Cardputer ADV PocketJS

M5Stack Cardputer ADV向けの、QuickJS版PocketJSを使うファームウェアプロジェクトです。

現在は設計・初期準備段階です。動作するファームウェアやビルド手順はまだありません。

## 設計ドキュメント

- [Windows / EIM開発環境](docs/build-environment.md)：このPCのIDF v6.0.1、環境切り替え、ビルド・書き込みコマンド。
- [ハードウェア仕様と制約](docs/hardware-constraints.md)：SoC、メモリ、ピン配置、共有バス、描画・実行時の注意点。
- [プラットフォーム設計](docs/architecture.md)：責務、アプリの起動・終了、入力、描画、メモリ管理。
- [ホームUI設計](docs/home-ui.md)：カテゴリと項目の選択、背景、アニメーション、画面遷移。
- [Hello World検証計画](docs/milestone-01.md)：実装順序、測定項目、完了条件。
- [SKK日本語入力設計](docs/japanese-input.md)：既存Cコアの再利用、入力優先順位、Flash辞書、候補表示、M2検証計画。

各ドキュメントの数値目標とAPI案は設計値です。実装済み・実測済みを意味しません。

## 最初の到達点

ホームからJavaScript製のHello Worldアプリを起動し、キー入力でカウンターを更新し、ホームへ戻れることを目指します。起動と終了を繰り返してメモリが回収されることも確認します。

## プラットフォームの方針

- 対象はCardputer ADV（ESP32-S3FN8、240×135 LCD）。PSRAMなしのメモリ予算で検証します。
- Pocket Vaporの事前C変換ではなく、QuickJS版PocketJSを使用します。
- アプリ管理は、JSの読み込み、実行環境の生成・終了、状態管理を担当します。
- UI管理は、入力の受け渡し、PocketJS描画、液晶更新を担当します。
- ネイティブ側にホームとエラー画面を置き、アプリは一度に1つ動かします。
- 終了やエラー時はアプリの資源を解放し、元のホーム選択位置へ戻します。
- メモリ使用量、実行中断、エラー復帰を実機で検証してから機能を増やします。

## ホームのデザイン

PSPのクロスメディアバー（XMB）の情報整理を参考に、左右でカテゴリ、上下で項目を選ぶ独自のホームを目指します。

- 暗い青を基調とした、リアルタイム描画の穏やかな波の背景。
- 選択中のアイコンと項目を明るさ・大きさで強調。
- 短いスライド遷移と、アニメーション中も反応するキー操作。
- アプリ実行中はホーム背景の更新を停止。
- 最初はAppsカテゴリとHello Worldのみを実装。

## 将来の機能

- デバイス上のJavaScriptエディタ、実行環境、コンソール。
- 検索と実行可能なサンプルを備えたオフラインDocs。
- JavaScript製のペットなど、コピーして改造できる同梱アプリ。
- PC中継ソフトを介したClaude Code／Codex連携。まずUSB、その後Wi-Fiを検討。

これらは構想であり、実装済みの機能ではありません。PocketJSのUIを含む実行時メモリと描画性能は、ADV実機での検証が必要です。

## 参考

- [Cardputer ADV公式仕様](https://docs.m5stack.com/ja/core/Cardputer-Adv)
- [PocketJS](https://github.com/pocket-stack/pocketjs)
- [PocketJS ESP-IDFガイド](https://github.com/pocket-stack/pocketjs/blob/main/site/content/docs/esp-idf.md)

設計調査時のPocketJS参照コミット: `6a0a1b6c91a506c473fc37a0256a47b12eceeca8`。

## ライセンス

本プロジェクト独自のコードとドキュメントは[MIT License](LICENSE)で公開します。
外部ライブラリ、フォント、画像などはそれぞれのライセンスに従います。
