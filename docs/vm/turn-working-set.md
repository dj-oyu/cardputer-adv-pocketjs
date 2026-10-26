# 1 ターンの作業集合を部品ごとに分ける（2026-10-23）

段階つきのインタプリタ（V8 のように段を分け、機械語は生成しない）を設計する前の下調べ。設計の規則は「どの段でも、
1 ターンが触るコードが予算に収まる」で、予算の案は**インタプリタの取り分が約 14 KiB、native との境界（Kasane の
提出・`pocket.*`・アロケータ）が約 12 KiB**。その前に、今のターンが触るコードがどの部品にどれだけあるかと、
3 つの試作がそれをどれだけ縮めたかを数えた。

道具は R5（[r5-icache.md](r5-icache.md)）の `tools/r5sim/` をそのまま使い、部品ごとの集計に
`tools/r5sim/turnset.py` を足した。**この文書の数字はすべて模型の推定か、ELF・コードから読んだ値で、実機では
測っていない。**

## 0. 結論

- **B（3 つの試作を入れた木）でも、インタプリタの取り分は 14 KiB に収まらない。** `JS_CallInternal` が 7.4 KiB、
  そこから呼ばれる小さな関数（プロパティ・atom・数値の遅い経路・変換・呼び出し）が 13.9 KiB で、合わせて
  **21.3 KiB（予算の約 1.5 倍、7.3 KiB 超過）**（模型の推定）。
- 超過の主因は `JS_CallInternal` ではなく**周りの helper**。内訳はプロパティの読み書き・定義 4.0 KiB、atom と遅延名 2.2 KiB、
  浮動小数の算術（`js_*_slow`）2.1 KiB、変換 1.6 KiB、呼び出し・フレーム 1.5 KiB。STRESS の算術は double なので、
  QuickJS の「遅い経路」がここでは普通の経路になっている。
- `JS_CallInternal` の 7.4 KiB のうち、稀な命令・遅い経路・例外・まれにしか通らない行は約 1.2〜1.5 KiB で、
  残りの約 6 KiB は毎ターン何度も通る行（§4）。稀な部分を全部追い出しても core は 6 KiB を切らない。
- 試作 3 つの効果は 1 ターン 42.5 → 41.8 KiB（−0.7 KiB）。ほぼ `QJS_DISABLE_DUMPS` による `JS_CallInternal` の縮小
  （関数が 25,170 → 20,915 B、ELF から読んだ値。触る行は 8.31 → 7.40 KiB）。
- **R5 の模型には行を水増しするバグがあった**（§5.1）。R5 の「1 ターン 45.5 KiB」は正しくは **42.5 KiB**、ミスも
  過大だった。修正済み。

## 1. 条件

| | A（基準） | B（合流後） |
| --- | --- | --- |
| 木 | `main` の `e0fe98a` | `e0fe98a` ＋ `53af3d2`（`QJS_DISABLE_DUMPS`）＋ `40b300d`（TDZ 検査の省略）＋ `e678a1f`（`js_call_math_fast`）を `--no-ff` で合流 |
| `JS_CallInternal` の大きさ（ELF） | 25,170 B | 20,915 B |

- STRESS LV1、ターン 61〜120 の 60 ターン（R5 と同じ）。QEMU の終了行はどちらも `frames=120 oom=0 fail=0 bad_present=0`。
- **配置**: 模型は各 case の `r5sim.elf` 自身の番地に写した（ファームの ELF は使っていない）。r5sim と実機のファームは
  関数の目的コードは同じだが、並びが違う。**触る行数（KiB）は配置にほとんど依存しない**が、8-way のミス回数は配置で
  大きく動く（A では 8-way の warm 1,326 に対して完全連想 1,044）。A と B の 8-way のミスの差には配置の引き直しが
  混ざるので、比較は完全連想の列で読む。
- 区切りは R5 と同じく `r5_turn_begin`/`r5_turn_end`。ターンは `JS_Call` ＋ ジョブの消化 ＋ `pocket_kasane_end_turn`。
  描画（`present()`）はターンの外。

## 2. 部品ごとの作業集合

「KiB」は 1 ターンに触る 32 B 行の数（60 ターンの平均）、「全ターン」はどれかのターンで触った行の和集合。
「稀」は 1 ターンに 4 回未満しか通らない行（取り込んで数回使うだけで、ミスの元が取れない行）。すべて模型の推定。

### 2.1 A（基準 `e0fe98a`）

| 部品 | KiB/ターン | 稀 KiB | 全ターン KiB | warm ミス（8-way / 完全連想） | cold ミス（8-way / 完全連想） |
| --- | --- | --- | --- | --- | --- |
| interp-core | 8.31 | 1.12 | 9.47 | 197 / 153 | 370 / 312 |
| interp-helpers | 14.20 | 2.09 | 18.00 | 451 / 343 | 632 / 510 |
| object/alloc/GC | 5.68 | 0.68 | 6.62 | 204 / 110 | 287 / 204 |
| builtins | 5.78 | 2.46 | 9.91 | 199 / 198 | 232 / 206 |
| native boundary | 8.37 | 1.87 | 14.06 | 272 / 238 | 338 / 273 |
| libc/other | 0.20 | 0.04 | 2.88 | 2 / 2 | 6 / 6 |
| **計** | **42.5**（1,361 行） | | 60.9 | **1,326 / 1,044** | **1,865 / 1,512** |

### 2.2 B（合流後）

| 部品 | KiB/ターン | 稀 KiB | 全ターン KiB | warm ミス（8-way / 完全連想） | cold ミス（8-way / 完全連想） |
| --- | --- | --- | --- | --- | --- |
| interp-core | **7.40** | 1.18 | 8.38 | 158 / 129 | 305 / 270 |
| interp-helpers | **13.93** | 2.38 | 17.72 | 357 / 327 | 553 / 497 |
| object/alloc/GC | 5.65 | 0.66 | 6.56 | 143 / 101 | 241 / 201 |
| builtins | 6.07 | 2.47 | 10.19 | 185 / 203 | 226 / 217 |
| native boundary | 8.51 | 1.95 | 14.16 | 227 / 233 | 298 / 275 |
| libc/other | 0.20 | 0.04 | 2.84 | 2 / 2 | 6 / 6 |
| **計** | **41.8**（1,337 行） | | 59.8 | **1,071 / 995** | **1,630 / 1,467** |

読み方:

- 予算と並べると（B）: インタプリタ（core＋helpers）**21.3 KiB 対 14 KiB**。native の側は、Kasane・`pocket.*`・
  r5sim の糊が 8.5 KiB で、アロケータまで含めるなら object/alloc/GC の 5.65 KiB を足して **14.2 KiB 対 12 KiB**。
  builtins の 6.1 KiB（libm を含む）はどちらの予算にも入っていない。合計 41.8 KiB 対 26 KiB。
- cold のミスが触る行より多い（B で 1,467 > 1,337、完全連想でも）。1 ターンの中でもすでに容量が足りず、同じ行を 2 回
  取り込んでいる。
- 全ターンの和集合（約 60 KiB）は 1 ターン（約 42 KiB）よりずっと大きい。STRESS は段階でコードが入れ替わるので、
  予算は「1 ターン」で切るのが正しく、アプリ全体の和で切ると過大になる。
- 試作の効果（完全連想の warm）: core 153 → 129、helpers 343 → 327。builtins は `js_call_math_fast`（0.27 KiB）の分だけ
  増えた（5.78 → 6.07 KiB）。3 つの寄与を個別には測っていない（合流後だけ）。

### 2.3 IRAM と ROM（キャッシュを通らない）

`-dfilter` を IRAM・ROM まで広げて記録した（§5）。命令の取り込み回数（模型の推定、B の 1 ターン）:

- アロケータ: `tlsf_malloc` 48.6k、`tlsf_free` 27.4k、`heap_caps_*`・`multi_heap_*` を合わせて約 161k。
  **ESP-IDF の既定（`CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH` なし）では IRAM にあり、行の予算を使わない。**
- ROM: `memcpy` 1.3k、`__udivdi3` 1.1k、double の soft-float（`__muldf3`・`__floatsidf`・`__gtdf2` ほか）合わせて約 1.8k。
  ほかに ld に名前の無い ROM 領域（0x40054000〜0x40057fff ほか）が約 11 万あり、正体は未確認。
- 割り込み・スケジューラ（`xtensa_vectors`、`vTaskSwitchContext`、`SysTickIsrHandler` など）が約 60 万。QEMU の時計は
  ホストの実時間で進み、記録で遅くなった分だけ tick が増えるので、**この数は人工物**。

## 3. 上位 25 関数（B、触る KiB の順）

| # | 関数 | 部品 | KiB/ターン | 関数の大きさ B | warm / cold ミス（8-way） |
| --- | --- | --- | --- | --- | --- |
| 1 | `JS_CallInternal` | interp-core | 7.40 | 20,915 | 158 / 305 |
| 2 | `JS_SetPropertyInternal2` | interp-helpers | 0.81 | 1,162 | 21 / 34 |
| 3 | `core_change`（ksn_core.c） | native boundary | 0.75 | 1,188 | 20 / 28 |
| 4 | `js_array_every` | builtins | 0.66 | 951 | 21 / 21 |
| 5 | `run_build`（pocket_kasane.c） | native boundary | 0.63 | 867 | 11 / 22 |
| 6 | `apply_placement`（ksn_cache.c） | native boundary | 0.62 | 689 | 22 / 23 |
| 7 | `parse_placement`（pocket_kasane.c） | native boundary | 0.53 | 554 | 17 / 17 |
| 8 | `js_binary_arith_slow` | interp-helpers | 0.50 | 1,386 | 18 / 22 |
| 9 | `js_relational_slow` | interp-helpers | 0.50 | 731 | 9 / 16 |
| 10 | `JS_NewObjectFromShape` | object/alloc/GC | 0.47 | 578 | 3 / 15 |
| 11 | `JS_GetPropertyInternal` | interp-helpers | 0.46 | 665 | 4 / 16 |
| 12 | `js_add_slow` | interp-helpers | 0.44 | 578 | 6 / 14 |
| 13 | `js_call_c_function` | interp-helpers | 0.38 | 548 | 6 / 13 |
| 14 | `core_begin`（ksn_core.c） | native boundary | 0.38 | 524 | 12 / 12 |
| 15 | `js_binary_logic_slow` | interp-helpers | 0.38 | 1,022 | 9 / 12 |
| 16 | `__kernel_cos`（libm） | builtins | 0.38 | 372 | 10 / 12 |
| 17 | `__kernel_sin`（libm） | builtins | 0.38 | 358 | 8 / 12 |
| 18 | `js_array_push` | builtins | 0.37 | 405 | 10 / 16 |
| 19 | `__rem_pio2`（libm） | builtins | 0.35 | 1,020 | 20 / 22 |
| 20 | `__JS_NewAtom` | interp-helpers | 0.34 | 670 | 22 / 22 |
| 21 | `JS_ConcatString2` | object/alloc/GC | 0.34 | 419 | 10 / 16 |
| 22 | `js_typed_array_constructor` | builtins | 0.33 | 1,103 | 9 / 12 |
| 23 | `JS_CreateProperty` | interp-helpers | 0.32 | 504 | 4 / 12 |
| 24 | `add_property` | object/alloc/GC | 0.32 | 340 | 10 / 15 |
| 25 | `parse_rect_mode`（pocket_kasane.c） | native boundary | 0.31 | 295 | 8 / 13 |

`JS_CallInternal` の次は 1 関数 0.8 KiB 以下の長い裾で、触る関数は B で 394 個。予算に収めるには個々の関数ではなく、
helper の群（§2 読み方、§0）を段の設計で減らすことになる。

## 4. `JS_CallInternal` の中: 稀な部分と普通の部分

触った命令の番地を `addr2line -i` で `quickjs.c` の行に戻し（インライン展開は一番外の `quickjs.c` の行）、32 B 行を、
その中の命令が多数を占める領域に割り当てた。領域は `CASE(OP_x)` と関数内の C のラベル（`*_slow*`、`exception`、
`flat_call` など）で切る。熱は「1 ターンに何回その行に入ったか」（基本ブロックの実行回数）で、4 回未満を「稀」とした。

| 区分（B） | KiB/ターン | A |
| --- | --- | --- |
| 普通の命令の、よく通る行 | **4.04** | 4.94 |
| 入口・フレーム・呼び出し・戻り（`frame_pushed`、`flat_call`、`done` など）の、よく通る行 | **1.81** | 1.81 |
| 名前が遅い経路の領域（`get_field_slow_path` など）の、よく通る行 | 0.31 | 0.41 |
| 普通の命令の中の、稀な行（命令内の稀な分岐） | 0.35 | 0.34 |
| 稀な命令（その命令の一番熱い行でも 4 回/ターン未満） | 0.44 | 0.42 |
| 入口・呼び出しの稀な行 | 0.28 | 0.31 |
| 遅い経路・例外の稀な行 | 0.06 | 0.04 |
| 領域不明（行番号が取れない） | 0.11 | 0.03 |
| **計** | **7.40** | **8.31** |

- 稀なもの（熱で数えて）は B で **1.18 KiB**、名前で遅い経路のものを足しても約 1.5 KiB。残る約 6 KiB は 1 ターンに何度も
  通る行で、これが core の床になる。`get_field_slow_path`（204 回/ターン）や `get_field2_slow_path`（129 回）は名前に反して
  毎ターン多く通る。
- B の稀な命令は 18 種（`OP_fclosure8`、`OP_get_loc_check`、`OP_gosub`、`OP_ret`、`OP_sar`、`OP_shl`、`OP_shr`、
  `OP_set_name`、`OP_initial_yield` など）。どれも 1〜3 行。
- A → B で減ったのは「普通の命令の、よく通る行」（4.94 → 4.04 KiB）。命令ごとの dump 検査が消えて、各ハンドラが
  短くなった分（`QJS_DISABLE_DUMPS`）。
- 熱の閾値を動かすと（B、1 ターンの KiB）: 1 回以上 7.23、4 回以上 6.22、16 回以上 4.47、64 回以上 1.66。

## 5. 道具（再現と、直したところ）

```bash
# r5sim を組む（Cardputer のファームではない。実機に触らない）
idf.py -C tools/r5sim -B <dir> -DSDKCONFIG=<dir>/sdkconfig build
python -m esptool --chip esp32s3 merge-bin --fill-flash-size 8MB -o qemu_flash.bin @flash_args   # <dir> で
# WSL の QEMU。-serial file: で受ける（-serial stdio をファイルへ流すと何も出なかった）
qemu-system-xtensa -machine esp32s3 -m 4M -drive file=qemu_flash.bin,if=mtd,format=raw \
    -serial file:serial.txt -monitor none -display none -d exec,nochain,in_asm \
    -dfilter 0x40000000..0x403dffff,0x42000000..0x43ffffff -D r5.log
# serial.txt に "R5SIM done" が出たら QEMU を止める（約 3.6 GB、3 分）
python tools/r5sim/icache.py extract r5.log <dir>/r5sim.elf turns.pkl.gz --nm <nm>
python tools/r5sim/icache.py model turns.pkl.gz <dir>/r5sim.elf <dir>/r5sim.elf --nm <nm> [--full]
python tools/r5sim/turnset.py turns.pkl.gz <dir>/r5sim.elf <dir>/r5sim.map --nm <nm> --addr2line <addr2line> \
    --src components/quickjs-ng/quickjs-ng/quickjs.c --rom-ld $IDF_PATH/components/esp_rom/esp32s3/ld \
    --top 25 [--full] [--mapping out.tsv]
```

`turnset.py` の総数（行・warm・cold）は `icache.py model` を同じ ELF に写したときと一致する（確認済み）。

### 5.1 直したところ

1. **模型が行を水増ししていた（`icache.py`、重大）。** 基本ブロックの最後の命令の「+2 バイト」（3 バイト命令の最後の
   バイト）を常に取り込みとして写していた。最後の命令が 2 バイトで関数の終わりにあると、+2 は関数の外になり、
   「未知のコード」として別の行番号の空間に置かれていた。存在しない行が 1 ターン約 93 本増え、ミスも増える。
   A の同じ記録で、修正前 1,456 行・warm 2,008・cold 2,524（8-way、自己配置）→ 修正後 **1,361 行・1,326・1,865**。
   R5 の「1,457 行（45.5 KiB）」は修正前の値と一致するので、**R5 の §2・§3 の数字（ミス 1,411 / 1,932 など）も同じ
   水増しを含む。** 打ち手の相対比較（並べ替えが効かない、IRAM が効く）は同じ人工物を両側に含むので結論は変わら
   ないと思われるが、絶対値と §1.1 の「ミス 1 回 450 サイクル」は引き直しが要る（未実施。ファームの ELF が要る）。
   修正: +2 が関数の外に出るときは +1 を使う。
2. 同名の static 関数（`get_u16`、`JS_NewString`×3、`utf8_decode`×3 など 21 名）を名前だけで写していた。
   k 番目の同名を `name#k` にして区別した（自己配置ではほぼ影響なし。ファームへ写すときに効く）。
3. `-dfilter` を IRAM・ROM に広げたとき、それらを行として数えないようにした（キャッシュを通らない）。
4. r5sim が組めなくなっていた: `pocket_kasane.c` が proc/grid/video/pixel の面を呼ぶようになったのに、r5sim は
   それらを持たない。STRESS は使わないので `tools/test_keytest_app.c` と同じく stub した（実機では毎ターン数命令の
   フラグ検査）。また、新しいビルドディレクトリでは対象が esp32 になったので `CONFIG_IDF_TARGET="esp32s3"` を
   `tools/r5sim/sdkconfig.defaults` に書いた。

### 5.2 注意

- r5sim の QuickJS は `JS_NewRuntime` の既定の確保（`malloc` → tlsf）を使う。実機の `app_session.c` の確保の
  経路とは違いうる（R5 からの前提）。
- 配置は r5sim 自身。8-way のミスの絶対値はファームと違う（§1）。
- 部品の割り当ては名前と目的ファイルの規則（`turnset.py` の `RULES`、§6）。境界の判断（atom を helper に、文字列の
  確保を alloc に、libm を builtins に）で、部品間で数百 B は動く。

## 6. 部品の割り当て（`tools/r5sim/turnset.py` の `RULES`、上から最初に当たったもの）

| 部品 | 規則 |
| --- | --- |
| interp-core | `quickjs.c.obj` の `JS_CallInternal` |
| object/alloc/GC | `tlsf_*`・`heap_caps_*`・`multi_heap_*`・`malloc`/`free` 類。`quickjs.c.obj` の `js_malloc*`/`js_calloc*`/`js_free*`/`js_realloc*`・`JS_NewObject*`・`JS_NewArray*`・形（`js_new_shape*`、`js_clone_shape`、`resize_properties`、`add_property`、`add_shape_property`）・`free_*`・`*_finalizer`・`gc_*`/`mark_*`・`js_closure*`・`async_func_init`・文字列の生成と連結（`js_new_string*`、`js_alloc_string*`、`JS_NewString*`、`JS_ConcatString*`、`string_buffer_*`） |
| builtins | `quickjs.c.obj` の `js_math_*`・`js_call_math_fast`・`js_array_*`・`js_string_*`・`js_object_*`・`js_function_*`・`js_promise_*`・`js_typed_array*`・`js_array_buffer*`・`js_async_*`・`js_dataview*`・`JS_ExecutePendingJob`・`promise_*`・`perform_promise*` ほか組み込みの接頭辞。`libregexp`/`libunicode`。libm（`libc.a(libm_*)`） |
| interp-helpers | 上に当たらない `quickjs.c.obj` の全部（プロパティ、atom、遅延名、`js_*_slow`、変換、`js_call_c_function`、`JS_Call*`、`js_vm_stack_*`、`JS_FreeValue*`）と `dtoa.c`・`cutils.c` |
| native boundary | 目的ファイル名が `pocket_*`・`ksn_*`・`app_*`・`r5sim`・`jpfont`・`pet_*` のもの（r5sim.c は app_session.c の代わり） |
| libc/other | 残り（newlib、FreeRTOS、ドライバ） |
| IRAM/ROM | 番地で判定。行として数えない（§2.3） |

B で触った 394 関数の部品は helpers 156、native boundary 107、alloc 57、builtins 49、libc/other 24、core 1。
一覧は `--mapping` で TSV に出る。helpers の内訳（B、KiB/ターン）: プロパティ 3.99、atom・遅延名 2.23、算術の遅い経路
2.08、変換 1.55、呼び出し・フレーム・VM スタック 1.49、`JS_FreeValue*` 0.22、その他 2.38（`JS_IsFunction`、
`JS_GetClassProto`、`JS_EnqueueJob`、`js_poll_safepoint` などの 1〜3 行ずつ）。
