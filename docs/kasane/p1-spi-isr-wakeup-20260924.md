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

## ISR / task affinity の独立比較

同じREOPEN reader、同じ01→02曲、2曲目約45秒、15秒時点で2秒pause/resume。
すべて`KASANE_P0_PROBE=ON`、`KASANE_P0_BUS_PROBE=ON`。
SPI2 ISRをcore 1へ固定した状態で、decoderと音声出力taskの配置だけを
切り替えた。出荷構成ではこれらの診断optionはすべてOFF。

| decoder core 0 | 出力 core 0 | 有効試行 | draw p99 | post-ISR p99 | draw >12 ms | 音声fault / underrun |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| いいえ | いいえ | 1 | 16.255 ms | 7.167 ms | 101 | 0 / 0 |
| はい | はい | 3 | 各9.471 ms | 各0.255 ms | 各0 | 0 / 0 |
| はい | いいえ | 2 | 各9.471 ms | 各0.383 ms | 各0 | 0 / 0 |
| いいえ | はい | 1 | 16.127 ms | 7.039 ms | 109 | 0 / 0 |

ISR core 1だけを変えた最初の有効試行もdraw p99 16.895 ms、post-ISR
p99 7.807 msで遅かった。終了時ログでSPI2 ISRがcore 1で走ることを
確認した。音声出力taskは未固定でも終了時core 0、decoderは未固定の
試行で曲によって終了時core 1 / 0となった。終了時の1点観測では
途中のmigrationは証明できない。

このA/Bではdecoderをcore 0に固定した2構成だけが再現して速い。
音声出力taskだけの固定とISR core 1だけでは足りない。主因の候補は
decoderがUI core 1を長く占有してUI taskのSPI完了復帰を遅らせること。
ただし今回の試行はすべてISR core 1なので、decoder固定**単独**で
出荷構成を改善するかは未測定。また45秒×数試行では長時間再生、
SD抜去、音声deadline、全体の消費電力を合格扱いにできない。

image SHA-256: ISR1 / task未固定
`C9F7D7C22ACACC64BFAB753BF8584BF49E11FFA65C826361B7C40A211684E8AF`、
両task core 0
`2EC8FC208F8D80FAEAF521BE7892FDB12FF4C32A6CDAB5C4CBCE14C5DD403005`、
decoderのみcore 0
`E6D30BFABD3A82FC8A1A99A97F16F28E7255DCEBA6E662B0848800373676795B`、
出力のみcore 0
`EEE9D237F46031E0E35B4D7E94E35DD2C749692DADE247A8F3B7CD5CA7A38B79`。
生ログは`.cache/kasane-p1-affinity-20260924/`。
最初のISR1および出力のみ試行には、前のアプリ状態からhelloが開いていた
無効試行が各1回あり、表から除外した。

試験後、保存してあった元のapp領域3 MiBを0x10000と0x110000の2領域に
復元し、双方`verify-flash`でdigest一致を確認した。COM3は解放済み。

## SPI2 ISRを既定配置に戻したdecoder単独gate

追加image SHA-256
`394DEBDD007AA8EAE5BC3676BC3FAFA16F05FBF3B0AB3BCD2E9510C8A1C5D089`。
`KASANE_P1_LCD_ISR_CORE1=OFF`、`KASANE_P1_DECODER_CORE0=ON`、
`KASANE_P1_OUTPUT_CORE0=OFF`。終了時にSPI2 ISR core 0、decoder core 0を
実測した。同じ01→02曲＋pause/resumeを3回、その後同一imageで240秒の
01→02→03曲＋pause/resumeを実施した。

| 試行 | draw p99 | post-ISR p99 | draw >12 ms | MP3 fault / underrun |
| --- | ---: | ---: | ---: | --- |
| 45秒 1 | 9.471 ms | 0.383 ms | 0/2,563 | 0 / 0 |
| 45秒 2 | 9.471 ms | 0.383 ms | 0/2,024 | 0 / 0 |
| 45秒 3 | 9.471 ms | 0.383 ms | 0/1,998 | 0 / 0 |
| 240秒 | 9.471 ms | 0.383 ms | 0/7,803 | 0 / 0 |

長時間試行では`missing_isr=0`、SD read最大17.764 ms、`slow=0`、
`low_water=1`、最低heap 35,412 B、UI stack残23,692 B。
この結果から**decoder core 0固定のみ**をMP3経路の既定動作へ採用する。
ISR core 1固定・音声出力task core 0固定は診断optionのままで既定OFF。
採用後の診断OFFファームはESP-IDF 6.0.1でリンク・容量検査に合格し、
実QuickJS統合はASan/UBSanのO1と`-O2 -fstrict-aliasing`で0失敗。
ただし採用後の診断OFF image自体は実機へ書き込んでいない。
汎用source API、長時間以外のSD抜去、全アプリ画素一致などは未達であり、
これだけでP1全体を完了扱いにしない。

追加ログは`.cache/kasane-p1-affinity-20260924/decoder-isr-default-*/serial.log`。
追加試行後も元の3 MiB app領域を復元して2領域を`verify-flash`、COM3を解放した。
