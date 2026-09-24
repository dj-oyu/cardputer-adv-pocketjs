# P1 音声observer OFF/ON、同一バイナリABBA

2026-09-24、Cardputer ADV、COM3。診断バイナリSHA-256
`0B5C3E05300F3EE3689EB2876AF8A515582428D049A6BA983DAD3C02A49BD83B`。
`KASANE_P0_PROBE=ON`、copy/bus probe OFF。SDの許可フォルダ`music`から
`KAKATO/KARA OK 2nd Edition/02 インザハウス.mp3`を各45秒再生した。
USB `u`は同じdescriptorと再生コードで`audio.outputSource()`を作らない対照、
`v`はsourceをbindして音声出力taskから1 Hz更新する。順序はOFF→ON→ON→OFF。
アプリ領域は試験前に保存済み3 MiBと全区画一致を確認し、試験後に復元・再照合した。

| 指標 | OFF 1 | ON 1 | ON 2 | OFF 2 |
| --- | ---: | ---: | ---: | ---: |
| playback位置ms | 45,008 | 44,997 | 45,002 | 45,013 |
| audio underrun / MP3 fault | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |
| source publish / skip | — | 47 / 0 | 48 / 0 | — |
| decoder mean / worst µs | 3,858 / 5,289 | 3,896 / 6,111 | 3,893 / 5,791 | 3,856 / 5,602 |
| SD read回数 / bytes | 885 / 1,812,480 | 同左 | 同左 | 同左 |
| SD max read µs / slow reads | 16,010 / 0 | 17,622 / 0 | 16,763 / 0 | 16,216 / 0 |
| MP3中のheap free / session min B | 99,612 / 54,724 | 97,776 / 52,908 | 97,776 / 52,908 | 99,496 / 52,908 |
| app render回数 / p99 µs | 2 / 767 | 48 / 1,023 | 48 / 1,023 | 2 / 767 |
| app render 12 ms超過 | 0 | 0 | 0 | 0 |

生ログ・集計は`.cache/kasane-output-source-abba-20260924/{u1,v1,v2,u2}/`。
SD読み出し量とdecode faultは全試行一致し、音声deadlineの失敗は観測しなかった。
ONのdecoder meanがOFFより約1%高いが、2試行ずつのため因果・安定性の証明には
足りない。ONの`app_render`は1 Hzの実際の表示更新を含み、OFFは初期描画2回だけ。
したがって両者のp99を同じ描画workloadの性能回帰値として扱わない。
ONの動的sourceとleaseによるMP3中のheap free差は約1.7–1.8 KiB。

このABBAは別task producerが音声・SDと共存できる短時間の証拠である。
固定P0のmusic overlayゲート、同じ描画workloadを使う性能A/B、長時間、
pause/resume・seek、pool枯渇時の終了invalid、数値telemetryはなお未達。
