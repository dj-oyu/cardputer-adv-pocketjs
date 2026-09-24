# P3: 音声task source pool枯渇と回復の実機ゲート

2026-09-24、Cardputer ESP32-S3 / COM3。同一診断app image SHA-256
`7a777076ec7bb85d41a58f704cebd638bf20b6efd750b558a88c7c77972815f9`、
`KASANE_P0_PROBE=ON, KASANE_P0_COPY_PROBE=ON`。
SDの`music/KAKATO/KARA OK 2nd Edition/02 インザハウス.mp3`を
各45秒再生した。`x`は音声taskが初期2世代をpinし、8秒の位置で解放する
**診断専用**経路。`v`は同じバイナリの通常購読で、`x→v`の順に実行した。

| 実行 | pin/release | pool skip | valid publish | producer buffer→core text copy | underrun / decoder fault |
| --- | ---: | ---: | ---: | ---: | ---: |
| `x` 強制枯渇 | 2 / 2 | 5 | 41 | 41回 / 328 B | 0 / 0 |
| `v` 通常購読 | 0 / 0 | 0 | 46 | 45回 / 360 B | 0 / 0 |

`x`の最終公開frameは1,080,064（45秒まで回復して更新）、
`positionMs=45040`、無音補填block 0。音声taskはpoolが埋まると
`ksn_source_pool_begin()`の固定3-slot探索から`KSN_BUSY`で戻り、
UIを待たず、旧2世代はpin中不変。pin解放後の新しいsnapshotは
coreに直接渡され、文字列長不一致0。続く`v`のskip 0・pin 0は
セッション終了時のpin解放と通常動作への復帰も示す。
`v`のvalid publish 46に対しcore copy 45なのは更新の合流が1件あり、
全publishを描画すると主張しない。

`x`のapp render/send p99は1,023/7,423 µs、`v`は
1,023/7,551 µs。両方とも12 ms超過0。ただし更新フレーム数44対48、
診断copy計数ONであり、描画速度が等しいという厳密なA/B判定には用いない。
ログとsummaryは`.cache/kasane-p3-exhaust-device-20260924/{x1,v-after}/`
（Git対象外）。

診断OFFの製品app imageは1,995,328 B、静的DIRAM 159,820 Bで前版と同値。
元3 MiB app領域をバックアップから復元し、3×1 MiB全区画のdigest一致を確認。
COM3は閉じた。P3の長時間、seek/pause、複数destination実機ゲートと、
producer生成・bank clone・render scratchを含む全経路1-copyは未達。
