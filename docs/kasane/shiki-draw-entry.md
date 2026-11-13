# Shiki の native の draw の入口 — DERBY の `ser()` の draw の内訳と、入口を軽くした変更（2026-11-13）

ブランチ `ser-draw`（`8e065fb` から）。[shiki-bundle.md](shiki-bundle.md)（JS からの draw の固定費と `drawMany`）の続きで、こちらは **native の呼び出し元（`pocket.derby` の `ser()`・`runners()`）が 1 本ずつ出す draw** を扱う。[derby-ser-division.md](../apps/derby-ser-division.md) §13.x の実機の DERBY_SER で、Shiki の draw が `ser()` の約半分を占めると分かったのが動機。**数値は、実測（どこで測ったか）・コード（読んで確かめた事実）・推定を必ず書き分ける。**

## 0. 結論

1. **draw 1 本の費用の大半は plan の中身（JIT の機械語と、線・模様線の helper）で、入口ではない。** host で数えた重い WIDE 2 相当（楕円、MID、全 WIDE 首振り）の 1 フレームは、`ser()` の draw が 34 本・Shiki のステップ 2,800（その 75% が柵と芝の plan）・`LINE_PATTERN` 約 47 回（実測、host）。
2. **それでも入口には削れる命令があった。** r5sim で `ser()` の draw 1 本あたり、エンジンの外に約 62 カウント（約 400 命令、推定）、エンジンの入口の vm の初期化（372 B の `memset`）と入力の複写が約 23 カウント。plan の照合の線形探索、毎回の 372 B の `memset`、入力の複写 2 回、呼び出しの 1 段が、どれも画素に関係しない仕事だった（コード）。
3. **入れた変更（画素は変わらない）:** handle → slot の 1 回目の当て推量の表、`pocket_proc_draw_fixed` 専用の入口（入力をその場で読む `ksn_shiki_draw_in_place`、呼び出し 1 段少ない）、draw ごとの vm の初期化を 162 B に（ベクトルの状態 207 B は最初の `VEACH` で）。
4. **効果（r5sim、実測）:** 重い WIDE 2 で `ser()` の draw が 27,352 → 26,058 カウント/ターン（−4.7%）、ターン全体 145,485 → 143,554（−1.3%）、レース全体 105,217 → 104,215（−1.0%）。`runners()` の draw も −6%。実機の見込みは draw_insn −約 1 万命令/フレーム、draw_cy −1〜1.5 万サイクル/フレーム（推定、§8）。
5. **束ね（弦ごと・系列ごとに 1 回の呼び出し）は入れなかった。** 入口を軽くした後に束ねで消える呼び出しの費用は 1 フレーム 0.1% 程度の見込みで、`derby_ser_int.c` の呼び出しの形と host の検査（`test_derby_serq.c`）を変える割に合わない（推定、§9）。
6. 実機用に、DERBY_SER の後に **`DERBY_SERDRAW`** の行（plan ごとの draw の数・サイクル・エンジンのサイクル・ステップ・命令・取得の待ち、模様線の回数とサイクル）を足した（§8）。

## 1. 問い（親セッションの実機の数、`89eff15`、実測の引用）

楕円の重い CAM WIDE 2（draws 約 50、pat 40〜51）でパンするフレーム 1 枚あたり: `ser_cy` 265k〜286k サイクル、うち Shiki の draw（`draw_cy`）126k〜147k、`draw_insn` 78k〜91k 命令、`draw_ist`（命令取得の待ち）約 25k。`SHIKI_TURN engine_ms` は 1 フレーム全体で約 0.6 ms。

draw 1 本の値段は、draw の本数の取り方で変わる。`ser()` の中の draw は host で 34 本（§3.1）なので、**1 本 約 4.0k サイクル・約 2.5k 命令**（計算。「約 50 本で 2.6k」は `SHIKI_TURN` の本数で割った値で、`ser()` の外の draw を含む）。

## 2. 道具と、それぞれが見るもの

| 道具 | 見るもの | 見えないもの |
| --- | --- | --- |
| r5sim（`tools/r5sim/fast.sh <dir> -DR5SIM_DEVICE=1 -DR5SIM_APP=derby`、`R5SIM_ICOUNT=shift=2,sleep=off`） | 実機と同じ入口のコード（`pocket_derby.c`・`pocket_shiki_frame.c`、どちらも -O2）の命令のカウント。1 カウント ≒ 6.54 命令（[api-reorg.md](api-reorg.md) §6.1） | **JIT と PIE**。r5sim の像は `r5sim_scalar.h` で Shiki のエンジンを C のモデル（インタプリタ）にする（`tools/r5sim/main/CMakeLists.txt`、QEMU に PIE が無い）。scalar の JIT は Xtensa の命令だけだが、`KSN_SHIKI_HAS_PIE` が 0 なので `ksn_shiki_jit_run` は呼ばれない（コード）。エンジンのカウントは実機のものではない。flash の待ちも無い |
| r5sim の計測用の像（`-DR5SIM_DERBY_PROF=1`） | `pocket.derby` の関数ごとのカウントと、その中の Shiki の draw の `total_cy`、**今回足した `engine=`**（`pocket_shiki_turn` の `engine_cy`、vm の初期化からエンジンを出るまで）。差がエンジンの外（引数の検査・slot・フレームの場所・統計） | 同上 |
| host の DERBY（`-DPOCKET_DERBY_SER_PROBE` で組んだ `test_derby_host`） | `DERBY_SERDRAW` の draw の本数・Shiki のステップ・`LINE_PATTERN` の回数（サイクルは 0）。ステップは JIT でもインタプリタでも同じ数（[shiki-jit.md](shiki-jit.md) §11.2） | 時間 |
| 実機の `DERBY_SER`・`DERBY_SERDRAW`（§8） | サイクル・命令・取得の待ち、plan ごと | — |

host の `LINE_PATTERN` の回数は**実際の 2 倍**に出る: harness は draw をすべて同じエンジンでもう一度描いてフレームを照合する（`tools/games/test_derby_host.c` の `sdraw_run`）ので、`ksn_shiki_curve_n` が 2 回数えられる。ステップは `pocket_shiki_turn` の値で、照合の分を含まない（コード）。

## 3. 内訳

### 3.1 draw の本数と plan ごとのステップ（host、楕円、MID、全 WIDE 首振り、実測）

`DERBY_SERDRAW` の 30 ティックの窓。`n=` は 1 フレームの draw の本数で、柵と芝（`prail`・`pt`、系列の plan）/ 観客席の段（`hl`）/ 観客（`crowd`）。

| 窓 | n | steps | うち 1 本あたり | `LINE_PATTERN`（host の数 ÷ 2） |
| --- | --- | --- | --- | ---: |
| tick 1530（重い） | 24 / 5 / 5 | 2,111 / 266 / 423 | 88 / 53 / 85 | 47 |
| tick 1650 | 24 / 5 / 5 | 2,102 / 258 / 410 | 88 / 52 / 82 | 45.5 |
| tick 1020（直線のような軽い窓） | 7 / 1 / 1 | 2,031 / 51 / 81 | 290 / 51 / 81 | 9 |

- 重い窓の 34 本・2,800 ステップのうち **75% は柵と芝の plan**（柱 1 本ごとの `MOVE`・`LINE` と逆数の Newton 3 段、芝の縞 1 本ごとの同じ形）。観客は 5 本で、1 本に `LINE_PATTERN` が約 9 回（3 行 × 3 本）。
- 実機の pat 40〜51 と host の 45〜47 は合う。**この窓が親の測った重い WIDE 2 に当たる**（推定。r5sim のレースの wide2 は `ser()` の draw 29.4 本・pat 37、§13.3 の r5sim と同じ形）。
- HEAVY では 24 / 5 / 5 本は同じで、ステップが 2,475 / 302 / 523（観客の行が多い）。

### 3.2 エンジンの外（r5sim の計測用の像、重い WIDE 2、455 ターンの平均、実測）

| 区間（`ser()` の draw 29.39 本/ターン） | 前（`8e065fb`） | 1 本あたり |
| --- | ---: | ---: |
| `emit_q` から戻るまで（`serprof draw=`） | 27,352 | 931 |
| 　うち `pocket_proc_shiki_draw_inputs` の中（`shiki=`） | 26,623 | 906 |
| 　　うちエンジン（`engine=`。vm の初期化・入力の複写・インタプリタ） | 25,544 | 869 |
| 　　エンジンの外（引数・slot・フレームの場所・統計） | 1,079 | 36.7 |
| 　`emit_q`・`pocket_proc_draw_fixed` と窓の出入り（`draw=` − `shiki=`） | 729 | 24.8 |

エンジンの外の 61.5 カウント（約 400 命令、推定）の中身（コード、`8e065fb` の ELF の逆アセンブル）:

- **`find_slot` の線形探索**: 32 個の slot を前から 1 つ 6 命令で見る（ゼロオーバーヘッドのループ）。DERBY の系列の plan はレースの場面で登録されるので、slot の番号が大きい（レースは `plans=26`）。
- **呼び出しの段**: `derby_ser_chord_q` → `emit_q` → `pocket_proc_draw_fixed` → `pocket_proc_shiki_draw_inputs` → `ksn_shiki_draw_index_cap` →（実機では `ksn_shiki_jit_run` → JIT の機械語 → `ksn_shiki_jit_curve` → `pattern_line`）。windowed ABI で、深くなるたびにレジスタの窓のあふれと戻しの例外が入る（r5sim の `emit_q` と `pocket_proc_draw_fixed` の 2 段は 1 本 約 25 カウント ≒ 160 命令だが、2 つの関数の本体は合わせて 25 命令前後。残りは窓の例外と、計測用の像の印 `SP()`、推定）。
- **入力の複写 2 回**: `pocket_proc_shiki_draw_inputs` の `fx[8]` の 0 埋め（固定小数の入力では使わない）と、`ksn_shiki_draw_index_cap` の `in[8]` への複写。
- **vm の初期化**: `ksn_shiki_vm` 372 B を draw ごとに ROM の `memset`。そのうち 207 B はベクトルの状態（`vraw`）で、`VEACH` を持たない plan（DERBY の全部）は一度も読まない。

### 3.3 エンジン（実機、コードと推定）

- `ksn_shiki_jit_run` は flash に 1,676 B（飽和の標本取りの関数が中に入った）、スタックフレーム 880 B。速い道では `ksn_jit_run` の 14 語を組み、時計と回数を進め、JIT の機械語（IRAM）を呼ぶ（コード）。
- 機械語は 1 ステップ 27〜33 サイクル（[shiki-jit.md](shiki-jit.md) §12.1、実測の引用）で、helper（`ksn_shiki_jit_draw`・`ksn_shiki_jit_curve`・`pattern_line`、IRAM）が残りを占める。2,800 ステップ × 30 ≈ 84k サイクルに helper が乗ると、draw_cy 126k〜147k の大半はここ（推定）。**`DERBY_SERDRAW` の `eng=`・`pat=` で実機の割合が出る**（§8）。
- `draw_ist` 約 25k/フレームは、flash にある入口の道（`pocket_proc_*`、`ksn_shiki_draw_*`、`ksn_shiki_jit_run`）が `ser()` の呼び出し（1 フレーム 5 回）のたびに冷えているため、が有力（推定。`ser()` と `ser()` の間で JS のインタプリタが命令キャッシュ 32 KB を入れ替える）。入口の道を短くすると、触る行が減る。

## 4. 入れた変更（画素・draw の順・誤りの順は同じ）

- **slot の当て推量**（`main/pocket/pocket_shiki_frame.c`）: `slot_hint[handle % 32]` に、attach したときの slot の番号を置く。`find_slot` はまずそこを見て、handle と plan が合わなければ今までどおり前から探す（古い当て推量は探索 1 回の費用になるだけ）。`.bss` 32 B。`drawMany`・`draw()`・`runners()` も通る。
- **`pocket_proc_draw_fixed` 専用の入口**: draw の本体を `draw_impl()`（always_inline）にし、`pocket_proc_shiki_draw_inputs` と `pocket_proc_draw_fixed` がそれぞれの写しを持つ。固定小数の入力で本数が plan の入力の数以上なら、`ksn_shiki_draw_in_place()` を呼ぶ: 入力を複写せずその場で読む（エンジンは `plan->inputs` 未満の入力しか読まない。登録の規則、`ksn_shiki.c` の `INPUT` の検査）。検査の順（mesh・再入・beginFrame・handle・vm の確保）と統計（`SHIKI_TURN`、`drawProfile`）は同じ。
- **vm の初期化を 372 → 162 B**（`main/ui/kasane/ksn_shiki.c`・`.h`）: `ksn_shiki_draw_in_place()` は `vraw` の手前までを 0 にし、`vraw` は `VEACH` の最初の実行で 0 にする（新しい欄 `vinit`）。ベクトルの命令は `VEACH` の本体にしか置けない（登録の検査、`ksn_shiki.c` の 265 行付近）ので、ベクトルの状態を読み書きする命令は必ず初期化の後に走る。`VEACH` を持たない draw の後の `vraw` は前の draw の値のまま（結果の行 `ksn_shiki_result` には出ない。cycle bench の `observe()` は同じ vm を 3 回とも `VEACH` のある例で使う）。

`ksn_shiki_draw_index_cap()`（`draw()`・`drawMany` の道）と `ksn_shiki_draw_in_place()` は、同じ本体 `draw_from()`（always_inline）の写しをそれぞれ持つので、どちらも呼び出しの段は増えない（ほかの呼び出し元の意味は同じ。vm の初期化が 162 B になるのは両方）。

## 5. 検査（host、実測）

| | 前（`8e065fb`） | 後 |
| --- | --- | --- |
| `run_derby.py`（64 bit、ASan/UBSan） | rc 0、PASS 24 行 | **ログ全体がバイト単位で同じ**。フレームごとの CSV 15 本もバイト単位で同じ |
| `run_derby.py --m32` | rc 0 | ログ全体・CSV 15 本ともバイト単位で同じ。Shiki の heap: 使用の最大 30,516、空きの最大ブロックの最小 18,204（12 本とも前と同じ。vm の大きさは 372 B のまま） |
| `tools/kasane_contract/run.sh` | — | rc 0（`shiki_diff`・`SHIKI VEC PASS (108 checks)`・`test_scalar.py`・`test_encoder`・`shiki api: pass 78 fail 0`・STRESS ほか） |
| 計測の像 | — | `-DPOCKET_DERBY_SER_PROBE` の host の harness（§3.1）と、ファームの compile_commands の `pocket_derby.c` に定義を足した Xtensa のコンパイル（警告なし）が通る |

## 6. r5sim（実測、`R5SIM_ICOUNT=shift=2,sleep=off`、カウント/ターン）

既定の像（`-DR5SIM_DEVICE=1 -DR5SIM_APP=derby`）:

| 場面 | 前（`8e065fb`） | 後 | 差 | うち Shiki（前 → 後） |
| --- | ---: | ---: | ---: | --- |
| wide1 | 129,342 | 128,340 | −1,002（−0.8%） | 31,880 → 30,912 |
| **wide2** | **145,485** | **143,554** | **−1,931（−1.3%）** | 36,517 → 34,701（−5.0%） |
| wide3 | 140,113 | 138,685 | −1,428（−1.0%） | 38,953 → 37,625 |
| field | 106,583 | 105,888 | −695 | 22,328 → 21,671 |
| vision | 115,983 | 114,979 | −1,004 | 24,251 → 23,327 |
| 全体（61..1773） | 105,217 | 104,215 | −1,002（−0.95%） | |

着順・時計（`DERBY FINISH`）は同じ。前の値は [derby-ser-division.md](../apps/derby-ser-division.md) §13.3 の「後」と同じ数（同じ像）。

計測用の像（`-DR5SIM_DERBY_PROF=1`、重い WIDE 2）:

| | 前 | 後 | 差 |
| --- | ---: | ---: | ---: |
| `ser()` の draw（`serprof draw=`） | 27,352 | 26,058 | −1,294（−4.7%） |
| 　うちエンジンの外 | 1,079 | 527 | −552 |
| 　うちエンジン（vm の初期化・入力の複写を含む） | 25,544 | 24,874 | −670 |
| 　`emit_q` と入口の段 | 729 | 657 | −72 |
| `runners()` の Shiki（16 本） | 8,660 | 8,103 | −557（−6.4%） |
| ターン全体 | 146,101 | 144,174 | −1,927 |

`ser()` の draw 1 本あたり −44 カウント（約 −290 命令、推定）。エンジンの側の −670 は、r5sim ではインタプリタの道（`ksn_shiki_resume`）だが、初期化と複写は実機の JIT の道でも同じコード。

## 7. ファーム（`idf.py size`、既定の Kconfig、ビルドディレクトリは `-DSDKCONFIG=` で分けた、実測）

| 項目 | 前（`8e065fb`） | 後 | 差 |
| --- | ---: | ---: | ---: |
| Flash Code | 1,699,496 | 1,700,728 | +1,232（`pocket_proc_draw_fixed` が `draw_impl` の写しを、`ksn_shiki_draw_in_place` が `draw_from` の写しを持った） |
| DIRAM | 172,592 | 172,624 | **+32**（`.bss` の `slot_hint`） |
| 　IRAM（`.text`） | 81,687 | 81,687 | 0 |
| 像（`check_flash.py` の app） | 2,587,056 | 2,588,288 | +1,232 |

Shiki の heap とゲストの heap は変わらない（§5）。

## 8. 実機の手順と見込み

**手順**（親が broker で）: [derby-ser-division.md](../apps/derby-ser-division.md) §13.6 の 4 と同じ。`main/pocket/pocket_derby.h` の `#ifdef ESP_PLATFORM` の行の**直前**に次の 1 行を足した一時の `measure:` コミットで組み、DERBY のデモのレースで楕円の重い CAM WIDE 2 の窓を取る。測り終えたらコミットを取り消す。

```c
#define POCKET_DERBY_SER_PROBE 1
```

`KASANE_PAINT` の直後に、今までの `DERBY_SER …` と、新しい次の行が出る（実際は 1 行）:

```
DERBY_SERDRAW frames=<views> n=<rails+turf>/<hl>/<crowd> cy=…/…/… eng=…/…/… steps=…/…/…
  insn=…/…/… ist=…/…/… pat=<calls>/<cycles>
```

すべてパンするフレーム（`view()` の呼び出し）1 枚あたり。`cy` は draw の呼び出し全体のサイクル（`DERBY_SER` の `draw_cy` を 3 つに分けたもの。同じ 1 回の計測で両方に足す）、`eng` はそのうちエンジン（vm の初期化と plan の機械語、`pocket_shiki_turn` の `engine_cy` の差）、`cy − eng` が入口。`insn`・`ist` は PM0・PM1（`DERBY_SER` と同じ数え方）。`pat` は `ser()` の draw の中の `LINE_PATTERN` の回数とサイクル（`ksn_shiki_curve_*[1]` の差、`engine` の内数）。

**見込み（推定。同じ重い WIDE 2 の窓、`8e065fb` の像との比較）:**

| 欄 | 前（実測の引用） | 後（推定） | 根拠 |
| --- | --- | --- | --- |
| `draw_insn` | 78k〜91k | 68k〜81k | r5sim の −290 命令/draw × 34 本 |
| `draw_cy` | 126k〜147k | 112k〜135k | 命令の減りに IPC 0.65〜1 を当てた |
| `DERBY_SERDRAW n` | — | 24 / 5 / 5 前後 | host §3.1 |
| `steps` | — | 2,100 / 260 / 420 前後 | host §3.1（JIT でも同じ数） |
| `pat` | — | 45〜50 回 | host §3.1。サイクルは実機でしか出ない |
| `ist`（3 つの和） | 約 25k（`draw_ist`） | 同じか少し減る | 入口の道が 1 段・約 1 KB 短くなった分（触る行は flash の写しが増えた分と相殺しうる） |

- **ビルド間の揺れ（15%）より小さい差なので、`draw_cy` の前後の比較だけでは主張できない。** 命令数（`draw_insn`）は配置で動かないので、そちらで確かめる。`SHIKI_TURN` の `total_ms` と `engine_ms` も、`runners()` の分と合わせて少し下がるはず。
- `DERBY_SERDRAW` の `eng` と `cy − eng` の比が、§0 の 1（draw の大半は plan の中身）の実機の確かめになる。`eng` の 75% 以上が柵と芝なら、次の手は plan の側（§9）。

## 9. 入れなかった案と次の候補

| 案 | 見込み | 判断 |
| --- | --- | --- |
| **弦ごとの束ね**（`derby_ser_chord_q` が 1 本の弦の draw 最大 9 本を配列にし、`emit` を 1 回） | 除外を通った弦 1 本の draw は平均 1.1 本（host の重い窓で 1 フレーム draw 35.2 本 / 弦 31.8 本、実測）なので、`emit_q`・`pocket_proc_draw_fixed` の入口が消えるのは draw の 1 割足らず。§6 の後に残る入口は 1 本 約 22 カウントで、1 フレーム 約 100 カウント（0.1%）以下（推定） | 入れない。`derby_emit_fn` の形・`test_derby_serq.c` を変え、`ser()` の側を触る別の作業とぶつかる割に合わない |
| **系列ごとの束ね**（`ser()` の最後でまとめて描く） | 弦ごとと同じ程度 | 入れない。`js_ser()` の本体（別の作業の場所）に吐き出しの呼び出しが要る |
| 入口の道を IRAM に置く | `draw_ist` のうち入口の分（未測定）。IRAM 約 1〜1.5 KB = DRAM | 実機の `ist` の内訳を見てから。DRAM はゲストの部屋と引き換え（CLAUDE.md） |
| `ksn_shiki_jit_run` の速い道を小さくする（飽和の標本取りを noinline に） | 1 本 数十命令、flash の行が数行（推定） | 実機の `ist` を見てから |
| plan の側: 柵・芝の 1/Z の Newton 3 段（柱・縞ごと） | ステップの 75% の側。画素を変えずに減らすには、同じ固定小数の値を出す別の計算が要る | 仕様（Shiki の式）と画素の基準の側の話。SLOW と持ち主の判断 |
| `LINE_PATTERN`（観客、1 フレーム約 47 回） | 実機の `pat=` のサイクルで決める | 未測定 |

## 10. 確信の低い点

- 実機の数は全部推定。r5sim はエンジンがインタプリタで、窓の例外と flash の待ちの重さが実機と違う。
- 「1 カウント ≒ 6.54 命令」は api-reorg.md の STRESS での値の引用で、この区間で確かめていない。
- `vraw` を `VEACH` まで初期化しないことで、`VEACH` の無い draw の後の vm の `vraw` は前の draw の値が残る。今のコードに、それを読む所は無い（`ksn_shiki_result` と cycle bench、コード）が、vm の全体を比べる道具を足すときはこの点に気をつける。

## 実機の結果（`a59ed42`＝draw の入口の軽量化＋PIE の改善＋プローブ、broker の app_session、2026-11-13、実測）

このレースは直線のコース（`chords` 5/フレーム）で、楕円の重い CAM WIDE 2 は出なかった。直線の CAM WIDE 3 の窓を、以前の像（`1954cdb`、同じプローブ）の同じ場面と比べた。値はパンするフレーム 1 枚あたり。

| 項目 | `1954cdb` | `a59ed42` |
| --- | ---: | ---: |
| `draw_insn`（配置に依らない） | 43.5k〜44.2k | **39.6k〜41.6k（約 −7%）** |
| `draw_cy` | 78.5k〜79.2k | 72.0k〜75.4k（約 −6%） |
| `ser_cy` | 160.6k〜161.8k | 154k〜160k |

`DERBY_SERDRAW`（同じ窓）: レール・芝 7 draw で `cy` 約 52k・`eng` 約 44k（入口の費用は約 8.5k、1 draw 約 1.2k）、`hl` 1 draw で約 2.1k、観客 1 draw で約 20.5k（`LINE_PATTERN` 9 回で約 18.6k）。draw の費用の大半はプランの側（レール・芝の Newton の段と `LINE_PATTERN`）で、入口の軽量化で取れるのは数 % に留まる。
