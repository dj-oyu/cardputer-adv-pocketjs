# P1 固定pool sourceの別task実機ゲート（2026-09-24）

Cardputer ADV、COM3、ESP-IDF 6.0.1。診断image SHA-256は
`EA1FAA809AAFA8C0387CB1B895ABF4D1AFC9097804CD55509F37286C37117F75`。
`KASANE_P0_PROBE=ON`、copy計数OFF、bus probe OFF。
元の3MiBアプリ領域は試験前後とも1MiB×3のdigest照合がPASSした。
生ログ・RGB565は`.cache/kasane-pool-source-device-20260924/`。

USB診断`0`の`apps/kasane/pool_source_probe.js`は汎用runtime descriptorを
mountし、`pocket.time.poolProbeSource()`の2つのtext fieldを1回だけbindする。
以後JSはslotをsetしない。別FreeRTOS taskが100 msごとに3 slot poolの
未公開slotへ完全snapshotを直接構築してpublishする。Kasaneの汎用
`ksn_source_pool_adapter`とregistryを通り、readerはpinした世代を借用する。
Kasane detach→task停止→registry解除の寿命順序を使い、診断OFF buildには
このtaskと公開methodを入れない。

| 試験 | 結果 |
| --- | --- |
| LCD前後画素、2回 | 各19画素変化、表示領域外0、publish 21回、skip 0、エラー0 |
| captureなし20秒 | publish 201回、skip 0、app_render p99 767 µs、app_send p99 639 µs |
| source終了後heap | free 222,144 B、min 117,412 B、largest 106,496 B、stack未使用 23,692 B |
| 再起動直後hello 180更新 | render p99 1,279 µs、send p99 895 µs、turn p99 255 µs、LCD 210,720 B / 557帯、固定P0閾値PASS |
| sourceをcaptureなしで起動・停止後hello 180更新 | largest 106,496 Bのまま、render p99 1,279 µs、LCD量不変、固定P0閾値PASS |
| 同じ診断imageのSD音声45秒＋pause/resume | draw p99 9,215 µs、send p99 5,247 µs、ui_frame p99 12,287 µs、underrun・decoder fault・IO ERROR 0、固定短期閾値PASS |
| 診断OFF製品build | PASS、静的DIRAM 159,804 B（前回と同値） |

`tools/kasane_pool_source_device.py --no-capture`の20秒試験では
202回描画、LCD 272,160 B / 419帯、12 ms超描画0。sourceが10 Hzで
数字を変えるのでhelloの180回Enter試験とは同じworkloadではない。
通常helloと音声試験は固定P0閾値で別に判定した。source taskと音声再生の
**同時実行は今回未試験**であり、音声共存をこの結果から推論しない。

最初の音声試験2件は、music overlayが未起動でEnterがhelloを開いたため
無効。`tools/overlay_device_test.py`のserial openでDTRが立つと、終了後の
再起動でoverlay設定値2だけが残り、boot/start guardによりoverlayは
再armされない。DTR/RTSをopen前からfalseにしてOFF→ONで再armし、
実際に`PICK 2 folders`、01→02曲の遷移、pause/resumeをログで確認した
3件目だけを音声試験として採用した。設定値は2へ復元した。

画素captureを2回含む診断の後に実行したhelloではlargest heapが
77,824 Bとなり固定下限102,400 Bを下回った。再起動直後は106,496 B、
captureを使わないsource起動・停止後も106,496 Bで、freeとmin heapは
いずれも基準内。原因はcaptureを含む先行workloadか繰返し使用による
断片化に絞られたが、単独原因は未確定。captureありの試行を
通常アプリのメモリgateに混ぜない一方、この異常値は未解決として残す。

hostではpool adapterの並行producer・複数consumer契約を
ASan/UBSanと`-O2 -fstrict-aliasing`で再実行し、両方PASS。
この実機結果は固定pool→汎用provider→mountの別task接続を証明するが、
P1全体の旧clock画素一致、音声との同時実行、長時間・SD抜去等の出口は
まだ満たさない。
