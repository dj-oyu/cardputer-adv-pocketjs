# Kasane P0 実機予備基準（2026-09-23）

Cardputer ADV / COM3。次期[source・dirty・copyロードマップ](source-dirty-copy-roadmap.md)の
P0を開始した記録。**P0完了ではない**。今回のファームはフレーム単位p95/p99と
copy分類カウンタを持たないため、短期平均と画面・heapの予備基準に限る。

## ファーム同定と保全

計測開始時の実機アプリ領域は、前回計測に使用した
`build_app_mount/cardputer_pocketjs.bin`（SHA-256
`be474aba13c07135bb1a85dc25efcd0193a22574519fad02726462175b8cb806`）と
`esptool verify-flash`で不一致だった。最初のhello測定が0.25 ms/turnで、
既知バイナリの以前の0.06 ms/turnと食い違った理由としてファーム差を確認した。
不一致の原因や実機にあったビルドの出所は未特定。

実機のfactory app領域 `0x10000..0x310000`（3 MiB）を上書き前に退避。
stubの連続readは`0x12a000`付近で2回中断したため、先頭1 MiBをstub、
残り2 MiBをROM bootloaderの`--no-stub`で取得した。各readは完了し、
ファイルサイズは正確に1,048,576 Bと2,097,152 B。SHA-256は順に
`4ea71a8a05a4032a090e75401553e0903708cc568fe0d943ad3f553937c8934c`、
`217306f265a1acedb01ce35652d8d85d8cdc0574a3b1dd23fbb49f369a494b21`。

既知バイナリをアプリ領域だけにflashして書込みハッシュ照合PASS。測定後、
退避した先頭・残りの両ファイルを元のoffsetへ書き戻し、esptool書込み照合PASS。
復元後のhelloも開始時と同じ0.25–0.26 ms/turnになった。NVSのHOME OVERLAY設定は
一時的に切り替えて元の2へ戻し、SDには書き込まず、COM3を閉じた。
退避ファイルは `.cache/kasane-p0-20260923/` に保存（git管理外）。

## 実測（既知バイナリ）

| workload | 今回の値 | 範囲・条件 |
| --- | ---: | --- |
| hello JS turn | 0.06 ms | 30描画平均×6窓 |
| hello render / LCD send | 0.90 / 0.70 ms | 同上。各窓768 B・3帯 |
| deskclock overlay 背景draw | 8.28–8.29 ms | 安定2窓、29.4–29.5 fps |
| deskclock 起動時free / largest / 追加確保 | 139,672 / 86,016 / 90,508 B | `OVERLAY_COST`と`app MEM` |
| deskclock 終了時worst turn | 1,927 us | 約182 turns |
| music overlay 背景draw | 8.88 ms | 安定2窓、30.1 fps |
| music 起動時free / largest / 追加確保 | 129,496 / 86,016 / 100,684 B | `OVERLAY_COST`と`app MEM` |
| music 終了時worst turn | 6,060 us | 約231 turns |

music help PNGは以前の既知バイナリ測定とSHA-256一致
（`f0a742386b23c38fe0b9832ae2dd82c85642394aded4fe7f82f00ee509aff2b6`）。
overlayは両方とも観測中のstop/panicなし。HOME OVERLAY設定は元の2に復元。
再生テストは新規フォルダ許可が必要だったため実行しなかった。

開始時に搭載されていた**別ファーム**のhelloは6窓で
turn/render/send平均 `0.25/0.93/0.69 ms`、復元後の確認2窓は
`0.26/0.98/0.78 ms`。別バイナリのためKasane変更のA/B効果には使用しない。

## 予備測定時に残ったもの（追加測定の到達点は後述）

- 1フレーム単位のturn/render/send分布とp95/p99/max、12 ms超過数。
  この時点の`KASANE_PAINT`は30フレーム平均であり分位点は復元できなかった。
- JS変換、producer snapshot、schema/core、bank clone、render scratch別の
  copy bytes/回数、最大構成のstack high-water・heap min/largest。
- 同一ファーム・同一曲での長時間再生、pause/resume、seek、underrun。
  フォルダ許可を得るまでは未測定のまま扱う。
- 既知UBSan警告の解消とsanitizer診断0件。各性能閾値は上記分布と
  測定揺らぎを得てから実装前に固定する。

生データ：`.cache/kasane-p0-20260923/hello-known-paint.log`、
`.cache/kasane-p0-20260923/overlay-known/serial.log`とPNG、
`.cache/kasane-p0-20260923/hello-paint.log`、
`.cache/kasane-p0-20260923/hello-restored-paint.log`。

## 診断ビルドによる追加測定

同日、`KASANE_P0_PROBE=ON`のアプリ専用ビルドをflashして測定した。
probeは静的DIRAMを約5.3 KiB使い、各サンプルに記録処理が入る。
そのため上の診断なしビルドと厳密なA/B性能比較には使わない。
分位点はマイクロ秒で、`sample=seen`は全件、`sample<seen`は256件の
セッション全体からのreservoir推定値。maxと12 ms超過数は全件集計。

| workload | 件数 | p50 / p95 / p99 / max (us) | 12 ms超過 |
| --- | ---: | ---: | ---: |
| hello JS turn | 1,619 / 256抽出 | 33 / 99 / 171 / 188 | 0 |
| hello render | 180 / 全件 | 813 / 1,031 / 1,142 / 4,597 | 0 |
| hello LCD send | 180 / 全件 | 643 / 755 / 833 / 7,386 | 0 |
| deskclock overlay work | 184 / 全件 | 1,643 / 1,750 / 1,787 / 1,929 | 0 |
| deskclock overlay draw | 184 / 全件 | 8,333 / 8,424 / 8,449 / 8,519 | 0 |
| music overlay work（無音） | 215 / 全件 | 4,846 / 4,934 / 4,981 / 6,079 | 0 |
| music overlay draw（無音） | 215 / 全件 | 8,942 / 9,046 / 9,089 / 9,246 | 0 |

helloの30描画平均×6窓はturn/render/send `0.05/0.88/0.69 ms`。
背景の安定窓はdeskclock `8.35 ms`、music `8.95 ms`。
music help画像のSHA-256は診断なしビルドと一致した。
overlayのstop/panicはなく、HOME OVERLAY設定は元の2に復元した。

診断カウンタで観測したhello 180描画のコピーは、bank command clone
550,400 B / 180回とbank text clone 183,424 B / 180回、合計
733,824 B（約4,077 B/描画）が突出。JS UTF-8 materializationは2,770 B、
adapter temp 4,210 B、adapter owned 2,950 B。これは計測を挿した箇所の
移動量であり、QuickJS内部・producer snapshot・描画scratchを含む全コピー量
ではない。特に`core_payload_read`と`core_render_text`はAPI境界・描画時の
反復読出しであり、そのまま永続的な重複保持量とは解釈しない。

計測後のfreeはhello 228,336 B、deskclock/music 224,836 B。
内部ヒープのboot以降minimumはhello/deskclock 123,616 B、music 120,000 B、
largestはhello 110,592 B、overlay 77,824 B。stack high-waterの
未使用量は23,708 B。ただしminimumはセッション専用ではなく起動以降の値。
この短時間・無音測定だけで長時間再生時の余裕は判断できない。

生データ：`.cache/kasane-p0-20260923/hello-diagnostic-final.log`、
`.cache/kasane-p0-20260923/overlay-diagnostic/serial.log`とPNG。
実機のfactory app領域は計測前に保全した3 MiBへ復元し、書込みハッシュ照合PASS。
SDは変更していない。再生は新規フォルダ許可が必要なため未実施。

その後、SDの対象フォルダ指定を受けて
[再生中の予備測定](p0-audio-baseline-20260923.md)を追加した。
描画12 ms超過とデコード最悪時間が無音時より大きく、音声underrun専用計数も
未取得なので、P0性能閾値の固定と合格判定にはまだ使わない。
