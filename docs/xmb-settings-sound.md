# XMBカテゴリ・設定・合成操作音

更新: 2026-09-06。操作は確定済みcommit `2b053b7`を基準とする。末尾の性能・容量は初期設定画面 `497dc8a` の履歴で、最新構成の実測ではない。

## 操作

- 左右: Apps / Settings のカテゴリを選択。
- 上下: カテゴリ内の項目選択。AppsはHello World / SKK Practice / Playground、Settingsは三件。チュートリアルは追加作業中。
- Enter: アプリ起動、または設定の選択肢を開く。選択肢は上下で選び、Enterで保存して戻る。
- Esc: 設定の変更取消、アプリ終了・エラー解除。編集欄ではIME取消を優先する。物理操作は[ホーム仕様](home-ui.md)を参照。カテゴリと項目の選択位置はホーム復帰時に維持。

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
ピークは第2倍音を含めて約-7dBFS。出力後には無音を送る。
当初は約-28dBFSで、PCMはI2Sへ届いていたが本体スピーカーでは耳を近づけないと聞こえなかった。

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
- 当時はSKK辞書2MiB・日本語フォント2MiB。後続の日本語入力追加でフォント領域を512KiBへ変更した。現在の容量は[仕様](architecture.md)を参照。

自動検証はPCMのI2S送信までしか確認しない。音量を変更したときは実機で聴感を確認する。
現在の振幅は本体スピーカーでの聴感で決めた。

```powershell
python tools/test_settings.py --port COM3
python tools/smoke_device.py --port COM3 --cycles 10
python tools/benchmark_background.py --port COM3
```

USB操作はa/b=左右、u/d=上下、e=Enter、q=Esc。
ベンチマークはSettingsで背景を変更するため、終了時には比較した背景が保存される。
