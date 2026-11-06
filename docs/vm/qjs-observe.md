# QuickJS のターンを QEMU で丸ごと観察する（2026-11-06）

[VMの文書](README.md)

問い: QuickJS（quickjs-ng）のボトルネックとして挙がっている 4 つ — (1) 実機には分不相応な計算精度（64 bit 除算など）、(2) 命令キャッシュのあふれと flash からの取得の遅さ、(3) PIE などコアの特殊回路への誘導路が無いこと、(4) パイプラインが埋まらないこと — を、手を付ける前に QEMU で観察する。

これまでの QEMU の記録（[turn-icache.md](turn-icache.md)、[qjs-pgo.md](qjs-pgo.md)）は `-dfilter` で **flash の命令だけ**を取っていた。libgcc のソフトウェア double と 64 bit 整数の補助関数は **ROM**（0x4000_0000〜）に、窓例外・割り込み・アロケータは **IRAM** にあるので、それらは見えていなかった。今回は ROM・IRAM・flash の全命令を取った。記録は [logs/qjs-observe-20261106.txt](logs/qjs-observe-20261106.txt)。

**この文書の数字は、断りの無いかぎり QEMU の命令列（実行した命令の数え上げ）と、それに当てたパイプラインの模型の推定である。QEMU はサイクルもキャッシュも模型にしていない。** 実機の値は「実測」と書き、出典を付ける。

## 0. 結論

**2026-11-06 追記（§7・§8）: 実機と同じゲスト部品で取り直した（`-DR5SIM_DEVICE=1`）。** 以下の §0〜§6 は確保関数だけを揃えた版（A）の記録で、比は大きくは変わらない。新しい数字は §7、PIE などの特殊命令のランキングは §8。

- **実機同等（§7）**: 1 ターン 54.9 万命令、模型 80.2 万サイクル（IPC 0.68）。アロケータは **37.9%** に増えた。実機の確保関数は 1 回ごとに `xTaskGetCurrentTaskHandle()`（割り込みレベルの上げ下げを含む）で所有タスクを確かめていて、1 ターン 493 回、約 2.2 万サイクル（模型の 2.7%）。helper の IRAM 化で flash の命令は 27.5 万 → 17.1 万。
- **PIE のランキング（§8）**: 候補は全部合わせて模型の **0.1%**（1 ターン約 750 サイクル）。最大は ROM の `memcpy` を PIE 版に替える案で 0.05%（1 回平均 37 B しか写さない）。GCC はスカラーの特殊命令（`moveqz`/`movnez`・`min`/`max`・`nsau`・ゼロオーバーヘッドの `loop`）を既に使っている。**STRESS のターンでは、PIE や特殊命令への誘導で取れるものはほぼ無い。**

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
| 確保関数の所有タスクの確認（`guest_uses_block_cache` → `xTaskGetCurrentTaskHandle`）をターンの始めの 1 回に（§7.2） | 493 回 | 約 2.2 万サイクル | 別タスクからの確保（音・ネットの完了など）がキャッシュを通らない、という今の保証を別の形で保つ |
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

## 7. 実機同等で取り直す（`-DR5SIM_DEVICE=1`、2026-11-06）

### 7.1 何を揃えたか

§1 の `-DR5SIM_GUEST_ALLOC=1` は確保関数だけを真似ていた。実機のゲストは、それ以外にもメモリ上限・GC の閾値・スタック上限・harden・割り込みハンドラ（ターンの期限）・ターンの予算を持つ。r5sim は上限と GC の閾値だけは評価後に設定していたが、残りは無かった。`-DR5SIM_DEVICE=1` では手で真似るのをやめ、**実機の部品そのもの**を使う:

| 項目 | 実機（`main/app_session.c`） | `-DR5SIM_DEVICE=1` |
| --- | --- | --- |
| ゲスト | `pocketjs_guest_create`（160 KiB、スタック 20 KiB、PSRAM なし） | 同じ関数・同じ値 |
| 確保 | `guest.c` の確保関数（R3 のブロックキャッシュ、所有タスクの確認） | 同じ（部品をリンク） |
| harden・GC の閾値・割り込みハンドラ | `pocketjs_guest_create` の中 | 同じ |
| 評価 | `pocketjs_guest_eval`、期限 2 秒 | 同じ |
| ターン | `arm_turn()`（期限 250 ms、予算 `VM_TURN_BUDGET_US`）→ `pocketjs_guest_frame` → `pocket_kasane_end_turn` | 同じ順。予算で切られた残り（継続ターン）は同じ区切りの中で続ける（今回 0 回） |
| helper の IRAM 化 | `CONFIG_POCKET_VM_HELPER_IRAM=y`（既定）で `main/vm/helper_iram8.lf` | 同じ断片をリンク |

揃っていないもの: `app_tick` のゲストの外の仕事（presenter、通知、`sample_memory_pressure`）、Shiki・proc などの面、描画（16×8 のスカラー）。ゲストの外は区切りの外か、STRESS が使わない面。

### 7.2 結果（STRESS LV1、ターン 61〜120）

`frames=120 frame_errors=0 yields=0 continuations=0 oom=0 fail=0`。

| 部品 | 命令/ターン | 模型のサイクル | % |
| --- | ---: | ---: | ---: |
| アロケータ（`guest_*` の確保関数と所有タスクの確認を含む） | 200,685 | 298,300 | 37.9% |
| quickjs.c | 169,286 | 249,698 | 31.7% |
| Kasane・pocket ほか | 63,612 | 86,753 | 11.0% |
| 窓例外 | 42,997 | 51,597 | 6.6% |
| libgcc（ソフトウェア double・64 bit） | 34,486 | 46,421 | 5.9% |
| `memcpy` など | 29,409 | 41,293 | 5.2% |
| RTOS・割り込み | 4,923 | 7,090 | 0.9% |
| libm（double） | 3,707 | 5,435 | 0.7% |
| 計 | 549,105 | 786,587（除算込みで 802,390） | |

- 置き場所: IRAM 295,168、flash 170,879、ROM 83,058 命令/ターン。A（§2）の flash 27.5 万から約 10 万が IRAM に移った。helper の IRAM 化（[p1-iram-placement.md](p1-iram-placement.md)）がそのまま見えている。
- A との差は +2.9 万命令。ほぼ全部が実機の確保関数のラッパー: `guest_usable_size` +7,045（QuickJS が確保・解放のたびに大きさを聞く、585 回/ターン）、`guest_uses_block_cache` +6,053 と `xTaskGetCurrentTaskHandle` +5,708 と `_xtos_set_intlevel` +5,574（所有タスクの確認、493 回/ターン）、`guest_malloc`/`guest_free` +6,265。
- **所有タスクの確認**（`components/pocketjs_guest/src/guest.c` の `guest_uses_block_cache`）は、確保・解放のたびに `xTaskGetCurrentTaskHandle()` を呼ぶ。この関数は割り込みを止めてコアの番号を読むので、1 回あたり約 35 命令と呼び出し 2 段。1 ターン約 2.2 万サイクル（模型の 2.7%）。ゲストのターンは常に所有タスクで走るので、ターンの始めに 1 回確かめて印を持てば消える（未着手、§4 の表）。
- 窓例外（overflow 2,018 回/ターン）、libgcc の表、`get_u32`/`get_u16` → `memcpy`（356 回/ターン）は A と同じ。

## 8. 特殊命令のランキング（`tools/r5sim/pie_rank.py`）

### 8.1 道具

`observe.py --json` は、全命令の実行回数と、関数の中で後ろへ戻る移行（成立した分岐、`j`、ゼロオーバーヘッドのループの LEND → LBEG）も書き出す。`pie_rank.py` はそれを読み（数秒、記録を読み直さない）、模型で減るサイクルの順に並べる:

| 種類 | 何を探すか | 置き換え |
| --- | --- | --- |
| PIE copy / fill / search / reduce / map | 本体が連続した読み書き（帰納変数のポインタ）だけでできたループ | `EE.VLD.128.IP`・`EE.LD.128.USAR.IP`＋`EE.SRC.Q`・`EE.VST.128.IP`・`EE.VCMP.*`・`EE.VADDS.*` など |
| LOOP | 帰納変数で回数が決まり、戻りが成立分岐（2 バブル）のループ | `LOOPNEZ`/`LOOPGTZ` |
| MOVcc / MIN/MAX | 1 命令の移動を飛び越す分岐 | `MOVEQZ`/`MOVNEZ`/`MOVLTZ`/`MOVGEZ`、`MIN`/`MAX`/`MINU`/`MAXU` |
| DIV | 2 の冪の定数による `quos`/`quou`/`rems`/`remu` | シフト・`EXTUI` |

- 退ける理由も出す: 本体に呼び出しがある、ポインタをたどる（番地が読んだ値から来る）、番地を計算している（gather）、1 回の入りで扱うバイトが 16 B のベクタに足りない。
- PIE の値段は実機の測定（1 命令 1 サイクル、`EE.VST.128.IP` は +0.6、[perf/pie-simd.md](../perf/pie-simd.md)）。整列していない読みは `EE.LD.128.USAR.IP`＋`EE.SRC.Q` の 2 命令、書きは頭の数バイトをスカラーで揃える前提で、頭と尾の合計 15 B と準備 6 サイクルをスカラーで払う（仮定）。命令名は esp32s3-hw-mcp の TRM 抽出（`data/pie_instructions.json`）で確かめ、ページを付ける。
- **これはパターン照合で、ベクトル化器ではない。** 候補は「命令の組み合わせと回数から見て PIE で速くなりうる量」で、意味・別名・整列の正しさは書き換える側が示す。
- 照合の試験: `tools/r5sim/test_pie_rank.py`（コピー・塗り・ポインタたどり・分岐越しの移動・回数の決まったループを手で書いて、分類が合うことを見る）。

### 8.2 結果（§7 の実機同等の記録）

| # | 減るサイクル/ターン | % | 種類 | 場所 | 1 回の入り | 置き換え |
| ---: | ---: | ---: | --- | --- | --- | --- |
| 1 | 382 | 0.05% | PIE copy | ROM の `memcpy` | 平均 37 B、127.5 回/ターン | `EE.LD.128.USAR.IP`＋`EE.SRC.Q`、`EE.VST.128.IP` |
| 2 | 105 | 0.01% | PIE fill | ROM の `memset` | 平均 64 B、23.4 回 | `EE.ZERO.Q`、`EE.VST.128.IP` |
| 3〜11 | 各 8〜56 | ≤0.01% | PIE reduce/search、LOOP、MOVcc | `apply_outcome`・`JS_NewStringLen`・`ksn_core_damage` ほか | | |
| 計 | 約 750 | 0.1% | | | | |

- 一番時間を使っているループ（`JS_CallInternal` の命令ループ 4.2 万サイクル/ターン、heap の排他区間、`find_own_property`、`heap_caps_free` の探索）は、どれも本体に呼び出しがあるか、ポインタをたどる。ROM の `memcmp` は 1 回 15 B しか比べない。
- GCC はスカラーの特殊命令を既に使っている（1 ターンあたり実行回数）: `moveqz` 1,832、`movnez` 747、`max`/`maxu` 1,129、`min`/`minu` 579、`nsau` 1,674、`salt`/`saltu` 856、ゼロオーバーヘッドのループの戻り 1,493。残っている「1 命令の移動を飛び越す分岐」は 16 箇所・472 回/ターンだけ。
- tlsf の `quos` は除数がレジスタ（制御構造の値）なので、この道具の DIV（定数の 2 の冪）には当たらない（§3.3.2 の判断は ESP-IDF の値を読んだもの）。

**読み方**: STRESS の JS のターンでは、PIE や特殊命令への誘導で取れるものはほぼ無い（[turn-jumps-pie.md](turn-jumps-pie.md) §4 の結論を、全命令と実機同等の構成で確かめた）。道具は記録を選ばないので、PIE が効きうるのは描画・ネイティブの面（Shiki、proc）のように連続したデータを回すコード。それを測るには、r5sim にその面を足すか、ターンの外を区切る印が要る。

### 8.3 再現

```bash
idf.py -C tools/r5sim -B <dir> -DSDKCONFIG=<dir>/sdkconfig -DR5SIM_DEVICE=1 build
# QEMU と observe.py は §6 と同じ（--json を付ける）
python tools/r5sim/observe_parts.py obs.json qjs_funcs.txt     # 部品ごと
python tools/r5sim/pie_rank.py obs.json --top 30 [--min-cycles 100] [--pie-db <esp32s3-hw-mcp>/data/pie_instructions.json]
python tools/r5sim/test_pie_rank.py
```

## 9. 速い計測（`tools/r5sim/fast.sh`、2026-11-06）

全命令のトレースと `observe.py` は 1 回 30 分かかり、改善を 1 つずつ試すには遅い。`-icount` の QEMU では仮想時計が実行した命令の数に比例して進むので、r5sim はターン 61〜120 の CCOUNT の差を `R5SIM ccount per_turn=` として出す。`fast.sh <dir> [-D...]` はビルドして `-d` なしで走らせ、その行と像の大きさを出す（約 23 秒）。

- 換算: 実機同等（`-DR5SIM_DEVICE=1`）の基準で 84,012 カウント/ターン ＝ トレースの 549,105 命令/ターン、**1 カウント ≒ 6.54 命令**。改善後のトレース（509,159 命令）とカウント（77,621）の比も 6.56 で、同じ比で動く。
- 同じ像を 2 回走らせて同じ値（84,012 / 84,012）。差がどれだけ小さくても、像が違えば意味がある（ただし命令数であってサイクルではない。flash の待ちは見えない）。
- 割り込み（1 ms の tick）とターンの前後の数命令を含む。

## 10. 確保のクラスを実機の寸法で選び直す

### 10.1 外れの大きさ（r5sim）

`-DR5SIM_GUEST_ALLOC=1` の r5sim は、ターン 61〜120 の確保・キャッシュの外れ・キャッシュに積めなかった解放を大きさ別に数える（`R5SIM alloc_by_size` ほか）。1 ターン 248 回の確保のうち外れ 130 回の内訳は 24 B 38・16 B 30・48 B 26・28 B 16・12 B 10 回で、**24・28 B はクラスに無かった**（R3 のクラスは 64 bit の host の分布から決めていた。[r3-small-block-cache.md](r3-small-block-cache.md) §1）。一方 72・88 B の確保は 1 ターン 0〜0.5 回。

### 10.2 r5sim の STRESS は古い

r5sim の STRESS（`stress_r5.js`、Kasane の保持型シーン）は今の Shiki 版と確保の大きさが違う。実機と同じ寸法（`-m32`）のホストのトレースで比べた:

| 負荷（`-m32`） | 確保の大きさの上位 |
| --- | --- |
| STRESS（Shiki 版、`tools/test_stress_app.c`、`STRESS_ALLOC_TRACE`） | 32 B 20.3%、16 B 19.7%、48 B 19.7%、12 B 10.0%、80 B 9.8%、36 B 9.0%、24・28 B 合わせて約 1% |
| DERBY（`tools/games/test_derby_host.c`、新しい `DERBY_ALLOC_TRACE`、frame() の中だけ） | 48 B 21.7%、16 B 21.1%、32 B 20.1%、80 B 10.8%、12 B 8.6%、**28 B 5.4%、24 B 4.8%**、64 B 1.9%、36 B 1.6% |
| vmtest の `bench_*` など（32 bit の vmrun の `--trace`） | 48・16・12・80・32 B が大半 |

**r5sim の STRESS で測った確保の改善の大きさは、今のアプリにそのまま当てはまらない。** クラスの選択はホストのトレースで行い、r5sim は「同じ規則の実装が組めて動くこと」と命令数の変化の向きを見る役にした。

### 10.3 選び方と結果

`tools/vmtest/prof/cache_classes.py` は `block_cache.c` と同じ規則（最大 8 クラス、クラスは丸めた長さ 1 つ、深さ K、realloc はヒープへ）でトレースを再生する。深さ 8 で、確保のうちキャッシュから出た割合（キャッシュが上限の外で持った最大のバイト数）:

| トレース | 旧 {12,16,32,36,48,72,80,88} | **新 {12,16,24,28,32,36,48,80}** | 貪欲法 {12,16,24,28,32,48,64,80} |
| --- | ---: | ---: | ---: |
| STRESS（Shiki 版） | 50.6%（3,072 B） | **51.8%**（2,208 B） | 47.8%（2,112 B） |
| DERBY | 78.5%（2,424 B） | **88.4%**（1,584 B） | 88.6%（1,516 B） |
| `bench_promise` | 94.5% | 94.5% | 99.8% |
| `closures` | 29.4% | 34.1% | 34.0% |
| その他のベンチ | 同じ | 同じ | 同じ |

新しい組はどのトレースでも旧以上で、持つメモリは減る（上限 3,072 → 2,208 B）。貪欲法は 36 B を落として STRESS で負けるので採らない。r5sim（実機同等、古い STRESS）では 77,621 → 69,073 カウント（−11.0%）。

### 10.4 深さ（決めていない）

深さを増やすと外れはさらに減るが、上限の外で持つメモリが増える（r5sim、新しいクラス）:

| 深さ | カウント/ターン | 持った最大 |
| --- | ---: | ---: |
| 8（今） | 67,531 | 2,208 B |
| 12 | 66,229（−1.9%） | 2,992 B |
| 16 | 64,947（−3.8%） | 3,776 B |
| 32 | 61,999（−8.2%） | 7,232 B |

（`-DR5SIM_GUEST_ALLOC=1` の版。古い STRESS なので比は目安。）DRAM と速さの交換なので、実機のアプリで測ってから持ち主が決める。

## 11. この日の改善（2026-11-06）

速い計測（`-DR5SIM_DEVICE=1`、カウント/ターン、§9）。各行はその前の行からの差。

| コミット | 内容 | カウント/ターン | 差 | flash | DIRAM |
| --- | --- | ---: | ---: | ---: | ---: |
| （基準） | | 84,012 | | | |
| `a97229f` | quickjs-ng の `get_u32`/`get_u16` などをリトルエンディアンではバイトから組み立てる（`cutils.h`、§3.3.1）。`get_u16` はインライン展開される。`get_u32` まで強制インラインにすると −0.24% のために flash +1,956 B なので採らない | 82,237 | −2.1% | +32 B | −4 B |
| `ffcbc8d` | ブロックキャッシュのリンクを `may_alias` の 1 語で読み書き（`memcpy` の呼び出しを消す） | 81,342 | −1.1% | −32 B | |
| `a78dfa0` | 所有タスクの確認を `rur.threadptr`（タスクごとの TLS の基点、切り替えで復元）で。割り込みを止める `xTaskGetCurrentTaskHandle()` をやめる（§7.2） | 79,354 | −2.4% | −8 B | |
| `27b0134` | `pocket.kasane` の `parse_i16`・`number_in` に整数の早道（§3.1） | 77,621 | −2.2% | +8 B | +12 B |
| `c73e62d` | R3 のクラスを実機の寸法で選び直す（§10） | 69,073 | −11.0% | | |
| 計 | | | **−17.8%** | | |

全命令のトレースでも、最初の 4 つの後で 549,105 → 509,159 命令/ターン（−7.3%、カウントでは −7.6%）。

検査: vmtest コーパス asan・o2 とも 88/88、Test262 部分集合の退行 0（22,021 pass）、`tools/test_block_cache.c`・`tools/test_pocket_kasane.c`・`tools/test_stress_app.c`（STRESS_HOST PASS）・`tools/test_guest_cache_pressure.py`・`tools/games/run_derby.py --m32`（ODDS_CHECK PASS）。**実機では測っていない**（命令数の減りが、flash の待ちを含む実機のサイクルにどう出るかは未確認。§10.2 のとおり確保の効きはアプリで違う）。

## 12. 今の STRESS を r5sim で回す（`-DR5SIM_APP=shiki`）

§10.2 のとおり、r5sim の STRESS（`stress_r5.js`）は今のアプリではない。`-DR5SIM_APP=shiki` は `apps/stress/stress.js`（Shiki で描く版）を、`tools/build_stress_app_test.sh` がホストで組むのと同じ部品で回す: Shiki の表は `stress_prog.js` から Node で作り（`emit_shiki_rom.mjs`）、Shiki の区画はゲストより先に取る（`app_session.c` と同じ順）。`pocket_kasane.c` は `pocket.kasane.shiki` を弱い参照で探すので、`pocket_shiki_api.c` はリンカの `-u` で引き込む。

- **Shiki のエンジンは C のモデルで走る**（QEMU に PIE が無く、スカラーの JIT は PIE の経路の中にある）。描画（`ksn_shiki_resume` など）の命令は実機のものではない。JS の側（QuickJS・アロケータ・libgcc・窓例外）は実機と同じコード。
- **ファイルごとの最適化を実機に合わせた。** ファームは描画と Shiki の 7 ファイル（`ksn_font`・`ksn_render`・`ksn_core`・`pocket_kasane`・`pocket_shiki_frame`・`ksn_shiki`・`ksn_shiki_frame`）を `-O2` で組む（`main/CMakeLists.txt`、[lcd-dma-driver.md](../platform/lcd-dma-driver.md) §9.12）が、r5sim は全部 `-Os` だった。`-Os` では `ksn_shiki.c` の可変量の 64 bit シフトが全部 libgcc の呼び出し（`__ashldi3` 1,714・`__ashrdi3` 1,324 回/ターン）になり、**最初の Shiki 版の記録は 64 bit シフトの関数呼び出しが目立つ、実機には無い姿を見せていた**。`-O2` では呼び出しは 0（逆アセンブルで確認）。命令数は同じ（インラインのシフトが呼び出しと同じだけの命令）。
- 1 ターン（Shiki 版、`-O2` に合わせた後、全命令のトレース）: 112.8 万命令のうち描画ほか 71.3 万、quickjs.c 17.3 万、アロケータ 12.7 万、libgcc 6.4 万（シフトを除くと約 4 万）、窓例外 2.7 万。JS の側（描画を除く）は約 38 万命令。実機の XTPERF の 1 ターンは 75.3 万命令（STRESS、Shiki の JIT の描画を含む、[turn-jumps-pie.md](turn-jumps-pie.md) §2）なので、残りの約 37 万が JIT の描画と commit という見積もりになる（推定、内訳は実機で分けていない）。
- `pocket_kasane_end_turn` は 1 ターン 24 カウント。Shiki の描画は `frame()` の中（JS から呼ぶネイティブ）で走る。

## 13. 続きの改善と、採らなかったもの

速い計測（カウント/ターン）。左が Shiki 版 STRESS、右が古い STRESS。

| 変更 | Shiki 版 | 古い STRESS | flash |
| --- | ---: | ---: | ---: |
| （§12 の `-O2` を合わせた後の基準） | 176,653 | 67,975 | |
| `17ef0d1` 割り切れる int/int の `/` はハードウェアの除算で（−0 と `INT32_MIN / -1` は double のまま） | 176,434（−0.12%） | 67,769（−0.3%） | +28 B |
| `f527f7e` 確保の補助関数（`guest_uses_block_cache` ほか 6 つ）を `always_inline`。`-Os` は `static inline` でも呼び出しのまま残していた | 175,750（−0.4%） | 66,852（−1.35%） | +20 B |
| `dfefdf8` ブロックキャッシュの当たりの経路をヘッダのインラインに、外れは `guest.c` が heap_caps を直接呼ぶ（確保の鎖が 2 段浅くなる） | 173,606（−1.2%） | 64,260（−3.9%） | −80 B |

**今日の全体**（今日の道具で、今日のソースの変更を取り消した木と比べた。`.cache` の作業ツリー）: Shiki 版 188,466 → 173,606（−7.9%）、古い STRESS 82,922 → 64,260（−22.5%）。flash ほぼ ±0、DIRAM +16 B。

**命令キャッシュの模型でも損をしていない**（`icache.py`、自己配置、古い STRESS、`f527f7e` の時点）: warm ミス 684 → 629、cold 1,234 → 1,206、触る行 1,146 → 1,128。

### 13.1 採らなかった: インタプリタの中だけ `js_dup`・`JS_FreeValue` をインライン化

`JS_CallInternal` の中だけで使うインライン版を作った（他の呼び出し元は今の呼び出しのまま）。命令は減る（Shiki 版 −0.73%、古い STRESS −1.2%）が、`JS_CallInternal` が 22,927 → 24,876 B（flash +2.2 KB）になり、模型の命令キャッシュのミスが増えた:

| | 前 | インライン版 |
| --- | ---: | ---: |
| 8-way warm / cold | 629 / 1,206 | 703 / 1,263 |
| 完全連想 warm / cold | 700 / 1,186 | 712 / 1,201 |

ミス 1 回を約 450 サイクル（QIO、[turn-icache.md](turn-icache.md) §1.1.1 の推定）とすると、8-way では +2.6〜3.3 万サイクルで、命令の減り（約 1 万サイクル）を上回る。完全連想ではほぼ相殺。この 2 つは IRAM（`helper_iram8.lf`）にあり、キャッシュを通らない場所から通る場所へ命令を移すことになる。**命令数だけで判断すると間違える例**として残す。

### 13.2 見つけたが手を付けていない: heap_caps が最初の領域で失敗する

Shiki 版のトレースで、ブロックキャッシュに外れた確保 80 回/ターンに対し `multi_heap_malloc`（tlsf の探索）が 161 回。QEMU のヒープの構成（`heap_caps_print_heap_info`）:

| 領域 | 長さ | 空き | 最大の空き |
| --- | ---: | ---: | ---: |
| 0x600fe000（RTC fast） | 8,168 | 7,788 | 7,680 |
| 0x3fce9710 | 22,308 | 936 | 224 |
| 0x3fcf0000 | 32,768 | 32,032 | 31,744 |
| 0x3fc9f100（主） | 304,656 | 168,340 | 143,360 |

heap_caps は満杯で細切れの 0x3fce9710 を先に試し、失敗してから主の領域へ行く。失敗する試行は 1 回あたり排他区間の出入りと tlsf の探索で約 150 命令、1 ターン約 1.2 万命令（JS の側の約 3%）。**領域の境目は静的 DRAM の量で決まるので、実機（静的 DIRAM 111 KB）で同じことが起きているかは未確認。** 実機で `heap_caps_print_heap_info` を見てから決める。

### 13.3 小さすぎて見送り

`js_malloc_rt`/`js_free_rt` は確保・解放のたびに関数ポインタ経由で `guest_usable_size` を呼ぶ（430 回/ターン、1 回約 12 命令）。QuickJS の `JSMallocFunctions` の形を変える上流の API の変更になり、効果は約 0.6%。

## 14. IRAM の helper の一覧は今の負荷でも保つ

`main/vm/helper_iram8.lf`（`CONFIG_POCKET_VM_HELPER_IRAM`、8 KiB）は古い STRESS のトレースから選んだ一覧。今の Shiki 版 STRESS で選び直すと良くなるかを、helper を flash に残した像（`-DR5SIM_HELPER_IRAM=0`）の flash の命令列で比べた（`icache.py model`、自己配置、8-way 32 KiB）:

| 一覧 | IRAM | warm | cold | 触る行 |
| --- | ---: | ---: | ---: | ---: |
| 一覧なし | 0 | 1,300 | 1,747 | 48.5 KiB |
| 今の一覧（`--force-file main/vm/helper_iram8.lf`） | 7,555 B | 984 | 1,430 | 41.6 KiB |
| 今の負荷で選び直し（`--iram-kib 8 --only <quickjs.c の関数>`） | 8,192 B | 976 | 1,391 | 40.9 KiB |

選び直しの差（warm −0.8%、cold −2.7%）は配置の揺れ（約 15%）の中なので、一覧は変えない。この像の Shiki は C のモデルで flash を使う（実機では JIT のコードが RAM にある）ので、「一覧なし」の行は実機より悪く出ている。`icache.py` に足した `--force-file`（名前か `.lf` の一覧を強制）と `--only`（候補をこの名前に限る）はこの比較のため。

## 15. 窓例外はどこで起きるか（Shiki 版、`dfefdf8` の後）

`observe.py` は窓例外の直前に走っていた関数（溢れを起こした呼び先）も数える（`exc_from`）。Shiki 版 STRESS で窓の overflow は 1 ターン 1,280 回（§12 の時点）→ **827 回**（当たりの経路のインライン化の後）。命令は 1,128,040 → 1,105,135/ターン。

| 溢れを起こした関数 | 回/ターン | % |
| --- | ---: | ---: |
| `esp_heap_adjust_alignment_to_hw` | 96.7 | 11.7% |
| `xPortInIsrContext` | 84.6 | 10.2% |
| `tlsf_get_pool` | 63.2 | 7.6% |
| `guest_malloc` | 56.4 | 6.8% |
| `heap_caps_aligned_alloc_base` | 56.1 | 6.8% |
| `heap_caps_malloc_base` | 54.2 | 6.6% |
| `js_malloc_rt` | 47.3 | 5.7% |
| `assert_valid_block` | 36.4 | 4.4% |
| `memcpy` | 28.1 | 3.4% |
| その他（`heap_caps_*`・`u32toa`・`guest_usable_size`・`js_shape_hash_unlink` …） | | |

7 割以上がブロックキャッシュに外れた確保の鎖（`js_malloc_rt` → `guest_malloc` → heap_caps → multi_heap → tlsf → 排他区間、ESP-IDF の部分だけで 8 段前後）の奥で起きる。ここから先を減らす手は「外れの回数」を減らすことで、§10.4 の深さ（DRAM との交換）か、アプリの確保そのもの。STRESS はわざとヒープを荒らすアプリなので、その確保を減らす手は取らない。

## 16. 次に決めること・確かめること

- **実機で測る**: 今日の変更はすべて QEMU の命令数と模型の上の結果。実機の turn（`stress_app.py`、`KASANE_PAINT`、XTPERF の `TURNPERF`）で確かめる。flash の配置が変わるので、ビルド間の 15% の揺れに注意（同じバイナリでの比較が要るものは A/B ビルドで）。
- **ブロックキャッシュの深さ**（§10.4）: 8 → 16 で古い STRESS −3.8%・上限の外 +1.5 KB。DRAM と速さの交換。
- **`Math.sin`/`cos` の精度**（§3.1）: double の libm が Shiki 版で 1 ターン約 2 万命令。仕様は近似を許す。アプリの見た目と決定性への影響を決めてから。
- **heap_caps の最初の領域**（§13.2）: 実機の `heap_caps_print_heap_info` で、ゲストの確保が満杯の小さな領域を毎回試しているかを見る。
- **DERBY を r5sim で**: 一番重いアプリ。時間の進め方・チャンクの読み込み・入力の台本が要る（`tools/games/test_derby_host.c` 相当）。
