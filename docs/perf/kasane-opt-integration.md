# Kasane 描画側の最適化 — 4 分岐の統合記録

対象は `perf/kasane-opt`。4 本の並行分岐（measure / decode / coverage / kernel）を
1 本ずつ取り込む。この文書は統合作業の記録で、**最適化そのものの主張は各分岐の
コミットメッセージと各分岐の文書にある**。ここに書くのは「何をどの順で入れたか」
「どこが衝突して何で解いたか」「各段でどのテストが実際に何を出したか」だけ。
実機には一切触れていない（`/dev/ttyACM0` は別セッションのもの）。IDF ヘッダが
無いのでファームウェアのビルドもしていない。

---

## 0. この容器の ASan は走らせられない（先に書いておく）

`tools/kasane_contract/run.sh` は 2 つのビルド（`-g -fsanitize=address,undefined` と
`-O2 -fstrict-aliasing`）で全ケースを走らせる。**この WSL2 容器では sanitizer 側の
腕が完了しない。**

実測（すべてホスト、`timeout` と DEADLYSIGNAL の行数を監視するラッパで計測）:

| 対象 | 結果 |
| --- | --- |
| 統合前の基点 `1e14ceb`（どの分岐もまだ入っていない、**pristine**） | `probe` → `core` → `runtime` の 3 本の ASan バイナリは通り、4 本目の `test_app_teardown` で `AddressSanitizer:DEADLYSIGNAL` の無限ループに入った。約 3 分で **7,236,193 行**、終了しないので打ち切った |
| `dc5dead` 取り込み後 `b3ebe64` | ループの**最初**の ASan バイナリ `probe`（`run.sh:9`）で同じループ。約 2 分で **848,772 行** |
| 同じ ASan バイナリを手で単発で回す（`test_app_teardown`、`-fsanitize=address,undefined`） | 12 回連続で 12 回とも正常終了（`PASS`）。`test_render_prof` も単発では ASan 版が 1 回通った |

つまり故障は**コードではなく環境**で、出方は「1 バイナリごとに低い確率で、出ると
無限ループ」。`run.sh` は ASan バイナリを 40 本近く直列に走らせるので、腕全体が
通る確率は実質ゼロである。**したがって、この容器での ASan の「腕が通った」という
主張は、どのコミットのものでも再現できない。** 単発で通った 1 本を「ASan 検証済み」と
書くこともできない（同じバイナリが次には無限ループに入る）。当面、この容器で
ASan に期待できるのは「たまたま通った 1 本の観測」までである。

対処として統合の各段では:

1. リポジトリの `run.sh` は**一切書き換えていない**（sanitizer の腕もそのまま）。
2. まず素の `run.sh` を監視付きで走らせ、DEADLYSIGNAL の嵐を検出したら打ち切る
   （歩留りを毎段で記録するため）。
3. 続けて `/tmp` に作った `run.sh` の複製（**コンパイル行の sanitizer フラグ文字列
   だけ**を外し、`cd` をリポジトリへ向けただけのもの）で全ケースを走らせる。
   レポートの「suite PASS」はこの `-O2 -fstrict-aliasing` 腕の結果である。

この節は統合の最初のコミットで先に置いた。以降の段の結果と衝突の記録は末尾に追記する。

---

## 1. 統合の順序と各段の結果

`perf/kasane-opt` の基点は `1e14ceb`。上から順に `git cherry-pick` した。

| 段 | 分岐 | 元 | 統合後 | その段で走らせたもの |
| ---: | --- | --- | --- | --- |
| 0 | — | — | `e7ee0d7` | ASan の主張の訂正（§0 を先に書いた） |
| 1 | `perf/kasane-measure` | `dc5dead` | `b3ebe64` | suite + `test_render_prof` |
| 2a | `perf/kasane-decode` | `da13f79` | `79fc53d` | suite + `test_render_prof` |
| 2b | `perf/kasane-decode` | `b4b9674` | `04dbf5e` | suite + `test_decode_reuse` |
| 3 | `perf/kasane-coverage` | `bbcb69c` | `7420e4d` | suite + `test_coverage_runs`（通常と `-DKSN_COUNT_CALLS`） |
| 4a | `perf/kasane-kernel` | `b769f01` | `c491042` | suite + `run_models.py` |
| 4b | `perf/kasane-kernel` | `8f68b96` | `b2eea49` | suite + `test_kernels.py`（`test_frost.py`/`test_piesim.py` も） |
| 5 | — | — | `33aef6f` | `read` 欄のコメントを 2a 統合後に合わせた（suite 再走） |

`run.sh` の suite は**各段のあと毎回通しで走らせた**。結果は全段で同じ:

- 基点 `1e14ceb`（sanitizer を外した腕のみ）: **exit 0**、PASS 行 46、34 秒。
- 段 1 `b3ebe64` 以降のすべて（1 / 2a / 2b / 3 / 4a / 4b / 5）: **exit 0**、
  PASS 行 48（段 1 で `test_render_prof` が 2 つのビルド腕で 1 行ずつ増えた分。
  decode/coverage の出力行は「PASS」の語を含まないのでこの数は動かない）。
- 素の `run.sh`（sanitizer の腕つき）は §0 のとおり段 1 で嵐になり、それ以降は
  試していない（同じ環境要因で、試すだけ時間が消える）。**sanitizer の腕が緑に
  なった段は 1 つもない。**

各分岐が自分で追加したテストの実出力（統合後の木、`-O2 -fstrict-aliasing`。
数値は分岐のコミットメッセージが主張した値と一致した）:

| テスト | 出たもの |
| --- | --- |
| `test_render_prof`（段 1、段 2a 以降も） | `RENDER_PROF pixels_off=0x88762a0f pixels_on=0x88762a0f identical=1 fill=35/0 span=31/0 tile=17/0 blend=225/0 read=284/0` → `render prof PASS: arms byte-identical, counts move only with the switch on`（`_cy` はホストに `rsr.ccount` が無いので 0 のまま） |
| `test_decode_reuse`（段 2b 以降） | `120 frames, 135 renders, 3 arms (on/off/alternating) byte-identical, 13 commands` / `read calls: switched on=1770 reference=32912 ratio=18.6x; worst render on=26 reference=375` / `script hash 010143045af205e4` |
| `test_coverage_runs`（段 3 以降） | `predicate and runs agree on 157111512 pixels over 10668504 rows (rect/text/round rect/gradient/stroke, radius 0..255, clipped)` / `120 frames identical both ways, 16 of them full 64,800-byte repaints (rolling hash a73ff47f)` / 計数版 `calls per full REPLACE frame: predicate arm covers=17672 runs=0; runs arm covers=0 runs=250 (switch on)` / `calls over 120 mixed frames: predicate arm covers=1744560 runs=0; runs arm covers=0 runs=27134` |
| `tools/pie/run_models.py`（段 4） | `all models agree`、`mismatches=0`。blend/pack の行: `kernel thin 1572864 blocks, 24 parameter sets x all 65,536 words: differing=0 worst step r/g/b=0/0/0`、`kernel dith 6291456 blocks ... differing=0 worst step r/g/b=0/0/0`、`kernel runs 331272 blocks ... differing=0` |
| `tools/pie/test_kernels.py`（段 4） | `Ran 10 tests ... OK`。`KasaneBlend8.test_thin` / `test_dither` / 2 アーム共通前置の比較が `ok`。`test_piesim.py` `OK`、`test_frost.py` `PASS` |

**mismatches 0・不一致 0 は統合後も保たれている**（網羅被覆テストとカーネルの
モデル比較の両方）。実機の時間は 1 つも測っていない（ホストでは測れない）。

## 2. 衝突と解決

`git cherry-pick` が衝突として止めたのは 3 ファイル 12 箇所。すべて**両側を残した**
（スイッチもテスト結線も落としていない）。1 つだけ、git が衝突として見せなかったが
型が噛み合わない箇所があり、そこも「両側の意味を残す」形で直した（§2.4）。

| # | ファイル | 何が衝突したか | 解いた形 |
| --- | --- | --- | --- |
| 2.1 | `main/ui/kasane/ksn_render.h`（`da13f79`） | measure と decode がどちらも `ksn_render_stats` の typedef 直後に挿入した（同じ位置への追加） | 両方の塊をそのまま並べた（measure の `ksn_render_prof`/帯ヘルパの宣言 → decode の `g_ksn_decode_once`）。宣言の順序だけの違いで、意味の衝突は無い |
| 2.2 | `main/ui/kasane/ksn_render.c` 9 箇所（`da13f79`） | 同じ行を両側が書き換えた。decode は `ksn_core_read(...&command)` を `frame_command(...)` に置換し、measure は同じ呼び出しを `KSN_PROF_BEGIN();`/`KSN_PROF_END(read);` の括りで包んだ | **両方適用**: 括りの中身を `frame_command(...)` にした。`command` は decode の `const ksn_frame_view *` になったので、フィールド参照は `command->draw` 側（decode）、括りは measure 側を残した。9 箇所は render_group の bounds パス・子ループ、`ksn_render_rects` の preflight・帯ループ・群の走査・直接経路の span/カバレッジループ |
| 2.3 | `main/ui/kasane/ksn_render.c` 2 箇所（`bbcb69c`） | 群の子ループと直接経路の per-pixel ループを、coverage が「行ごとの x 区間」に書き換え、measure が同じループをブレンド括りで包んでいた | **両方適用**: 括りは外側のまま、中身を coverage の 2 アーム（`g_ksn_row_coverage` on = run 経路 / off = 述語経路）にした。述語経路は decode の `covers(command,...)` を、run 経路は `coverage_runs(command,...)` を使う |
| 2.4 | `tools/kasane_contract/test_coverage_runs.c`（`bbcb69c`、**git は衝突と言わなかった**） | `covers`/`coverage_runs`/`group_pixel` の第 1 引数は decode が `ksn_frame_view *` に変えた。coverage 側のテストは `ksn_frame_command` のローカルを渡していたので、統合すると `-Werror` でコンパイルが通らない（git の 3-way では検出できない型の噛み合わせ） | テスト側のローカルと `same_row` の引数を `ksn_frame_view` にした（このテストは `ksn_render.c` を include しているので型は見える）。**比較している画素・行・期待値は 1 つも変えていない**。`ksn_render.c` 側の `coverage_runs`/`group_pixel` の宣言も同じ型に揃え、その旨をコメントに書いた |
| 2.5 | `tools/kasane_contract/run.sh`（3 分岐） | 3 本とも別の場所に 6〜11 行を足す形だったので、git は自動で併合し衝突を出さなかった | 併合結果に 4 つの結線が全部あることを確認した: `render-prof`（measure, `:55-56`）・`decode-reuse`（decode, `:75-76`）・`coverage-runs`（coverage, `:87-88`）・`coverage-count`（coverage の `-finstrument-functions` 腕, `:112-113`）。`test_coverage_runs.c` は 3 回ビルドされる（2 つの `$options` と計数腕）が、そのうち計数腕の呼び出しは併合で消えていない |

非機械的な衝突は無かった（両側とも独立した追加か、同じ式の周辺を違う目的で包んだもの）。
判断が要ったのは 2.2 の「括りの中身」と 2.4 の型で、どちらも**片方を落とす選択をしていない**。

`main/app_session.c` は measure だけが触るので衝突しなかった。統合後の内容は
`origin/perf/kasane-measure` と**バイト一致**（`git diff --quiet` で確認）。
`KASANE_PAINT` の既存フィールドの位置は動かしておらず、新しい欄は末尾に付く。
`tools/kasane_device_test.py:51` の正規表現
`KASANE_PAINT turn_ms=([\d.]+) render_ms=([\d.]+) send_ms=([\d.]+)` は行末を固定して
いないので、そのまま一致する（この正規表現を実際に走らせるには実機が要る）。

## 3. 統合後の検証

### 3.1 スイッチと既定値（コード上）

| スイッチ | 既定 | 場所 | 意味 |
| --- | --- | --- | --- |
| `g_ksn_prof` | **0** | `ksn_render.c:10` | 位相別カウンタを読むか（off のあいだ cycle を 1 本も読まない） |
| `g_ksn_decode_once` | **1** | `ksn_render.c:99` | 1 フレーム 1 回だけ展開する（0 で従来の帯ごと読み直し） |
| `g_ksn_row_coverage` | **1** | `ksn_render.c:149` | 行ごとの x 区間で被覆を解く（0 で画素ごとの述語） |
| （kernel） | — | — | **スイッチは無い**。未結線なので `ksn_render.c` からは呼ばれていない（§3.2） |

### 3.2 カーネルは未結線のまま

`main/ui/kasane/ksn_blend_pie.c` は **`main/CMakeLists.txt` に入っていない**
（`KASANE_SOURCES` の 9 つの kasane ソースに無い。`grep ksn_blend_pie` も 0 件）。
`ksn_render.c` からも参照していないので、ファームウェアのリンクには一切入らない。
`ksn_blend_pie.c`・`tools/pie/*`（`piesim.py`/`stalls.py`/`test_kernels.py`/`models/blend_pack_model.c`）・
`docs/perf/kasane-blend-pie.md` は `origin/perf/kasane-kernel` と**バイト一致**で、
取り込みで 1 バイトも変わっていない（`git diff --quiet origin/perf/kasane-kernel -- <file>`）。
**結線は次のコミットの仕事で、ここではまだ何も速くなっていない。**

### 3.3 decode 分岐の静的コスト（オブジェクト実測）

`da13f79` は「静的メモリ +5,824 B（.bss: view 4,608 + valid 12 + text 1,024 + read 176）
と +4 B（.data、スイッチ）、`ksn_render_rects` のスタックは 928 → 560 B」と書く。
target ツールチェーン（`xtensa-esp32s3-elf-gcc 15.2.0`、`-Os`）で各段の
`ksn_render.c` を単体コンパイルして測った:

| 段 | `.text` | `.data` | `.bss` | `.rodata` |
| --- | ---: | ---: | ---: | ---: |
| `1e14ceb`（基点） | 0xd20 (3,360) | 0 | 0 | 0x34 |
| `e7ee0d7`（measure のみ） | 0xebf (3,775) | 0 | 0x2c (44) | 0x34 |
| `04dbf5e`（+ decode） | 0xfbb (4,027) | **4** | **0x16ec (5,868)** | 0x34 |
| `7420e4d`（+ coverage） | 0x11d3 | 8 | 0x16ec | 0x48 |
| `b2eea49`（+ kernel） | 0x11d3 | 8 | 0x16ec | 0x48 |

- measure → decode の差は **`.bss` +5,824 B**（5,868 − 44）と **`.data` +4 B**。
  5,824 は decode の `decoded` そのもののサイズ（`nm -S` で `decoded` = `0x16c0` = 5,824 B）。
  内訳の主張（4,608+12+1,024+176 = 5,820）に 4 B の整列が乗った値で、**主張は一致する**。
  `.data` の 4 B は `g_ksn_decode_once`（`=1` なので `.data` に行く）。**主張は再現できた。**
- coverage は `.data` +4 B（`g_ksn_row_coverage`）だけで `.bss` は増えない。
- kernel の段では `ksn_render.o` は**1 バイトも変わらない**（未結線の裏付け）。

スタックフレーム（`-fstack-usage`、`ksn_render_rects`）: 基点 928 B → measure のみ 944 B →
decode 込み 576 B。decode 単独なら 944−368 = 576、基点からは 928−368 = 560 で、
**分岐の主張「928 → 560」は自分の木で正しい**。統合木では measure の括りが 16 B 足して
**576 B** になる（`kasane-progress.md:229` の 1 KiB 目標に対しては、統合木でも 576 B）。
`ksn_render_rects` の命令数（`objdump -d`）は 972（基点）→ 1,068（統合木）。
分岐の「972 → 930」も自分の木の値で、統合木の 1,068 は measure の括りの分だけ多い。

### 3.4 `tools/memlog.py` は device 無しで走らせられない

`--port` は**任意**（`--port PORT  read the device too; without it, static only`）だが、
`--map` が**必須**で、静的側は `python3 -m esp_idf_size` を呼ぶ。この容器には
リンクマップを作るファームウェアビルドが無く（IDF ヘッダも `idf.py` も無い）、
`esp_idf_size` モジュール自体も入っていない（`ModuleNotFoundError`）。
つまり静的側も含めて**この容器では走らせられない**。したがって decode のメモリの
判定は §3.3 のオブジェクト単位（`.bss`/`.data`）までで、**実機の空きヒープで見る
検査はまだ負っている**（`memlog.py --map build_api/cardputer_pocketjs.map --port ...` を
ビルドできる環境で）。

## 4. まだ負っているもの（この統合で閉じていない）

1. **sanitizer の腕**（§0）。この容器では通らない。CI か、ASan が動く環境で走らせること。
2. **実機の時間**。位相別カウンタ（`_cy`）はホストでは 0 のまま。`g_ksn_prof=1` の窓と
   off の窓を同一バイナリで比べる（`main/app_session.c` の `KASANE_AB`）。`render_ms` の
   内訳・帯分布は実機でしか出ない。
3. **`KASANE_PAINT` の新しい欄を実機のログで 1 度読む**こと（`tools/kasane_device_test.py`
   の既存の正規表現は通るはずだが、走らせてはいない）。
4. **カーネルの結線**。`ksn_blend_pie.c` は未結線のまま。結線するコミットが
   `ksn_render.c` のブレンド経路にスイッチ（`g_ksn_pie_blend` 相当）を足すことになる。
5. **coverage の `-finstrument-functions` 腕**は run.sh の最後で `-O2 -fno-inline` で
   1 回だけ走る（2 つの `$options` の外）。統合で消えていないことは確認したが、
   計数の値そのものは coverage 分岐の主張（17,672→250 など）と一致したことを記録する。
6. `main/app_session.c` は IDF ヘッダが要るのでこの容器ではコンパイルできない。
   確認できたのは measure 分岐との**バイト一致**まで（中身の変更は分岐の作者が
   ビルド無しで書いたもので、こちらも同じ）。
