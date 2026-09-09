# MP3 PLAYBACK

Apps末尾から起動する実機検証アプリ。既存Opusと同じ `pocket.audio.player` を使い、
再生・停止・closeを繰り返す。奇数回は約700msでpauseしてからresumeする。ESCで終了。

音源はFFmpeg/libmp3lameで合成したもの。第三者の曲は含まない。
`python tools/make_mp3_assets.py --ffmpeg <ffmpegの実行ファイル>` で再生成できる。

| ファイル | 入力 | 内容 |
| --- | --- | --- |
| test-tone.mp3 | 44.1kHz stereo 128kbps、4秒 | 541.7Hz、左右の振幅を変更 |
| test-48k.mp3 | 48kHz stereo 320kbps、2秒 | 左右別の複数正弦波と雑音 |
| test-24k.mp3 | 24kHz mono 64kbps、2秒 | 541.7Hz |

`-write_xing 0 -id3v2_version 0` を指定。MP3フレームのパディング込みの長さは元の
PCM秒数より長い。`tools/test_mp3_device.py` はファイルの全ヘッダーから独立に
出力サンプル数を計算して照合する。

```
python tools/test_mp3_device.py --port COM3 --cycles 20
```

`MP3DEC` の mean/worst は復号・モノラル化・レート変換・PCM発行の時間。
リングが満杯で待った時間と入力待ちは除外するが、他タスクからのプリエンプトは含む。
`MP3_DONE` は実際の音声出力完了、位置、underruns、描画フレーム時間を記録する。
音切れカウンター0はソフトウェア側の判定であり、人間による試聴の代わりではない。
