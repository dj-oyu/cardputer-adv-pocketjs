# P3: 音声 producer から core 受理までの実機 copy 境界

2026-09-24、Cardputer ESP32-S3、COM3。診断専用ビルド
`KASANE_P0_PROBE=ON, KASANE_P0_COPY_PROBE=ON`、app image SHA-256
`6570d501680586e3e16bd97c0fd9ed22cdc6ae2ed68a62f94e39adf005e3a1ae`。
同一バイナリで、SD の
`music/KAKATO/KARA OK 2nd Edition/02 インザハウス.mp3`を45秒ずつ再生した。
ログと機械可読summaryは
`.cache/kasane-p3-copy-20260924/{v1,u1}/`に保存（Git対象外）。

| 経路 | 有効publish | producer buffer→core text copy | bytes | 長さ不一致 | underrun / decode fault |
| --- | ---: | ---: | ---: | ---: | ---: |
| `v` outputSource購読あり | 46 | 46 | 368 | 0 | 0 / 0 |
| `u` 購読なし | 該当なし | 0 | 0 | 0 | 0 / 0 |

`v`の総publishは48（終了invalid等を含む）、pool skip 0。`core_submit_text`全体は
48回/384 Bで、そのうち46回/368 Bが3 slot固定pool内の`clock[8]`を
**コピー元ポインタとして**直接使用した。残り2回は静的初期値・失効時のbase復帰。
watch計数は`core_submit_text`の内数で、合計へ二重加算しない。
`u`では`core_submit_text`全体が初期値1回/8 B、watchは0。

hostの`test_source_copy.c`は、同じ元ポインタが2 destinationへ提出された時に
それぞれ1回のcore受理copyとなることを確認する。REPLACE、PATCH、
lease解放後の描画とrepair、DISCARDED後の再取得・再提出を
ASan/UBSanと`-O2 -fstrict-aliasing`で通した。実機の1 node / 46更新では
有効publish 46件と直接copy 46回が一致した。よって**この実測workloadの
producer公開済みtext→各core destination受理**は1回/destination。

続くhost統合ケースでは、2 nodeを非表示にしたままtext sourceを更新すると
core text copyは増えず、再表示時には最新値だけが2 destinationへ1回ずつ
直接コピーされ、lease解放後にも描画できた。実際の3-slot poolと汎用adapterを
接続したケースでは、古い2世代をpinしたまま次のpublishを`BUSY`で
boundedに拒否（skip 1）、古いsnapshotを保持し、pin解放後に公開した
最新値をpool pointerからcoreへ直接コピーした。これらもASan/UBSanと
`-O2 -fstrict-aliasing`でPASS。ただし後者のpool枯渇はhost試験であり、
Cardputer実機での音声task同時実行中の枯渇ではない。

これは音声task内の文字列生成、snapshot全体、core bank clone、render scratch、
QuickJS内部を含む「producer原データからLCD描画完了まで1回」の証明ではない。
実際、`v`では`core_clone_text`が47回/376 B、`core_render_text`が
49回/392 B、`render_decode_text`も49回/392 B発生した。
観測済みcopy合計は824回/16,763 Bだが、計数範囲はpartial。
hidden→visibleの実機は後続の
[非表示→再表示試験](p3-hidden-source-device-20260924.md)で確認した。
pool枯渇の実機は後続の
[強制pin試験](p3-pool-exhaust-device-20260924.md)で確認した。
長時間・pauseは後続の[全曲再生試験](p3-long-output-device-20260924.md)で確認した。
seek可能形式・複数destinationの実機ゲートは残る。
P3全体と全経路1-copyは未達と判定する。

診断OFFの製品ビルドはapp 1,995,328 B、静的DIRAM 159,820 Bで前版と同値。
診断ON版の時間分位点はprobe overheadを含むため速度ゲートに使わない。
実機の元3 MiB app領域を3×1 MiBバックアップから復元し、各区画のdigest一致を
再確認した。COM3は閉じた。
