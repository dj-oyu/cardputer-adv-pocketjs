# P1: 1ターンに触るコードを IRAM に置く（設計・素案 v2、2026-10-25）

`interpreter-stages.md` §9 の続き。P0（guard の要らない T0 の変更）で、DERBY の命令の数から見える取り分はおおむね取った（dispatch −4.9%、host）。残る大きな手は、1ターンに触るコードを flash の命令キャッシュから外すこと（P1）。**数字は実測（実機）/ 実測（host・静的）/ 模型 / 推定を書き分ける。今日の変更は実機ではまだ何も測っていない。**

v1（同日）を SLOW（Fable）がレビューし、予算の見積もりと、比べる相手の選び方に重大な誤りがあった。v2 はそれを直した版。指摘の要約は §8。

## 0. 結論（素案 v2）

1. **インタプリタ本体（`JS_CallInternal`）を IRAM に置く案は、今の時点では第一候補にしない。** 冷たい命令を出しても本体は 13〜16 KB 残る（推定、§2.3）。模型の上では、本体は 1 KiB あたりの効きが R5 の helper 群の半分程度（推定、§2.4）。DRAM は 1:1 で払うので、同じ予算なら helper 群が先。
2. 比べる相手を揃える: 「本体」「helper 群（R5 の候補を選び直したもの）」「両方」を、**同じ DRAM の予算で、検証済みの模型（`tools/r5sim/icache.py model`）に通して順位を付けてから**、実機へ行く。R5 §4 の再開手順（そのアプリで模型を回し直し、候補を選び直す）どおり。
3. DERBY の現状（実測、`docs/apps/derby-pan-memory.md`）: 横見 WIDE の JS ターン 14.5〜15.7 ms、首振り WIDE 21.6 ms（フレーム間隔 41.0 ms、25.3 fps）。27 fps には JS ターンを約 4 ms 縮める必要がある。P1 の論拠は「30 fps に届く」ではなく「何 ms 削れるか」で書く。R5 と同率（−15%）なら首振り WIDE で約 3.2 ms（推定）。
4. 先にできる安い手: upstream の 1 行呼び出しのハンドラ（`OP_for_of_next` 193 B、`OP_for_in_next` 205 B、`OP_in` 164 B、`OP_delete` 156 B など、静的）は -Os で helper が inline されて膨らんでいる。**helper に `no_inline` を付けるだけ**で、正しさの検査をほぼ要さずに本体が数 KB 縮む（推定）。flash のままでも、熱いハンドラの密度が上がる。
5. ジャンプテーブル（`dispatch_table`、1 KiB、flash の `.rodata`）を DRAM に置く手は、本体の配置とは独立に測れる（別 Kconfig）。

## 1. 前提（既存の記録）

| 事実 | 区分 | 出典 |
| --- | --- | --- |
| QIO の今、JS のターンのサイクルの 57% が flash キャッシュのミス待ち（IPC 0.25）。DIO のときは 70%・0.17 | 実測（実機） | `turn-cpi.md` §3 |
| **STRESS** の 1ターンが触るコード 41.8 KiB（core 7.40、helpers 13.93、object/alloc 5.65、builtins 6.07、native 8.51）。`JS_CallInternal` のミスは warm 158/1,071（15%）、cold 305/1,630（19%） | 模型 | `turn-working-set.md` §2.2・§3 |
| helper 8 KiB（95 関数）を IRAM へ: STRESS の JS 7.00 → 5.93 ms（−15%）、`I_STALL_BUSY` 819K → 588K、空き −8,752 B、最大連続ブロック 139,264 → 131,072 B。模型の予測（ミス 496 回減 × 約 450 サイクル）と合った | 実測（実機） | `r5-icache.md` §3.4 |
| 同じ 8 KiB は見送り（ユーザーの判断、2026-09-27）。再開の条件は 30 fps に届かないアプリで、模型を回し直して候補を選び直す | 記録 | `r5-icache.md` §4 |
| 同一バイナリでも揺れる: R5 の基準 1 → 基準 2 で LV3 が 7.09 → 7.75 ms（+9%）。アラインメントだけで同じカーネルが 15% 動く | 実測（実機） | `r5-icache.md` §3.4、`CLAUDE.md` |

注意: 41.8 KiB と core の床（約 6 KiB）は STRESS の値で、DERBY の host opprof と同じアプリの数字ではない。r5sim は今 STRESS しか回せない（DERBY は `pocket.derby` などの面が無い）。

## 2. 調べたこと

### 2.1 GCC に hot/cold 分割を任せられるか — できない（実測、コンパイラ）

computed goto の小さなインタプリタ（熱い op と、`__attribute__((cold))` のラベルの冷たい op）を、IDF v6.0.1 の `xtensa-esp-elf-gcc` 15.2.0（`-mdynconfig=xtensa_esp32s3.so -mlongcalls -ffunction-sections`）で `-O2` / `-O2 -freorder-blocks-and-partition` / `-Os -freorder-blocks-and-partition` に通すと、どれも `.text.run` 1 つだった。理由は Xtensa 固有ではなく、profile が無いと GCC は「決して走らない」と言えるブロック（noreturn の後など）しか cold にしないため（レビューの指摘）。profile（`-fprofile-use`）で分けても、分割された部分の間の `j` は IRAM と flash の間の距離を越えられない。**分割は手で関数を分ける。**

### 2.2 `JS_CallInternal` の命令ごとの大きさ（実測、静的。系統誤差あり）

PR #69 の image（`92c9822d`。`quickjs.c` は main `e0fe98a` と同一で、本番のダンプ処理がまだ入っている）の ELF を逆アセンブルし、命令ごとに `addr2line -i` の一番外側の `quickjs.c` の行を、その行を含む `CASE(OP_x)` か関数内のラベルの領域に割り当てた（scratch の `case_sizes.py`）。`JS_CallInternal` は 25,498 B。

DERBY の opprof（OPS 行、449 窓）で分類し直した値（レビューの再集計）:

| 区分 | バイト |
| --- | ---: |
| 入口・フレーム・呼び出し・戻り・例外などのラベル領域と prologue（**outline できない**。すべての局所変数を触る） | 5,256 |
| DERBY で 1ターン 4 回以上使う命令のハンドラ | 5,535 |
| まれに使う命令 | 668 |
| DERBY で使わない命令 | 14,039 |

この割り当ての系統誤差（レビューの指摘。今のブランチの ELF で取り直すまで、数字は目安）:
- 8 つの比較命令は `OP_CMP` マクロ 1 行で展開され、`CASE\(` の正規表現に掛からず、直前の `OP_xor` に帰属していた（`OP_xor` 874 B の正体。v1 の「論理演算の共通部分」は誤診）。比較は DERBY で 425 回/ターン（host）の熱い命令。
- 連続した `CASE` ラベル（`push_0`〜`push_7` など）は末尾のラベルに全部帰属する。
- -Os の cross-jumping で共通化された `goto exception` の尾は 1 つの領域に落ちる。
- ダンプありの `BREAK` は 1 ハンドラ 25〜40 B を足す（`OP_nop` 39 B、`OP_undefined` 51 B）。ダンプ無しの縮みは小さな熱いハンドラに集中するので、全体の 18% を一律に当てるのは誤り。

### 2.3 outline の後の本体の大きさ（推定）

ラベル領域 5.3 KB と熱いハンドラ 5.5 KB の計 **10.8 KB が、冷たい命令を全部ただで出せた場合の床**（ダンプあり）。outline した命令にも呼び出しの stub（約 28 B、ダンプあり）が残るので、現実の取り分は（レビューの見積もり）:

| 出す範囲 | 領域数 | 正味 | 本体 |
| --- | ---: | ---: | ---: |
| 未使用・まれで 150 B 超 | 23 | −4.5 KB | 21.0 KB |
| 同 100 B 超 | 36 | −5.7 KB | 19.8 KB |
| 同 60 B 超 | 117 | −9.6 KB | 15.9 KB |

ダンプ無しの縮みを当てても **13〜16 KB**。さらに、IRAM に置いた本体から flash の関数への `call8` はリンカの緩和を受けられず `l32r＋callx8` に戻るので、呼び出し 1 箇所あたり +7 B 前後（推定。箇所数は未計測で、数百なら +2〜3 KB）。v1 の「8〜10 KB」は成立しない。

### 2.4 本体と helper 群の効きの比（推定、模型の値の比）

STRESS の模型で、`JS_CallInternal` が占めるミスは warm 15%・cold 19%。本体を 13 KB 以上 IRAM に出して取れるのはこの範囲。R5 の helper 8 KiB は warm −37%・cold −26%（模型）、実機 −15%。**1 KiB あたりでは本体は helper 群の半分程度**。R5 の模型が「バイトあたりのフェッチ回数」で候補を選んだので、当然そうなる。

### 2.5 模型での順位（模型、STRESS、2026-10-25）

`tools/r5sim/icache.py model` に、指定した関数を先に IRAM に入れる `--force`（と、その値段を差し替える `--force-cost`）を足し、同じ DRAM の予算で並べた。記録は前回の STRESS の B（合流後、`turn-working-set.md` §2.2。今日の P0 の変更より前）、配置は r5sim 自身、8-way 32 KiB。本体の値段は §2.3 の見積もりの中間 14.5 KB（14,848 B）で計上した（outline した部分は実行されないので、模型の上では本体全体を IRAM に置くのと同じ）。

| IRAM の予算 | 置くもの | ミス/ターン warm | cold | 触る行 |
| --- | --- | ---: | ---: | ---: |
| 0 | なし | 1,071 | 1,630 | 41.8 KiB |
| 4 KiB | helper 群（76 関数） | 724 | 1,312 | 37.3 KiB |
| 8 KiB | helper 群（103 関数） | 576 | 1,163 | 33.7 KiB |
| 14.5 KB | **helper 群（135 関数）** | **386** | **965** | 28.5 KiB |
| 14.5 KB | 本体だけ | 639 | 1,226 | 34.4 KiB |
| 22.5 KiB | **helper 群（172 関数）** | **196** | **777** | 23.7 KiB |
| 22.5 KiB | 本体＋helper 群 | 276 | 862 | 26.3 KiB |
| 30 KiB | **helper 群（223 関数）** | **53** | **591** | 18.4 KiB |
| 30 KiB | 本体＋helper 群 | 124 | 656 | 20.3 KiB |

**どの予算でも helper 群だけのほうが効く。** 本体に予算を割くと、同じ予算の helper 群だけより悪くなる。レビューの「1 KiB あたりの効きは本体が helper 群の半分程度」を模型で確かめた。

ms への換算（推定、ミス 1 回 450 サイクル・240 MHz、`r5-icache.md` §1.1）: helper 群 8 KiB で cold −467 回 ≒ −0.88 ms/ターン、14.5 KB で −665 回 ≒ −1.25 ms。R5 の実機（8 KiB で −1.07 ms）と同じ桁。DERBY では未確認（r5sim は DERBY を回せない）。

**P1 の結論（v2.1）: IRAM の予算は helper 群に使う。インタプリタ本体の outline と IRAM 配置は採らない。** 残る判断は予算の大きさ（DRAM との交換）で、ユーザーが決める。候補の関数の一覧は、実際のファームの ELF に写して選び直す（`--emit`。r5sim と本番で関数の大きさが違う）。

### 2.6 本番の ELF に写した候補（模型、2026-10-25）

今日の P0 を含むファーム（`8e410547`、broker でビルド、焼いていない）を Xtensa でビルドできることを確かめ、その ELF に模型を写した（記録は §2.5 と同じ STRESS の B）。`JS_CallInternal` は 22,289 B（ダンプ無し、P0 込み）。命令ごとの大きさは `tools/r5sim/case_sizes.py`（比較命令の `OP_CMP` / `OP_CMP_NUM` の展開を数えるように直した。`OP_lt` などは各 193 B、`OP_xor` は 67 B）。

| 予算 | 関数（fragment に書けた数） | ミス/ターン warm | cold | 推定の短縮（cold の減少 × 450 サイクル、240 MHz） |
| --- | --- | ---: | ---: | ---: |
| 0 | — | 1,241 | 1,791 | — |
| 4 KiB | 72（68） | 793 | 1,368 | 約 0.8 ms |
| 8 KiB | 98（93） | 599 | 1,175 | 約 1.2 ms |
| 14.5 KB | 128（122） | 366 | 975 | 約 1.5 ms |

fragment は `tools/r5sim/candidates/p1_iram{4,8,145}.lf`。`noflash` ではなく `noflash_text`（コードだけ IRAM、rodata は flash に残す。IDF v6.0.1 の `esp_system/app.lf` にある scheme）。`$constprop` / `$isra` の付いた名前は fragment に書けないので、各 4〜6 関数が外れる。どれも STRESS で選んだ候補で、DERBY での効きは未確認。

## 3. 作り方（模型の順位が出た後に、勝った側を作る）

### 3.1 helper の `no_inline`（先にやる。単独で入れられる）

upstream の 1 行呼び出しのハンドラ（`js_for_of_next`、`js_for_in_next`、`js_operator_in`、`js_operator_delete` など）が -Os で inline されて膨らんでいるものに `no_inline` を付ける。意味は変わらない。今のブランチの ELF で `case_sizes` を取り直し、どれだけ縮むかを静的に確定する。flash のままでも、熱いハンドラの並びが詰まる。

### 3.2 outline（本体を IRAM に置くと決めた場合）

冷たい `CASE` の本体を `static no_inline` の関数に移す。**局所変数のアドレスを渡さない**（`&sp, &pc` を渡すと、GCC は関数全体でそれらを「呼び出しで変わりうる」と扱い、熱い経路のすべての呼び出しの前後で store/reload が入る）。値で渡し、戻り値で返す:

```c
typedef struct { JSValue *sp; const uint8_t *pc; } JSOpResult;   /* a2/a3 の 2 語で返る */
CASE(OP_define_method): {
    JSOpResult r = js_op_define_method(ctx, sf, sp, pc, opcode);  /* flash */
    sp = r.sp; pc = r.pc;
    if (unlikely(!pc)) goto exception;   /* 失敗の印。sp は「元のコードが goto exception に着いた時点」の値 */
}
BREAK;
```

- 例外のときの `sp` は、元のコードが `goto exception` に着いた時点の値を返す（`with_put_var` のように pop してから失敗する命令がある）。`exception:` の `while (sp > stack_buf)` の巻き戻しが参照の数を合わせるので、これを守れば参照の数は今と同じ。
- `sf->cur_pc` を、今は設定していない命令（`define_var`・`regexp`・`get_super`（Proxy の trap で JS に再入する）・`set_proto`・`define_field`・`check_brand`）にも書くと、trap の中で投げた例外の backtrace の行が変わる。改善だが挙動の変更なので、意図した変更として記録する。
- 移せる（本体を読んで確認、レビュー）: `with_*`（6。`opcode` と分岐の `pc` を返す）、`define_method(_computed)`、`eval`/`apply_eval`、private field 3 種、`private_in`、`check_define_var`/`define_var`/`define_func`/`delete_var`、`put_ref_value`/`get_ref_value`、`make_var_ref(_ref)`、`get/put_super_value`、`iterator_call`、`regexp`、`set_proto`、`copy_data_properties`、`throw_error`。
- 条件付きで移せる: `special_object`（572 B、最大の候補）と `rest` は、`ENSURE_CALL_INPUTS` の後なら `argc`・`argv`・`new_target` を値で渡せる（マクロの `inputs_ready_*:` ラベルはインタプリタ側に残す）。`define_class`/`fclosure` は `var_refs`・`sf`・`b` を渡せば可。`nip_catch` の `stack_buf` は `sf->var_buf + b->var_count` から出せる。`close_loc` は `b`・`sf`。
- 移さない: `goto done`（`return`/`return_undef`・`call_method`・`has_call_argc` の tail call）、`goto done_generator`（`await`・`yield`・`yield_star`・`async_yield_star`・`return_async`・`initial_yield`）、`goto flat_call`、`goto vm_yield`、`JS_VM_POLL_SAFEPOINT` を含む分岐命令（`goto`・`goto16`・`goto8`・`if_true`・`if_false`・`if_true8`・`if_false8`）。`gosub`/`ret`/`catch` は小さすぎて価値が無い。
- VM スタックの segment は動かない（`quickjs-vmstack.h`）ので、outline 関数の中のポインタが古くなる経路は無い。generator/async のフレームはヒープにあり `sf` 経由なので影響しない。

### 3.3 配置（Kconfig、既定 n）

- 本体: `noflash` ではなく `noflash_text` の linker fragment にする。`noflash` はエラー文字列や GCC の switch 表（`restore_call_inputs`・`special_object`・`with_*`）まで DRAM へ移し、予算外のバイトになる。
- `dispatch_table`: 関数内の static なので `.rodata.dispatch_table.N` になり、fragment の規則に掛からない。IDF のときだけ定義するマクロ（`JS_DISPATCH_TABLE_ATTR`）で `DRAM_ATTR` を付ける。
- 断片を差し替えたら `idf.py reconfigure`（`r5-icache.md` §4）。

## 4. 予算

| 項目 | 量 | 区分 |
| --- | --- | --- |
| 本体を IRAM に置く場合 | 13〜16 KB ＋ 呼び出しの緩和の喪失 +2〜3 KB → 空きヒープが同じだけ減る | 推定 |
| helper 群（R5 の 8 KiB 版） | +8,705 B（静的 DIRAM）、空き −8,752 B | 実測（実機） |
| `dispatch_table` | 1 KiB | 静的 |
| ホーム画面の空き | 274 KiB 前後（Wi-Fi リンク後） | 実測（実機、`CLAUDE.md`） |
| DERBY の評価の余裕 / ゲスト最大 | 約 28.9 KB（線 20 KB）/ 134.1〜134.8 KB | 実測（実機） |

最大連続ブロックが段で落ちる（R5 で 139,264 → 131,072 B）。**`memlog.py --port --check` の空きと最大連続ブロックを、DERBY の評価の余裕と並べて受け入れ基準にする。**

## 5. 測り方

### 5.1 実機の前（模型）

1. 今のブランチの ELF で `case_sizes` を取り直す（比較命令の帰属を直し、ダンプ無しで）。§3.1 の `no_inline` の前後も。
2. `icache.py model --iram-kib` で「本体」「R5 の候補を選び直したもの」「両方」を、同じ DRAM の予算で比べる（STRESS）。DERBY で決めるなら、先に r5sim に DERBY を載せる（`pocket.derby` などの面）。

### 5.2 実機（模型で勝った構成だけ）

- **配置の揺れを分ける**: (a) IRAM=n の側でも、動かす関数を flash の text の末尾に置く断片を使い、抜いても他の関数の番地が動かないようにする。(b) 各構成で 0/32/64/96 B の padding を入れた 3〜4 通りの配置を作り、分布で比べる。
- **読む値**: ms ではなく `I_STALL_BUSY` と退役命令数を主にする（TURNPERF。R5 §3.4 と同じ）。outline は命令数を、IRAM は待ちだけを変えるので、2 段を分けて読める。
- **反復**: 各脚 3 回以上、`turn_ms_med` の中央値で比べる（同一バイナリでも +9% 動いた）。
- **ターン間の追い出し**: 同じターンを 2 回呼ぶプローブ（`interpreter-stages.md` §6.2(a)）。IRAM 化の後に残る flash のコードの取り直し量が読める。
- **DMA**: LCD の転送は非同期で、JS のターン中に DMA が SRAM を読む。IRAM の命令フェッチと同じプールなので、IRAM 化しても `I_STALL` が残るならこれを疑う。
- `memlog.py --port --check`（§4）。

## 6. 決めること（ユーザー）

- 模型の順位が出た後で、どの構成を実機で試すか。
- 入れるか: 削れる ms と空きの減少（KB）を並べて判断する。前例は −1.07 ms 対 −8.7 KiB で見送り。

## 7. 他の文書との関係

- `interpreter-stages.md` §9.2 は「cold への追い出しは作業集合の 3〜4% で優先しない」（模型）とした。ここでの outline の目的は作業集合ではなく「IRAM に収める大きさ」で、別の理由。§3.1 の `no_inline` は両方に効く。
- P1 は T0 だけの配置を扱う。T1 の専用ハンドラ（`interpreter-stages.md` §9.3）を入れると本体の熱い部分は増えるので、T1 を入れるときは予算を書き直す。

## 8. v1 へのレビュー（SLOW、2026-10-25）の要約

重大度順。すべて v2 に反映した。

1. 本体の見積もり「8〜10 KB」は成立しない。ラベル領域 5.3 KB は出せず、stub と呼び出しの緩和の喪失を足すと 13〜16 KB＋α（§2.3）。
2. 検証済みの代替（helper 群を IRAM）と比べていない。模型も使っていない。1 KiB あたりの効きは本体が helper 群の半分程度（§2.4）。R5 の再開手順（模型を回し直す）を飛ばしていた。
3. outline の雛形が `&sp, &pc` を渡しており、熱い経路全体に store/reload を入れる。値で渡して戻り値で返す。先に既存 helper の `no_inline` で数 KB 取れる（§3.1・§3.2）。
4. 測り方が 15% の揺れに足りない。配置の固定、padding の分布、`I_STALL` と命令数、各脚 3 回（§5.2）。
5. 引用の誤り: 70%/0.17 は DIO の値（今は 57%/0.25）。41.8 KiB は STRESS の値。「DERBY の JS ターン 14.5〜44 ms」の 44 ms は記録に無い（引き継ぎの誤り。実測は §0-3）。`OP_xor` 874 B は比較命令の帰属の誤り。`noflash` は rodata まで DRAM に移す。
