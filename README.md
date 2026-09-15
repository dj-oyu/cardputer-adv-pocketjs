# Cardputer ADV PocketJS

M5Stack Cardputer ADV向けの、QuickJS版PocketJSを使うファームウェアプロジェクトです。

XMBホーム（背景は LEVEL WAVE / OCEAN + STARS / SOLAR SAIL / FLOWER）、JSアプリ、SKK日本語入力、JavaScript Playground、共通JS API `pocket.*` を実装しています。アプリの一覧は `main/ui/shell.c` の `apps[]` が正です。

## 設計ドキュメント

入口は [docs/README.md](docs/README.md) です。いまの主線は VM の高速化（[docs/vm/](docs/vm/)）、PIE と描画の高速化（[docs/perf/](docs/perf/)）、デザインシステム Kasane（[docs/kasane/](docs/kasane/)）の3本です。よく使うもの:

- [共通JS API](docs/api/common-api.md)：`pocket.*` の仕様。冒頭に節ごとの実装状況。
- [ファイルシステムAPI](docs/api/filesystem-api.md)：`pocket.fs`、`app:` / `assets:` / `sd:`。
- [Windows / EIM開発環境](docs/platform/build-environment.md)：IDF v6.0.1、ビルド・書き込み。
- [ハードウェア仕様と制約](docs/platform/hardware-constraints.md)：SoC、メモリ、ピン配置、共有バス。
- [プラットフォーム設計](docs/platform/architecture.md)：ディレクトリ構成、責務、アプリの起動と終了。
- [ESP32-S3 PIE（SIMD）](docs/perf/pie-simd.md)：描画カーネルの書き方と実測値。

設計目標・現在の実装・過去の実機結果を区別して記録します。数値は測った時点のもので、現在の空きヒープなどは `tools/memlog.py` で測り直します。

## 操作

アプリには [POCKET PET](apps/pet/README.md) と、同じペットを Codex／Claude Code の使用量コンパニオン・リセット通知・目覚まし・タイマーに使う [Pet Companion](docs/apps/pet-companion.md) も含まれます。

- ホーム: 矢印刻印のキーでカテゴリ／項目、Enterで開く。Fn付き矢印にも対応。
- 設定: Enterで選択肢を開き、上下で選択、Enterで保存、Escで取消。
- Playground: Ctrl+Sで保存、Ctrl+Rで実行、Ctrl+Nで空の文書。Fn＋矢印でカーソル移動。
- 日本語入力: Ctrl+JまたはOpt+SpaceでIME切替。変換中はIMEが先にキーを処理。
- Esc: Fn＋左上のバッククォートキー。ホームでは同キー単独も戻るとして扱う。

保存失敗時の未保存表示、空文書の再読込、強制停止・入力キューの扱いには未解決事項があります。[docs/platform/backlog.md](docs/platform/backlog.md)を参照してください。

## プラットフォームの方針

- 対象はCardputer ADV（ESP32-S3FN8、240×135 LCD）。PSRAMなしのメモリ予算で検証します。
- Pocket Vaporの事前C変換ではなく、QuickJS版PocketJSを使用します。
- アプリ管理は、JSの読み込み、実行環境の生成・終了、状態管理を担当します。
- UI管理は、入力の受け渡し、PocketJS描画、液晶更新を担当します。
- ネイティブ側にホームとエラー画面を置き、アプリは一度に1つ動かします。
- 終了やエラー時はアプリの資源を解放し、元のホーム選択位置へ戻します。
- JSヒープの上限・空きヒープ・Flash配分はビルドごとに動くので、ここに数字を書きません。現在値は `CLAUDE.md` と `tools/memlog.py`、パーティションは `partitions.csv` を見てください。任意アプリのOOM復帰は保証していません。

## ホームのデザイン

PSPのXMBの情報整理を参考に、左右でカテゴリ、上下で項目を選ぶ文字主体のホームです。アイコンを使わず、文字列が選択位置へ移動し、明暗で選択を表示します。アプリ実行中はホーム背景の更新を止めます。詳細は [ホームUI](docs/scenes/home-ui.md)。

## まだ無いもの

開いている作業は各ディレクトリの `backlog.md` にあります（例: [docs/apps/backlog.md](docs/apps/backlog.md)、[docs/platform/backlog.md](docs/platform/backlog.md)）。

## 参考

- [Cardputer ADV公式仕様](https://docs.m5stack.com/ja/core/Cardputer-Adv)
- [PocketJS](https://github.com/pocket-stack/pocketjs)
- [PocketJS ESP-IDFガイド](https://github.com/pocket-stack/pocketjs/blob/main/site/content/docs/esp-idf.md)

設計調査時のPocketJS参照コミット: `6a0a1b6c91a506c473fc37a0256a47b12eceeca8`。

## ライセンス

本プロジェクト独自のコードとドキュメントは[MIT License](LICENSE)で公開します。
外部ライブラリ、フォント、画像などはそれぞれのライセンスに従います。
