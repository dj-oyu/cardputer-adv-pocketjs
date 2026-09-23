# P1 wallSource実機ゲート（2026-09-24）

Cardputer ADV、ESP-IDF 6.0.1、COM3。診断ビルドは`KASANE_P0_PROBE=ON`、
`KASANE_P0_COPY_PROBE=OFF`、`KASANE_P0_BUS_PROBE=OFF`。音声試験に使った
アプリimageのSHA-256は`65FC81B9D2C48D2CEE8CE0A60243CB236EF472E46A8F780867C20AB567B92133`。
固定閾値は[P0時間ゲート](p0-timing-gates-20260924.md)の値を変更せず適用した。
生ログと画面は`.cache/kasane-wall-source-device-20260924/`に保存した。

まず`tools/overlay_device_test.py`でdeskclockとmusic overlayを起動し、
両方のRGB565画面を135行取得した。clockには時刻・`UTC`が表示され、
試験スクリプトはHOME OVERLAY設定を元の2へ復元した。

| ゲート | 1回目 | 2回目 | 固定上限・下限 |
| --- | ---: | ---: | ---: |
| hello app_render p99 | 1,151 µs | 1,151 µs | ≤1,407 µs |
| hello app_send p99 | 895 µs | 1,023 µs | ≤1,151 µs |
| hello app_turn p99 | 383 µs | 383 µs | ≤383 µs |
| hello描画 / LCD | 181回 / 210,720 B / 557帯 | 同左 | これ以下 |
| hello free / min / largest heap | 222,144 / 126,588 / 106,496 B | 222,144 / 117,412 / 106,496 B | ≥218,000 / 113,000 / 102,400 B |
| music overlay_draw p99 / max | 9,343 / 9,472 µs | 9,343 / 9,498 µs | ≤9,599 / 10,000 µs |
| music overlay_send p99 / max | 5,247 / 5,349 µs | 5,247 / 5,300 µs | ≤5,247 / 5,500 µs |
| music overlay_work p99 | 3,583 µs | 3,583 µs | ≤3,839 µs |
| music ui_frame p99 / max | 12,287 / 192,762 µs | 12,287 / 193,073 µs | ≤12,287 / 200,000 µs |
| music free / min / largest heap | 217,812 / 43,700 / 69,632 B | 同左 | ≥210,000 / 40,960 / 65,536 B |
| music underrun / decoder fault / IO ERROR | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 |

helloは各180更新、12ms超過0。1回目はoverlay画面確認後、2回目は再起動後。
musicは各回、許可済みSD `music/KAKATO/KARA OK 2nd Edition`で
01→02曲へ遷移し、02を45秒再生、15秒時点で2秒pause/resumeした。
`ui_frame`の12ms超過は14/2,149、11/2,119でいずれも1%未満。
描画と送信の12ms超過は0。以上の**短時間固定ゲートは両回合格**。

公開APIの実機確認には診断ビルド専用のUSB `7`と
`apps/kasane/wall_source_probe.js`を追加した。診断専用imageのSHA-256は
`7D48252B4E2B15507C578961A3E88888D13F2426572ED9834D32F97B83267B57`。
汎用runtime descriptorをmountし、初回提出の表示後に
`view.bind(pocket.time.wallSource(), {face:0, tag:1})`を1回だけ行う。
以後JSは表示slotを更新しない。`tools/kasane_wall_source_device.py`で
bind成功ログと前後の実LCD RGB565を取得し、65秒後に**19画素が変化**、
時計領域（x<96、y<24）の外の変化は0、APP_FAILED/panicは0だった。
診断専用コードを除いた製品ビルドもPASSし、静的DIRAMは159,804 B。

試験前の3MiBアプリ領域は1MiBずつ退避し、書込み前に3領域を
`verify-flash`でdigest照合した。全試験後に3領域を復元し、再度3件とも
digest一致を確認してCOM3を閉じた。

この結果は短時間の描画・音声共存、公開clock sourceの実機描画を証明する。
長時間音声、SD抜去、全アプリ、clock旧経路との同一時刻画素一致、
別task producerと固定snapshot poolの実機gateはまだ残る。
