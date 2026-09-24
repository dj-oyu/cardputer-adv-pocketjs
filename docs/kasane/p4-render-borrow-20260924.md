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

captureあり試験のapp send p99にはシリアル転送時間が混ざり、
描画性能比較には用いない。captureなしの交互A/Bを下記で追加した。

診断OFF製品app imageは1,995,232 B（前版比-96 B）、静的DIRAMは
158,780 B（前版比-1,040 B）。元app領域は書き込み前後に
3×1 MiBのdigest一致を確認して復元した。COM3は閉じた。
ログとLCD captureは`.cache/kasane-p4-render-borrow-20260924/`（Git対象外）。

`core_clone_text`は2描画先試験で47回/752 Bのまま残り、
producer→coreのコピーと合わせれば全経路1-copyではない。
bank cloneの除去、JS所有の検討、広いworkloadでの性能判定はP4の残件である。

## captureなしの交互A/B

同日、変更前`4ef8019`を隔離checkoutから同じESP-IDF設定で再ビルドした
診断app（SHA-256 `c537f60ad3633063a3d6f614b182701ec9f2dc1baa6d729e30f89bde69ee2e8f`）
と上記借用版を、同じCardputer・SD曲・45秒再生・captureなしで
**借用→旧→旧→借用**の順に測定した。1回目の借用試行は先述のものを使用。
全4試行で音声underrun、decoder fault、IO ERROR、pool skipは0。
`app_render`の分位点は128 µs幅ヒストグラムの上限値。

| 順 | 描画p95/p99/max µs | 送信p99 µs | 描画text copy | LCD frames/bytes/bands |
| --- | --- | ---: | ---: | --- |
| 借用1 | 895/1151/1036 | 7423 | 0 | 49/179,776/128 |
| 旧1 | 1023/1023/1013 | 7423 | 48+48回/768 B | 48/178,752/126 |
| 旧2 | 1023/1151/1074 | 7551 | 48+48回/768 B | 48/178,752/126 |
| 借用2 | 895/1023/962 | 7423 | 0 | 49/179,776/128 |

借用版のfree/min heapは222,028/53,260 B、旧版は
220,988/52,220 Bで各2回とも一致。largest block 69,632 B、
stack free 23,132 Bも全件一致。free/minの1,040 B差は
診断版静的DIRAMの1,040 B減と一致する。
借用版は有効公開46件・描画49回、旧版は有効公開45～46件・描画48回。
そのためLCD転送量の1,024 B差を描画経路だけの効果とは扱わない。

描画p99は両版とも1,023～1,151 µsで、2回ずつのこの音声workloadでは
劣化を検出しなかった。一方、試行数と音源は限定的であり、
これをP5の全アプリ・長時間・低heap性能ゲートの代用にはしない。
生ログは`.cache/kasane-p4-render-abba-20260924/`と上記`v1/`。
比較後、元app領域3×1 MiBを再度復元・digest一致を確認しCOM3を閉じた。
