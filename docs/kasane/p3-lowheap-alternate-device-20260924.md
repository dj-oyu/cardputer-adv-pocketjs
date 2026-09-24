# P3: 別曲・低heapの実機ゲート（2026-09-24）

Cardputer ADV、COM3。診断`b`は許可済みSDフォルダ
`sd:/KAKATO/KARA OK 2nd Edition`を読取り専用で列挙し、従来測定の
01/02以外で最初のMP3を選ぶ。今回は`03 うーあ.mp3`だった。JSが
16,384 Bの`Uint8Array`を保持して各4 KiBをtouchしたまま再生する。
native audio taskの固定snapshot poolを汎用Kasane mountのtext slotへ
購読し、JSは再生時刻slotを書かない。画面シリアルcaptureは音声補充を
妨げるため行わない。事前ゲートは最小heap 20,000～45,000 B、
stack free 22,500 B以上、再生位置45,000 ms以上、underrun/fault/
IO ERROR/pool skip 0、render p99 1,407 µs以下・12 ms超過0、
send p99 9,000 µs以下・12 ms超過0とした。

最初の試行`b1`はMP3の`info.durationMs`が`null`なのを「短い曲」と
誤判定した診断側の不備で、再生前に停止。`b2`は45秒のwall clockで
停止して再生位置44,986 msとなり、固定位置ゲートを14 ms下回った。
閾値は変更せず、診断の保持時間を46秒へ延長した。同じ診断バイナリの
`b3`・`b4`はともにPASS。生ログ・集計は
`.cache/kasane-p3-lowheap-device-20260924/b{3,4}/`（git管理外）。

| 指標 | b3 | b4 |
| --- | ---: | ---: |
| 再生位置 | 46,000 ms | 46,042 ms |
| 有効source公開 / pool skip | 47 / 0 | 47 / 0 |
| producerポインタ→core text copy | 46回 / 368 B | 47回 / 376 B |
| render text copy | 0 | 0 |
| underrun / decoder fault / IO ERROR | 0 / 0 / 0 | 0 / 0 / 0 |
| 最小heap / 停止後free / largest | 32,480 / 222,028 / 65,536 B | 同左 |
| stack free | 23,692 B | 23,692 B |
| app_render p99 / 12 ms超過 | 1,151 µs / 0 | 1,023 µs / 0 |
| app_send p99 / 12 ms超過 | 7,423 µs / 0 | 7,423 µs / 0 |

公開済みの8-byte source textは、core受理時に描画先ごとに1回コピー
された。bank clone等を含む全経路1-copyの証明ではない。これらは
診断アプリでの46秒・1曲・16 KiB保持という境界であり、music overlay
全体の低heap性能や別曲との同条件A/Bを証明しない。

試験前に元アプリ領域3×1 MiBのdigest一致を確認し、試験後に元ファームを
復元して全3区画を再照合した。COM3は閉じた。診断OFF製品ビルドは
1,995,232 B、静的DIRAM 158,780 Bで前版から不変。
