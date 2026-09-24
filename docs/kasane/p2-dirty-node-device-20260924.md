# P2 dirty-node 実機時間ゲート（2026-09-24）

Cardputer ADV / COM3、ESP-IDF 6.0.1。`65c2770`（直前版）と
`31b78f8`（dirty-node版）を同じ`KASANE_P0_PROBE=ON`、
copy/bus probe OFF、`POCKET_PROBES=OFF`、同じ`sdkconfig`でbuildした。
直前版image SHA-256は
`399DE067390B2E745561DBCDD303A140CA4DB08B8B2B56EFDF9DDD4C9BE3DDE7`、
今回buildしたdirty-node版は
`8AB1A3DA94AD25F8CF758A17404075CF6311759CC4FC01C0468415AC78AAF84C`。
以前準備したdirty-node imageのhashとは異なり、build/sourceパスも直前版と
一致しないため、バイナリ配置差を完全には隔離していない。

## hello 180更新

`tools/kasane_p0_hello_run.py`を各imageで2回。毎回181描画、
LCD 210,720 B / 557 bands、異常終了0。

| 指標 | 直前版 1 / 2 | dirty-node 1 / 2 | 固定上限 |
| --- | ---: | ---: | ---: |
| app render p95 | 1151 / 1151 µs | 1023 / 1023 µs | 1279 µs |
| app render p99 | 1151 / 1151 µs | 1151 / 1151 µs | 1407 µs |
| app send p99 | 895 / 895 µs | 1151 / 1023 µs | 1151 µs |
| app turn p99 | 255 / 255 µs | 255 / 255 µs | 383 µs |
| render / send max | 4556/7398、4590/7525 µs | 4597/7392、4562/7395 µs | 6000 / 9000 µs |
| 12 ms超過 | 0 / 0 | 0 / 0 | 0 |
| free / min / largest heap | 222176 / 117444 / 106496 B | 同一 | 下限218000 / 113000 / 102400 B |

全項目が[P0固定閾値](p0-timing-gates-20260924.md)内。render p95の
1 histogram bucket低下は見えるが、p99は同一。send p99はdirty-node版が
1～2 bucket高く、上限ちょうどの試行もある。これを純粋な高速化とは判定しない。

## SD music overlay 45秒＋pause/resume

許可済みSDの`music/KAKATO/KARA OK 2nd Edition`で、01→02へ遷移後に
15秒地点で2秒pause/resume。`tools/sd_async_compare_run.py`を
dirty-node→直前版→dirty-node（B-A-B）で実行した。

| 指標 | dirty 1 | 直前版 | dirty 2 | 固定上限 |
| --- | ---: | ---: | ---: | ---: |
| overlay draw p95 / p99 / max (µs) | 9087 / 9215 / 9405 | 9087 / 9215 / 9342 | 9087 / 9215 / 9397 | 9343 / 9599 / 10000 |
| overlay send p99 / max (µs) | 5247 / 5301 | 5119 / 5270 | 5247 / 5299 | 5247 / 5500 |
| overlay work p99 (µs) | 3583 | 3455 | 3455 | 3839 |
| ui frame p99 / max (µs) | 12287 / 192956 | 12287 / 193250 | 12287 / 192788 | 12287 / 200000 |
| ui frame >12 ms | 12/2147 | 12/2093 | 13/2059 | 1%以下 |
| min heap (B) | 43732 | 43732 | 43660 | 40960以上 |
| underrun / decode fault / IO ERROR | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 | 0 |

各overlay frameは64,800 B / 17 bands。free/largest heapは全試行
217,844 / 69,632 B、stack未使用量は23,692 B。全項目が固定閾値内。
draw p99は3回同一、send p99はdirty-node版で1 bucket高い。
music overlayはこの比較ではdirty-node schemaの主な更新対象ではないので、
共存回帰の短期確認でありdirty-nodeの速度改善を測るworkloadではない。

試験前に元の3 MiBアプリ領域が保存済みbackup 3区画と全件digest一致することを
確認した。試験後に3区画を復元し、再び全件digest一致を確認してCOM3を解放した。
生ログは`.cache/kasane-p2-ab-20260924/`。画面pixelの実機capture、
24 slot/hidden/page・LCD失敗repairの実機試験、厳密な同一配置A/Bは残る。
したがってP2の固定時間・音声共存ゲートは通過したが、P2出口全体は未達。
