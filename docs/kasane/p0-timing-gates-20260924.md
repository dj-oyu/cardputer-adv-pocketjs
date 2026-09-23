# P0 時間・転送量ゲート（2026-09-24 固定）

以下は P1 以降の実装**前**に固定したゲート。計測器のコピー計数を外した
`KASANE_P0_PROBE=ON`, `KASANE_P0_COPY_PROBE=OFF`, `POCKET_PROBES=OFF`
を比較用ビルドとする。P0 のヒストグラムは `sample=seen`、128 µs 幅の
上端値（`ui_frame` は1024 µs幅）。計数あり版は
[別のABBA試験](p0-copy-probe-abba-20260924.md)に記録し、時間の閾値と混ぜない。

## 通常アプリ hello

ESP-IDF 6.0.1、Cardputer ADV。`tools/kasane_p0_hello_run.py` を使い、
毎回再起動してhelloを開く→1秒静止→Enterを180回、各 `HELLO_COUNT n`
を確認→1秒静止→終了。同じ操作を2回。
アプリ領域のみの診断image SHA-256 は
`8772E8E774FEA28B7106AC29BF389C35D6C0C950E400165D806D9A55FF0E4683`。
各回181描画、LCD計210,720 B / 557 bands、`app_turn` 426/427件。
生ログ：`.cache/kasane-p0-20260924/hello-transfer-{1,2}/`。

| 指標 | 基準 1 / 2 | 後続の合格上限・下限 |
| --- | --- | --- |
| app_render p95 / p99 | 1151 / 1151, 1151 / 1279 µs | p95 ≤ 1279、p99 ≤ 1407 µs |
| app_send p95 / p99 | 767 / 895, 767 / 1023 µs | p95 ≤ 895、p99 ≤ 1151 µs |
| app_turn p99 | 255 / 255 µs | ≤ 383 µs |
| cold-startを含むrender/send max | 4638/7436、4600/7425 µs | ≤ 6000 / 9000 µs |
| 12 ms超え | 全系列で0 | 0 |
| LCD bytes / bands（181描画） | 210720 / 557（両回一致） | これ以下 |
| free / minimum / largest heap | 222176 / 117444 / 106496 B（両回一致） | ≥ 218000 / 113000 / 102400 B |
| stack high-water の未使用量 | 23692 B | ≥ 22500 B |

描画サンプルの少ない1更新試験は閾値決定に使っていない。旧計測の
hello画面は[既存P0記録](p0-device-baseline-20260923.md)で保存されている。
今回の計測は画面写真を追加していないため、新経路では同一画素のhost照合に加え、
実機画面取得を別途行う。

## SD再生中の music overlay

同じCardputer、SD `music/KAKATO/KARA OK 2nd Edition`。
`tools/sd_async_compare_run.py` の 45秒再生、15秒時点で2秒pause/resume。
計数なし2試行のimage SHA-256は
`07DCC123B72B909E54C026F57BCD7AAFCB31105BBE0654EF1E5FB42C9892723C`。
両回とも01→02曲へ遷移し、`player=2`、underrun=0、decoder faults=0、
IO ERROR=0。生ログ：`.cache/kasane-p0-20260924/trial-off-{1,2}/`。

| 指標 | 基準 1 / 2 | 後続の合格上限・下限 |
| --- | --- | --- |
| overlay_draw p95 / p99 / max | 9215/9343/9490、9215/9471/9542 µs | ≤ 9343 / 9599 / 10000 µs |
| overlay_send p99 / max | 5119/5265、5119/5280 µs | ≤ 5247 / 5500 µs |
| overlay_work p99 | 3711 / 3711 µs | ≤ 3839 µs |
| ui_frame p99 / max | 12287/192838、12287/192944 µs | ≤ 12287 / 200000 µs |
| overlay_draw / send 12 ms超え | 両回0 | 0 |
| ui_frame 12 ms超え | 16/2184、17/2093 | ≤ 1% |
| LCD per frame | 64800 B / 17 bands（両回一致） | これ以下 |
| free / minimum / largest heap | 217844/43632/69632、217844/43732/69632 B | ≥ 210000 / 40960 / 65536 B |
| stack high-water の未使用量 | 23692 B | ≥ 22500 B |
| underrun / decoder faults / IO ERROR | 両回0 / 0 / 0 | 0 / 0 / 0 |

この閾値は短い実機試験の揺らぎとヒストグラム幅を使った**回帰防止線**で、
長時間再生やSD着脱の安全性を証明しない。画素一致、BUSY/DISCARDED/repair、
低heap、長時間音声は各段階の別ゲート。閾値を超えたときは試験条件、
外乱、コード配置、ホットパスの順に切り分け、数値を事後的に緩めない。
同一バイナリA/Bが可能ならそれを優先し、旧→新→旧→新でも確認する。

各実機試験後、保存済み元ファームの 3 MiB アプリ領域を復元し、
`verify-flash` で両領域のdigest一致を確認した。COM3は解放済み。
診断OFFのESP-IDFビルドもPASSし、静的DIRAM 159,772 B、
アプリimage 1,982,976 Bと計測転送量追加前の値から変わらない。
