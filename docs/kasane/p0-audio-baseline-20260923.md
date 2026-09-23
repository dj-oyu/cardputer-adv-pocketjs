# Kasane P0: SD再生を伴う実機予備測定（2026-09-23）

Cardputer ADV / COM3。`KASANE_P0_PROBE=ON` の診断アプリ
`.cache/kasane-p0-20260923/diagnostic/optimized-live-clone.bin`
（SHA-256 `12ebb9e3e07ec155680e726c5564c7d7581439d9407d47038681a8a373ebb46c`）を
アプリ領域 `0x10000` に書き込み、esptool のhash照合を確認した。
SDの許可対象は `music`、選曲は
`music/KAKATO/KARA OK 2nd Edition/01 KAKATORO.mp3`。
最初の曲の終了後、同じフォルダの `02 インザハウス.mp3` に自動で進んだ。
`PLAYER_OPEN` から終了までの実機時刻は約248秒。SDへの書込みはしていない。

| 観測項目 | 値 |
| --- | ---: |
| MP3デコーダー（2曲目停止時） | 5,769 packets、3,616,810 frames、faults 0、平均4,259 us、最悪147,592 us |
| 再生セッションのoverlay work | 7,505件、p95 3,211 us、p99 3,393 us、最大184,387 us、12 ms超3件 |
| 同overlay draw | 7,507件、p95 9,046 us、p99 11,721 us、最大16,054 us、12 ms超114件 |
| 終了時heap | free 223,996 B、boot以降minimum 54,820 B、largest 73,728 B |
| stack high-water | UI task未使用23,708 B、MP3 task使用17,680/24,576 B |

p95/p99は256件reservoir推定で、maxと12 ms超過件数は全件集計。
ログ中に `IO ERROR`、`PLAYER_FAIL`、panicは見られなかった。
ただし `faults=0` は音声underrun 0を意味しない。現在のログに
underrun専用カウンタがなく、実際の可聴連続性も機械計測していない。
ENTERを一度送ったが、pause/resumeの往復・再開待ち時間を示すイベントログを
得られなかったため、この操作の性能は未判定。

無音時のmusic overlay drawは[p0予備基準](p0-device-baseline-20260923.md)で
p99 9,089 us / 12 ms超0件。再生時はp99 11,721 us / 114件となり、
音声共存ゲートの閾値を「無音時から劣化なし」と置くことはできない。
この測定は診断版1条件のみで旧・新の同一曲A/Bではない。
画素・SD状態・電源条件の完全固定も未実施であり、P0出口は未達。

最初のSDルート選択で `System Volume Information` を誤選択したが、
再生には用いず、アプリを終了した。`pocket_fs_reset()` が
`sd_media_unmount()` を呼んでそのセッションの許可を破棄する。
次の起動で `music` を明示的に選び直した。
終了後は退避済みの3 MiBアプリ領域を元のoffsetへ書き戻し、2領域とも
esptoolのhash照合PASS。HOME OVERLAY設定は元の2のまま、COM3は閉じた。

生ログ：`.cache/kasane-p0-20260923/music-audio/serial.log`。
ホスト契約テストは `UBSAN_OPTIONS=halt_on_error=1` と
`ASAN_OPTIONS=halt_on_error=1` でPASSし、24-slot workloadはsanitizer/
`-O2 -fstrict-aliasing` の両方で画素一致。
ログ：`.cache/kasane-p0-20260923/contract-workloads.log`。

この測定後、診断構成の`app_stop()`に音声snapshotの`underruns`と
位置・状態の終了時ログを追加し、clock sourceのproducer materializationも
copy内訳へ追加した。ESP-IDF診断ビルドはPASS、次回用バイナリは
`.cache/kasane-p0-20260923/diagnostic/probe-underruns.bin`
（SHA-256 `57ae582a2a7c6acef59e692612dd0958a3ad1ad244ce49ddf6d1e011210cf1ac`）。
**この新バイナリは今回の実機測定には使っていない**。

## 追試：全件ヒストグラムとunderrun（同日）

256件reservoirは長時間セッションのp99推定が不安定なので、診断専用の
128 µs幅・全件ヒストグラムへ変更した。表示する分位点は該当binの上端で、
真値に対する誤差は0–127 µs。32,640 µs以上のtailに分位点が入った場合は
最大値を表示する（tail件数も記録）。12 ms超過数と最大値は従来どおり厳密。
静的DIRAMは旧reservoirより32 B減。host単体テストとESP-IDFビルドを通した。

実機バイナリ：`.cache/kasane-p0-20260923/diagnostic/probe-hist128.bin`
（SHA-256 `799e45a12c54ec484d8ed1740c4279690e5bb3c390c4045ff96230e55da32cdc`）。
同一boot・同一バイナリで `music/KAKATO/KARA OK 2nd Edition` の先頭2曲を
連続再生、無音music overlay、再び同じ2曲を連続再生の順に計測した。
Settingsは `background=1 fps=0 sound=1 overlay=2 volume=2`、SDは25 MHz。
電源電圧・背景アニメーション位相は固定していない。

| 区間 | overlay draw件数 | p95 / p99 / 最大 (us) | 12 ms超 | 音声underrun / decode fault |
| --- | ---: | ---: | ---: | ---: |
| 再生1（約182秒） | 5,784 | 15,743 / 15,999 / 19,761 | 1,578 (27.3%) | 0 / 0 |
| 無音music（約60秒） | 1,762 | 8,575 / 8,575 / 8,590 | 0 | 対象外 |
| 再生2（約163秒） | 4,904 | 15,743 / 16,127 / 19,632 | 1,404 (28.6%) | 0 / 0 |

再生1・2とも `01 KAKATORO.mp3` が約14秒で終了して
`02 インザハウス.mp3` へ自動移行した。2曲目の停止時positionはそれぞれ
167,648 / 149,168 ms。2曲目のMP3 decode最悪は4,261 / 4,228 µs。
`IO ERROR`、`PLAYER_FAIL`、panicは観測されなかった。
別の追試では同じ2曲目で`playing → paused → playing`を確認し、
同じstream idを保持、終了時underrun 0だった。ただしキー押下から
可聴音の再開までを計測したわけではない。

再生2回のp99差は128 µs（ヒストグラム1bin）、12 ms超過率差は1.3pt。
無音との差は約7.4–7.6 ms。少なくともこの設定では、再生中の描画が
無音基準より大きく遅い。どの部分がCPU競合、SD、LCD転送、背景合成に
由来するかは未分離であり、Kasane差分処理の劣化と断定しない。
最適化前の再生基準として保存し、新経路の合否は同一条件A/Bと
固定閾値で判定する。P0の画面・電源条件の固定と通常アプリworkload、
計数範囲の完成はまだ残る。

生ログ：`.cache/kasane-p0-20260923/music-audio-transition/serial.log` と
`.cache/kasane-p0-20260923/music-audio-hist128/serial.log`。
アプリ領域は測定後に退避済み3 MiBへ復元し、esptool照合PASS。
SDは変更せず、HOME OVERLAY設定は元の2、COM3は閉じた。

この追試の後に`render_decode_text`、音声snapshot materialization、
music status文字列materializationの計数箇所を追加した。
上のcopyログには**これらの後追加項目は含まれない**。全経路copyの
完成判定には、計数を追加した診断版での再測定が必要。
同様にoverlay drawをLCD送信時間とそれ以外に分け、実際の転送bytes/帯数を
集計する診断も後追加した。上のp99とLCD値の同時帰属は次の実機測定で検証する。
この次回用バイナリは`.cache/kasane-p0-20260923/diagnostic/probe-transfer-copy.bin`
（SHA-256 `21340951ee9168698797a2867d3be4a4a4cfa55d10728286d4f96c1d11dd5650`）。
診断用DIRAMは約7.5 KiBで、ヒストグラム追試版より約2.1 KiB増える。
上表には混ぜず、次節にこの診断版の測定値を分けて示す。

## 追試：描画の送信・計算とcopy内訳（同日）

`probe-transfer-copy.bin` をCardputer ADVへ書き込み、esptoolのhash照合PASS。
同一boot・同一バイナリで先頭2曲を再生し、その後に無音music overlayを測った。
再生は2曲目のposition 205,269 msまで継続し、decoderは7,862 packets、
4,928,992 frames、faults 0、平均3,691 µs、最大4,243 µs、
stack使用17,660/24,576 B。音声underrunは0。無音側は約82秒間の描画。

| 区間 | 件数 | draw p95 / p99 / 最大 (µs) | send p95 / p99 / 最大 (µs) | compute p95 / p99 / 最大 (µs) | draw 12 ms超 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 再生 | 6,990 | 15,743 / 16,127 / 19,080 | 11,263 / 11,775 / 12,118 | 8,063 / 11,135 / 13,867 | 1,881 (26.9%) |
| 無音 | 2,407 | 8,447 / 8,575 / 8,820 | 5,247 / 5,247 / 5,296 | 3,455 / 3,455 / 3,558 | 0 |

sendとcomputeは同一フレームで分割計測したが、各分位点は別々の
フレームを指すためp99同士を足さない。再生中は送信側・送信以外の
両方が悪化した。転送は再生6,990フレームで452,952,000 B / 118,830帯、
無音2,407フレームで155,973,600 B / 40,919帯。どちらも
**1フレーム64,800 B・17帯で全画面転送**し、転送量差はない。
したがってLCD bytesの削減効果はこの測定では示されていない。

再生時の計数対象copyは204,432回 / 2,165,809 B、無音時は
31,325回 / 308,315 B。再生時の内訳はproducer materialized
102,304 B、music plan 371,575 B、music materialized 44,966 B、
core payload write 298,088 B、core payload read 606,824 B、
core submit text 147,114 B、core render text 297,469 B、
render decode text 297,469 B。これは**観測済み部分の合計**で、
QuickJS内部や未計数の構造体copyを含む全経路の総量ではない。
この実機版を作った後、music view model構造体のcopy計数も追加したので、
上の数値にその後追加分は含まれない。

heap終了時 free 221,892 B、boot後minimum 52,740 B、largest 73,728 B、
UI task stack未使用23,708 B。音声fault、underrun、IO ERROR、panicは
観測されなかった。ただし今回も電源・画面位相を固定しておらず、
計測なしバイナリとのp99差、通常アプリworkload、旧・新経路A/Bは未測定。
送信時間の増加をKasane処理だけに帰属させない。

生ログ：`.cache/kasane-p0-20260923/music-transfer-copy/serial.log`。
測定後、退避した3 MiBアプリ領域を元のoffsetへ書き戻し、
esptoolのhash照合PASS。HOME OVERLAY設定は元の2、COM3は閉じた。
後追加したmusic model copy計数を含むhost契約テストとQuickJS統合テストは
それぞれPASS（統合テスト0 failures）。ESP-IDF通常buildは再実行時に
コンパイルjobが進まず中断したため、**後追加分の実機build検証は未了**。
再flashはしていない。
