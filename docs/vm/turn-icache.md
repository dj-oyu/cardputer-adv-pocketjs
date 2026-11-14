# JS のターンと命令キャッシュ — R5 の模型、部品ごとの作業集合、行の使用率

[VM の文書](README.md)

吸収した旧文書: r5-icache.md（§1）、turn-working-set.md（§2）、turn-line-use.md（§3）。節番号は旧文書の節に合わせ、「旧 r5-icache.md §3.4」は本文書の §1.3.4、「旧 turn-working-set.md §4」は §2.4、「旧 turn-line-use.md §4.1」は §3.4.1 を指す（コードの注記・他文書はこの読み替えで引く）。

問い: STRESS の JS のターンはサイクルの 70%（QIO にした後も 57%）が flash キャッシュのミス待ち（[turn-cpi.md](turn-cpi.md)、実測）。ミスの**回数**を減らす手はあるか。QEMU で命令列を取りキャッシュの模型（`tools/r5sim/`）に通して調べた。**数字は断りの無いかぎり模型の推定で、実機の計数ではない。実測のものには「実測」と書く。**

## 0. 結論（三つの調査を通して）

- 1 ターンが触る命令ラインは **42.5 KiB**（1,361 行）で 32 KiB のキャッシュより大きい（R5 の最初の報告 45.5 KiB は模型の水増しで、修正後の値、§2.5.1）。衝突は 15% 前後で、配置を変えて取れる上限。
- **flash 内の並べ替えは効かない**（実リンクで cold −0.6%、§1.3.1）。**IRAM に移すのは効くが DRAM を 1:1 で払う**（8 KiB で実機 JS −15%・空き −8.7 KiB、実測、見送り、§1.3.4・§1.4）。
- インタプリタ（core＋helper）は 21.3 KiB で、段つきインタプリタの予算 14 KiB を超え、**主因は `JS_CallInternal` ではなく周りの helper**（§2.0）。
- 取り込んだ行の使用率は **57.5%**。走らないバイトのほとんどは、どのターンでも走らない冷たいコード。打ち手は並べ替えでなく**関数内の hot/cold 分割**で、走らないバイトを抜いて詰めた上限では 27.7 KiB に収まり warm ミス 940 → 73（§3.0、§3.3）。プロファイルでの試行は [qjs-pgo.md](qjs-pgo.md)、IRAM の置き方は [p1-iram-placement.md](p1-iram-placement.md)。
- **訂正と続き（2026-11-13）**: `icache.py extract` は `-icount` の記録で熱いブロックの後半を落とすことがあり、§3 の「行の使用率」と実行バイトは低く出ていた（今の STRESS の同じ記録で 59.8% → 66.2%。触る行とミスはほぼ同じ。当時の記録は取り直していない）。関数内の hot/cold 分割は、アセンブリの上でブロックを並べ替える形で試作した（[qjs-hotcold.md](qjs-hotcold.md)、模型でインタプリタの取り分 17.9 → 13.2 KiB、実機は未測定）。
- **ハードウェアの先読み（自動・手動）と固定は効かない、または得が小さい**（自動は実機で同じバイナリの A/B、手動と固定は模型、§4）。関数を手で分ける上限も上位 20 関数で温まったミス −19%（推定、§4.0）。

## 1. R5: 命令キャッシュのミスを実機なしで数える（旧 r5-icache.md、2026-09-27、`vm/r5-icache`）

COM3 を使えない間に進めたので、実機の代わりに QEMU で命令列を取り、キャッシュの模型に通した。

### 1.1 道具（`tools/r5sim/`）

- **`r5sim` イメージ**: STRESS を画面なしで回す ESP-IDF のプロジェクト。quickjs コンポーネントは実機と共有、Kasane・pocket の側は `tools/test_stress_app.c` と同じソースを実機のコンパイラ設定（`-Os` ほか）で組む。frame() は `app_tick` と同じく `JS_Call` → ジョブの消化 → `pocket_kasane_end_turn`。ターンの前後で `r5_turn_begin` / `r5_turn_end` を呼び、ログ上の番地で区切る。
- **QEMU**: Espressif 版（`esp_develop_9.2.2_20250817`、`-machine esp32s3`）。プラグインは無効のビルドなので、`-d exec,nochain,in_asm -dfilter 0x42000000..0x43ffffff` で flash 上の基本ブロックの実行順と中身を記録する（120 フレームで約 1 GB）。
- **`icache.py`**: `extract` がログから対象ターンの基本ブロック列だけを抜き、`model` がそれを任意のファームの ELF の番地に写して 8-way・32 KiB・32 B ラインの LRU に流す。関数の目的コードは両イメージで同一で、リンク後のサイズがずれる（最大 1.3%）のはリンカの呼び出し緩和なので、関数内のオフセットは比例で写す。ファームで IRAM にある関数はキャッシュを通らないものとして扱う。
- **`gen_hot_lf.py`**: 関数の一覧と map から ESP-IDF のリンカ断片を作る（flash 内の並べ替え、または `--noflash` で IRAM）。

**模型が見ないもの**: 描画。QEMU は PIE を実装していない（最初の `ee.vldbc.16` で IllegalInstruction）ので、イメージは描画を 16×8 のスカラーで回して提出を消費するだけにした。実機はターンの間に描画するので、真の値は「ターンの間でキャッシュが残る（warm）」と「毎ターン空から（cold）」の間にある。置換方式は LRU と仮定した（TRM に記載が無い）。

#### 1.1.1 実機の測定との照合（推定）

R4 の同一配置の測定では、DIO→QIO で `I_STALL_BUSY` が 1 ターン 1.61M → 0.88M サイクルになった。ミス回数が同じなら 1 回の値段の比は 1.83 で、SPI のクロック比と整合する。模型の cold 1,932 回を当てはめると、QIO のミス 1 回は約 450 サイクル（SPI の読み出し約 250 サイクル＋キャッシュ制御）で、ありえる範囲。**ミス回数を実機で直接測ってはいない**ので、ミリ秒は「1 回 450 サイクル」を仮定した推定。

### 1.2 結果（STRESS LV1、ターン 61〜120、`build_cpi` の配置）

**訂正（2026-10-23）**: この節と §1.3 の数字は、`icache.py model` が基本ブロック末尾の「+2 バイト」を関数の外の存在しない行として数えていたバグを含む（§2.5.1）。直した模型では 1,361 行（42.5 KiB）で、ミスも過大だった。表は元の値のまま残す。打ち手の相対比較は同じ人工物を両側に含むので結論は変わらないと思われるが、絶対値と §1.1.1 の「ミス 1 回 450 サイクル」は引き直しが要る（未実施）。

| | warm | cold |
| --- | --- | --- |
| ミス / ターン | 1,411 | 1,932 |
| 同じく、完全連想 32 KiB（衝突を除いた場合） | 1,201 | 1,648 |

- 1 ターンに触る命令ラインは 1,457 本 = 45.5 KiB（修正前の値）。cold のミスの 1,457 回は初回の読み込みで、配置では消えない。
- 衝突（8-way と完全連想の差）は **15% 前後**。配置を変えて取れるのはこれが上限。
- ミスは `JS_CallInternal` が 17.6%（25 KB のうち 9.6 KiB を触る）、残りは 1 関数 2% 未満の長い裾。

### 1.3 打ち手の比較

#### 1.3.1 ホットな関数を flash 内で連続に置く — 効かない

模型の上で関数を抜き出して連続に置くと cold −12% と出たが、**実際にリンカ断片で並べ替えたファーム**（ホットな 8 KiB、95 関数）を模型に通すと 1,932 → 1,921（−0.6%）。残りのコードも一緒に動くので衝突の当たり外れが引き直され、並べ替えた分の利得はその揺れに埋もれる。断片は採らなかった。ESP-IDF の挙動の補足: `(default)` だけの割り当ては親と同じとして ldgen が捨てる。フラグ（`text->flash_text ALIGN(4, pre)`）を付けると個別の規則になり、`.flash.text` の汎用の規則の後ろに置かれる。ただしオブジェクトに既存の個別規則があると、その隣に置かれる。

#### 1.3.2 IRAM に移す — 効くが DRAM を 1:1 で払う

模型が選んだ「バイトあたりのフェッチが多い順」の関数を `noflash` で IRAM に置いて**実際にビルド**し、その ELF を模型に通した:

| | 静的 DIRAM（`memlog`） | cold | warm | 1 ターンの短縮（推定） |
| --- | --- | --- | --- | --- |
| 現状 | 159,052 B | 1,932 | 1,411 | — |
| IRAM 4 KiB（68 関数、`candidates/r5_iram4.lf`） | **+4,240 B** | 1,659（−14%） | 1,086（−23%） | 約 0.5 ms |
| IRAM 8 KiB（95 関数、`candidates/r5_iram8.lf`） | **+8,705 B** | 1,436（−26%） | 884（−37%） | 約 0.9〜1.0 ms |

移した関数は小さなもの（`JS_FreeValueRT`、`js_malloc_rt`、`JS_NewObjectFromShape`、`add_property`、`parse_i16` など）で、`JS_CallInternal`（25 KB）は含まない。

#### 1.3.3 その他の候補（未評価）

flash 120 MHz（ESP-IDF では実験的機能。ミスの回数ではなく値段を下げる）。描画側のコードを IRAM に置いてターンのコードをキャッシュに残す（描画の flash の作業集合は QEMU で測れないので実機の計装が要る）。

#### 1.3.4 実機の確認（2026-09-27、実測）

8 KiB 版を計装付き（`CONFIG_POCKET_VM_TURNPERF`、QIO）で組み、基準 → 8 KiB 版 → 基準の順に STRESS を各段 60 秒。書き込みは esptool でビルド済みのイメージを直接書いた（`idf.py flash` は書く前に組み直すので、断片を登録したままのツリーでは基準まで IRAM 入りになる。最初の 3 回はそれで全部 8 KiB 版を測っていて、捨てた）。

| STRESS | 基準 1 | 8 KiB 版 | 基準 2 |
| --- | --- | --- | --- |
| LV1 の 1 フレームの JS | 7.00 ms | **5.93 ms（−15%）** | 7.02 ms |
| LV2 | 6.99 ms | 5.92 ms | 6.99 ms |
| LV3 | 7.09 ms | 6.27 ms | 7.75 ms |
| LV1 の `I_STALL_BUSY` / ターン | 819K | **588K** | 832K |
| 描画（LV1） | 6.15 ms | 6.16 ms | 6.15 ms |

命令数は変わらず、減ったのはフェッチ待ちだけ。減った約 23 万サイクルは模型の予測（ミス 496 回減 × 約 450 サイクル）とよく合った。計装なしの 8 KiB 版の空き（`memlog --port --check`）: ホーム 234,364 → 225,612 B、アプリ実行中 183,648 → 174,896 B（どちらも −8,752 B）、最大連続ブロック 139,264 → 131,072 B。smoke 20 周・STRESS PASS。同じバイナリでも基準 1 → 基準 2 で LV3 が 7.09 → 7.75 ms（+9%）と揺れた。

### 1.4 判断

- **並べ替えは採らない**（§1.3.1）。
- **IRAM 8 KiB は見送り**（2026-09-27、ユーザーの判断）: 1 ターン −1.07 ms（−15%）に対して空き −8.7 KiB。STRESS はすでに 30 fps に届いていて、払う DRAM に見合う戻りが軽い。候補の断片は `tools/r5sim/candidates/` に残す。再開するのは 30 fps に届かないアプリが出たとき（そのアプリで模型を回し直し、候補を選び直す）。その後の helper 群の IRAM 化の決定は [p1-iram-placement.md](p1-iram-placement.md)。
- 断片ファイルを差し替えただけでは ldgen が読み直さない。`idf.py reconfigure` を挟む。

### 1.5 再現

手順（QEMU の記録、`icache.py`、`gen_hot_lf.py`）は §2.5 にまとめた。QEMU は `R5SIM done` を出した後も止まらないので、見張って終了させる。ログを Python で丸ごと読む処理は、最初の版が基本ブロックの両端を別々に写して 2 GiB の範囲を作るバグで 2 回メモリ不足で止められた（今は命令ごとに写す）。

## 2. 1 ターンの作業集合を部品ごとに分ける（旧 turn-working-set.md、2026-10-23）

段つきのインタプリタ（V8 のように段を分け、機械語は生成しない、[interpreter-stages.md](interpreter-stages.md)）の予算の下調べ。設計の規則は「どの段でも、1 ターンが触るコードが予算に収まる」で、予算の案は**インタプリタの取り分が約 14 KiB、native との境界（Kasane の提出・`pocket.*`・アロケータ）が約 12 KiB**。部品ごとの集計に `tools/r5sim/turnset.py` を足した。数字はすべて模型の推定か、ELF・コードから読んだ値。

### 2.0 結論

- **B（3 つの試作を入れた木）でも、インタプリタの取り分は 14 KiB に収まらない。** `JS_CallInternal` が 7.4 KiB、そこから呼ばれる小さな関数（プロパティ・atom・数値の遅い経路・変換・呼び出し）が 13.9 KiB で、合わせて **21.3 KiB（予算の約 1.5 倍、7.3 KiB 超過）**。
- 超過の主因は `JS_CallInternal` ではなく**周りの helper**。内訳はプロパティの読み書き・定義 4.0 KiB、atom と遅延名 2.2 KiB、浮動小数の算術（`js_*_slow`）2.1 KiB、変換 1.6 KiB、呼び出し・フレーム 1.5 KiB。STRESS の算術は double なので、QuickJS の「遅い経路」がここでは普通の経路になっている。
- `JS_CallInternal` の 7.4 KiB のうち、稀な命令・遅い経路・例外・まれにしか通らない行は約 1.2〜1.5 KiB で、残りの約 6 KiB は毎ターン何度も通る行（§2.4）。稀な部分を全部追い出しても core は 6 KiB を切らない。
- 試作 3 つの効果は 1 ターン 42.5 → 41.8 KiB（−0.7 KiB）。ほぼ `QJS_DISABLE_DUMPS` による `JS_CallInternal` の縮小（関数が 25,170 → 20,915 B、ELF から読んだ値。触る行は 8.31 → 7.40 KiB）。

### 2.1 条件

| | A（基準） | B（合流後） |
| --- | --- | --- |
| 木 | `main` の `e0fe98a` | `e0fe98a` ＋ `53af3d2`（`QJS_DISABLE_DUMPS`）＋ `40b300d`（TDZ 検査の省略）＋ `e678a1f`（`js_call_math_fast`）を `--no-ff` で合流 |
| `JS_CallInternal` の大きさ（ELF） | 25,170 B | 20,915 B |

- STRESS LV1、ターン 61〜120 の 60 ターン（§1 と同じ）。QEMU の終了行はどちらも `frames=120 oom=0 fail=0 bad_present=0`。
- **配置**: 模型は各 case の `r5sim.elf` 自身の番地に写した（ファームの ELF は使っていない）。**触る行数（KiB）は配置にほとんど依存しない**が、8-way のミス回数は配置で大きく動く（A では 8-way の warm 1,326 に対して完全連想 1,044）。A と B の差は完全連想の列で読む。
- 区切りは `r5_turn_begin`/`r5_turn_end`。ターンは `JS_Call` ＋ ジョブの消化 ＋ `pocket_kasane_end_turn`。描画（`present()`）はターンの外。

### 2.2 部品ごとの作業集合

「KiB」は 1 ターンに触る 32 B 行の数（60 ターンの平均）、「全ターン」はどれかのターンで触った行の和集合、「稀」は 1 ターンに 4 回未満しか通らない行。すべて模型の推定。

#### 2.2.1 A（基準 `e0fe98a`）

| 部品 | KiB/ターン | 稀 KiB | 全ターン KiB | warm ミス（8-way / 完全連想） | cold ミス（8-way / 完全連想） |
| --- | --- | --- | --- | --- | --- |
| interp-core | 8.31 | 1.12 | 9.47 | 197 / 153 | 370 / 312 |
| interp-helpers | 14.20 | 2.09 | 18.00 | 451 / 343 | 632 / 510 |
| object/alloc/GC | 5.68 | 0.68 | 6.62 | 204 / 110 | 287 / 204 |
| builtins | 5.78 | 2.46 | 9.91 | 199 / 198 | 232 / 206 |
| native boundary | 8.37 | 1.87 | 14.06 | 272 / 238 | 338 / 273 |
| libc/other | 0.20 | 0.04 | 2.88 | 2 / 2 | 6 / 6 |
| **計** | **42.5**（1,361 行） | | 60.9 | **1,326 / 1,044** | **1,865 / 1,512** |

#### 2.2.2 B（合流後）

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

- 予算と並べると（B）: インタプリタ（core＋helpers）**21.3 KiB 対 14 KiB**。native の側は Kasane・`pocket.*`・r5sim の糊が 8.5 KiB で、アロケータまで含めるなら object/alloc/GC の 5.65 KiB を足して **14.2 KiB 対 12 KiB**。builtins の 6.1 KiB（libm を含む）はどちらの予算にも入っていない。合計 41.8 KiB 対 26 KiB。
- cold のミスが触る行より多い（B で 1,467 > 1,337、完全連想でも）。1 ターンの中でもすでに容量が足りず、同じ行を 2 回取り込んでいる。
- 全ターンの和集合（約 60 KiB）は 1 ターン（約 42 KiB）よりずっと大きい。STRESS は段階でコードが入れ替わるので、予算は「1 ターン」で切るのが正しく、アプリ全体の和で切ると過大になる。
- 試作の効果（完全連想の warm）: core 153 → 129、helpers 343 → 327。builtins は `js_call_math_fast`（0.27 KiB）の分だけ増えた。3 つの寄与を個別には測っていない。

#### 2.2.3 IRAM と ROM（キャッシュを通らない）

アロケータ: `tlsf_malloc` 48.6k、`tlsf_free` 27.4k、`heap_caps_*`・`multi_heap_*` を合わせて約 161k の命令取り込み（B の 1 ターン）。**既定（`CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH` なし）では IRAM にあり、行の予算を使わない。** ROM は `memcpy` 1.3k、`__udivdi3` 1.1k、double の soft-float 約 1.8k。割り込み・スケジューラの約 60 万は、QEMU の時計がホストの実時間で進み記録で遅くなった分だけ tick が増える**人工物**。

### 2.3 上位の関数（B、触る KiB の順、上位 12）

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

`JS_CallInternal` の次は 1 関数 0.8 KiB 以下の長い裾で、触る関数は B で 394 個。予算に収めるには個々の関数ではなく、helper の群を段の設計で減らすことになる。

### 2.4 `JS_CallInternal` の中: 稀な部分と普通の部分

触った命令の番地を `addr2line -i` で `quickjs.c` の行に戻し、32 B 行を、その中の命令が多数を占める領域（`CASE(OP_x)` と関数内の C のラベル）に割り当てた。熱は「1 ターンに何回その行に入ったか」で、4 回未満を「稀」とした。

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

- 稀なもの（熱で数えて）は B で **1.18 KiB**、名前で遅い経路のものを足しても約 1.5 KiB。残る約 6 KiB は 1 ターンに何度も通る行で、これが core の床になる。`get_field_slow_path`（204 回/ターン）や `get_field2_slow_path`（129 回）は名前に反して毎ターン多く通る。
- B の稀な命令は 18 種（`OP_fclosure8`、`OP_get_loc_check`、`OP_gosub`、`OP_ret`、`OP_sar`、`OP_shl`、`OP_shr`、`OP_set_name`、`OP_initial_yield` など）。どれも 1〜3 行。
- A → B で減ったのは「普通の命令の、よく通る行」（4.94 → 4.04 KiB）。命令ごとの dump 検査が消えて各ハンドラが短くなった分。
- 熱の閾値を動かすと（B、1 ターンの KiB）: 1 回以上 7.23、4 回以上 6.22、16 回以上 4.47、64 回以上 1.66。

### 2.5 道具（再現と、直したところ）

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
python tools/r5sim/icache.py model turns.pkl.gz <dir>/r5sim.elf <firmware.elf> --nm <nm> \
    [--full] [--iram-kib N --emit list.txt] [--top 30]
python tools/r5sim/gen_hot_lf.py [--noflash] list.txt <firmware.map> > fragment.lf
python tools/r5sim/turnset.py turns.pkl.gz <dir>/r5sim.elf <dir>/r5sim.map --nm <nm> --addr2line <addr2line> \
    --src components/quickjs-ng/quickjs-ng/quickjs.c --rom-ld $IDF_PATH/components/esp_rom/esp32s3/ld \
    --top 25 [--full] [--mapping out.tsv]
# 触った行のうち、ターンが実際に実行したバイト（行の使用率）。命令長は ELF の逆アセンブルから取る
python tools/r5sim/lineuse.py turns.pkl.gz <dir>/r5sim.elf <dir>/r5sim.map --nm <nm> --objdump <objdump> \
    --top 25 [--tsv out.tsv] [--packed-model]
```

クラウドのコンテナ（Linux の QEMU）で記録が先へ進まないときは `-icount shift=2` を足す（§3.4.1）。`turnset.py` の総数（行・warm・cold）は `icache.py model` を同じ ELF に写したときと一致する（確認済み）。

#### 2.5.1 直したところ

1. **模型が行を水増ししていた（`icache.py`、重大）。** 基本ブロックの最後の命令の「+2 バイト」（3 バイト命令の最後のバイト）を常に取り込みとして写していた。最後の命令が 2 バイトで関数の終わりにあると、+2 は関数の外になり、「未知のコード」として存在しない行になる。1 ターン約 93 本増え、ミスも増える。A の同じ記録で、修正前 1,456 行・warm 2,008・cold 2,524（8-way、自己配置）→ 修正後 **1,361 行・1,326・1,865**。R5 の「1,457 行（45.5 KiB）」は修正前の値と一致する。修正: +2 が関数の外に出るときは +1 を使う。
2. 同名の static 関数（`get_u16`、`JS_NewString`×3、`utf8_decode`×3 など 21 名）を名前だけで写していた。k 番目の同名を `name#k` にして区別した（自己配置ではほぼ影響なし。ファームへ写すときに効く）。
3. `-dfilter` を IRAM・ROM に広げたとき、それらを行として数えないようにした。
4. r5sim が組めなくなっていた: `pocket_kasane.c` が proc/grid/video/pixel の面を呼ぶようになったのに r5sim はそれらを持たない。STRESS は使わないので stub した。新しいビルドディレクトリでは対象が esp32 になったので `CONFIG_IDF_TARGET="esp32s3"` を `tools/r5sim/sdkconfig.defaults` に書いた。

#### 2.5.2 注意

r5sim の QuickJS は `JS_NewRuntime` の既定の確保（`malloc` → tlsf）を使う。実機の `app_session.c` の確保の経路とは違いうる。配置は r5sim 自身で、8-way のミスの絶対値はファームと違う。部品の割り当ては名前と目的ファイルの規則（§2.6）で、境界の判断（atom を helper に、文字列の確保を alloc に、libm を builtins に）で部品間で数百 B は動く。

### 2.6 部品の割り当て（`tools/r5sim/turnset.py` の `RULES`、上から最初に当たったもの）

| 部品 | 規則 |
| --- | --- |
| interp-core | `quickjs.c.obj` の `JS_CallInternal` |
| object/alloc/GC | `tlsf_*`・`heap_caps_*`・`multi_heap_*`・`malloc`/`free` 類。`quickjs.c.obj` の `js_malloc*`/`js_calloc*`/`js_free*`/`js_realloc*`・`JS_NewObject*`・`JS_NewArray*`・形（`js_new_shape*`、`add_property` など）・`free_*`・`*_finalizer`・`gc_*`/`mark_*`・`js_closure*`・文字列の生成と連結 |
| builtins | `quickjs.c.obj` の `js_math_*`・`js_call_math_fast`・`js_array_*`・`js_string_*`・`js_object_*`・`js_function_*`・`js_promise_*`・`js_typed_array*` ほか組み込みの接頭辞。`libregexp`/`libunicode`。libm |
| interp-helpers | 上に当たらない `quickjs.c.obj` の全部（プロパティ、atom、遅延名、`js_*_slow`、変換、`js_call_c_function`、`JS_Call*`、`js_vm_stack_*`、`JS_FreeValue*`）と `dtoa.c`・`cutils.c` |
| native boundary | 目的ファイル名が `pocket_*`・`ksn_*`・`app_*`・`r5sim`・`jpfont`・`pet_*` のもの |
| libc/other | 残り（newlib、FreeRTOS、ドライバ） |
| IRAM/ROM | 番地で判定。行として数えない（§2.2.3） |

B で触った 394 関数の部品は helpers 156、native boundary 107、alloc 57、builtins 49、libc/other 24、core 1。helpers の内訳（B、KiB/ターン）: プロパティ 3.99、atom・遅延名 2.23、算術の遅い経路 2.08、変換 1.55、呼び出し・フレーム・VM スタック 1.49、`JS_FreeValue*` 0.22、その他 2.38。

## 3. JS のターンの行の使用率（旧 turn-line-use.md、2026-11-05）

問い: 「flash キャッシュ待ちがターンの 34〜43%（[turn-jumps-pie.md](turn-jumps-pie.md)、実測）なのは、命令が行に十分集まっていないせいか」。1 ターンが取り込む 32 B 行のうち、そのターンが実際に実行するバイトの割合を測った。数字はすべて QEMU の記録を模型に通した推定。

### 3.0 結論

- **使用率は 57.5%**（ターンごとに 55.0〜59.6%）。1 ターンは 1,304 行（40.8 KiB）を取り込み、実行するのは 23.4 KiB。取り込んだ 17.3 KiB 分はそのターンで走らないバイト。
- **走らないバイトのほとんど（15.1 KB / 17.8 KB）は、窓の 60 ターンのどれでも走らない。** 別のターンが使うコードは 2.7 KB だけ。並べ替え（§1 で効かなかった）ではなく、**冷たいコードを熱いコードの行から追い出すこと**（関数内の hot/cold 分割）が効く形をしている。
- **熱い行ほど詰まっているわけではない。** 1 ターンに 64 回以上通る行でも 56%。冷たい分岐が熱い経路の間に挟まっている（インライン展開と、関数内に置かれたエラー処理）。
- 上限の模型（走らないバイトを抜いて詰める、§3.3）では、1 ターンが **886 行（27.7 KiB）に縮んで 32 KiB に収まり、前のターンからの持ち越し（warm）のミスは 940 → 73 回/ターン**。上限であって、コンパイラが出せる配置ではない。
- 部品では **interp-helpers が最悪（48.8%、6.55 KiB の無駄）**。`JS_SetPropertyInternal2` 41%、`JS_GetPropertyInternal` 42%、`JS_DefineProperty` 43% など、場合分けのうち少しだけを使う大きな汎用関数。`JS_CallInternal` は 65.8%（2.68 KiB の無駄）。

問いへの答えは「**はい、大きな原因の一つ**」。ただし効く打ち手は並べ替えではなく関数内の分割で、Xtensa の GCC はプロファイル無しでは関数内の hot/cold 分割をしない（[p1-iram-placement.md](p1-iram-placement.md)、実測）。手で冷たい経路を `noinline`/`cold` の関数へ出すか、プロファイル（`-fprofile-use` ＋ `-freorder-blocks-and-partition`）が要る。後者の試行は [qjs-pgo.md](qjs-pgo.md)。

### 3.1 条件

| | |
| --- | --- |
| 木 | `cbb039e`（r5sim、STRESS LV1、ターン 61〜120 の 60 ターン） |
| IDF / QEMU | ESP-IDF v6.0.1、QEMU `esp_develop_9.2.2_20250817`（Linux、クラウドのコンテナ） |
| QEMU の引数 | `-icount shift=2 -d exec,nochain,in_asm -dfilter 0x42000000..0x43ffffff`（§3.4.1） |
| 終了行 | `R5SIM done frames=120 oom=0 fail=0 bad_present=0` |
| 配置 | r5sim 自身の番地。8-way 32 KiB LRU |
| 照合 | `turnset.py` で 1,305 行/ターン・warm 942・cold 1,496。§2.2.2 の B（1,337 行）と近い |

記録は [logs/turn-line-use-20261105.txt](logs/turn-line-use-20261105.txt)。この後 r5sim は QuickJS を `CONFIG_POCKET_VM_ROM_INTRINSICS` 付きで組むようになった（[qjs-pgo.md](qjs-pgo.md) §1.1）。この節の数字はその前の r5sim（変更後は 1 ターン 1,281 行、使用率 58.1%）。

### 3.2 結果

全体（1 ターンあたり）: 触る行 1,304 行（40.8 KiB）、実行したバイト 23,982 B（23.4 KiB）、使用率 **57.5%**、走らないバイト 17,753 B（うち別のターンで走る 2,670 B、どのターンでも走らない 15,083 B）。

部品ごと:

| 部品 | 触る KiB | 実行 KiB | 使用率 | 無駄 KiB |
| --- | --- | --- | --- | --- |
| interp-core（`JS_CallInternal`） | 7.82 | 5.15 | 65.8% | 2.68 |
| interp-helpers | 12.80 | 6.25 | **48.8%** | **6.55** |
| object/alloc/GC | 5.52 | 3.03 | 54.9% | 2.49 |
| builtins | 5.99 | 3.89 | 64.9% | 2.10 |
| native boundary | 8.39 | 5.02 | 59.9% | 3.37 |
| libc/other | 0.23 | 0.08 | 34.4% | 0.15 |

熱さごと（1 ターンにその行へ届くブロック実行の回数）:

| 届く回数/ターン | 行/ターン | 実行 KiB | 使用率 |
| --- | --- | --- | --- |
| 1 未満 | 38.5 | 0.62 | 51.7% |
| 1〜4 | 235.9 | 4.09 | 55.5% |
| 4〜16 | 360.6 | 6.87 | 61.0% |
| 16〜64 | 344.6 | 6.14 | 57.0% |
| 64 以上 | 324.6 | 5.69 | 56.1% |

熱さと使用率はほぼ無関係。全部（32 B）を使う行は 14%、半分以下（16 B 以下）の行が 44%、4 B 以下（命令 1〜2 個）が 10%。

### 3.3 詰めたらどうなるか（上限）

窓の 60 ターンのどれかで走るバイトだけを、順序を保って詰めた像（38.0 KiB）で同じ記録を流し直した。**上限**: 学習に使った窓そのものを測っている、ブロックの整列・分割で増える分岐・飛び越えるためのバイトを数えていない。

| 配置 | 構成 | 行/ターン | warm ミス | cold ミス |
| --- | --- | --- | --- | --- |
| そのまま | 8-way | 1,304 | 940 | 1,493 |
| そのまま | 完全連想 | 1,304 | 942 | 1,423 |
| 詰めた | 8-way | 886 | 73 | 891 |
| 詰めた | 完全連想 | 886 | 21 | 886 |

- 今の配置では 8-way と完全連想の warm がほぼ同じ（940 対 942）。**ミスは衝突ではなく容量で起きている**（1 ターン 40.8 KiB は 32 KiB に入らない）。
- 詰めると 1 ターンは 27.7 KiB になりキャッシュに収まる。warm のミスはほぼ消える。cold（ターンの始めに空）は触る行の数と同じになる。
- 実機は描画がターンの間にキャッシュを使うので、真の値は warm と cold の間（§1.1）。詰めても cold 側の 891 回は残る。

### 3.4 道具

`tools/r5sim/lineuse.py`（手順は §2.5）。`icache.py extract` の pickle と r5sim の ELF・map を読む。命令長は ELF の逆アセンブルから取る。行は、その行で実行したバイトを一番多く持つ関数に付ける。`--packed-model` で §3.3 を出す。逆アセンブルに無い 27 か所の取り込みは長さを推定した（全体への影響は数十 B）。手で作った 2 ターンの記録で、行数・実行バイト・持ち越しの内訳が手計算と一致することを確かめた。

#### 3.4.1 クラウドのコンテナでの記録（`-icount`）

§2.5 の引数（`-dfilter` に IRAM も含め、`-icount` なし）では、このコンテナの QEMU は起動後に先へ進まなかった（11 分、7.5 GB で flash のコードが 1 度も走らない）。QEMU の仮想時計がホストの時計に従うので、記録で遅くなるほど tick 割り込みが詰まり、IRAM の tick と idle の処理だけが回っていたと考えている（記録なしでは 4 秒で `R5SIM done`）。`-icount shift=2`（仮想時計を命令数で進める）にし、記録を flash に絞ると 1 分弱・837 MB で終わった。IRAM の取り込みの統計（`turnset.py` の non-flash）は、この記録からは出ない。

### 3.5 次の候補（未着手）

- （2026-11-13: 下の 1 つめは、手で外へ出す代わりにアセンブリの上の並べ替えとして [qjs-hotcold.md](qjs-hotcold.md) で試作した。）
- 関数内の冷たい経路を外へ出したときの模型: `JS_SetPropertyInternal2` などの helper の上位から、「走らない連続区間を関数の外に出す」配置を、分岐の追加分込みで見積もる。
- プロファイル（STRESS と DERBY）での `-freorder-blocks-and-partition` を r5sim で試し、詰めた上限にどこまで近づくかを測る（試行は [qjs-pgo.md](qjs-pgo.md)）。実機の A/B は同一バイナリにならないので、命令キャッシュのアラインメントの 15% の揺れ（CLAUDE.md）を超える差かどうかで判断する。

## 4. ハードウェアの先読み・固定と、行を詰める打ち手の上限（2026-11-09）

問い（持ち主）: flash の読み出し待ちと命令の実行を、時間をずらして重ねられないか。順番は 1) 自動の先読み、3) 1 ターンのコードを 32 KiB に収める、2) 次のターンの入口を手動で先読み。

### 4.0 結論

- **自動の先読み（ICache Auto-Preload、TRM v1.8 p.406）は効かない（実測、同じバイナリ）。** 起動時はオフ。flash のコード全体を区画にしてオンにすると、STRESS の 1 フレーム（JS＋描画＋転送）は 16.20 ms →「外れで次の 1 ブロック」16.23・「外れで 4 ブロック」16.88・「当たり外れで 1 ブロック」16.16 ms。決まった量を回す Shiki の実行時間は、どの設定でも遅くなった（+1.1〜+4.1%）。DERBY の同じ場面の窓（CLOSE、draw 20・1,053 ステップ）では JS のターン 7.55 → 8.45 ms（+12%）。先読みした行が、まだ使う行を追い出す。**既定はオフのまま。**
- **フレームをまたいで先に読む形は、効く余地が小さい。** 毎フレーム同じコードを走らせていて、それでも読み直すのは 1 ターン（約 42 KiB）がキャッシュ（32 KiB）より大きく、フレームの中で自分を追い出すから（§3.3）。
- **よく通る関数を集めて固定する（Lock）と、手動の先読み（2）は、模型では得が小さい（推定）。** 固定 16 KiB で冷えたミス 1,466 → 1,208 回/ターン、温まったミスは 1,016 → 1,043 回と悪化（関数ごと固定するので、走らないバイトも固定する）。毎ターン始めに 16 KiB を先読みすると冷えたミス −248 回（1 回 約 1.9 µs として約 0.47 ms）だが、16 KiB を読む時間（QIO 40 MB/s で約 0.4 ms）とほぼ相殺する。得が出るのは空き時間に読めるときだけで、空き時間がある 30 fps ではそもそも速さが要らず、60 fps では空き時間が無い。
- **関数を手で hot/cold に分ける（3）の上限は、上位 20 関数で温まったミス −19%（推定）。** 走らないバイトを抜いて後ろを詰めた模型（`lineuse.py --squeeze-top`）で、上位 20 関数 1,293 → 1,053、60 関数 865、200 関数 745 回/ターン（r5sim 自身の配置、IRAM の helper も flash に置いた像）。`JS_CallInternal` だけでは 1,239（−4%）。ミス待ちがターンの 34〜43%（[turn-jumps-pie.md](turn-jumps-pie.md)）なので、上位 20 関数を完全に分けても JS のターンは数 % で、配置を変えるのでビルド間の比較になり、命令キャッシュのアラインメントの揺れ（最大 15%）に埋もれる。上流のパッチも大きい。**手を付けていない。**（2026-11-13: ソースを変えない形で試作した。熱いブロックを関数をまたいで続けて置くので、この節の「関数の中だけで詰める」上限より大きく縮む。[qjs-hotcold.md](qjs-hotcold.md)）
- 命令キャッシュの行は 32 KiB の構成では 32 B が最大（16 B は 16 KiB のときだけ）。行を長くする手は無い。

### 4.1 自動の先読みの A/B（実測）

道具: `CONFIG_POCKET_ICACHE_AB`（既定 n、`main/hal/icache_ab.c`）。区画 0 を `_instruction_reserved_start`〜`_end`（flash のコード）にし、`KASANE_PAINT` の窓（30 フレーム）ごとにモードを off → miss0 → miss3 → both0 と回す。`miss`/`both` は先読みのきっかけ（外れ／当たりと外れ）、数字は ROM の `autoload_config.size`（`CACHE_AUTOLOAD_STEP`）。flash への書き込みは起動時の設定で先読みを戻すので、ターンの前に毎回モードを当て直す（DERBY で 1 回あった、`resets=1`）。測定用のコミット `6e2dea0`（sdkconfig で有効にしただけ、`eccfd15` で revert）。

STRESS LV1（`icab-stress-1`、窓 24〜73、各モード 12〜13 窓の中央値）:

| モード | turn ms | render ms | send ms | Shiki engine ms | 合計 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| off | 6.29 | 6.89 | 2.92 | 1.611 | 16.20 |
| miss0 | 6.55（+4.1%） | 6.90 | 2.89 | 1.628（+1.1%） | 16.23 |
| miss3 | 6.61（+5.0%） | 7.33（+6.3%） | 2.99 | 1.677（+4.1%） | 16.88 |
| both0 | 6.19（−1.7%） | 6.96（+1.0%） | 2.91 | 1.639（+1.8%） | 16.16 |

- engine ms は窓ごとに同じ仕事（18 draw、7,332 ステップ）で、ばらつき ±0.01 ms。モードの差がそのまま見える。
- both0 の turn −1.7% は窓の幅（5.96〜6.52 ms）の中で、描画と Shiki の遅れで消える。
- DERBY（`icab-derby-1`）はレースの場面が窓ごとに変わり、4 モードの巡回では比べにくい。同じ場面の隣り合う窓だけを比べた（CLOSE: off 7.55 ms・engine 0.362、miss0 8.45 ms・engine 0.390）。

### 4.2 模型の変種（推定）

`tools/r5sim/icache.py` に `--lock-kib`（上位の関数を詰めて固定。固定した行は常に当たり、その組の 1 way を使う）と `--preload-kib`（詰めた範囲を毎ターンの始めに読み込む）、`tools/r5sim/lineuse.py` に `--squeeze`/`--squeeze-top`（選んだ関数の中の走らないバイトだけを抜く）を足した。記録は `.cache/ic_shiki_noiram`（Shiki 版 STRESS、60 ターン）。配置はいまのファーム（`icache.py`）か r5sim 自身（`lineuse.py`）。

| 変種 | 温まったミス/ターン | 冷えたミス/ターン |
| --- | ---: | ---: |
| そのまま（ファームの配置） | 1,016 | 1,466 |
| 固定 8 / 16 KiB | 1,031 / 1,043 | 1,338 / 1,208 |
| 毎ターン先読み 8 / 16 / 24 KiB | 1,042 / 1,053 / 1,063 | 1,350 / 1,218 / 1,123 |
| （r5sim の配置）そのまま | 1,293 | 1,741 |
| 走らないバイトを抜く: `JS_CallInternal` だけ | 1,239 | 1,673 |
| 同: 上位 20 / 60 / 200 関数 | 1,053 / 865 / 745 | 1,523 / 1,400 / 1,302 |

実機がどちら寄りか（描画がターンの間にどれだけ追い出すか）は分けていない（§1.1）。
