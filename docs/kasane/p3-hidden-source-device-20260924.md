# P3: 音声sourceの非表示→再表示を実機で確認

2026-09-24、Cardputer ESP32-S3 / COM3。診断版のapp SHA-256は
`79a5360b0705e53dd1c3d816439e7ecae76e62b053a36a18699689e1d798ddc3`。
`KASANE_P0_PROBE=ON, KASANE_P0_COPY_PROBE=ON`、
`music/KAKATO/KARA OK 2nd Edition/02 インザハウス.mp3`を45秒再生した。
`output_source_probe_hidden.js`は、音声taskの固定pool textを購読し、
JSが`elapsed`を`set`することなく、開始8秒でtext nodeだけを非表示、
28秒で再表示にする。SDや音声producerは表示切替中も動く。

- ログ上の非表示・再表示は再生開始から約8秒・28秒。総publish 47、
  valid publish 45、pool skip 0。
- producer pool bufferをコピー元にしたcore text受理は27回/216 B、
  長さ不一致0。約20秒の非表示期間に対応して有効publishより少ない。
- 非表示時の全画面RGB565 captureでは、text bounds内の背景色以外の画素は0。
  再表示時は111画素が文字色になり、差分のbboxは`x=4..50,y=4..10`。
  text bounds外の差分は0。拡大画像の表示は`00:00:28`で、再表示時の
  再生位置に追いついており、古い`00:00:08`ではない。
- 45秒再生の`positionMs=44986`、underrun 0、MP3 decoder fault 0。

再現ログ・2枚のRGB565・拡大画像・summaryは
`.cache/kasane-p3-hidden-device-20260924/w2/`（Git対象外）。
非表示captureのSHA-256は
`a6b8ecc09eeffa47d65b8163e9d518faaf99609f639c307ed9542f4a9d9f080b`、
再表示captureは
`9e9149a2ce55c41282b7b26a553c487f3bf6cbb749f2ce80a3e80279aa165fe3`。

全画面captureをシリアルへ転送した試行なので、`app_send`のp99
960,666 µsは診断I/O負荷を含み、製品の描画性能値に使わない。
固定pool枯渇と長時間・pauseは後続の
[枯渇試験](p3-pool-exhaust-device-20260924.md)・
[全曲再生試験](p3-long-output-device-20260924.md)で確認した。
seek可能形式と複数destinationは別ゲートとして残す。
診断OFFの製品app image 1,995,328 B、静的DIRAM 159,820 Bは前版と同値。
元3 MiB app領域をバックアップから復元し、3×1 MiB全区画のdigest一致を確認。
COM3は閉じた。
