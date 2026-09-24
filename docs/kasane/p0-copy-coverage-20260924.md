# Kasane P0 copy計数の境界（2026-09-24）

`KASANE_P0_PROBE=ON`の`KSN_P0`ログは、owner turn内で明示的に転記・
生成した**論理バイト数と操作回数**を数える。実際のメモリバス転送量、
同時生存する重複メモリ量、値ごとのend-to-end copy回数とは異なる。
コンパイラが消せる構造体代入も論理転記として計数する。
空文字など0-byteの操作は回数に含めない。`observed_total`は
下記の観測済み分類の和だけで、必ず`coverage=partial`を付ける。

| ログの`group` | 計数する境界 |
| --- | --- |
| `js_utf8` | mount/schema `set`とmusic presenter入力で`JS_ToCStringLen`から得たUTF-8 payload長。QuickJS内部の変換過程・終端NUL・一時確保は未計数。 |
| `producer_snapshot` | AV UI snapshotの構造体代入とclock viewの文字生成。producerの内部状態更新や全sourceの網羅ではない。 |
| `adapter_schema` | schema候補配列・文字一時領域・所有文字・型付きslot確定、schema参照ID確定、music model/status/planの明示転記。 |
| `core_submit` | payloadのcommand局所領域への書込み、commandのbank登録、textのbank登録。これらが同じ値の複数回転記なら個別に数える。 |
| `bank_clone` | PATCH/REPLACE開始時のcommand/text/track複製とbank headerの構造体代入。headerのポインタは直後に戻すが、C上の代入は全体を数える。 |
| `render_scratch` | core payload/textの読出し、frame viewと文字のdecode cache転記。画素合成で新たに生成するtile/stripは既存payloadの複写ではない。 |

`C kind=...`は細分類、`G group=...`は上記の小計で、両方に`calls/bytes`を出す。
6小計の和と`C kind=observed_total`は一致する。同じ元の文字が複数nodeへ
提出されるとdestinationごとの転記を全部足す。従って小計だけから
「1-copy達成」や「X bytesのheapを削減できる」とは判断しない。
2026-09-24追加の`W source_text_core_calls`は、登録したproducer text bufferの
ポインタがcore提出のコピー元と一致した回数・bytesを示す。
`C kind=core_submit_text`の**内数**であり、合計へ再加算しない。
[音声producer実機計測](p3-source-copy-device-20260924.md)で使用した。

未網羅：QuickJS内部の文字列複製・GC、native producer内部の複写、
すべてのconstructor/小さなscalarの代入、他の描画/API経路、コンパイラが
実際に発行するload/store。P0の出口には診断ON/OFFの同条件実機時間比較と
通常アプリ・musicの再現可能な基準値が必要であり、今回の分類追加だけでは未達。
診断用カウンタはreleaseの`KASANE_P0_PROBE=OFF`でinline no-opになる。
時間ヒストグラムだけを残して計数を外すときは
`KASANE_P0_PROBE=ON -DKASANE_P0_COPY_PROBE=OFF`を指定する。
[実機ABBA比較](p0-copy-probe-abba-20260924.md)ではdraw p99の回帰は
検出されなかったが、send p99には計数ありで128 µsの差が出たため、
時間基準とcopy量は別版の結果として扱う。

ホストの`test_p0_histogram`は分類別bytes/calls、0-byte除外、reset、
全分類のgroup対応を確認した。Kasane契約テストのASan/UBSan・O2、
実QuickJS統合のASan/UBSan・`-O2 -fstrict-aliasing`はPASS。
ESP-IDF 6.0.1のCardputer ADV向けビルドは診断ON/OFFともPASS。
この時点の静的DIRAMはON 171,452 B、OFF 159,772 Bで、診断全体の差は
11,680 B。アプリimageはON 1,986,784 B、OFF 1,982,976 B。
これはビルドの静的比較であり、実行時p99・heap peakの差ではない。
