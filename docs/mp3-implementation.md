# Cardputer ADV MP3実装

2026-09-09。対象: `cardputer-adv-pocketjs`、ESP32-S3、ESP-IDF v6.0.1。

## 先行実装と採用理由

- [bigbag/cardputer_adv_player](https://github.com/bigbag/cardputer_adv_player/blob/master/docs/architecture-audio.md)
  はADVでminimp3と専用FreeRTOSタスクを使う先行例。スタック確保とUIからの分離を参考にした。
  ハードウェア初期化は既に動く本プロジェクトのものを再利用し、コピーしない。
- [lieff/minimp3](https://github.com/lieff/minimp3) の
  `ea99364f61c14656440e8d77e9c233ccf3124633` を採用。CC0、単一ヘッダー。
  `MINIMP3_ONLY_MP3`、`MINIMP3_NO_SIMD`、`-O2`。
  SSE/NEONはXtensaには使わない。独自のPIE最適化も追加していない。
- [ESP8266Audio](https://github.com/earlephilhower/ESP8266Audio) も先行例として確認したが、
  既存ESP-IDF出力を保つためArduinoプレイヤー層を追加する必要はないと判断した。

## 実行経路

`pocket.audio.player` → 既存ファイルread_at → 圧縮リング（3×2048B）→
再生時だけ作る`mp3dec`タスク → PCMリング（3×2048B）→ 既存`sfx` → I2S/ES8311。

`mp3_feed.c` がリングをまたぐフレームを2048Bの作業領域へ組み立て、
`mp3_decode.c` がminimp3の状態を曲全体で維持する。フレームごとの状態初期化はしない。
ステレオは32bit中間値で平均する。24kHzより高い入力は32tapの窓付きsinc FIRを通し、
整数位相による補間で24kHzへ変換する。位相・フィルター状態はフレーム境界でも維持する。
これは組込みスピーカー用の低コスト変換であり、高品質オーディオ用SRCの品質保証ではない。

## 性能とメモリ

初回実機試験（44.1kHz stereo 128kbps、合成音、描画と同時、COM3）では、
20回の完走と10回のpause/resumeでunderruns=0。MP3フレームは1152/44100秒≒26.12ms、
復号・変換・PCM発行の平均は約5.9〜6.0ms。約23%相当はこの比からの導出であり、
CPU使用率モニターの測定値ではない。ログ: `.cache/mp3-device-44100.log`。

最終ファームの3形式巡回試験も20回完走、奇数10回はpause/resume付きで、
全回underruns=0、復号faults=0。下表は完走した各曲のログを集計した実測。
ログ: `.cache/mp3-device.log`。時間はリング待ちを差し引き、他タスクの割り込みは含む。

| 入力 | 完走回数 | 曲内平均の範囲 | 全フレーム中の最悪時間 | 音声1フレームの長さ |
| --- | ---: | ---: | ---: | ---: |
| 44.1kHz stereo 128kbps | 7 | 5.925〜6.661ms | 10.020ms | 26.12ms（導出） |
| 48kHz stereo 320kbps | 7 | 5.989〜6.136ms | 7.040ms | 24ms（導出） |
| 24kHz mono 64kbps | 6 | 2.026〜2.151ms | 2.658ms | 24ms（導出） |

初回の20回試験と最終20回試験のアプリ終了時は、ともにfree=278,724B、
largest=159,744Bだった。開始状態と全アロケーションを照合した厳密なリーク証明ではないが、
この2試験の終了時メモリに差はない。人間による試聴やSDカード性能の測定はしていない。

実測stack_usedは17,640〜17,648B。スタックは24,576Bを確保。
S3ビルドのコンパイラー出力では `mp3dec_decode_frame` 自体が16,608Bを使用する。
Opusの12KiBスタックや`sfx`の4KiBに直接載せることはできない。

S3でworker状態6,896B、別途PCM4,608B、圧縮フレーム2,048B、stack24,576B、
2本のリング12,288B。合計50,416Bはソースとsizeofからの導出で、TCB・allocator管理費は別。
小さなバッファは分けて確保し、全体50KiBの単一連続領域を要求しない。
ただしstack24KiBの連続領域は必要で、空き総量だけでは可否を決められない。
リングは初回playからcloseまで保持し、workerとstackは停止・EOFで解放する。

## 実装範囲と残る制約

- 通常のMPEG-1/2/2.5 Layer III、CBR/VBR、mono/stereo。
- `app:` / `assets:` / 許可済みの`sd:`。HTTP(S) MP3は未実装。
- ID3v2.2〜2.4の先頭タグとID3v1末尾タグを除外。APE等の末尾タグは未対応。
- free-format、曲の途中のサンプルレート変更、破損フレームは拒否する。
- encoder delay/paddingやXing/LAMEによるgapless処理は未実装。
- 総時間はEOFまでnull、seekは無効。pause/resumeは先頭から復号して位置を復元するため、
  長い曲では待ち時間が伸びる。効率的なランダムシークには別途インデックスとreservoirの扱いが必要。
- SDは既存設定で400kHz。128kbpsは16KB/s、320kbpsは40KB/s、バスの理論上限は50KB/s
  （すべて導出）。ファイルの開閉・seek・描画との同居分を含めると320kbpsを保証できない。
  Flashからの実機試験をSD性能の証明には使わない。SD速度の実測と読み出し経路の改善は別の作業。
- すべての音源・無線同時通信・SDカード・UI負荷を網羅した性能保証ではない。

## 再現

`python tools/prepare_dependencies.py` または `python tools/prepare_minimp3.py`。
ライセンスは `licenses/minimp3.txt`。

```
. 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1'
idf.py -B build_mp3 build
idf.py -B build_mp3 -p COM3 flash
python tools/test_mp3_device.py --port COM3 --cycles 20
```

ホストでは `bash tools/test_mp3.sh <MP3ファイル...>`。ASan/UBSanで実物のデコーダーと
レート変換を実行し、24kHzのPCM一致と、各入力レートの整数出力サンプル数を照合する。
上流の44.1/48/24kHz・VBRベクトルと同梱合成音源を検査済み。
16/22.05/32kHzの上流ベクトルも同じ検査を通過した。
既存の`tools/test_stream.c`もASan/UBSanで通過。

最終ビルドのFlash予算: app=2,521,296B、上限3,145,728B、spare=624,432B。
MP3テスト音源3本も含む。これはビルド全体の実測で、他の未コミット変更を含むため
MP3デコーダー単独の増分とは扱わない。ソースの変更前から存在した変更は保持した。
