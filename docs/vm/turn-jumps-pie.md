# JS のターンの分岐と PIE（2026-11-05）

[VMの文書](README.md)

> 60 fps への道のり全体の要約は [perf/60fps-roadmap.md](../perf/60fps-roadmap.md) にある。

「QuickJS の本体はジャンプしまくりだろうから、分岐を減らせないか。SIMD 化（PIE）や複数命令を PIE にまとめることも検討して」への答え。実測（XTPERF、`CONFIG_POCKET_VM_TURNPERF`）、コード（逆アセンブル）、推定を分けて書く。記録は [logs/turn-jumps-20261105.txt](logs/turn-jumps-20261105.txt)。

## 1. 結論

- **ジャンプは JS のターンのサイクルの 13〜15%（実測）。** 成立した制御移行 1 回 = バブル 2 サイクル（[perf/pie-simd.md](../perf/pie-simd.md) §3.9）が、ここでもそのまま成り立つ（STRESS で 2.10）。
- **そのうちインタプリタの命令の振り分けは 1% 前後しかない。** 1 ターンに実行されるオペコードは STRESS で約 4,300、DERBY のレースで約 11,000（`jx` の回数、実測）。`JS_CallInternal` の振り分けを命令ごとに複製しても、得は STRESS 0.5%、DERBY 1.2%（推定）で、コードは +1.2 KB 増える。**採らない。**
- **一番大きいのは今も flash キャッシュの待ち（34〜43%、実測）。** 分岐より先にここ（[turn-cpi.md](turn-cpi.md)、[turn-working-set.md](turn-working-set.md)、[p1-iram-placement.md](p1-iram-placement.md)）。
- **PIE（SIMD）で速くなる部分は QuickJS のターンにほぼ無い。** データ並列のループ（`memcpy`/`memset` など）は host の命令数で 2% 未満で、残りはポインタをたどる分岐の多いコード（推定、§4）。

## 2. 1 ターンの内訳（実測）

30 ターンあたり。ターンは `dispatch_guest` と `pocket_kasane_end_turn`（QuickJS、そこから呼ぶ Shiki の draw（JIT）、commit を含む）。

| | STRESS LV1 | DERBY レース |
| --- | ---: | ---: |
| サイクル | 50.8 M（1 ターン 1.69 M、7.0 ms） | 54.5 M（1.82 M） |
| 退役した命令（IPC） | 22.6 M（**0.445**） | 20.8 M（**0.38**） |
| 命令フェッチ待ち（`I_STALL`） | 18.3 M（36%） | 23.6 M（43%） |
| うち flash キャッシュ（`I_STALL_BUSY`） | 17.3 M（34%） | |
| うち `L32R` | 2.18 M（4.3%） | |
| バブル | 8.5 M（17%） | |
| うち分岐（`BUBBLES_CTI`） | 7.36 M（14.5%） | |
| うちレジスタ依存 | 1.29 M（2.5%） | |
| データ待ち（`D_STALL`） | 1.6 M（3.2%） | 1.8 M（3.3%） |
| 窓例外（`EXR_WINDOW`） | 0.114 M 回 | |

成立した制御移行の種類（30 ターン）:

| | STRESS | DERBY レース |
| --- | ---: | ---: |
| `jx`（振り分け） | 0.129 M | 0.333 M |
| 分岐の成立・`j` | 2.36 M | 1.89 M |
| `call`・`callx`・`ret` | 1.02 M | 1.42 M |
| 分岐の不成立 | 1.62 M | 1.50 M |

STRESS では分岐のバブル 7.36 M ÷ 成立した移行 3.51 M = 2.10。

## 3. 振り分けの形（コード）

QuickJS は computed goto（`DIRECT_DISPATCH 1`）だが、**`-Os` の GCC は全ハンドラの `goto *dispatch_table[*pc++]` を 1 本の共有の `jx` に畳む**（`JS_CallInternal` の中の `jx` は 4 本、`j <共有の振り分け>` が 215 本）。GCC が畳んだものを複製し直す pass（`duplicate_computed_gotos`）は、サイズ最適化の関数では動かない。1 命令の振り分けは

```
l8ui a3, pc, 0 / l32r a9, <表> / addi pc, pc, 1 / addx4 a9, a3, a9 / l32i a9, a9, 0 / j <共有> → jx a9
```

で、成立した移行が 2 回（`j` と `jx`）。`JS_CallInternal` だけ `__attribute__((optimize("O2","no-inline-small-functions")))` にすると、`jx` が 273 本に複製され、関数は 23,384 → 24,626 B（+1.2 KB、ファイル全体 +3.8 KB）。`-O2` そのままだと 30,505 B（+30%）。

**値段の見積もり（推定）**: 減るのはオペコードごとの `j` 1 回 = 3 サイクル。STRESS 4,300 × 3 = 1.3 万サイクル/ターン（0.8%）、DERBY 11,100 × 3 = 3.3 万（1.8%）。ただし増えた 1.2 KB は、すでに 32 KiB を超えている 1 ターンの作業集合（42 KiB、[turn-working-set.md](turn-working-set.md)）に乗る。得はビルド間の配置の揺れ（15%）の中にあり、同一バイナリの A/B もできない（関数属性はビルド時に決まる）ので、実機では測らず採らない（arm D はビルドしたが焼いていない）。

**なぜ少ないのか**: STRESS の 1 ターンは QuickJS のバイトコードを 4,300 命令しか実行しない。時間の多くは、そこから呼ばれる C（Shiki の JIT の描画、commit、プロパティ・atom・数値の helper）と、そのコードを flash から取ってくる待ち。host の命令数でも `JS_CallInternal` はターンの 8%（[logs](../kasane/logs/stress-js-turn-prof-20261104.txt)）。

## 4. PIE（SIMD）と「複数命令をまとめる」

- **PIE が効く形**: 16 B に揃った連続データに、同じ演算を 8〜16 レーン並べる（[perf/pie-simd.md](../perf/pie-simd.md) §1〜2）。PIE の命令は AR（整数レジスタ）ではなく Q レジスタで動き、AR と Q の行き来にも命令が要る。
- **QuickJS のターンの中身**: バイトコードの解釈（1 命令ごとに型タグで分岐）、プロパティ・shape・atom の検索（ポインタをたどる）、参照数の増減、確保。どれもレーンに並ばない。データ並列なのは `memcpy`/`memset` 程度で、host の命令数で 2% 未満（2026-11-05 の callgrind、STRESS のターン）。
- **複数命令をまとめる**: スカラーの組み合わせ命令（`addx2/4/8`、`extui`、`minu`/`maxu`、`saltu`）は GCC がすでに使っている（§3 の `addx4` など）。PIE に「スカラーの数命令を 1 命令に」という形の命令は無い（PIE は Q レジスタのベクタ演算と、その読み書き）。
- **PIE の候補が残るのは QuickJS の外**: ネイティブ側のデータ並列の仕事（描画のカーネルは既に PIE、[perf/pie-simd.md](../perf/pie-simd.md) §7）。JS の側では、大きな TypedArray の一括コピー・塗りを C の API で出すときに、その中を PIE にする、という形しか無い（今のアプリに該当する重い使い方は無い、推定）。

## 5. 次に効くもの

- flash キャッシュ（34〜43%）: 1 ターンの作業集合を 32 KiB に寄せる（[p1-iram-placement.md](p1-iram-placement.md)、[interpreter-stages.md](interpreter-stages.md)）。
- 窓例外: STRESS で 30 ターン 11.4 万回（1 ターン 3,800 回）。PM は例外の中を数えないので、サイクルは `us` と `cy` の差（1 ターン約 0.9 ms、割り込みを含む）からしか見えない。1 回の値段は未測定。
- `L32R` の待ち 4.3%: `-mlongcalls` の呼び出しと、定数・表の番地の読み込み。

## 6. 再現

`sdkconfig.turnperf.defaults` を重ねたビルド（[turn-cpi.md](turn-cpi.md) §1）で、`TURNPERF insn_jx` / `insn_brj` / `insn_callret` / `insn_ntaken` が出る（`main/app_session.c` の `turnperf_events`）。30 ターンに 1 事象ずつ巡回するので、全 30 事象を 1 回りするのに 30 秒（STRESS、30 fps）。
