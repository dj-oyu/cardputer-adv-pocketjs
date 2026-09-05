# ハードウェア仕様と開発上の制約

対象: 標準のM5Stack Cardputer-Adv（K132-Adv）。確認日: 2026-09-06。
仕様は末尾の公式資料、数値計算は本ドキュメントの式に基づく。実機性能は未測定。
旧Cardputer、Cardputer v1.1、PSRAM搭載の別のESP32-S3ボードを同一視しない。

## 基本仕様

| 項目 | 仕様 | 開発への影響 |
| --- | --- | --- |
| モジュール / SoC | Stamp-S3A / ESP32-S3FN8 | Xtensa向けビルド。P4のRISC-V用バイナリは使用不可 |
| CPU | 32bit LX7デュアルコア、240MHz | JSと描画のコストを計測。コア数はRAM容量を増やさない |
| 内部メモリ | SRAM 512KB、ROM 384KB、RTC SRAM 16KB | 512KB全体がアプリ用heapではない。ROM・RTC SRAMを通常heapへ加算しない |
| PSRAM | FN8に内蔵PSRAMなし。標準ADVはPSRAMなしで設計 | PSRAM必須設定・数MiBの実行時確保に依存しない |
| Flash | 8MB | ファームウェア、データ、必要ならOTA領域で分割。全容量がアプリ保存領域ではない |
| LCD | ST7789V2、1.14インチ、240×135 | 小型文字とSPI転送量に制約。タッチ入力を前提にしない |
| キーボード | 56キー（4×14）、TCA8418RTWR | ADV固有のI²Cキーボード処理。旧機種の走査コードを流用しない |
| IMU | BMI270、6軸 | 追加時は初期化・軸方向・校正を確認 |
| 音声 | ES8311、MEMSマイク、NS4150B、8Ω/1Wスピーカー、3.5mm出力 | コーデック設定とI²Sバッファが必要。旧機種の音声設定は非互換 |
| 保存媒体 | microSD | ファイル保存用。JS heapの代替ではない |
| 無線 | ESP32-S3の2.4GHz Wi-Fi / Bluetooth LE | 通信スタック分のRAMと電力を追加で確保 |
| その他 | IR送信、Grove、EXT 14ピン | 任意GPIOを無条件にJSへ公開しない |
| 電池 | 1750mAh | 背景描画・無線・音声込みの稼働時間は実測する |
| 外形 / 重量 | 84×54×19.6mm / 81g | 片手でのキー操作を実機で評価 |
| 動作温度 | 0〜40℃ | 製品の使用範囲として扱う |

## 配線と共有資源

以下は公式ピンマップのGPIO番号。I²Sの信号名はコーデック側の表記に合わせている。

| 機能 | ピン |
| --- | --- |
| LCD | BL=38、RST=33、DC(RS)=34、MOSI(DAT)=35、SCK=36、CS=37 |
| キーボード | SDA=8、SCL=9、INT=11 |
| IMU | SDA=8、SCL=9 |
| ES8311制御 | SDA=8、SCL=9 |
| ES8311音声 | SCLK=41、ASDOUT=46、LRCK=43、DSDIN=42 |
| microSD | CS=12、MOSI=14、CLK=40、MISO=39 |
| IR送信 | 44 |
| 電池電圧ADC | 10 |
| Grove | 信号=2/1、電源=5V/GND |
| EXTバス | I²C=8/9、SPI=40/14/39、CS=5、INT=4、BUSY=6、RESET=3、UART TX/RX=13/15 |

- キーボード、IMU、音声コーデックはGPIO8/9のI²Cを共有する。バスを一元管理し、処理の排他とタイムアウトを設ける。
- EXTのSPI信号はSDと共有する。拡張機器を追加する際はCSとバス使用の競合を確認する。LCDの配線はSDとは別。
- GPIO38はLCDバックライトとRGB LED電源有効化に関係する。LED用の独立した自由なGPIOとして扱わない。
- ヘッドフォン挿入時はスピーカーアンプが無効になる。音が出ない場合にソフトウェア不具合と決めつけない。
- Groveの電源5VとGPIOの信号電圧を混同しない。接続する機器の電気仕様は個別に確認する。
- SPIホスト番号、クロック、LCDの回転・表示オフセット、I²Cアドレスとキー配列は、HAL実装時に回路図・ドライバーで固定する。未確認値を推測で埋めない。

## RAM制約

最大の成立性判断はQuickJS、PocketJS core、フォント、描画、タスクが同時にRAMへ収まるかである。
上流のESP32-S3対応はPSRAMなしのADVで動くことの証明ではない。

| 項目 | 計算・制約 | 対策 |
| --- | --- | --- |
| 全画面RGB565 | 240×135×2 = 64,800byte（約63.3KiB） | 常駐二重バッファを避ける |
| 8行strip | 240×8×2 = 3,840byte（3.75KiB） | 初期候補。DMA完了前の書き換え禁止 |
| JS評価 | ソース、生成コード、実行オブジェクトでピークが変わる | ソースサイズだけで可否を判断しない |
| QuickJS初期値 | 上流はheap上限4MiB、stack上限256KiB、PSRAM優先 | 最低必要量ではない。ADVの実測予算へ変更 |
| タスクstack | QuickJSのstack上限はタスクstackを確保しない | 実領域、ネイティブ呼び出しの余裕、high-water markを確認 |
| Rust/native | JS heap limitの管理外。上流Rust OOMは致命的 | ノード・画像・字形の上限とnative予算を別に設ける |
| 断片化 | 空き合計があっても連続領域を確保できない | 最大空きブロックと起動終了の反復を測定 |
| ネットワーク・音声 | バッファとタスクが増える | M1は無効。追加時に予算を測り直す |

Flash/SDのデータを常に全量RAMへコピーしない。ただし上流PAKをFlashから借用しても、core内のフォント・画像登録などで追加確保されうる。
日本語docsは保存容量だけでなく字形メモリと読み込み待ちも設計する。最初のHello Worldは英数字に限定する。
ログ、入力イベント、エラー文字列は上限付き。ユーザーJSの無制限なnative資源確保を許可しない。

## 描画と操作の制約

全画面を30回/秒転送すると、ピクセルだけで1,944,000byte/秒（約15.6Mbit/秒）が必要。
これは理論上のペイロードであり、SPIコマンド、転送待ち、波の計算、文字描画を含まない。達成FPSの保証ではない。
背景が全面で動くとdirty regionは小さくならないため、背景は低い頻度で更新し、キー入力処理を待たせない。
S3の本構成はソフトウェアRGB565描画を使用し、P4専用PPAを前提にしない。

6×8pxの英数文字でも画面全体で最大40列×16行程度。ヘッダーや操作案内があればさらに減る。
Editor、Run、Console、Docsは切り替え式とし、PC用エディタの複数ペインを縮小して載せない。
物理キーの押下・解放、Fn、長押しrepeatを区別する。専用ForceStopは先取りし、通常BackはIME取消を優先する。
SKKの状態サイズと辞書・字形の予算は[日本語入力設計](japanese-input.md)を参照する。

## 実行・保存・開発環境の制約

- JS実行は1タスク・1アプリ。QuickJS contextへの同時アクセスをしない。
- 割り込みはJSループ対策になるが、停止しないネイティブ関数の代わりにはならない。I/Oには期限を設ける。
- JSX、TypeScript、Vue SFCには変換工程が必要。本体の素のJS編集とPC側の変換を区別する。
- Node.jsやブラウザーAPIが自動で使えるわけではない。実装済みのデバイスAPIだけをDocsに掲載する。
- `.pocket` のABIとhost profileを検証する。QuickJS bytecodeを将来使う場合もエンジン版の互換性を仮定しない。
- SD保存では書き込み失敗・電源断・カード抜去を想定し、将来のEditorで元データを失わない保存手順を設計する。
- 8MB Flashのpartitionはfirmwareサイズ確認後に決定する。M1でOTAを約束しない。
- PocketJS調査基準はIDF `>=6.0,<6.2`。既存ADV公式デモの5.4.2とは異なり、移植時にドライバー互換性を確認する。
- 充電は公式手順どおり電源ONで行う。復旧用ダウンロードモードは電源OFFからG0を押し、給電後に離す。

## 未確定・実機で決める項目

JS heap上限、各タスクstack、フォント量、最大アプリサイズ、LCDのSPI速度、安定FPS、停止応答、消費電力は未測定。
[M1検証計画](milestone-01.md)に従い、空きRAM・最大連続領域・描画時間・入力遅延を記録してから決定する。

## 公式資料

- [M5Stack Cardputer-Adv仕様・ピンマップ・操作説明](https://docs.m5stack.com/ja/core/Cardputer-Adv)
- [Espressif ESP32-S3データシート（メモリ、FN8品種表）](https://documentation.espressif.com/esp32_s3_datasheet_en.pdf)
- [PocketJS ESP-IDF設計（調査コミット固定）](https://github.com/pocket-stack/pocketjs/blob/6a0a1b6c91a506c473fc37a0256a47b12eceeca8/site/content/docs/esp-idf.md)
- [PocketJS guest設定（調査コミット固定）](https://github.com/pocket-stack/pocketjs/blob/6a0a1b6c91a506c473fc37a0256a47b12eceeca8/hosts/esp-idf/components/pocketjs_guest/src/guest.c)
