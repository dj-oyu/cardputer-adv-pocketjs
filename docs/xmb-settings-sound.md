# XMBカテゴリ・設定・合成操作音

2026-09-06、実機書き込み・USB操作による検証済み。

## 操作

- 左右: Apps / Settings のカテゴリを選択。
- 上下: カテゴリ内の項目選択。現在AppsはHello World一件、Settingsは三件。
- Enter: アプリ起動、または設定値の変更。
- Esc: アプリ終了・エラー解除。カテゴリと項目の選択位置はホーム復帰時に維持。

左上の機種タイトルと背景名は表示しない。
Settingsには BACKGROUND / FPS DISPLAY / SOUND を配置。
背景はLEVEL WAVEとOCEAN + STARS、FPS表示の初期値はOFF、操作音はON。
FPS DISPLAYをONにすると右上へ数値を表示する。
三設定をNVSのhome名前空間へ保存する。既存NVSを初期化できない場合でも、
パーティション全体を消去せず初期値で起動する。

## 音声

ES8311 (0x18) をI2Cで設定し、I2S1のBCLK=41 / WS=43 / DOUT=42で出力。
専用MCLKを使わずBCLKからクロックを生成する。
初期化レジスタは[M5UnifiedのCardputer ADV実装](https://github.com/m5stack/M5Unified/blob/master/src/M5Unified.cpp)を参照。
関連ライセンスをlicenses/M5Unified.txtに保存。

WAVなどの音声ファイルは使用しない。24kHz・16bit、左右同一のPCMを128フレームずつ演算する。
基本正弦波＋弱い第2倍音、約3msの立ち上がり、二次曲線の減衰を組み合わせる。
移動は約30ms・660Hz、決定は約60ms・880Hzから上昇、戻るは約45ms・440Hzから下降。
低い振幅で短い操作音とし、出力後には無音を送る。

音声タスクはUIと分離し、最大4件のキューとI2S DMAを使う。
キュー満杯時は音イベントを破棄して入力を待たせない。
ミュート時は待機イベントと生成中のサンプルを無音化する。
音声初期化に失敗してもホームを利用できる。

## 検証

- 左右カテゴリ、上下項目、背景・FPS・音のトグル: 成功。
- ミュート中の移動でSFX送信なし、再有効化でPCM送信: 成功。
- 再起動後に保存した海面設定が復元されることを確認。
- 起動・加算・ホーム復帰10回、6種のエラー回復: 成功。
- Settings画面の安定後FPS: 波25.9、海面29.3。
- 本体972,000 bytes。アプリ領域空き2,173,728 bytes。
- SKK辞書・日本語フォントは各2MiBの予約を維持。

音の聴感・実際の音量は実機で確認する。今回の自動検証はPCMのI2S送信まで。

```powershell
python tools/test_settings.py --port COM3
python tools/smoke_device.py --port COM3 --cycles 10
python tools/benchmark_background.py --port COM3
```

USB操作はa/b=左右、u/d=上下、e=Enter、q=Esc。
ベンチマークはSettingsで背景を変更するため、終了時には比較した背景が保存される。
