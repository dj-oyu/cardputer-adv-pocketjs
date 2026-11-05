# `-O2` は何を速くしたのか — 機械語と性能カウンタで見る（2026-11-05）

[性能と描画の文書](README.md)

> 60 fps への道のり全体の要約は [60fps-roadmap.md](60fps-roadmap.md) にある。

[lcd-dma-driver.md](../platform/lcd-dma-driver.md) §9.12 で、描画と転送の毎フレームの道（`ksn_font.c`・`ksn_render.c`・`ksn_core.c`・`pocket_kasane.c`・`pocket_shiki_frame.c`）を `-O2` にし、`render_group` だけ `-Os` に戻した。この文書は「なぜ速くなったか」を、逆アセンブルと XTPERF の実測で分けたもの。数字の区分は 3 つ: **実測**（実機の XTPERF）、**コード**（逆アセンブルで数えた事実）、**推定**。

- 逆アセンブルは host の Xtensa GCC 15.2（`-mlongcalls -ffunction-sections -fdata-sections -fstrict-volatile-bitfields`、契約テストのスタブのヘッダ）で単体コンパイルしたもの。**ファームの ELF ではない**（形は同じはずだが、行数・番地は目安）。
- 実測は、出荷版と、5 ファイルだけ `-Os` に戻した版の 2 ビルド（`59319d2` / `896e9fc`、計測後に revert 済み）。`render_rects_body` を囲み、その中の `display->present` を引いた区間で、PM0 = cycles、PM1 = 1 窓（30 フレーム）に 1 イベントで巡回（[ray-stall-census.md](ray-stall-census.md) と同じ方法）。記録は [logs/o2-xtperf-20261105.txt](logs/o2-xtperf-20261105.txt)。

## 1. 結論（実測）

描画区間の 30 フレームあたり（STRESS / DERBY パドック）:

| | STRESS `-Os` | STRESS `-O2` | DERBY `-Os` | DERBY `-O2` |
| --- | ---: | ---: | ---: | ---: |
| サイクル | 55.45 M | **47.48 M（−14.4%）** | 40.80 M | **33.10 M（−18.9%）** |
| 退役した命令 | 36.18 M | 31.04 M（−5.14 M） | 26.93 M | 21.94 M（−4.99 M） |
| 分岐・ジャンプ・call・ret（成立したもの） | 5.27 M | 4.26 M（−1.01 M） | 3.35 M | 2.29 M（−1.06 M） |
| バブル（全部） | 12.70 M | 10.24 M（−2.46 M） | 8.11 M | 5.64 M（−2.47 M） |
| うちレジスタ依存 | 2.19 M | 1.77 M | 1.40 M | 1.08 M |
| 命令フェッチ待ち（`I_STALL`） | 4.94 M | 4.68 M | 4.70 M | 4.67 M |
| うち flash キャッシュ待ち（`I_STALL_BUSY`） | 4.62 M | 4.45 M | 4.57 M | 4.44 M |
| うち `L32R` | 0.47 M | 0.33 M | 0.42 M | 0.32 M |
| データ待ち（`D_STALL`） | 0.20 M | 0.23 M | 0.19 M | 0.17 M |
| IPC | 0.653 | **0.654** | 0.660 | **0.663** |

（命令＋I_STALL＋バブル＋D_STALL はサイクルの 97〜98% で閉じる。）

**減ったサイクルの約 3 分の 2 は「命令の数そのもの」、約 3 分の 1 は「成立した分岐の数 × 2 サイクル」。** IPC は変わっていない — `-O2` のコードが 1 命令あたり速く流れるようになったのではなく、走る命令と飛ぶ回数が減った。命令キャッシュ（flash）の待ちは、コードが 12.6 KB 増えても絶対量で増えていない。

## 2. この石の性質（実測から読めること）

**成立した分岐 1 回 = バブル 2 サイクル。** 「バブル − レジスタ依存」を「成立した制御移行（分岐・`j`・`jx`・`call`・`callx`・`ret`）」で割ると、2 アプリ × 2 ビルドの 4 条件で **1.99 / 1.99 / 2.00 / 1.99**。レジスタ依存以外のバブルは、ほぼ全部が分岐の後の詰め直しと読める（推定、ただし 4 条件で一致）。LX7 に分岐予測は無く、飛ぶと 3 サイクル（発行 1 ＋ バブル 2）、飛ばなければ 1 サイクル。**「分岐を減らす」は、このコアではそのまま 2 サイクル × 回数の得になる。** ループを `loop` 命令（ゼロオーバーヘッド）に載せる価値もここにある。

**IPC の天井は 0.65 付近。** 描画区間のサイクルの内訳は、命令 65%、バブル 17〜23%、命令フェッチ待ち 9〜14%、データ待ち 0.5%。データ側（DRAM の読み書き）はほぼ止まらない — **このコードで高いのはメモリではなく、命令の数と分岐**。

**flash キャッシュの待ちは 1 割、コード増では増えなかった。** 32 KB の命令キャッシュ（[ray-stall-census.md](ray-stall-census.md) §2.5: この石では `I_STALL_BUSY` が flash キャッシュのミス待ち）で、描画の道の動く部分は `-O2` でも収まっている（推定）。ただし割合は 8.3% → 9.4%（STRESS）、11.2% → 13.4%（DERBY）に上がった — 次にコードを足すと、ここが先に効く。

## 3. 何が命令を減らしたか（コード）

### 3.1 小さな関数の呼び出しが消えた（インライン化）

`-Os` は大きさを優先し、1 画素ごとに呼ぶ小さな関数を外に置く。`render_rects` の中の呼び出し先（`-Os`）: `sample` 8、`frame_command` 7、`covers` 4、`coverage_runs` 4、`blend` 4、`pack565` 3、`group_pixel` 3、`footprint` 3、`blend_lut_pack` 3、`rgb565` 2、`row_table_for` 2 など、`callx8` が 96 か所。出荷版（C）では 60 か所（B は 86）で、 `pack565`・`rgb565`・`group_pixel`・`blend_lut_pack`・`row_table_for` などが展開されている。`ksn_font.c` の `span` では `utf8_decode`（1 文字ごと）、`ksn_core.c` では `bank_command`・`check_transaction` など 19 関数が展開された。

Xtensa の窓レジスタ ABI では、1 回の呼び出しは `l32r`＋`callx8`（`-mlongcalls`）・`entry`・`retw` に、引数と戻り値の `mov` が付き、**成立した制御移行が 2 回**（call と ret）入る。§2 の値段では、空の関数でも 6 命令前後＋バブル 4 サイクル（推定: 命令は逆アセンブルの数、バブルは §2 の 2 サイクル × 2 回。`entry` の窓あふれが起きればさらに重い）。例（1 画素の 565 合成、`blend_lut_pack`）:

```
-Os: mov a10,a9 / l32r a8,<lit> / callx8 a8 → entry / srli / add / l8ui / extui / add / l8ui / extui
     / add / slli / or / l8ui / slli / or / extui / retw.n → j <次>
-O2: extui / srli / extui / … / l8ui ×3 / slli ×2 / or ×2 / s16i   （同じ表引きが直列に並ぶだけ）
```

### 3.2 1 KB を超えるスタックフレームの「遠い」読み書き

`l32i`/`s32i` のオフセットは 8 bit × 4 で **0〜1020 バイト**（`l32i a8,a1,1020` は 3 バイト 1 命令、`1024` はアセンブラが複数命令に展開する — esp32s3-hw の `check_asm` で確認）。フレームが 1 KB を超えると、奥の変数は 1 回の読み書きが `movi`＋`add`＋`l32i` の 3 命令になる。

| `render_rects` | フレーム | 1,020 B より奥への読み書き（`tools/frame_lint.py`） | 余分な命令 | `a1` 相対の普通の読み書き |
| --- | ---: | ---: | ---: | ---: |
| `-Os` | 1,232 B | 460 | 862 | 319 |
| `-O2`（B: `render_group` も展開） | 1,312 B | **856** | **1,685** | 89 |
| 出荷（C: `render_group` は `-Os` で外） | **896 B** | **0** | 0 | 616 |

（最初の版はここを `movi 0x4xx` 以上の行の数で 434 / 835 / 2 と書いたが、それは `addmi` の形を数えず、フレームと無関係な定数の `movi` を数えていた。上の値は読み書きそのものを数える `tools/frame_lint.py` のもの。）

`-Os` の 1 画素の道には、同じ番地へ `s32i` した直後に `l32i` し直す列（`movi a9,0x448 / add a9,a9,a1 / s32i a8,a9,0 / l32i a8,a9,0`）まであった。

**これが §9.12 の B の退行の正体と読める（推定、B には計測器を付けていない）。** `-O2` は `render_group`（タイルの 256 B の共用体、到達表 144 B などを持つ）を `render_rects` に展開し、フレームが 1,312 B に膨らんで、退避した値の読み書きが「遠い」3 命令の形に倍増した。MEGADEMO の CORE の場面（群の不透明度）でタイルが 27% 遅くなったのはこの区間。C は `render_group` を外に出して `render_rects` のフレームを 896 B に収め（`render_group` 自身は 640 B）、遠いアクセスが 0 になった。

### 3.3 ループの中から不変量と分岐が出ていった

`ksn_font.c` の 1 列（グリフの 1 画素）の内側のループ:

| | `-Os` | `-O2` |
| --- | ---: | ---: |
| 1 列あたりの命令（インクのある列） | 約 22 | 約 13 |
| うちスタックからの読み込み（`l32i …, a1, …`） | 3 | 0 |
| 成立する分岐・ジャンプ | 4 | 3 |
| グリフの種類（bitmap / ASCII / 豆腐）の判定 | 毎列 | ループの外（3 本のループに分割） |

`-Os` は 3 種類を 1 本のループに入れて毎列 `beqz mapped` で振り分け、グリフの高さ・ビットの先頭・出力の先頭をスタックから毎列読み直す。`-O2` は種類ごとにループを分け（unswitching）、それらをレジスタに置いたまま回す。可変シフト（`ssr`＋`srl` の 2 命令）は両方に残る — Xtensa のシフト量は SAR レジスタ経由で、これはコンパイラの選択ではなく ISA の形。

## 4. 読み方と次の手

規則としての要約は [pie-simd.md](pie-simd.md) §3.9（スカラー整数コードのサイクルの式）にある。遠いアクセスはビルドのたびに `tools/frame_lint.py` が数える（§5）。

- **このコアで描画の道を速くするのは、命令数と成立分岐の数を減らすこと。** メモリ待ちはほぼ無く、IPC は最適化で変わらない。分岐 1 回 = 2 サイクルは、命令 2 個分と同じ重さとして数えてよい（実測）。
- **大きな関数のフレームは 1,020 B 未満に保つ。** 越えると全部の退避が 3 倍の命令になる。インライン化はフレームを膨らませる方向に働くので、大きな局所配列を持つ関数（`render_group`）は外に置く。
- **命令キャッシュは今は効いていないが、余白は小さい。** 次にコードを `-O2` にするときは `I_STALL_BUSY` の割合を見る（今 9〜13%）。
- 未測定: B（`render_group` 展開）の XTPERF、MEGADEMO の CORE の場面の内訳、BIG WAVE。窓例外（`entry` の窓あふれ）の回数は、PM が CINTLEVEL 0 しか数えないので見えていない（`ccount − PM0` は 0.4〜0.5% で、割り込みと例外を合わせても小さい）。

## 5. 機械的な検出: `tools/frame_lint.py`

objdump を読み、関数ごとに `entry a1, N` からフレームを取り、`a1`＋K（K＞1020）を `movi`＋`add` か `addmi` で作ってから、そのレジスタを土台にした読み書きを数える（1 回あたり余分に 2 命令か 1 命令）。分岐の着地点では作った番地を忘れるので、数は下限（作り話はしない）。`tools/test_frame_lint.py` が既製の objdump の文で検査する。

ビルドのたびに `main/` の object を報告する（`CMakeLists.txt` の `frame_lint`、`FRAME_LINT ...` の行）。`tools/frame_lint_baseline.txt` にない関数に遠いアクセスが出ると `NEW`、増えると `GREW` が付く。objdump が見つからないときは飛ばす（ビルドは止めない）。

```bash
python tools/frame_lint.py build_api/esp-idf/main --far-only     # main/ の関数だけ
python tools/frame_lint.py build_api/cardputer_pocketjs.elf --top 20   # ELF 全体（ESP-IDF・QuickJS も）
```

出荷版のファーム（`e318f9f`、broker の `fl-build`）で `main/` に遠いアクセスが残る関数は 16 本、合計 726 か所（余分な命令 1,484、静的な箇所数）。それを `tools/frame_lint_baseline.txt` に書いた。

| 関数 | 遠い読み書き | フレーム | 走る頻度 |
| --- | ---: | ---: | --- |
| `ksn_shiki_jit_scalar` | 169 | 1,600 B | JIT のコンパイラ。plan ごとに 1 回（冷たい） |
| `run_impl`（`pocket_grid.c`） | 98 | 1,168 B | grid の run ごと（熱い可能性） |
| `js_run`（`pocket_shiki.c`） | 95 | 1,616 B | Shiki の draw の呼び出しごと（熱い可能性） |
| `js_list`・`js_open`・`js_write_text`・`js_copy`・`js_rename`・`js_file_read`（`pocket_fs.c`） | 12〜84 | 1.1〜1.5 KB | ファイル操作ごと（冷たい） |
| `flower_draw`（`flower.c`） | 53 | 1,632 B | FLOWER の行ごと（熱い） |
| `measure_impl`・`register_resize_source_impl`・`register_resize_impl`（`pocket_grid.c`） | 10〜37 | 1.3〜2.0 KB | 登録・計測ごと |
| `perform_accel_foc`（BMI270） | 15 | 1,600 B | 較正（冷たい） |
| `ksn_pixel_span_eval`・`file_picker_next_path` | 4・1 | 3,904・2,464 B | |

`render_rects` は 0（出荷版）。B の形（`render_group` を展開）の object を `--check` に通すと `render_rects: 856 far accesses (baseline 0)` で落ちる — **この道具があれば、B の退行はビルドの時点で見えていた。**

host のスタブのヘッダでコンパイルした object では数が少し変わる（`flower_draw` が 53 ではなく 63）。基準は必ず本物のビルドから取ること。QuickJS（`quickjs.c`、`-Os`、host でのコンパイル）は 0（`build_backtrace` はフレーム 2,720 B だが配列をポインタで触る）。**数は静的な箇所で、実行回数ではない** — 熱いかどうかは人が見る（候補: `flower_draw`、`js_run`、`run_impl`）。
