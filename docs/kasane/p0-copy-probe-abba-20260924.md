# P0 コピー計数オーバーヘッド ABBA 実機比較（2026-09-24）

P0 の `KASANE_P0_PROBE=ON` は時間ヒストグラムを常に残し、
`KASANE_P0_COPY_PROBE` だけを切り替える。両版とも `POCKET_PROBES=OFF`。
同じソース・ESP-IDF 6.0.1・設定からビルドした。計数あり/なしの
アプリ image SHA-256 はそれぞれ
`BFDB96A7593AF3D9B8B702513D770FB03DC8CEA23CD869F3EB9150B10901D9AD` /
`07DCC123B72B909E54C026F57BCD7AAFCB31105BBE0654EF1E5FB42C9892723C`。
計数なしは静的 DIRAM が 256 B、image が 1,488 B 小さい。

Cardputer ADV、COM3、SD の `music/KAKATO/KARA OK 2nd Edition`、
background `mode=1 async=1 clock=DEMO`。各回の新規起動後、
`01 KAKATORO.mp3` を開いて `02 インザハウス.mp3` への自動遷移を待ち、
15 秒再生→2 秒一時停止→再開後30秒で終了した。
操作は `tools/sd_async_compare_run.py --play-seconds 45 --pause-after 15
--pause-seconds 2`。試行順は計数なし→あり→あり→なし。
ログは `.cache/kasane-p0-20260924/trial-{off,on}-{1,2}/`。
電源条件と画面の写真は取得していない。フレーム数は起動時刻差で一致しない。

| 順 | copy計数 | draw p95/p99/max µs | send p99 µs | ui_frame p99 µs | frames | heap min B | audio |
| --- | --- | --- | ---: | ---: | ---: | ---: | --- |
| 1 | なし | 9215/9343/9490 | 5119 | 12287 | 2179 | 43632 | underrun/fault/IO ERROR = 0 |
| 2 | あり | 9215/9343/9431 | 5247 | 12287 | 2115 | 43476 | 同 0 |
| 3 | あり | 9215/9343/9532 | 5247 | 12287 | 2117 | 43476 | 同 0 |
| 4 | なし | 9215/9471/9542 | 5119 | 12287 | 2088 | 43732 | 同 0 |

分位点は 128 µs 幅ヒストグラムの上端。`ui_frame` は 1024 µs 幅。
全試行の LCD は 1 frame 当たり 64,800 B / 17 bands。
draw の p99 回帰は検出できず、通常動作時の 12 ms 超えは 0。
ただし send p99 は計数ありで 128 µs 高く、2秒 PERF 平均の send も
計数なし約 4.84 ms、あり約 4.88 ms だった。この差をゼロとみなさず、
時間の基準には**計数なし版**を用いる。差の原因はこの4試行では
カウンタ処理とバイナリ配置/キャッシュを分離できない。

計数ありでは observed total が約 2.06–2.07 MB / 45秒、約
87.9–88.4k calls。これは[計数境界](p0-copy-coverage-20260924.md)に
限定した論理転記量であり、1-copy または実メモリ帯域の測定ではない。

試験前の 3 MiB アプリ領域は保存済みファームと全領域一致した。
試験後に `0x10000` の 1 MiB と `0x110000` の 2 MiB を復元し、
両領域で `verify-flash` digest 一致。COM3 は解放済み。
