# P1 音声sourceのU32 fieldとnative ABI v2

2026-09-24、Cardputer ADV。Kasaneの汎用slotに`u32`を追加し、
`pocket.audio.outputSource()`を4 fieldの完全snapshotへ拡張した。

| field | 型 | 値 |
| ---: | --- | --- |
| 0 | text | 実際に消費したframeから整形した`HH:MM:SS` |
| 1 | u32 | seek/resumeの開始位置を含む論理的なframe位置 |
| 2 | u32 | 現streamの累積無音補填block数 |
| 3 | u32 | 現stream ID |

全fieldは同じ3-slot固定poolの同一revisionに属し、streamがない間は一緒にinvalidとなる。
数値は`u16`への飽和・折返しをしない。JSは`view.bind(source,{elapsed:0,frames:1,
starved:2,streamId:3})`と宣言するだけで、毎frameの状態管理や数値変換をしない。
Kasane coreは音声fieldの意味を知らず、汎用型検証とsource leaseだけを担う。
`u32`は現状データslotで、図形座標等の`u16`演算子へ暗黙変換しない。

`ksn_schema_slot`の数値初期値・上限を32-bitへ広げたのでnative C descriptor ABIは
`KSN_SCHEMA_ABI_VERSION=2`、source field型集合は`KSN_SOURCE_ABI_VERSION=2`とした。
旧C ABIはhost契約試験で拒否する。既存JS `mount({version:1})`は変更せず、
一度だけ新C descriptorへコンパイルする。この区別は将来PIEを別バイナリとして
導入する際に旧構造体を誤読しないために必要である。

## 検証

- host契約：`u32`最大値、上限違反時の原子性、旧C schema/source ABI拒否を
  ASan/UBSanとO2で確認。QuickJS統合は既存JS descriptorと`u32`の
  4,000,000,000および範囲外拒否をASan/UBSan・O2 strict-aliasingで確認。
- 診断OFF製品build：app 1,995,328 B、静的DIRAM 159,820 B（拡張前と同じ）。
- 実機：診断image SHA-256
  `499E487CCFD10383AE99599674E3A75EE3BFF9CB391E182A2AB25EE11F4E63FE`。
  許可済みSDの`music/KAKATO/KARA OK 2nd Edition/02 インザハウス.mp3`を
  45秒再生した2試行で各48 publish/skip 0、最大公開frame 1,080,064、
  無音補填0、player underrun0、MP3 decode fault0。画面capture試験は
  再生中→停止後122画素だけがx<96,y<24で変化し、領域外0。
  captureなしのapp render p99は1,023 µs、12 ms超過0。
- 元の3 MiBアプリ領域は試験前後とも3区画すべて保存済みdigestと一致した。
  COM3は解放済み。生ログと画素は`.cache/kasane-output-u32-20260924/`。

## 残る判定

field更新は表示の必要に合わせた約1 Hzであり、元の設計案の約33 ms数値telemetry
ではない。短いstreamや秒境界間のstarvationは途中snapshotとして観測できない。
一方、30 Hzに増やすと数値だけの変更でもowner側のsource取得・検証が増えるため、
必要性と音声deadlineを同条件で測るまで既定周期には採用しない。
seek/pause/resume、pool全pin時の終了invalid再試行、同描画workloadの性能A/B、
music overlay固定P0 gateと長時間再生は未達。P1全体の完成判定は行わない。
