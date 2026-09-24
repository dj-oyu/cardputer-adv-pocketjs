# P5途中: 汎用playbackSourceの実機ゲート（2026-09-24）

`pocket.audio.playbackSource()`を追加した。UI ownerが現行playerの
state/positionMs/durationMs/underruns/playingを固定3-slot poolへ公開し、
任意のruntime Kasane mountが型付きslotとして購読できる。player未開放・
close後は全field invalidとなり、slotはJS base値へ戻る。確保は最初の
capability要求時だけで、1秒以内に変化しない値は再公開しない。音声taskは
このsourceにもKasaneにも入らない。

診断`h`は通常アプリのmountで`playing:bool`を`PLAY`文字の表示に束縛し、
02 インザハウス.mp3を再生→pause→resume→closeする。同じ診断バイナリで
全画面captureあり`h1`、captureなし`h2`をCardputer/COM3で実施した。
生ログ・summary・RGB565は
`.cache/kasane-p5-playback-source-device-20260924/h{1,2}/`（git管理外）。

| 指標 | h1（画素） | h2（時間） |
| --- | ---: | ---: |
| ready→playing→paused→resumed→closedの`PLAY`文字画素 | 0→54→0→54→0 | captureなし |
| 全公開 / 有効公開 / pool skip | 24 / 22 / 0 | 25 / 23 / 0 |
| 最終位置 | 19,936 ms | 19,968 ms |
| underrun / decode fault / IO ERROR | 0 / 0 / 0 | 0 / 0 / 0 |
| app_render回数 / p99 | captureのため比較対象外 | 6 / 895 µs |
| app_send p99 | captureのため比較対象外 | 7,679 µs |
| 停止後free / 最小heap | 未比較 | 222,024 / 50,636 B |

h2の有効公開23回に対しrenderは6回で、表示しない数値slotの秒更新では
描画を増やしていない。h1/h2が別アプリセッションなので、sourceのresetと
再登録も同じバイナリで通った。診断OFFの常駐DIRAM総量は前版と同じ
158,780 B（追加したstatic pointerは4 B、配置内に収まる）。

これは**AV事実を共通source契約で公開・購読できる**証拠であり、既存music
overlayの`view.bind('playback')`をまだ置換していない。music固有のstatus
文字、help、進行barと入力を移した後、同一曲・同条件A/Bと長時間/低heap
ゲートが必要。試験前後のCardputerアプリ領域3×1 MiBは元バックアップと
digest一致し、COM3は閉じた。
