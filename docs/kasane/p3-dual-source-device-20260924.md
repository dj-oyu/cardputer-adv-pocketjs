# P3: 1つのnative textを2つの描画先で使う実機ゲート

2026-09-24、Cardputer ESP32-S3 / COM3。診断app image SHA-256
`46687590ece9b87c4f65c3eb467403820a0d88848ffd0505b7c25c9ea0232df7`。
`KASANE_P0_PROBE=ON, KASANE_P0_COPY_PROBE=ON`で、SDの
`music/KAKATO/KARA OK 2nd Edition/02 インザハウス.mp3`を45秒再生。
JSは音声時刻slotを書かず、固定poolの同じnative `elapsed` fieldを
左右2つのtext nodeが参照する。生ログ・LCD capture・summaryは
`.cache/kasane-p3-dual-device-20260924/z1/`（Git対象外）。

- 有効snapshot公開46件。producer pool内の元ポインタからcoreへの直接text
  copyは92回/736 B、長さ不一致0。つまりこの試行では**各描画先1回、
  2描画先の合計2回**だった。core text提出全体は初期値・無効化時の
  base復帰を含め96回/768 B。
- 再生中と終了後のLCD capture差分は左122画素、右122画素。
  対応する左右の文字領域は両captureで画素完全一致（不一致0）、
  source領域外の変化0。
- pool skip 0、音声underrun 0、decoder fault 0、IO ERROR 0。
  最終再生位置45,045 ms。
- LCD captureの長いシリアル転送がapp送信時間の統計へ混入したため、
  この試行の`app_send` p99は通常描画の性能値として比較しない。

診断OFFの製品app imageは1,995,328 B、静的DIRAMは159,820 Bで
前版と同値。試験前に元app領域の3×1 MiB digest一致を確認し、
試験後に全領域を復元して再び3×1 MiB digest一致を確認した。
COM3は閉じた。

これは**公開済みpayload→各core destination受理**の1-copy実機証拠。
1つの値全体で合計1回のcopyではない。bank cloneと描画scratchを含む
全経路1-copyも未達。別曲、低heap、seek可能形式のP3実機ゲートは残る。
