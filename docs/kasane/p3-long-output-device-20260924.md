# P3: 音声output sourceの全曲再生・pause/resume実機ゲート

2026-09-24、Cardputer ESP32-S3 / COM3。診断app image SHA-256
`dea4e4ecf8678eb26ac41fc03e135eefeaa1cf85bdb5bb485b42bc5b3efa3b9a`。
`KASANE_P0_PROBE=ON, KASANE_P0_COPY_PROBE=ON`で、SDの
`music/KAKATO/KARA OK 2nd Edition/02 インザハウス.mp3`を自然終端まで再生した。
JSはKasaneの`elapsed`を設定せず、音声taskの固定pool sourceを購読する。
45秒と140秒付近で各3秒pauseし、再開操作の受理と再生位置の進行再開を
別々に計測。実行前にpause中位置ずれ100 ms以下、再開受理1秒以下、
進行再開2秒以下、underrun/fault/IO ERROR 0を成功条件とした。

| 回 | pause位置 | 3秒後の位置ずれ | 再開受理 | 位置が50 ms進むまで |
| --- | ---: | ---: | ---: | ---: |
| 1 | 44,997 ms | 0 ms | 3.074 ms | 70.088 ms |
| 2 | 136,965 ms | 0 ms | 3.076 ms | 99.805 ms |

自然終端の再生位置は235,937 ms、2回のpauseを含む実時間は約242秒。
有効snapshot公開236件、固定pool skip 0、producer pool bufferからの
core text直接copy 236回/1,888 B、長さ不一致0。underrun、MP3 decoder fault、
source fault、IO ERRORは0。app render/send p99は1,023/767 µs、
12 ms超過はいずれも0。終端の`MP3DEC worst_us=3,035,912`はpauseを含む
観測区間に現れた値で、decode計算単独の所要時間とは解釈しない。
生ログとsummaryは`.cache/kasane-p3-long-device-20260924/y1/`（Git対象外）。

このMP3の`player.info().seekable`は`false`で、`seek(1000)`は
`NOT_AVAILABLE`を返した。これは**非対応契約の確認**であり、MP3の時間seekを
実装・検証したことにはならない。時間seek可能な別形式の実機試験は残る。
複数text destinationの実機copy計数と、別のSD曲・低heap条件も未検証。
診断OFFの製品app image 1,995,328 B、静的DIRAM 159,820 Bは前版と同値。
元3 MiB app領域をバックアップから復元し、3×1 MiB全区画のdigest一致を確認。
COM3は閉じた。
