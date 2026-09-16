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
| `test_coverage_runs`（段 3 以降） | `predicate and runs agree on 157111512 pixels over 10668504 rows (rect/text/round rect/gradient/stroke, radius 0..255, clipped)` / `120 frames identical both ways, 16 of them full 64,800-byte repaints (rolling hash a73ff47f)` / 計数版 `calls per full REPLACE frame: predicate arm covers=17672 runs=0; runs arm covers=0 runs=250 (switch on)` / `calls over 120 mixed frames: predicate arm covers=1744560 runs=0; runs arm covers=0 runs=27134`。**この表の値は第 1 次統合の時点のもの**。タイル段のあとは述語の回数だけが 16,184 / 1,569,456 へ減る（画素の 2 行は不変。§5.4） |
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

## 5. 第 2 次統合（画像変換とタイル）の記録

画像変換の 5 分岐（`perf/kasane-imgopt` / `-anchor` / `-stretch` / `-pet` /
`-tile`）を `perf/kasane-opt` へ取り込んだ統合。前回（§1〜§4）と同じ手順で、
1 段ごとに suite を走らせ、分岐が持つ専用ハーネスもその木で回し、
**分岐の報告の数値が動かないことを確かめてから**次へ進んだ。

### 5.1 統合の順序と各段の結果

| 段 | 分岐とコミット | 元 → 統合後 | 衝突 | その段で走らせたもの |
| ---: | --- | --- | ---: | --- |
| 0 | 基点（`origin/vm/design-contracts` `8655363`） | — → `3394f8e` | 0 | suite（前段とバイト一致） |
| 1 | `perf/kasane-imgopt`: `b17262a` / `8e57dcb` | `4bd388e` / `d9a17d1` | 1 | suite + `test_image_rotate_arms`（2,104 場面 / 4,207 ハッシュ） |
| 2 | `perf/kasane-anchor`: `590f5aa` / `55b3c74` | `ca59e21` / `b2968af` | 0 | suite + 同ハーネス（5,917 → 11,833 場面、`-DKSN_ANCHOR_COUNT`） |
| 3 | `perf/kasane-stretch`: `d025d9d` | `b9e65e4` | 2 | suite + `test_image_stretch_arms`（131 場面 / 259 ハッシュ） |
| 4 | `perf/kasane-pet`: `b93dcf1` ＋ 結線 | `dc0bb19` / `8486bbb` | 0 | suite + `test_pet_row_cache_arms`（48,495 比較、hash `d6eb4edb`） |
| 5 | `perf/kasane-tile`: `e7282ed` / `ca2e3fd` / `a3e6426` | `a84b118` / `d0bf4a1` / `7391fab` | 5 + 2 + 1 | suite + `run_group_tile.sh`（到達判定・16 画素・滑らかな層の 8 アーム） |

段 0 の基点統合は衝突 0 だったが、前回と同じ手順で自動併合を手検査した:
`main/app_session.c` が両側の差分（先方の `scene_mem_release` と `scene_mem.h`、
こちらの計器）をちょうど持つこと、`main/CMakeLists.txt` が先方の
`SYSTEM_SOURCES` を足しつつ `KASANE_SOURCES` を変えていないこと（`ksn_blend_pie.c`
は未結線のまま）、`ksn_render.h` が 1 バイトも変わっていないこと（4 つの extern は
すべて `ksn_render.c` に定義がある）。

### 5.2 衝突と解決（すべて両側を残した）

| # | ファイル | 何が衝突したか | 解いた形 |
| --- | --- | --- | --- |
| 5.2.1 | `ksn_render.h`（`8e57dcb` / `d025d9d` / `e7282ed`） | 新しい切替の extern が、どれも「同じ位置（`g_ksn_row_coverage` の後ろ）」に挿入された | 先方の塊をこちらの塊の後ろへ順に並べた。落ちた切替は 1 つも無い（`g_ksn_prof` / `g_ksn_decode_once` / `g_ksn_row_coverage` / `g_ksn_image_rotate_step` / `_anchor` / `_reject` / `_stretch_step` / `g_ksn_pet_row_cache` / `g_ksn_tile_pixels` / `g_ksn_tile_reach` / `g_ksn_tile_smooth`） |
| 5.2.2 | `ksn_render.c`（`e7282ed`） 境界パス | こちらはクリップ済み箱から左右上下を取り、先方は同じ箱を到達表 `reach[]` へ入れる | 先方の表を採り、`command.draw` を `command->draw` に（2a の retype を戻さない）。こちらの「見えない・不透明でない子を落とす」は先方の `if(command->visible&&d->opacity)` に含まれる |
| 5.2.3 | `ksn_render.c`（`e7282ed`） 子ごとの窓 | こちらのブロック単位の棄却（箱がこのブロックと交わらない子を読まない）と、先方の `lo..hi`（子の窓） | 両方。棄却は無条件、窓は `if(g_ksn_tile_reach||g_ksn_tile_smooth)` の中（`ca2e3fd` で滑らかな層も同じ窓を要るため） |
| 5.2.4 | `ksn_render.c`（`e7282ed` / `ca2e3fd`） 合成ループ | こちらの `g_ksn_row_coverage` の 2 腕と、先方の `smooth_block` ＋ 先方の述語ループ | 先方の `smooth_block`（半径 0 の勾配だけ）を先に置いて `continue` し、その後ろに 2 腕を残した。滑らかな層の呼び出しも `KSN_PROF_BEGIN/END(blend)` の中 |
| 5.2.5 | `ksn_render.c` / `ksn_render.h` **`group_pixel` の名前の衝突** | どちらも同名・同目的の関数を別の署名で持っていた（こちらは `const uint8_t *coverage`、先方は `uint8_t coverage` の値）。git は別々の挿入として見るが、そのまま並べると二重定義 | **先方の実装を残し**（`KSN_TILE_MEASURE` の out-of-line 属性と、`kasane-tile.md` が objdump で数える名前はそちら）、こちらの 2 つの呼び出し側をその署名へ寄せた（`coverage` を `scratch->text[dest]` で渡す）。合成の中身は 1 命令も変わらない（同じ `sample`・`premultiply_over`・同じ provenance ビット）。切替 `g_ksn_row_coverage` の 2 腕は両方生きている |
| 5.2.6 | `docs/README.md`（`a3e6426`） | 索引の 1 行が同じ位置 | 両方の行を残した（pet の行、その下に tile の行） |

`ksn_render.c` の関数ブロックでは、git が共通文脈として残した `}` が
`tile_row_over` を閉じてしまうため、`frame_command` の閉じ括弧を統合側で補った
（`a84b118`）。

### 5.3 統合後のハーネス出力（分岐の報告と一致したもの）

- `image rotate arms PASS: 11833 configs, 23665 panel hashes, worst pixel step 0,
  fetches identical`（5 アーム）。`-DKSN_ANCHOR_COUNT` の腕は
  `span rejection: 2089935 interval tests, 1521819 whole spans skipped`、
  `anchor table: 990409 builds, largest row served 15 entries`（報告の数値そのまま）。
- `image stretch arms PASS: 131 configs, 259 panel hashes, worst pixel step 0,
  fetches identical`、`stretched panel: 2025 spans`、
  `animated stretch track: 120 frames both arms, alternating arm identical,
  43692 fetches`。
- `pet row cache arms PASS: 48495 panel comparisons, worst pixel step 0,
  hash d6eb4edb, cache decodes 0.019% of the fetches, 64 of 330420`
  （provider 直接 46,080 取得で復号 46,080 → 4,608、animated 330,420 → 64、
  取得回数は 4 アームすべて同一）。
- `group tile: PASS`。`arithmetic: 5439488 blocks swept; chord worst 8-bit
  deviation 1 ...; exact stepping deviation 0`、
  `parameter space: 1555200 pixels compared, 9942 moved (0.64%)`、
  `exact arms: 4050000 pixels identical (reach on/off, tile 64/16, smooth=1)`、
  `120 frames: 73388 of 3888000 pixels (1.89%), worst frame 902 of 32400 (2.78%)`、
  `tile64 reach-on ... blocks 100 skipped 23 | child loop 4770`。
- 動いた数は 1 つだけ: 計数腕の reach-off の行の「子ループ画素」が tile64 で
  17,040 → 6,312、tile16 で 17,040 → 5,608。5.2.3 で残したこちらのブロック棄却が
  `g_ksn_tile_reach=0` の腕でも効くため。画素の一致（4,050,000＋120 フレーム）と
  タイル画素・被覆・ブロック数・飛ばした数は動いていない。

### 5.4 計器の値が統合で動いた 1 箇所

§1 の表にある `test_coverage_runs` の計数腕は、タイル段（`e7282ed`）のあと
`predicate arm covers=17672` → **`16184`**、`calls over 120 mixed frames:
predicate arm covers=1744560` → **`1569456`** になる（runs 腕の `runs=250` /
`27134` は不変）。理由はタイル段が「子の箱の外の画素」で述語を呼ばなくしたためで、
**比較している画素は 1 つも動いていない**（`predicate and runs agree on
157111512 pixels` と `120 frames identical both ways, rolling hash a73ff47f` は
不変）。テストの非空虚性も `frame0_runs[1] < frame0_covers[0]`（250 < 16184）で
成り立っている。

### 5.5 統合木のオブジェクト（xtensa-esp32s3-elf-gcc 15.2.0、`-Os`、`size -A`）

`main/ui/kasane/ksn_render.c` と `main/pet/ksn_pet.c` を単体でコンパイルした値。

| 段 | `ksn_render.o` `.text` | `.rodata` | `.data` | `.bss` | `ksn_pet.o` `.text` | `.bss` |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `fcc7e5a`（統合前） | 7,119 | 586 | 8 | 6,644 | 258 | 0 |
| 段 0 のあと | 7,119 | 586 | 8 | 6,644 | 258 | 0 |
| 段 1 imgopt | 7,767 | **586** | 12 | 6,644 | 258 | 0 |
| 段 2 anchor | 8,795 | **586** | 12 | 6,952 | 258 | 0 |
| 段 3 stretch | 8,911 | **586** | 12 | 6,952 | 258 | 0 |
| 段 4 pet | 8,911 | **586** | 12 | 6,952 | 406 | 8,208 |
| 段 5a tile（到達判定） | 9,083 | **586** | 20 | 6,952 | 406 | 8,208 |
| 段 5b tile（滑らかな層） | 10,007 | **586** | 24 | 6,952 | 406 | 8,208 |

- **`.rodata` は統合のどの段でも 586 B のまま**（flash に静的表を 1 つも足して
  いない）。増えたのは `.text`（実行時構築のコード）、`.data`（切替の既定値）、
  `.bss`（アンカー表 308 B と PET の行キャッシュ 8,208 B、いずれも SRAM）で、
  これは各分岐が選んだ内訳のまま。
- `.bss` の 2 つの増分は分岐の主張と 1 バイトまで一致する（`g_anchor_row` =
  `0x134` = 308、`cache_rows` 0x2000 + キー 16 = 8,208）。
- `ksn_render_rects` のスタック（`-fstack-usage`）: 656 B（段 3 のあと）→
  800 B（段 5a、到達表 16 × 8 B）→ 896 B（段 5b）。
- 未定義シンボルは統合木でも `__divdi3` のみ（`__moddi3` は出さない）。
- `-DKSN_TILE_MEASURE` の計測ビルドでは `smooth_block` / `smooth_chord_block$isra$0`
  / `smooth_exact_block` / `group_pixel` / `tile_row_over` が out-of-line に出る
  （分岐の計測と同じ形）。

### 5.6 既定値

**統合で既定を 1 つも変えていない。** 各分岐が自分の木で決めた値をそのまま
採用した: `g_ksn_prof=0`、`g_ksn_decode_once=1`、`g_ksn_row_coverage=1`、
`g_ksn_image_rotate_step=1`、`g_ksn_image_rotate_anchor=1`、
`g_ksn_image_rotate_reject=1`、`g_ksn_image_stretch_step=1`、
`g_ksn_pet_row_cache=1`、`g_ksn_tile_pixels=64`、`g_ksn_tile_reach=1`、
`g_ksn_tile_smooth=1`（近似の 2 は分岐自身の判断で既定 OFF）。

### 5.7 まだ負っているもの（この段で閉じていない）

1. **sanitizer の腕**（§0）。この段も `/tmp` の写しで旗の文字列だけ外して走らせた。
2. **実機の時間**。`/dev/ttyACM0` は別セッション。5 分岐とも時間の主張はしていない。
3. **PET の行キャッシュ 8,208 B を払う判断**は `tools/memlog.py --port --check` の
   実機実測待ち（`kasane-pet-row-cache.md` §7）。
4. **タイルの `run_group_tile.sh` は run.sh に結線していない**（分岐の判断。
   `run_group_tile.sh` の冒頭コメントのとおり、sanitizer が回るセッションの仕事）。
5. **`perf/kasane-affine` との衝突は先送り**（次段）。あちらは
   `render_group` の per-pixel 合成を `group_pixel` として切り出すので、この段で
   `group_pixel` を 1 本に統合したことがその 1 箇所に重なる。合意した順序は
   「タイル（この段）→ affine」。


## 6. 第 3 次統合（旧基点の 5 分岐: 行テーブル・LUT・アフィン畳み・alpha256・可視しきい値）

`perf/kasane-opt` の基点を最新（`origin/vm/design-contracts` `d2d4d21`）へ進めたうえで、
旧基点（`87eb92a`、design-contracts の統合前）から切られていた 5 分岐
（`perf/kasane-rowtable` / `-lut` / `-affine` / `-alpha256` / `-visible`）を取り込んだ段。
手順は前 2 段と同じ: 1 本ずつ cherry-pick し、**両側を残して**衝突を解き、
その木で契約スイート（`/tmp` の写しから sanitizer の旗の文字列だけ外したもの、§0）と
**分岐が持つ専用ハーネス**を回し、分岐の報告値が動かないことを確かめてから次へ進んだ。

### 6.1 統合の順序と各段の結果

| 段 | 分岐とコミット | 元 → 統合後 | 衝突 | その段で走らせたもの |
| ---: | --- | --- | ---: | --- |
| 0 | 基点（`origin/vm/design-contracts` `d2d4d21`、4 コミット） | — → `5019bf8` | 0 | suite（exit 0・66 PASS / 0 FAIL） |
| 1 | `perf/kasane-rowtable`: `0896f47` | `f7e46dc` | 2 | suite + `test_row_table`（4 アーム・120 フレーム・起動回数腕） |
| 2 | `perf/kasane-lut`: `ce56f69` / `12ecdd5` ＋ 結線 | `930e0bc` / `983268b` / `1688e7b` | 4 | suite（exit 0・66 PASS）+ `test_blend_lut`（全パラメータ掃引・起動回数腕） |
| 3 | `perf/kasane-affine`: `1b643a2` / `eb22dfb` | `b6a8211` / `7795bb9` | 4 + 4 | suite（66 PASS）+ `run_group_affine.sh`（2 段）+ `run_group_tile.sh` |
| 4 | `perf/kasane-alpha256`: `58b717a` ＋ 結線 | `1af4a90` / `21bd6cf` | 1 | suite（66 PASS）+ `run_models.py scale256`（exit 0・mismatches=0） |
| 5 | `perf/kasane-visible`: `286573d` ＋ 結線 | `9ad51f2` / `607a897` | 3 | suite（exit 0・69 PASS / 0 FAIL、可視しきい値の 2 腕を含む） |

段 0 の基点統合は衝突 0 だったが、前段と同じ手順で自動併合を手検査した:
`main/CMakeLists.txt` が `SYSTEM_SOURCES` に `sys_notify.c` / `sys_timer.c` /
`pocket_clock.c` を足すだけで `KASANE_SOURCES` に触っていないこと（`ksn_blend_pie.c` は
未結線のまま）、`docs/kasane/*.md` は基点側の文書更新だけであること、そして**この統合が
触ったファイル（`main/ui/kasane/*`、`main/app_session.c`、`tools/kasane_contract/*`、
`docs/perf/*`）は基点側の 4 コミットから 1 つも触られていない**こと（差分の名前で確認）。

### 6.2 衝突と解決（すべて両側を残した）

| # | ファイル | 何が衝突したか | 解いた形 |
| --- | --- | --- | --- |
| 6.2.1 | `ksn_render.h`（`ce56f69` / `1b643a2` / `58b717a`） | 新しい切替の extern がどれも同じ位置（`g_ksn_row_coverage` の後ろ）に挿入された | 統合木側の塊（行テーブル、LUT の 2 つ）を先に、先方の塊（`g_ksn_group_affine`、`g_ksn_scale256`）をその後に並べた。落ちた切替は 1 つも無い |
| 6.2.2 | `ksn_render.c`（`ce56f69`）直接経路の合成ループ | 統合木側は LUT の腕（定色の行を引く）／行テーブルの腕／画素ごとの `sample` の 3 段、先方は LUT だけの 2 段 | **LUT を先、行テーブルを後ろ**にして 3 段を保った。LUT は定色コマンド（`one_color`）でしか成立せず、その行は行テーブルが持つ値と同じ色なので、どちらの腕でも画素は一致する |
| 6.2.3 | `ksn_render.c`（`ce56f69`）TEXT ループ | 統合木側は `scratch.text[i]`（2a の retype で共有スクラッチ）、先方はローカルの `coverage[64]` | 共有スクラッチのまま。ローカル配列は復活させない（512 B のスクラッチ予算） |
| 6.2.4 | `ksn_render.c` / `ksn_render.h` **`group_pixel` の名前の衝突**（`1b643a2`） | どちらも同名・同目的で署名が違う（統合木側は `command` と `coverage` の値、先方は `coverage` のポインタ） | §5.2.5 で決めたとおり**統合木側の定義 1 つ**を残した（`objdump` がその名前で計測する側）。先方の呼び出しは最初からこの定義を使っているので、この段では追加の寄せは不要だった |
| 6.2.5 | `ksn_render.c`（`1b643a2`）前提走査 | 統合木側は子ごとに到達表 `reach[]` を埋める（見えない子も箱 `{0,0,0,0}` で 1 エントリ）、先方は見えない子を `continue` で落として `opaque_chain` を集める | 到達表を残したまま、`opaque_chain` の判定を `if(command->visible&&d->opacity)` の内側に置いた。`continue` は採らない（表の添字が子の添字と一致している必要がある） |
| 6.2.6 | `ksn_render.c`（`1b643a2`）反復側 | 統合木側のタイルループ（ブロック幅・到達判定）と、先方の畳みの早期 return | 畳み（`g_ksn_group_affine==1` かつ `opacity==255` かつ `opaque_chain`）をタイルループの**前**に置いて早期 return。畳みが成立する場面ではタイルは使われないが、画素集合と算術はスイッチ OFF の腕と同じもので、ハーネスが両腕を全画素比較している |
| 6.2.7 | `ksn_render.c`（`eb22dfb`）スクラッチ | 先方は `union {tile; ksn_fold_acc}` を `scratch` の名で導入（関数引数の `ksn_span_scratch *scratch` と同名になる） | union を採り、名前を **`buf`** にした。タイル側の本文は配列名 `tile` で書かれているので `ksn_premultiplied_rgba8 *const tile=buf.tile;` を 1 行置いて本文は無改変。`coverage[64]` は復活させない |
| 6.2.8 | `ksn_render.c`（`eb22dfb`）合成ループ | 統合木側の行テーブル・ブロック幅・`tile_row_over` と、先方の 64 固定ループ・インラインの `group_over` | 統合木側を残した（同じ画素集合を別の算術で書いたもの。行テーブルと LUT は統合木側にしかない）。`memset` の対象だけ `buf.tile` に合わせた |
| 6.2.9 | `ksn_render.c`（`286573d`）`group_over` ほか 2 箇所 | 統合木側の 3 箇所（`g_ksn_scale256` の分岐、LUT／行テーブルを持つ合成ループ、TEXT ループ）と、先方の計数呼び出し | 計数ブロックを `scale256` の分岐の後ろへ。合成と TEXT は統合木側の構造を保ち、**連鎖の呼び出しだけを `KSN_BLEND`（計数ビルドでだけ計器を通るマクロ）へ通した** |

段 2 では `blend_lut_build_row` に 1 つ足した（6.3 の末尾）。段 5 では
`tools/kasane_contract/test_visible_skip.c` に 1 つ足した（6.4）。
どちらも分岐のファイルそのものへの適応で、コミットメッセージに理由を書いてある。

### 6.3 統合後のハーネス出力（分岐の報告と一致したもの）

- `row table PASS: every arm byte-identical over the whole panel`、
  `rolling hashes 1a4ab349`（4 アーム同一）、
  `sample calls in one full REPLACE frame: (1,0) reference=18930 (1,1) table=28
  (0,0) predicate=18930 (0,1) predicate=18930; row builds: 328/328/0/0`。
- `blend lut: solid arm, 214499328 per-channel comparisons over 1110016
  (alpha, value, threshold) parameters, 72417280 whole words ...: 0 moved, worst
  channel step 0`、`alpha arm 16 levels, 8355840 ...: 2047578 moved (24.505%),
  worst channel step 2`、`120 frames ... rolling hash 1c401d77`（solid は
  フレーム一致）、起動回数腕は `1,258,651 → 456,797 → 161,280`（旧連鎖）、
  `0 → 801,854 → 1,097,371`（表）、`0 → 2,522 → 6,362`（行生成）。
- `group affine: folded arm and isolated tile chain agree on 6966000 pixels ...;
  rolling hash 3db0d222`（第 1 段）、段 2 は `7b68ea32`・
  `step 2 moved 86650 of 3888000 pixels over the parameter space (worst 8-bit
  channel step r/g/b=9/5/9, worst 565 step 1; 53498 of 224736 pixels under a
  covering child with a<255 moved, 0 elsewhere)`、
  `folded rows: switch off=0, on=10575`。`run_group_tile.sh` も第 2 段の値で
  PASS（`waste 27.25%`、`blocks 214564 skipped 39773`、`smooth blocks 84118
  pixels 3427220`）。
- `run_models.py scale256` は exit 0・`mismatches=0 / all models agree`。
  `120 full REPLACE frames ...: 3888000 pixels, moved 140838 (3.622%), frames
  with a moved pixel 120, worst 565 step r/g/b=0/1/1`、`old path`: `OFF arm over
  256 opacities x 8 colours x 256 words x 16 phases (8388608 calls) equals the
  pre-change reference: 0 differences`、群の経路は最悪 2 段差（α は両アーム不変）。
- 可視しきい値（計数ビルド）: `CORPUS 5 scenes x 120 frames, 11221760 blend
  pixels (empty=0 already free): step=1.2% bound=6.0% both=6.7% exact=0.5%;
  moved(both)=74931 worst(both)=1 moved(exact)=0`、種別では `TEXT both 28.0%`、
  `RECT` / `ROUND_RECT` / `STROKE` は 0%、`GRADIENT 0.9%`（dither 2.1%）、
  `GROUP 3.5%`。**計器あり／なしの 5 シーン・ハッシュが一致**（`diff` で検査）＝
  「計器は画素を変えない」が主張ではなく差分になっている。

**段 2 で足した適応（LUT と粗スケールの整合）**: `blend_lut_build_row` は
`blend()` の式を書き写しているので、`g_ksn_scale256` にも従わせた（src と dst の
重みは相補なので、粗いアームでは `mix256` がそのまま使える）。これが無いと
「表は厳密・比較相手の連鎖は粗い」になり、solid 腕が粗いアーム自身の 1 段差を
「移動」として報告する。検査は run.sh の新しい腕（`-DKSN_SCALE256_ARM=1` で
`test_blend_lut` を再ビルド）で、**solid は両アームで 0 移動**（強制した粗いアームは
`214,499,328` 比較で移動 0・最悪段差 0、ハッシュ `2e2c06a8`）。

### 6.4 統合の帰結として直したもの（分岐のハーネス側）

可視しきい値の計数器は連鎖の中（`blend`、`group_over`）に居る。出荷既定の LUT
（`g_ksn_blend_lut=1`）は定色コマンドを連鎖に入れずに答えるため、**固体の種別
（RECT / ROUND_RECT / STROKE）が計数 0 画素**になり、このハーネスの非空虚性チェック
（種別ごとに 1 画素以上）で落ちる。このハーネスが測っているのは「連鎖の画素に対する
閾値の当たり率」なので、ハーネス側で LUT を切った（両ビルドで同じ腕にして、
「計器は画素を変えない」のシーン・ハッシュ比較も成立させる）。行テーブルは連鎖では
なく `sample()` を置き換えるので切っていない（計数の母集団は変わらない）。
この 1 行を入れると CORPUS の数値は分岐の報告値と一致する（6.3）。

### 6.5 計器の値が統合で動いた箇所

- `run_models.py scale256` が印字する 1 フレームの内訳: 分岐の報告は
  `blend 497 / read 560`、統合木では **`blend 476 / read 518`**。行テーブルと LUT が
  画素あたりの `sample()` 呼び出しを減らしたぶんである。**画素の主張は変わらない**
  （同じモデルが「既定アームは変更前の式と一致」を 8,388,608 呼び出しで検査している）。
- それ以外に動いた計器の値は無い（`test_coverage_runs` の `predicate arm covers=16184`、
  `test_row_table` の起動回数、`test_blend_lut` の 3 アーム、タイルの全項目が
  段 1〜2 と同じ値のまま）。

### 6.6 統合木のオブジェクト（xtensa-esp32s3-elf-gcc 15.2.0、`-Os`、`size -A`）

`main/ui/kasane/ksn_render.c` を単体でコンパイルした値。この段が触ったのは
`ksn_render.c` / `ksn_render.h` と `tools/kasane_contract/*`、`docs/*` だけなので、
他のオブジェクト（`ksn_pet.o` など）は段 5 の値のまま。

| 段 | `.text` | `.rodata` | `.data` | `.bss` |
| --- | ---: | ---: | ---: | ---: |
| `2ed9ed6`（第 2 次統合の先端） | 9,475 | 586 | 24 | 6,952 |
| 段 0（基点を `d2d4d21` へ） | 9,475 | 586 | 24 | 6,952 |
| 段 1 rowtable | 10,263 | **586** | 28 | 7,928 |
| 段 2 lut | 10,951 | **586** | 32 | 12,044 |
| 段 3 affine（2 段） | 12,655 | **586** | 36 | 12,044 |
| 段 4 alpha256 | 12,655 | **586** | 36 | 12,044 |
| 段 5 visible（計数は `-DKSN_COUNT_VISIBLE` の中だけ） | 13,039 | **586** | 36 | 12,048 |

- **`.rodata` は 586 B のまま**（flash に静的表を 1 つも足していない）。増えたのは
  `.text`（実行時構築のコードと切替）、`.data`（切替の既定値）、`.bss`（SRAM）だけ:
  `row_table` 976 B（`0x3d0`）、`blend_lut_solid` 2,048 B、`blend_lut_alpha` 2,048 B、
  鍵 2 × 8 B、`g_anchor_row` 308 B（第 2 次統合）、PET の行キャッシュ 8,208 B（同）。
- `ksn_render_rects` のスタック（`-fstack-usage`）: **880 B で段 0〜5 を通して不変**。
  アフィン段の union（タイル 256 B / 8.8 アキュムレータ 224 B）を採ったので、
  畳みを足してもスタックは増えていない（6.2.7）。
- 未定義シンボルは `__divdi3` のみ（段 2 の anchor が消した `__moddi3` は出ない）。

### 6.7 既定値（統合後の全一覧）

**統合で既定を 1 つも変えていない。** 各分岐が自分の木で決めた値をそのまま採用した:
`g_ksn_prof=0`、`g_ksn_decode_once=1`、`g_ksn_row_coverage=1`、`g_ksn_row_table=1`、
`g_ksn_blend_lut=1`、`g_ksn_blend_lut_alpha=0`、`g_ksn_group_affine=1`（近似の 2 は
分岐の判断で既定 OFF）、`g_ksn_scale256=0`（厳密経路）、
`g_ksn_image_rotate_step=1` / `_anchor=1` / `_reject=1`、`g_ksn_image_stretch_step=1`、
`g_ksn_pet_row_cache=1`、`g_ksn_tile_pixels=64`、`g_ksn_tile_reach=1`、
`g_ksn_tile_smooth=1`。可視しきい値スキップは**実装を持たない**（計数のみ、既定の
ビルドでは 1 命令も入らない）。

### 6.8 まだ負っているもの（この段で閉じていない）

1. **sanitizer の腕**（§0）。この段も `/tmp` の写しで旗の文字列だけ外して走らせた。
2. **実機の時間**。`/dev/ttyACM0` が無い（ノード自体が無い）ため、この段も時間の
   主張はしていない。実機の A/B（同一バイナリでスイッチを窓ごとに落とす）は
   挿し直しのあとの仕事。
3. **`ksn_blend_pie.c` のカーネルは厳密な `/255` を再現する**。粗いアーム
   （`g_ksn_scale256=1`）を既定にするなら、カーネル側も粗い版に揃える必要がある
   （`kasane-alpha256.md` の注記のまま。この統合では既定を動かしていないので未処理）。
4. **`run_group_tile.sh` は run.sh に結線していない**（前段からの持ち越し。
   sanitizer が回るセッションの仕事）。
