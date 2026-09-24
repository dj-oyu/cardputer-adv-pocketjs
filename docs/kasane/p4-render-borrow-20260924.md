# P4: sealed bankのtext借用描画

2026-09-24、`ksn_core_read_borrowed`をrenderer専用に追加し、sealed ticketの
bank内textを提示試行中だけ読み取り専用で参照する。従来の`ksn_core_read`は
独立したcopyを返す契約のまま維持する。display callbackはcoreを変更・再入
できず、ticketが提示・破棄・resetされた後に借用ポインタを保持しない。
frame view cacheも同じbankポインタを使い、文字列をframe scratchへ再コピーしない。

hostの`tools/kasane_contract/run.sh`はASan/UBSanと`-O2 -fstrict-aliasing`で
全件PASS。seal/previous/失敗/破棄時のborrow境界、120 frameの描画比較、
5件のIO retryを含む。decode reuseのscript hashは`010143045af205e4`。

Cardputer / COM3ではSDの`02 インザハウス.mp3`で45秒再生を2回行った。
診断app image SHA-256は
`96e84316b31fef923e941da5badf9286f2ca318b4a670d234d30ec93acdb74f0`。

| 試験 | 有効公開 | producer→core text | render text copy | 音声 |
| --- | ---: | ---: | ---: | --- |
| 1描画先、画面captureなし | 46 | 46回/368 B | 0 | underrun/fault/IO ERROR 0 |
| 2描画先、画面captureあり | 46 | 92回/736 B | 0 | underrun/fault/IO ERROR 0 |

2描画先の左右文字は両captureで画素一致（差分画素各122、不一致0）、
source領域外の差分0。旧版の同じ2描画先試験では
`core_render_text` 102回/816 Bと`render_decode_text` 102回/816 Bがあり、
新版では両分類とも0。観測copy合計は1,420回/27,450 Bから
1,216回/25,818 Bへ減った。ただし計数範囲はpartialであり、
producer生成やQuickJS内部まで網羅した総コピー量ではない。

captureなしの1描画先試験のapp render p99は1,151 µs、12 ms超過0。
旧版の別試行には1,023 µsがあるが、別バイナリ・別時点なので
この差を性能劣化と断定も否定もしない。captureあり試験のapp send
p99にはシリアル転送時間が混ざり、描画性能比較には用いない。
今後、同一条件A/BをP4性能ゲートで実施する。

診断OFF製品app imageは1,995,232 B（前版比-96 B）、静的DIRAMは
158,780 B（前版比-1,040 B）。元app領域は書き込み前後に
3×1 MiBのdigest一致を確認して復元した。COM3は閉じた。
ログとLCD captureは`.cache/kasane-p4-render-borrow-20260924/`（Git対象外）。

`core_clone_text`は2描画先試験で47回/752 Bのまま残り、
producer→coreのコピーと合わせれば全経路1-copyではない。
bank cloneの除去、JS所有の検討、性能A/BはP4の残件である。
