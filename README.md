# Cardputer ADV PocketJS

M5Stack Cardputer ADV向けの、QuickJS版PocketJSを使うファームウェアプロジェクトです。

現在は設計・初期準備段階です。動作するファームウェアやビルド手順はまだありません。

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
