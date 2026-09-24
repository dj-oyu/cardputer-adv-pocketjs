# P5途中：musicの汎用source購読と実機時間ゲート（2026-09-24）

music native presenterはAV状態を`pocket_av_ui_read()`から直接取得しなくなった。
音声moduleの`playbackSource`へ`ksn_source_subscribe/acquire`し、完全snapshotを
owner turnだけpinする。player IDを6番目のfieldとして加え、旧playerへの
bindingで新曲を表示しない。music固有のstatus文字、help、bar、system status
優先順位とJS入力はmusic moduleに残す。Kasane coreに音声固有条件は追加しない。
購読cursorは表示keyが同じと確認した時か、plan提出が受理された時だけ進める。
BUSY/失敗時はleaseを解放し、次turnで再取得する。

診断OFF製品ビルドはPASS、静的DIRAMは移行前と同じ158,780 B。
QuickJS統合はWindows MSYS GCCの`-O2 -fstrict-aliasing`で0失敗。
ASan/UBSanはこのWindows環境にruntimeがなく実行できなかった。

Cardputer/COM3で同じ診断image SHA-256
`7460ED4551FEAD6717B081793010DF59D5F14F729D620C06D169599D80777458`
を2回起動した。SDの01 KAKATORO.mp3から02 インザハウス.mp3へ自動移行し、
後者を45秒再生、15秒時点で2秒pause/resume。生ログは
`.cache/kasane-p5-music-source-device-20260924/final-{1,2}/`（git管理外）。

| 指標 | 試行1 / 2 | P0固定閾値 |
| --- | ---: | ---: |
| overlay work p99 | 3711 / 3711 µs | ≤3839 µs |
| overlay draw p99 / max | 9215/9342、9215/9418 µs | ≤9599 / 10000 µs |
| overlay send p99 / max | 5247/5299、5247/5304 µs | ≤5247 / 5500 µs |
| ui frame p99 / max | 12287/192882、12287/192846 µs | ≤12287 / 200000 µs |
| free / min / largest heap | 218828/43936/69632 B（両回） | ≥210000 / 40960 / 65536 B |
| stack free | 23692 B（両回） | ≥22500 B |
| underrun / decode fault / IO ERROR / pool skip | 0 / 0 / 0 / 0（両回） | すべて0 |

MP3のdurationはunknown=0で、今回の曲では既知duration時のbar更新間隔は
実機で検証していない。最初の試行は起動直後にEnterを送ってoverlay起動前の
helloを開いたため測定から除外した。HOME OVERLAYの保存値は元から2で、
変更していない。計測前後に元のアプリ領域3×1 MiBを照合し、試験後は
全区画を復元・再照合した。COM3は閉じた。

この2回は固定P0閾値の短期ゲートを通した証拠で、同一バイナリ旧新A/B、
画素一致、長時間/低heap/seek/repairは未実施。`view.bind('playback')`の
JS互換入口は残しているので、P5および全経路1-copyの完成とは判定しない。
