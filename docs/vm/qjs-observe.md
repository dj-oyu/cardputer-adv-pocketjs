# QuickJS のターンを QEMU で丸ごと観察する（2026-11-06）

[VMの文書](README.md)

問い: QuickJS（quickjs-ng）のボトルネックとして挙がっている 4 つ — (1) 実機には分不相応な計算精度（64 bit 除算など）、(2) 命令キャッシュのあふれと flash からの取得の遅さ、(3) PIE などコアの特殊回路への誘導路が無いこと、(4) パイプラインが埋まらないこと — を、手を付ける前に QEMU で観察する。

これまでの QEMU の記録（[turn-icache.md](turn-icache.md)、[qjs-pgo.md](qjs-pgo.md)）は `-dfilter` で **flash の命令だけ**を取っていた。libgcc のソフトウェア double と 64 bit 整数の補助関数は **ROM**（0x4000_0000〜）に、窓例外・割り込み・アロケータは **IRAM** にあるので、それらは見えていなかった。今回は ROM・IRAM・flash の全命令を取った。記録は [logs/qjs-observe-20261106.txt](logs/qjs-observe-20261106.txt)。

**この文書の数字は、断りの無いかぎり QEMU の命令列（実行した命令の数え上げ）と、それに当てたパイプラインの模型の推定である。QEMU はサイクルもキャッシュも模型にしていない。** 実機の値は「実測」と書き、出典を付ける。

## 0. 結論

- **(1) 精度: 64 bit 整数の除算は STRESS のターンにほぼ無い。** `__udivdi3` は FreeRTOS の tick 割り込みの 1 ターン 4 回だけ、`__ashldi3` が `js_number` から 20 回（§3.1）。**重いのは 64 bit 整数ではなく double**で、ソフトウェア double（ROM の libgcc）と libm の double が 1 ターン 3.8 万命令、全命令の 7.3%。`__divdf3` は 1 回 **392 命令**、`Math.sin`/`cos` は 1 回あたり double の乗算・減算を 10 本前後呼ぶ。整数で済む場所（`parse_i16`、整数どうしの演算）も double を通っている。
- **(2) flash: 命令の 53% は flash、32% は IRAM、15% は ROM で走る（§2）。** IRAM の大半はアロケータと窓例外。flash の取得待ちは模型に無いので、実機の `I_STALL_BUSY`（実測）との差で読む（§2.2）。
- **(3) 特殊回路への誘導路: 一番多いのは「1 命令で済むことを関数呼び出しでしている」形。** QuickJS がバイトコードのオペランドを読む `get_u32`/`get_u16` はインライン展開されず、中で **ROM の `memcpy` を呼ぶ**（1 ターン 356 回、§3.3）。R3 のブロックキャッシュも、ポインタ 1 語の読み書きを `memcpy` の呼び出しで行う（236 回）。ESP-IDF の tlsf は小ブロックの区分を毎回**ハードウェア除算 2 本**で求める（2 の冪の割り算）。PIE（SIMD）で速くなるループは、今回もターンの中に見つからなかった（§3.3.4、[turn-jumps-pie.md](turn-jumps-pie.md) §4 と同じ結論）。
- **(4) パイプライン: 模型の上では、命令 52 万に対してサイクル 75.7 万（IPC 0.69、flash の待ち無し）。** 増えた分の内訳は、成立した制御移行（1 ターン 9.4 万回 × 2 バブル）が 25%、窓例外のハンドラが 6.9%（約 4,000 回、命令 4.3 万）、ロード直後の使用が 4.3%、ハードウェア除算が 2.1%（§3.4）。
- **最大の部品はアロケータ（模型のサイクルの 35.7%）で、QuickJS 本体（32.7%）より大きい**（§2）。r5sim の既定（ブロックキャッシュ無し）では 45.6% だったので、r5sim に実機の確保経路を入れた（`-DR5SIM_GUEST_ALLOC=1`、§1）。それでもキャッシュの命中は 1 ターンの確保 248 回のうち約 48%。模型のアロケータ 26.5 万サイクルは、R3 の実機の測定 0.92 ms（約 22 万サイクル、実測、[r3-small-block-cache.md](r3-small-block-cache.md)）と同じ桁。

## 1. 道具と条件

- **`tools/r5sim/observe.py`（新規）**: QEMU の `-d exec,nochain,in_asm` の記録を 1 回読み、`r5_turn_begin`/`r5_turn_end` で区切ったターンの命令を全部数える。ROM の関数名は ESP-IDF の ROM ELF（`esp-rom-elfs` の `esp32s3_rev0_rom.elf`）から取る。画像自身の ELF は ROM を `PROVIDE` の入口（`__call___adddf3` などの飛び先の表）でしか知らないので、それだけで引くと ROM の本体が手前の名前（最初の試行では `rom_rx_gain_force`）に付く。`__call___X` は X と同じ関数として数える。
  - 関数ごとの命令数（排他的）、成立した制御移行、ロード直後の使用、`l32r`、乗除算と FPU の命令、libgcc の補助関数の呼び出しと呼び出し元、窓例外の回数、関数内で後ろへ飛ぶ分岐（ループ）の回数と本体の大きさ、呼び出しの辺（caller → callee）。
  - パイプラインの模型: サイクル ≈ 命令 ＋ 2 × 成立した制御移行 ＋ ロード直後の使用 ＋ 16 × ハードウェア除算。係数は実機の値: 成立した移行のバブル 2（実測、XTPERF、[perf/pie-simd.md](../perf/pie-simd.md) §3.9）、ロード直後の使用 1 サイクル（実測、esp32s3-hw の `native_load_use`）、整数除算 約 16 サイクル（実機の節約からの逆算、[perf/pie-simd.md](../perf/pie-simd.md) §3.7）。flash の取得待ち・データキャッシュ・窓例外の出入りの値段は入っていない。
- **`-DR5SIM_GUEST_ALLOC=1`（r5sim の新しい選択肢、既定 OFF）**: ゲストの確保を `components/pocketjs_guest/src/guest.c` と同じにする — R3 のブロックキャッシュ（同じ 8 クラス 12/16/32/36/48/72/80/88 B）を heap_caps の上に置き、ブロック長を tlsf のヘッダから読む。既定の r5sim は `JS_NewRuntime` の `malloc`（tlsf ＋ heap の排他区間）を毎回通り、実機より確保が重い。既定を変えないのは、R5・PGO の記録と比べられるようにするため。
- 作業: STRESS LV1（`tools/r5sim/main/stress_r5.js`、Kasane の保持型シーンの頃の STRESS。今の Shiki 版ではない）、ターン 61〜120 の 60 ターン。どちらの像も `frames=120 oom=0 fail=0 bad_present=0`。
- QEMU: Espressif 版 `esp_develop_9.2.2_20250817`、`-icount shift=2`。記録は 1 回 10.4〜10.8 GB、解析は 1 回約 25 分。

## 2. 1 ターンの内訳（A: 実機の確保経路）

| 部品 | 命令/ターン | 命令の % | 成立した移行 | ロード直後 | `l32r` | 模型のサイクル | % |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| アロケータ（ブロックキャッシュ・heap_caps・multi_heap・tlsf・排他区間） | 178,214 | 34.3% | 34,748 | 17,257 | 7,701 | 264,967 | 35.7% |
| quickjs.c | 165,629 | 31.8% | 34,238 | 8,382 | 2,857 | 242,487 | 32.7% |
| Kasane・pocket ほか | 62,276 | 12.0% | 9,378 | 3,292 | 1,441 | 84,324 | 11.4% |
| 窓例外（`_WindowOverflow8`/`Underflow8`） | 42,816 | 8.2% | 4,282 | 0 | 0 | 51,380 | 6.9% |
| libgcc（ソフトウェア double・64 bit） | 34,478 | 6.6% | 5,302 | 1,328 | 1,898 | 46,411 | 6.3% |
| `memcpy`/`memcmp` など | 29,464 | 5.7% | 5,155 | 1,590 | 1,110 | 41,363 | 5.6% |
| libm（double） | 3,709 | 0.7% | 682 | 366 | 636 | 5,438 | 0.7% |
| RTOS・割り込み | 3,704 | 0.7% | 634 | 443 | 164 | 5,415 | 0.7% |
| 計 | 520,290 | | 94,418 | 32,658 | 15,808 | 741,784（除算込みで 757,430） | |

- 置き場所: flash 274,950、IRAM 167,809、ROM 77,531 命令/ターン。
- 呼び出し 21,171 回、戻り 27,446 回、`jx`（命令の振り分けなど）4,601 回/ターン。
- 部品の割り当ては関数名と `quickjs.c.obj` の記号表による（`tools/r5sim/observe_parts.py` の規則。境界で数百命令は動く）。

### 2.1 B: r5sim の既定（ブロックキャッシュ無し）との比較

| | A（実機の確保経路） | B（既定） |
| --- | ---: | ---: |
| 命令/ターン | 520,290 | 646,230 |
| うちアロケータ | 178,214 | 282,270 |
| 模型のサイクル/ターン | 757,430 | 960,183 |
| 窓例外（overflow の回数） | 2,010 | 2,742 |

quickjs.c の命令数は 2% しか違わない（168,684 と 165,629）。動いたのはアロケータ（B は A の 1.6 倍）、窓例外（B は 1.4 倍。確保の呼び出しが深いので）、`memcpy` など（A のほうが 26% 多い。ブロックキャッシュの `memcpy`、§3.3.1）、Kasane・pocket ほか（B が 12% 多い、理由は見ていない）。**r5sim の既定で測った過去の記録は、確保を約 1.6 倍に見ている**（R5・PGO の記録は flash の命令だけを数えていて、tlsf は IRAM にあるので、その結論には効かない）。

### 2.2 実機との照合

- **アロケータ**: 模型 26.5 万サイクル/ターン、R3 の実機の測定は 0.92 ms ≒ 22 万サイクル（実測、計時、[r3-small-block-cache.md](r3-small-block-cache.md) §6.1）。アロケータのコードはほぼ IRAM なので flash の待ちが乗らず、模型と実機が近くて当然の部品。
- **窓例外**: 模型 約 4,000 回/ターン（overflow 2,010、underflow もほぼ同数）、実機の `EXR_WINDOW` は 3,800 回/ターン（実測、Shiki 版の STRESS、[turn-jumps-pie.md](turn-jumps-pie.md) §2）。
- **命令数は合わない。** 実機の XTPERF は 37.4〜37.8 万命令/ターン（実測、[turn-cpi.md](turn-cpi.md) §3）、模型は 52.0 万。性能カウンタは `kernelcnt 0` で数えているので、割り込みレベルを上げた区間（heap の排他区間）と例外の中（窓例外のハンドラ）は数えていない可能性がある（推測、未確認）。その 2 つは模型で 1 ターン約 10 万命令ある。差を詰めるには、実機で排他区間を挟んだ計数か、PM の `tracelevel` を変えた計数が要る。
- **flash の待ち**: 実機の STRESS の通常のターンは QIO で 1.51 M サイクル、うち `I_STALL_BUSY` 0.88 M（実測、[turn-cpi.md](turn-cpi.md) §3）。模型の 0.76 M（待ち無し）と足すと 1.64 M で、同じ桁。

## 3. 4 つの問いへの観察

### 3.1 計算精度（64 bit・double）

libgcc の補助関数（1 ターンあたり。「命令/回」は補助関数の中で実行した命令の平均）:

| 関数 | 回/ターン | 命令/ターン | 命令/回 | 主な呼び出し元 |
| --- | ---: | ---: | ---: | --- |
| `__muldf3` | 171.1 | 10,589 | 62 | `__kernel_sin` 63、`__kernel_cos` 56、`__rem_pio2` 34、`JS_CallInternal` 18 |
| `__divdf3` | 21.0 | 8,236 | **392** | `JS_CallInternal` 21（JS の `/`） |
| `__subdf3` | 129.0 | 6,006 | 47 | `__rem_pio2` 47、`__kernel_sin` 37、`__kernel_cos` 35、`JS_CallInternal` 10 |
| `__floatsidf` | 161.6 | 2,830 | 18 | `JS_CallInternal` 82、`JS_ToFloat64Free` 65 |
| `__fixdfsi` | 102.2 | 1,813 | 18 | `parse_i16` 63、`JS_ToInt32Free` 25 |
| `__adddf3` | 57.1 | 1,441 | 25 | `__kernel_cos` 17、`JS_CallInternal` 16、`__kernel_sin` 13 |
| `__ltdf2` | 73.9 | 838 | 11 | `parse_i16` 63 |
| `__gtdf2` | 133.8 | 803 | 6 | `parse_i16` 126 |
| `__ashldi3` | 20.3 | 162 | 8 | `js_number` 17 |
| `__udivdi3` | 4.2 | 183 | 44 | `SysTickIsrHandler` 4 |
| 計（libgcc） | | 32,988 | | 全命令の 6.3% |

- **64 bit 整数の除算・剰余は QuickJS からは 1 回も呼ばれていない**（`__divdi3`・`__moddi3`・`__umoddi3` は 0、`__udivdi3` は tick 割り込みだけ）。ただし QuickJS の値（`JSValue`）は 32 bit 機では NaN boxing の 64 bit（`quickjs.h` の `JS_NAN_BOXING`）で、値の受け渡しは常に 2 レジスタになる。そのインラインの 64 bit の扱いは補助関数を呼ばないので、この表には出ない（未計測）。
- **`Math.sin`/`cos`**: libm の double の実装（`__kernel_sin`・`__kernel_cos`・`__rem_pio2`、計 3,709 命令）が double の乗算・減算・加算を 1 ターン約 310 回呼ぶ。補助関数の側を足すと 1 ターン約 2 万命令（推定、表の呼び出し元の比で配分）。ECMAScript は `Math.sin` などの精度を「実装による近似」としているので、double の全精度は仕様の要求ではない（ただし、アプリが結果の桁に依存していないかは別に確かめる必要がある）。S3 の FPU は単精度だけを持つ。
- **`__divdf3` の 392 命令/回** は他の補助関数の 6〜20 倍。21 回で 8,236 命令。
- **`parse_i16`**（`main/pocket/pocket_kasane.c:680`、Kasane のコマンドの座標の読み取り）は、値が整数（`JS_TAG_INT`）でも `JS_ToFloat64` → `round` → `<`・`>` の比較 → `(int16_t)` と、1 回に double の補助関数を 4〜5 本通る。1 ターン約 63 回、本体と補助関数で約 8 千命令（推定）。

### 3.2 命令キャッシュと flash

この道具はキャッシュの模型を持たない（それは `icache.py`・`turnset.py`・`lineuse.py`、[turn-icache.md](turn-icache.md)）。今回わかったのは置き場所の比だけ:

- flash で走る命令は 1 ターン 27.5 万（53%）。IRAM の 16.8 万は、アロケータ（tlsf・multi_heap・排他区間、ESP-IDF が IRAM に置く）と窓例外のハンドラ。ROM の 7.8 万は `memcpy`/`memcmp`・libgcc・libm の一部。
- **ROM と IRAM の命令は flash のキャッシュを通らない。** 過去の模型が「1 ターンに触る flash のコード 42.5 KiB」と言うとき、アロケータと double の計算はそこに入っていない。逆に言えば、それらを減らしても flash の待ちは減らない（減るのは §3.4 の模型のサイクル）。
- `JS_CallInternal` は 1 ターン 4.4 万命令（全体の 8.5%）、`l32r` が 1,825 回。

### 3.3 特殊回路への誘導路

#### 3.3.1 1 命令で済む読み書きが関数呼び出しになっている

呼び出しの辺（1 ターンあたり）:

| 呼び先 | 回/ターン | 主な呼び出し元 |
| --- | ---: | --- |
| `memcpy`（ROM、`__call_memcpy` 経由） | 905 | `get_u32` 198、`get_u16` 158、`block_cache_free` 118、`block_cache_malloc` 118、`JS_ConcatString2` 62、`private_command` 61 |
| `memcmp`（ROM） | 93 | `ksn_core_damage` 78、`__JS_FindAtom` 15 |

- **`get_u32`/`get_u16`**（`components/quickjs-ng/quickjs-ng/cutils.h:291`・`:310`、`static inline` で `memcpy(&v, tab, 4)`）は `-Os` でインライン展開されず、本体は `entry` → `l32r`（`memcpy` の番地）→ `callx8` → `l32i.n` → `retw.n` になっている（r5sim の ELF の逆アセンブル）。1 回の読み出しが `JS_CallInternal` → `get_u32` → ROM の飛び先の表 → `memcpy` の 2 段の呼び出しで、`memcpy` は 1 回平均 23 命令。バイトコードのオペランドは整列していないので `l32i` 1 本にはできないが、`l8ui` 4 本とシフトで 7 命令前後になる（推定）。STRESS は 1 ターン 4,300 オペコード、DERBY のレースは 11,000（実測、[turn-jumps-pie.md](turn-jumps-pie.md)）なので、オペランドの多い DERBY ほど効く。
- **R3 のブロックキャッシュ**（`components/pocketjs_guest/src/block_cache.c:58`・`:81`・`:99`）は、ブロックの先頭 1 語（次のブロックへのリンク）の読み書きを `memcpy(.., sizeof(void *))` で書いていて、これも `memcpy` の呼び出しになる。ブロックは確保器が返す 4 B 整列の領域なので、整列した 1 語の読み書きで足りる。
- 見積もり（推定、模型）: 2 つ合わせて 1 ターン約 590 回の `memcpy` の呼び出しが消えると、1 回あたり命令約 30 と成立した移行 4〜5 回、合わせて 1 ターン約 2 万サイクル（模型の 2.7%）。窓例外も減るはず（呼び出しの深さが 2 段浅くなる）だが、量は見ていない。

#### 3.3.2 tlsf のハードウェア除算

`quos` は 1 ターン 935 回で、静的に `quos`/`rems` を持つ関数のうち走っているのは `tlsf_malloc`（4 本）・`tlsf_free`（6 本）・`JS_CallInternal`（1 本）・`js_vm_stack_push` ほか。tlsf の 2 本ずつは ESP-IDF の `mapping_insert`（`components/heap/tlsf/tlsf_control_functions.h:312`）の `size / (small_block_size / sl_index_count)` で、制御構造の値で割るので定数のシフトにならない。値は 2 の冪なので、シフト量を持てば除算は要らない（ESP-IDF 側の変更になる）。除算 1 本 約 16 サイクル（推定）× 1 ターン数百本。

#### 3.3.3 使われている特殊命令

`loopnez`/`loop`（ゼロオーバーヘッドのループ）は 1 ターン 983 回、`mull` 1,686、`muluh` 750。FPU の単精度命令（`*.s`）は 0（JS の数は double なので、全部ソフトウェア）。

#### 3.3.4 ループ

後ろへ飛ぶ分岐の多い順で、上位はアロケータの探索（`heap_caps_aligned_alloc_base` 258 回、本体 17 B）、ROM の `memcmp`（202・192 回）、ブロックキャッシュ（130 回）、`JS_CallInternal` の中の 1 つ（119 回、35 B）。どれも 1 回の呼び出しで数回しか回らず、本体も短い。**同じ演算を 8〜16 レーンに並べられる長いループは、ターンの中に無い**（[turn-jumps-pie.md](turn-jumps-pie.md) §4 と同じ）。

### 3.4 パイプライン

模型のサイクル 757,430/ターンの内訳（flash の待ちを除く）:

| 項 | サイクル/ターン | % |
| --- | ---: | ---: |
| 命令 | 520,290 | 68.7% |
| 成立した制御移行 × 2（分岐の成立 30,355、`ret` 27,446、`call` 21,171、`j` 10,844、`jx` 4,601） | 188,836 | 24.9% |
| ロード直後の使用（実測 1 サイクル） | 32,658 | 4.3% |
| ハードウェア除算 × 16（推定） | 15,648 | 2.1% |

- 成立した移行の半分以上は呼び出しと戻り（48,617 回）。呼び出しの多い短い関数（`js_dup`、`JS_FreeValue`、`get_u32`、確保の各段）が、それぞれ `entry`/`retw` と 2 回の移行を払っている。
- 窓例外は約 4,000 回/ターン、ハンドラの中だけで 4.3 万命令。例外の出入りでパイプラインが空になる値段は模型に無く（未計測）、[turn-jumps-pie.md](turn-jumps-pie.md) §5 でも未測定のまま。1 回 6 サイクルなら 1 ターン約 2.4 万サイクル（仮定）。
- 実機の IPC は 0.445（実測、STRESS、[turn-jumps-pie.md](turn-jumps-pie.md) §2）、模型（待ち無し）は 0.69。差は flash の取得待ち。

## 4. 打ち手の候補（未着手、効果は模型の推定）

観察から見えた手を、軽い順に並べる。どれも実装しておらず、実機でも測っていない。

| 候補 | 対象（1 ターン） | 模型での減り（推定） | 気を付けること |
| --- | --- | --- | --- |
| `get_u32`/`get_u16`（と `get_i32` など同族）を `l8ui` の組み立てで常にインラインに | `memcpy` の呼び出し 356 回 | 約 1.2 万サイクル（STRESS）、DERBY はオペコードが 2.6 倍 | 上流パッチ（`cutils.h`）。flash は増えうる（呼び出し 1 箇所 → 7 命令前後） |
| ブロックキャッシュのリンクを整列した 1 語の読み書きに | `memcpy` 236 回 | 約 8 千サイクル | strict aliasing（[aliasing-types.md](aliasing-types.md)）に触れない書き方にする |
| `parse_i16` などに `JS_TAG_INT` の早道 | double の補助関数 約 300 回 | 約 8 千命令 | Kasane の契約（丸め・範囲外の拒否）を保つ |
| ブロックキャッシュの命中率（約 48%）を上げる | 外れ 1 ターン約 130 回 → heap_caps | アロケータ 26.5 万サイクルの一部 | まず外れた確保の大きさの分布を取る（[r3-small-block-cache.md](r3-small-block-cache.md) のクラスは host の分布から決めた） |
| tlsf の `mapping_insert` の除算をシフトに | `quos` 数百本 | 数千〜1 万サイクル | ESP-IDF のパッチになる |
| `Math.sin`/`cos` などを単精度 FPU（または表）で近似する選択肢 | double の乗減算 約 310 回 | 約 2 万命令 | 仕様上は許されるが、アプリの見た目と決定性（DERBY の再現など）への影響を先に決める必要がある。持ち主の判断 |
| `__divdf3`（392 命令/回）をハードウェアの 32 bit 除算や FPU の逆数を種にした実装へ | 21 回 | 約 6 千命令 | IEEE の丸めを正確に保つ必要がある。効果に比べて検査が重い |

アロケータ（35.7%）と窓例外（6.9%）は、ここに並べた小さな手より大きい。アロケータは「何を確保しているか」（`js_malloc_rt` の呼び出し元は `js_malloc` 151、`js_alloc_string_rt` 60、`js_realloc_rt` 29 回/ターン、解放は `free_gc_object` 113、`js_free_string0` 55）の側から減らす話になる。

## 5. 限界

- STRESS は Kasane の保持型シーンの頃の版（`stress_r5.js`）で、今の Shiki 版でも DERBY でもない。DERBY の数字は r5sim にその面（Shiki・proc）を足すまで取れない。
- QEMU の `-icount` の割り込みの入り方は実機と違う。tick 割り込みの命令（RTOS・割り込み 0.7%）は目安。
- 模型は flash・データキャッシュの待ち、窓例外の出入り、`l32r` の待ち（実機で 4.3%、実測、[turn-jumps-pie.md](turn-jumps-pie.md) §2）を含まない。
- 命令数が実機の XTPERF と 39% 違う（§2.2）。原因は推測のまま。

## 6. 再現

```bash
idf.py -C tools/r5sim -B <dir> -DSDKCONFIG=<dir>/sdkconfig -DR5SIM_GUEST_ALLOC=1 build
cd <dir> && python -m esptool --chip esp32s3 merge-bin --fill-flash-size 8MB -o qemu_flash.bin @flash_args
qemu-system-xtensa -machine esp32s3 -m 4M -drive file=qemu_flash.bin,if=mtd,format=raw \
    -serial file:serial.txt -monitor none -display none -icount shift=2 -d exec,nochain,in_asm \
    -dfilter 0x40000000..0x403fffff,0x42000000..0x43ffffff -D r5.log
# serial.txt に "R5SIM done" が出たら QEMU を止める（約 10.5 GB）
python tools/r5sim/observe.py r5.log <dir>/r5sim.elf --nm xtensa-esp32s3-elf-nm \
    --rom-elf <esp-rom-elfs>/esp32s3_rev0_rom.elf --first 61 --last 120 --json obs.json
```

`--json` は関数ごと・呼び出しの辺ごとの数を残すので、部品の集計や呼び出し元の表は記録を読み直さずに作れる。
