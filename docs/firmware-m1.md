# M1 ファームウェアの実装・実機検証

2026-09-06: Cardputer ADV に書き込み済み。書き込みデータのハッシュ検証成功。
QuickJS による Hello World の実行と液晶への転送完了を USB ログで確認。
液晶の向き・色・見た目と物理キー操作はユーザーの実機確認待ち。

## 操作

- 電源投入で波背景のホーム。
- Enter: Hello World を起動。アプリ内ではカウンターを加算。
- Esc（左上のキー）: アプリを終了してホームへ戻る。
- M1 のメニューは Apps / Hello World の一項目。複数カテゴリは未実装。

Hello World は `apps/hello/main.js` にあり、ファームウェアに埋め込む。
PocketJS の低レベル UI API を使用する。編集後はファームウェアの再ビルドが必要。
外部アプリのインストール、.pocket パッケージ検証、エディタ、SKK変換自体は未実装。

## 容量と実測

| 対象 | サイズ |
| --- | ---: |
| アプリ本体 | 912,400 bytes（約891 KiB） |
| factory 領域 | 3 MiB、空き2,233,328 bytes（71%） |
| SKK辞書予約 | 2 MiB |
| 日本語フォント予約 | 2 MiB |
| storage予約 | 960 KiB |
| Hello World開始時QuickJSヒープ | 79,264 bytes |
| アプリ終了後の内部空きRAM | 340,684 bytes |
| アプリ終了後の最大連続空きRAM | 221,184 bytes |

Flash全体は8 MiB。予約領域には今回辞書・フォントを書き込んでいない。
`tools/check_flash.py` は領域の重なり、Flash上限、辞書・フォント各2 MiBの維持とアプリ容量を検査する。
QuickJS上限128 KiB、JSスタック上限20 KiB、UIタスクスタック32 KiB。
描画は240×8画素のstrip単位。PSRAM、Wi-Fi、BLEは使用しない。
Rust側の割り当て失敗は回復可能なJS例外として保証できないため、任意アプリへの一般開放には追加対策が必要。

## 確認結果

USB操作で同じ入力キューを通した起動 → Enter加算 → ホーム復帰を100回実行。
毎回カウンターが1に更新され、終了後の空きRAM・最大連続領域は上記値で一定。
構文エラー、評価中無限ループ、フレーム中無限ループ、JSメモリ上限、実行例外、
無限Promise連鎖の6ケースで終了し、その後のHello World再起動が成功した。
実機テスト終了時にはHello Worldを起動した状態にした。

```powershell
python tools/test_flash_budget.py
python tools/smoke_device.py --port COM3 --cycles 100
```

USB診断: `e`=Enter、`q`=戻る、`1`〜`6`=上記エラー試験（ホームから）、
`s`=描画RGB565データをテキスト取得。画面取得は転送待ちで描画を遅らせる診断用。

## ビルド

ESP-IDF v6.0.1 / GCC15.2.0、QuickJS-NG Registry版0.14.0。
PocketJS固定コミット: `6a0a1b6c91a506c473fc37a0256a47b12eceeca8`。
RustはWSL Ubuntu上のespup 0.17.1で用意した`pocketjs-esp`ツールチェーン、
rustc 1.97.0-nightly (`8ea53bcd7`) / Espressif 1.97.0.0を使用。

EIMのv6.0.1環境を有効化し、リポジトリルートで実行する。

```powershell
python tools/prepare_dependencies.py
# WSL内で本リポジトリへ移動し、bash tools/build_native.sh を実行
idf.py build
idf.py -p COM3 flash
```

`build_native.sh`は`.cache/tools/export-esp.sh`（espup生成）と名前付きRustツールチェーンを前提とする。
PocketJSのguestコンポーネントは別の作業コピーへ展開する。
Registry版QuickJSのソースハッシュを確認し、immutable-bufferの書き込み防止パッチを適用する。
上流固定版が想定するソースハッシュとの相違に対応しているが、ハッシュ検査自体は維持する。

初回書き込み前の8 MiB完全バックアップは`.cache/backups/pre-pocketjs-8mb.bin`に保存した。
バックアップおよびビルド出力はGit管理対象外。
