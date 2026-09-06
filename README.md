# Cardputer ADV PocketJS

M5Stack Cardputer ADV向けの、QuickJS版PocketJSを使うファームウェアプロジェクトです。

文字主体のXMBホーム、QuickJS製Hello World、SKK Practice、JavaScript Playgroundを実装しています。Playgroundは編集・実行・保存・構文色分け・ログ／エラー表示に対応します。機能記述の基準は確定済みcommit `2b053b7`です。開発チュートリアルは実装進行中です。

## 設計ドキュメント

- [ファイルシステムAPI仕様案](docs/filesystem-api.md)：pocket.fs、ディレクトリとファイル操作、SD、逐次読書き、保存保証、メディア・PC転送との接続。

- [共通JS API仕様案](docs/common-api.md)：将来のアプリ、UI・入力・保存、センサー、Wi-Fi／BLE、外部I/O、PC連携の公開契約と実装段階。実装前の提案です。

- [Windows / EIM開発環境](docs/build-environment.md)：このPCのIDF v6.0.1、環境切り替え、ビルド・書き込みコマンド。
- [ハードウェア仕様と制約](docs/hardware-constraints.md)：SoC、メモリ、ピン配置、共有バス、描画・実行時の注意点。
- [プラットフォーム設計](docs/architecture.md)：責務、アプリの起動・終了、入力、描画、メモリ管理。
- [ホームUI設計](docs/home-ui.md)：カテゴリと項目の選択、背景、アニメーション、画面遷移。
- [ESP32-S3 PIE（SIMD）](docs/pie-simd.md)：描画カーネルのベクトル化。命令セットの制約、パイプラインのストール、ビット一致の検証、実測値。
- [Hello World検証計画](docs/milestone-01.md)：実装順序、測定項目、完了条件。
- [SKK日本語入力設計](docs/japanese-input.md)：既存Cコアの再利用、入力優先順位、Flash辞書、候補表示、M2検証計画。

- [実装と設計のレビュー](docs/implementation-audit.md)：現在の保証範囲、未解決事項、次の機能候補。
- [M1実機検証結果](docs/firmware-m1.md)：初期ファームウェアの検証記録。

設計目標・現在の実装・過去の実機結果を区別して記録します。過去のRAM/FPS・バイナリサイズは最新構成の測定値ではありません。

## 最初の到達点と現在の操作

ホームからHello Worldを起動し、Enterでカウンターを更新してホームへ戻る経路は実装・検証済みです。

- ホーム: 矢印刻印のキーでカテゴリ／項目、Enterで開く。Fn付き矢印にも対応。
- 設定: Enterで選択肢を開き、上下で選択、Enterで保存、Escで取消。
- Playground: Ctrl+Sで保存、Ctrl+Rで実行、Ctrl+Nで空の文書。Fn＋矢印でカーソル移動。
- 日本語入力: Ctrl+JまたはOpt+SpaceでIME切替。変換中はIMEが先にキーを処理。
- Esc: Fn＋左上のバッククォートキー。ホームでは同キー単独も戻るとして扱う。

保存失敗時の未保存表示、空文書の再読込、強制停止・入力キューの扱いには未解決事項があります。[レビュー](docs/implementation-audit.md)を参照してください。

## プラットフォームの方針

- 対象はCardputer ADV（ESP32-S3FN8、240×135 LCD）。PSRAMなしのメモリ予算で検証します。
- Pocket Vaporの事前C変換ではなく、QuickJS版PocketJSを使用します。
- アプリ管理は、JSの読み込み、実行環境の生成・終了、状態管理を担当します。
- UI管理は、入力の受け渡し、PocketJS描画、液晶更新を担当します。
- ネイティブ側にホームとエラー画面を置き、アプリは一度に1つ動かします。
- 終了やエラー時はアプリの資源を解放し、元のホーム選択位置へ戻します。
- JSヒープ上限は128KiB。ネイティブ側のメモリはこの制限と別で、任意アプリのOOM復帰は保証していません。
- Flashはアプリ3MiB、辞書2MiB、日本語フォント512KiB、storage 2496KiB。辞書・フォントは別アセットです。

## ホームのデザイン

PSPのXMBの情報整理を参考に、左右でカテゴリ、上下で項目を選ぶ文字主体のホームです。

- 暗い青を基調とした、リアルタイム描画の穏やかな波の背景。
- アイコンを使わず、文字列が選択位置へ移動し、明暗で選択を表示。
- 短いスライド遷移と、アニメーション中も反応するキー操作。
- アプリ実行中はホーム背景の更新を停止。
- AppsにHello World、SKK Practice、Playground。Settingsに背景・FPS・操作音。波／海面背景とパーティクル、IMU連動、リアルタイム合成音を実装。

## 将来の機能

- 名前付き作品管理、保存の復旧強化、動作中JSへの共通文字入力・方向入力API。
- 検索と実行可能なサンプルを備えたオフラインDocs。
- JavaScript製のペットなど、コピーして改造できる同梱アプリ。
- PC中継ソフトを介したClaude Code／Codex連携。まずUSB、その後Wi-Fiを検討。

この節は未実装の構想です。オフラインDocs／開発チュートリアルは別途実装進行中で、統合完了後に機能一覧と検証結果へ反映します。

## 参考

- [Cardputer ADV公式仕様](https://docs.m5stack.com/ja/core/Cardputer-Adv)
- [PocketJS](https://github.com/pocket-stack/pocketjs)
- [PocketJS ESP-IDFガイド](https://github.com/pocket-stack/pocketjs/blob/main/site/content/docs/esp-idf.md)

設計調査時のPocketJS参照コミット: `6a0a1b6c91a506c473fc37a0256a47b12eceeca8`。

## ライセンス

本プロジェクト独自のコードとドキュメントは[MIT License](LICENSE)で公開します。
外部ライブラリ、フォント、画像などはそれぞれのライセンスに従います。
