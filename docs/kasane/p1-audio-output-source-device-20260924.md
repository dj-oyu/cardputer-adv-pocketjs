# P1 別task音声source：45秒の実機接続試験

2026-09-24、Cardputer ADV、COM3。これはP1全体または描画性能のA/B合格を宣言するものではない。
後続の[U32拡張実機試験](p1-audio-source-u32-device-20260924.md)で
数値field 1–3を追加した。本書の測定値は当初のtext-only版である。

## 接続した経路

`pocket.audio.outputSource()`がセッション単位でsource capabilityを発行する。任意の
runtime `mount`は`view.bind(capability,{elapsed:0})`でtext slotを購読できる。
音声出力taskのHAL observerが開始・1秒境界・終了で実消費frameを通知し、音声serviceが
3-slot固定poolの空きslotへ8 byte `HH:MM:SS`を直接生成して公開する。
Kasane coreには音声固有条件を足していない。購読中にJSは`set`や画面全体の再構築をしない。
終了snapshotはfieldをinvalidにし、mountのbase値へ戻す。seek/resumeで開始frameを
HALへ渡すため、別streamの時刻を0から誤表示しない。pool枯渇時は待たずにskipし、
終了invalidの失敗だけは音声taskのhand-back後にowner taskで再試行する。

この版は[数値telemetry設計](p1-cross-task-audio-source-design-20260924.md)の
全項目を実装したものではない。`U32` slot、frame・starvation・stream idの数値公開、
約33 ms周期は未実装。短時間observer OFF/ONの同一バイナリABBAは
[別記録](p1-audio-source-abba-20260924.md)で実施した。8文字の時刻だけを1 Hzで
公開するのは現在の表示用途の低コストな接続試験であり、数値APIの代替完成扱いにしない。

## 実機手順と結果

診断バイナリに`v`アプリを追加し、SDの許可フォルダ`music`から
`KAKATO/KARA OK 2nd Edition/02 インザハウス.mp3`を再生した。
先にアプリ領域3 MiBの保存済み3区画を`verify-flash`で照合してから診断版を書き込み、
試験後は同じ3区画を復元し、再び全区画digest一致を確認した。COM3は閉じた。

| 試験 | 結果 |
| --- | --- |
| 画面captureなし、45秒 | source publish 47、skip 0、停止時audio task離脱確認、位置44,997 ms、underrun 0、MP3 decode fault 0 |
| 描画時間 | app render p95 895 µs / p99 1,023 µs、send p99 7,423 µs、renderの12 ms超過0 |
| 音声・SD | MP3 1,727 packets、decoder worst 6,103 µs、SD 885 reads/1,812,480 B、slow read 0 |
| メモリ | 終了時free 221,280 B、session min 53,040 B、largest 69,632 B |
| LCD captureあり、45秒 | 再生中→停止後122画素変化、96×24 source領域外0、publish 47/skip 0、underrun 0 |
| 診断OFF製品build | app 1,994,784 B、静的DIRAM 159,820 B（接続前159,804 Bから+16 B） |

`sound`の4要素queueでは、requestあたり`start_frame`の1 wordが増えるため、
queue payloadの動的確保は理論上+16 B。上表の静的DIRAM差には含まれない。
source service自体も`outputSource()`呼び出し時だけ動的確保される。

capture試験中はUSBへ全画面画素を送るためapp send maxが961 msに達した。
この値は音声との通常性能比較から除く。captureなし試験のapp turn maxも181 msで、
SD pickerとmountを含む全セッション値であり、steady-state p99とは区別する。

ログと画素は `.cache/kasane-output-source-device-20260924/no-capture-sd2/` と
`.cache/kasane-output-source-device-20260924/capture-sd/` に保管した。
収集器は`tools/kasane_output_source_device.py`。診断JSは
`apps/kasane/output_source_probe.js`。Kasane契約一式のhost ASan/UBSan/O2と
診断OFF製品buildを再実行し、両方exit code 0でPASSした。host契約試験は
pool/adapter並行読者とsource copy/discard/repair、dirty-node参照比較を含む。
新しいaudio observerとserviceのseek・stop競合そのものをhost試験したわけではない。

## 未達の出口

- 同じ描画workloadでのobserver OFF/ON性能比較とmusic overlayの固定P0閾値判定。
- 1 Hz text以外の数値source契約（必要なら汎用`U32` slot）、seek・pause/resume・
  複数stream連続・pool全slot pin時の終了invalid再試行のhost/実機試験。
- 長時間再生、音声とhome/overlay切替、全copy内訳、P2 dirty-nodeの実機A/B、
  P4の全経路1-copyおよびP5 music統合。

したがってP1、P2、P3、P4、P5の完成判定はまだ行わない。
