# P1 音声共存時のLCD送信分解（2026-09-24）

P1のclock source移行後、music overlayの描画p99が同じ実行payloadでも
約9 msと約16 msに揺れた。clock sourceはmusicから購読されていない。
原因を切り分けるため、製品ではOFFの`KASANE_P0_BUS_PROBE`を追加した。
このオプションは`KASANE_P0_PROBE`を必須とし、LCD転送のbyte-swap、
前回転送完了待ち（`spi_device_get_trans_result`）、次回転送queueを
フレーム単位で集計する。SD workerは`read_at`の間だけatomic counterを
更新する。音声taskのUI待ち、フレーム中のログ、動的確保は追加しない。
診断版の常駐DIRAM増加は約6.2 KiBであり、製品版のコストではない。

Cardputer ADV / ESP-IDF 6.0.1、COM3、同一診断app image
SHA-256 `7958FB8132A054567F0ABA27C461174802233B3255867C59A86D245F3365CBE3`。
SDの`music/KAKATO/KARA OK 2nd Edition`で01→02曲を約45秒再生し、
約15秒で2秒間pause/resumeを行った。いずれもMP3 decoder fault、
underrun、IO ERRORは0。p99は各系列内の分位点で、列を足して一つの
フレームの所要時間とはしない。

| 同一image試行 | draw p99 | send p99 | swap p99 | reap p99 | queue p99 | work p99 | send >12 ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1（速い） | 9,343 µs | 5,247 µs | 127 µs | 4,735 µs | 255 µs | 3,583 µs | 0/2,140 |
| 2（遅い） | 16,767 µs | 12,159 µs | 127 µs | 11,391 µs | 255 µs | 7,423 µs | 24/2,098 |

`lcd_send_sd`は**同一フレーム内の少なくとも1回のreap待機にSD readが
重なった**分類であり、send全体でSDが常時activeだったという意味ではない。
判定はreap前後のactive値とread開始epochで行う。厳密な開始・終了時刻や
DMA実行区間は測っていない。試行1の重複547フレームではsend p99
5,119 µs、非重複1,593フレームでは5,247 µs。試行2の重複650フレーム
では12,415 µs（>12 msが24）、非重複1,448フレームでは9,343 µs
（>12 msは0）。試行2の`lcd_reap`最大は12,278 µs。

遅い試行ではbyte-swapとqueueのp99は変わらず、reap待機が主に増えた。
一方で`overlay_work`/`overlay_compute`も悪化しており、LCD送信だけの問題
とも断定できない。SD readとの重複は遅いフレームと相関したが、
LCD（SPI2）とSD（SPI3）のDMA/メモリ競合、SPI driver完了処理、
UI taskのスケジューリング遅れのどれが支配的かは未確定。
SD側の最大read時間は速い試行16,783 µs、遅い試行10,654 µsで、
単純な「SD read自身が遅い」説明には一致しない。

この結果でP1描画gateを合格扱いにはしない。次はSPI転送の物理完了と
`get_trans_result`からUI task復帰までを区別できる計測、または安全な
SD workerのboundedな実行タイミング比較を行う。リング空き・音声deadline
を無視したSD停止や、閾値の事後緩和は採用しない。診断機は試験後に
元の3 MiBアプリ領域を復元し、両領域の`verify-flash`一致を確認した。
生ログは`.cache/kasane-p1-bus-20260924/trial-{1,2}/serial.log`。
