# P1 音声共存時のSPI割込み後待機（2026-09-24）

[LCD送信分解](p1-lcd-sd-contention-20260924.md)の次の診断として、
`KASANE_P0_BUS_PROBE`にSPI2 `post_cb`の時刻を追加した。callbackは
SPI master ISRが転送完了を処理し、結果queueへ通知する直前に走る。
これは**物理DMA完了の正確な時刻ではない**。`tx_reap()`の開始から
callbackまでを`lcd_pre_isr`、callbackから`get_trans_result`復帰までを
`lcd_post_isr`としてフレームごとに集計した。ISRでは32-bit atomicへ
時刻を書くだけで、ログ・確保・待機をしない。診断OFFにはcallbackを
登録しない。

Cardputer ADV、同一診断app SHA-256
`39CB57B321372ABB2626DEE7138B1A2AB0840E3D4C0B6AE7880195F5AD499DC2`。
SDの`music/KAKATO/KARA OK 2nd Edition`で01→02曲、2曲目を約45秒再生し、
15秒で2秒pause/resumeを行った。両回ともMP3 fault、underrun、IO ERRORは0。
`missing_isr=0`。分位点は系列ごとの上限値であり、横方向に加算しない。

| 同一image試行 | draw p99 | send p99 | reap p99 | pre-ISR p99 | post-ISR p99 | send >12 ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1（遅い） | 16,767 µs | 12,287 µs | 11,519 µs | 4,607 µs | 7,807 µs | 36/2,041 |
| 2（速い） | 9,471 µs | 5,247 µs | 4,735 µs | 4,607 µs | 383 µs | 0/1,964 |

遅い/速い試行でpre-ISR p99は同じ4,607 µsだった。送信p99の差は
主として**ISR callback後からUI taskが結果を受け取るまで**に現れた。
したがって、この条件の主要な不安定性を物理SPI転送時間だけで説明する
仮説は弱い。前回と同様、遅い試行の送信>12 msはSD readとの重複群に
偏った（36件、非重複群0件）。ただし今回の集計はSD overlap別の
post-ISR分位点を持たないため、SDがUIの復帰遅延を引き起こしたとは
まだ断定できない。高優先度taskの配置、CPU1上の実行、ISR core、
cross-core通知のどれが主因かは次の切り分け対象である。

次の比較は、現在のREOPEN readerと同じ音声経路を維持し、SPI2 ISRの
core配置とdecoder/audio taskのcore配置を**別要因として**測る。
既存のC variantは両者とpersistent readerを同時に変えたため、
その結果だけでは因果を判定できない。描画p99、音声deadline、
低水位、stack/heap、入力、停止・pause/resumeを固定gateで確認し、
優先度を安易に上げたり音声taskを待たせたりしない。

生ログは`.cache/kasane-p1-isr-20260924/trial-{1,2}/serial.log`。
実機は試験前の先頭1 MiBと、試験前にdigest一致を確認した既存の後半2 MiBに
復元し、2領域とも`verify-flash`で一致を確認してCOM3を解放した。
P1の音声描画gateは引き続き未合格。
