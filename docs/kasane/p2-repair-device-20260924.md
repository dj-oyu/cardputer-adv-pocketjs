# P2 部分送信失敗→repairの実機試験

2026-09-24、Cardputer ADV / COM3。`KASANE_P2_REPAIR_PROBE=ON`だけで有効な
一度限りの診断経路をアプリのKasane表示ポートに追加した。USB `%`を受けた
owner turnで全画面をinvalidateし、正常に3 band（行0～23）をSPIへ送った後、
4 band目のy=24で一度だけ`KSN_IO`を返す。次turnのrepairは失敗を注入しない。
`board_capture`は二つの転送のSPI直前RGB565を記録する。

`tools/kasane_p2_repair_device.py`を同一診断imageで2回実行した。
image SHA-256は
`211A02CED47671786B158C802F88BA9CB75F0320BE6E32D1E2CA6EFFE503AAD8`。
両回とも`INJECT_FAIL y=24 after=3`とKasaneの
`LCD transfer failed; retaining display work for retry`を観測し、
次turnで`REPAIR_OK bands=17 bytes=64800`を観測した。失敗前のcaptureは
正確に24行、repair captureは正確に135行。helloの`HELLO_COUNT 1`から
追加のJS入力・slot更新なしでrepairした。

両回の正常時、repair時、repair後に再描画した240×135 RGB565は
すべて同一SHA-256
`0D7B3EA6BBAD780640A6F20E9B8744CDE54171A4AF0234B974A1A7253E02F8C1`、
不一致画素0。生ログ、3枚のraw、summaryは
`.cache/kasane-p2-repair-20260924/trial-{1,2}/`。

診断OFF製品ビルドもPASSし、app image 1,995,328 B、静的DIRAM
159,820 Bで注入追加前と同じ。診断ON buildは1,996,032 B、静的DIRAM
159,820 B。試験前の元アプリ領域3 MiBは保存済み3区画とdigest一致し、
試験後に復元して全3区画digest一致を再確認した。COM3は解放済み。

これは実SPIの故障や液晶GRAM読戻しではなく、Kasane表示ポートへの
一回の`KSN_IO`注入試験。全画面invalidateからの途中失敗・修復を証明するが、
狭いdirty PATCH中の部分転送失敗、24 slot/pageの実機試験、
厳密な同一配置A/Bを置き換えない。P2出口全体は引き続き未達。
