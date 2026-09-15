# XMBカテゴリ・設定・合成操作音

## 操作

- 左右: Apps / Settings のカテゴリを選択。
- 上下: カテゴリ内の項目選択。Appsの行数は `main/ui/shell.c` の `apps[]` テーブルが真実（末尾追記のみで、途中挿入は `shell_app()` の番号を暗黙に変えるので禁止）。Settingsは BACKGROUND / FPS DISPLAY / SOUND / VOLUME の4件（`main/ui/shell.c` の設定テーブル1つが真実で、ラベル・選択肢数・保存・詳細行はすべてそこから引く）。
- Enter: アプリ起動、または設定の選択肢を開く。選択肢は上下で選び、Enterで保存して戻る。
- Esc: 設定の変更取消、アプリ終了・エラー解除。編集欄ではIME取消を優先する。物理操作は[ホーム仕様](home-ui.md)を参照。カテゴリと項目の選択位置はホーム復帰時に維持。

左上の機種タイトルと背景名は表示しない。
BACKGROUNDは LEVEL WAVE / OCEAN + STARS / SOLAR SAIL の3択（[SOLAR SAIL](solar-sail.md)、[FLOWER背景](flower.md)）。FPS表示の初期値はOFF、操作音はON。
FPS DISPLAYをONにすると右上へ数値を表示する。
設定値をNVSのhome名前空間へ保存する。既存NVSを初期化できない場合でも、
パーティション全体を消去せず初期値で起動する。

## 音声

ES8311 (0x18) をI2Cで設定し、I2S1のBCLK=41 / WS=43 / DOUT=42で出力。
専用MCLKを使わずBCLKからクロックを生成する。
初期化レジスタは[M5UnifiedのCardputer ADV実装](https://github.com/m5stack/M5Unified/blob/master/src/M5Unified.cpp)を参照。
関連ライセンスをlicenses/M5Unified.txtに保存。

WAVなどの音声ファイルは使用しない。24kHz・16bit、左右同一のPCMを128フレームずつ送る。波形は基本正弦波＋弱い第2倍音、約3msの立ち上がり、二次曲線の減衰の式で決まる。移動は約30ms・660Hz、決定は約60ms・880Hzから上昇、戻るは約45ms・440Hzから下降。ピークは第2倍音を含めて約-7dBFS。出力後には無音を送る。

**波形は起動時に一度だけ生成し、テーブルから読む。** 再生のたびに合成する旧方式は1回あたり約25.6msのCPUを使い（音の長さ59.4msの43%）、`audio_task`が優先度7で描画タスクと同じcore 0に張り付くため、キー入力のたびにフレーム時間を圧迫していた（詳細は[pie-simd.md §6.5](../perf/pie-simd.md)）。`main/hal/sfx_synth.h`が参照実装（旧合成）を保持し、`tools/test_sfx.py`が焼き込んだテーブルとの一致をホストで検査する。当初のピークは約-28dBFSで、PCMはI2Sへ届いていたが本体スピーカーでは耳を近づけないと聞こえなかった。

音声タスクはUIと分離し、最大4件のキューとI2S DMAを使う。
キュー満杯時は音イベントを破棄して入力を待たせない。
ミュート時は待機イベントと生成中のサンプルを無音化する。
音声初期化に失敗してもホームを利用できる。

## 検証

- 左右カテゴリ、上下項目、背景・FPS・音・音量のトグル、ミュート中はSFX送信なし・再有効化でPCM送信再開、再起動後の設定復元、エラー画面からの復帰: `tools/test_settings.py` が押下回数を数えてこれらを検査する。設定行を増減させたら同じ変更の中で調整すること（CLAUDE.md）。
- 容量・フラッシュ配分の現在値は[アーキテクチャ仕様](../platform/architecture.md)と`tools/test_flash_budget.py`を参照。この文書には数値を置かない——SKK辞書・フォント領域は変更のたびに動く。
- 自動検証はPCMのI2S送信までしか確認しない。音量を変更したときは実機で聴感を確認する。現在の振幅は本体スピーカーでの聴感で決めた。

```powershell
python tools/test_settings.py --port COM3
python tools/smoke_device.py --port COM3 --cycles 10
python tools/benchmark_background.py --port COM3
```

USB操作はa/b=左右、u/d=上下、e=Enter、q=Esc。
ベンチマークはSettingsで背景を変更するため、終了時には比較した背景が保存される。
