# FLOWER の性能記録（装飾光線のコスト・shade・行不変量・棄却）

> 吸収した旧文書: `flower-decor-cost.md`（旧 `docs/flower-decor-cost.md` の移動案内も含む）、`flower-shade.md`、`flower-row-invariants.md`、`tools/flower-rejection/README.md`（楕円体 span 狭めの棄却実験）、`backlog.md` の FLOWER 実測内訳表（2026-09-19）。各節の中の節番号（§N）は旧文書のものを保つ。`ray-stall-census.md`（命令キャッシュ側の調査）は別文書。

**読み方の訂正（1回だけ書く）:** 装飾光線の節（旧 flower-perf.md）の本文にある「FPU がない」「変換が soft-float」という解釈は、後続の実測と整合しない。この石のスカラー FPU の性質は [fpu-latency.md](fpu-latency.md)、平方根の再測定は本文書の shade 節（旧 flower-perf.md (flower-shade section) §6）を現行判断の入口にする。計測値そのものは当時の条件で残す。

## 装飾光線のコストと固定小数点平方根・profile gate（2026-09-09〜15）（旧 flower-decor-cost.md）

This is a workload estimate, not an ESP32-S3 frame-time measurement. Rendering
behaviour was not changed for this measurement. Reproduce the host comparison
with `python tools/bench_decor_cost.py` (MSYS2 UCRT64 GCC on this workstation).

The prior broad-volume variant is reconstructed from the current source with
the preceding width, protection and fixed-colour settings. It is not a saved
firmware binary. Each comparison runs inside one process over the same 256
moments spanning 128 seconds, with a constant input row and no LCD transfer.

| Per-frame work (mean) | Before widening/colour following | Wide rays | Wide rays + events |
| --- | ---: | ---: | ---: |
| Candidate pixel visits | 15,679 | 21,868 | 21,123 |
| Pixels evaluating profiles | 14,489 | 18,022 | 17,437 |
| RGB565 composites | 9,972 | 13,539 | 13,160 |

Counts include overlapping rays separately. The busiest sampled frame by blend
count performs 37,395 visits, 33,041 profile evaluations and 20,598 composites.
The reveal skips unreached rows, reducing average composites by about 2.8%.

Before adding events, the longer host trials measured 0.170..0.181 ms for the
prior variant and 0.223..0.246 ms for the wide variant: paired increases of
29..45%. Absolute host times varied substantially with workstation load.
Event-on / event-off ratios in subsequent trials were 0.85, 1.06 and 0.98.
That spread does not support a precise event overhead or a claimed speedup.

ESP32-S3 object disassembly confirms a scalar auxiliary pass: the blend path
alone is about 48 instructions, in addition to two profiles, protection and
loop work. Two variable divisions are per active ray-row, not per pixel.
Using measured path counts, roughly 1.5..2 million scalar instructions/frame
is a useful first-order budget for wide rays, with branch/load/cache costs
additional. At 240 MHz, allow roughly 6..12 ms/frame for the whole auxiliary
pass, and roughly 2..4 ms for the latest widening/colour change. These are
engineering estimates with substantial uncertainty, not calibrated timings.
The new events should be a smaller contribution than the widening; they add
row decisions and a conditional narrow-fragment operation, partly offset by
skipped reveal rows. A sub-millisecond event budget is a hypothesis to measure,
not a validated upper bound.

### The fixed-point square root bought the picture's 0.02% and no time (2026-09-15)

`sqrtf` is the largest named single cost in the scene: 154 cycles a call, 4,010
calls a frame, 2.1 ms of a 45 ms frame. Replacing it with an integer restoration
root is a nine-fold cut in *instructions* (7 a step against 88; tools/flower_sqrt_variants.c)
and costs the picture 0.02% of its pixels -- a few white pixels where the ellipsoid
root feeds the depth test at a silhouette, and at B=14 one pixel of 32,400 differing
by <=4/255 (tools/flower_frame_dump.c).

On the board it is worth nothing. 79 adjacent 60-frame windows, only
`g_flower_fixed_sqrt` moving (`sq=` in SPLIT3), paired differences in ms/frame:

    ellipsoid root (sqrt=)     +0.03      bell root (bell:sqrt=)   -0.08
    shade (control)            +0.31      garden (control)         -0.37
    total                      -0.11      decor (control)          -0.36

The signal is a tenth of the control band, so the answer is "no difference", not
"a small win". The reason is in the code and not in the timing: `d` arrives as a
float and the root leaves as one, and on a part with no FPU each conversion in and
out of the integer loop is a call into soft-float. The loop got nine times cheaper
and the two conversions around it cost what the loop used to.

So the fixed point has to be carried by the caller -- `d` computed as an integer and
the root consumed as one -- or the round trip eats the saving. That is the same
conclusion the PIE work reached from the other side: the root is now integer and
ready to sit in a lane (56 PIE instructions for eight roots, at the lane width's
precision), and what is missing is the integer geometry around it.

### Measured on the device (2026-09-15)

The 6..12 ms band above is an estimate built from instruction counts. It now has a
measurement: `decor` -- `garden_row_blend`'s row time minus the vector pixel pass,
the one number in SPLIT that had never been split -- is bracketed into its two
jobs by `garden_prof_vegetation()` / `garden_prof_rays()` and reported as SPLIT3
(`main/scene/garden.c`, `main/scene/flower.c`). 36 sixty-frame windows on the
Cardputer ADV (`vm-L1-60-g8779e3e` + the brackets), species 1 / 10 / 11 / 12,
log `\.cache/hosttests/logs/flower-instrument-20260915.log`, re-read with
`python tools/flower_instrument_summary.py <log>`:

| term | measured (ms/frame) | cycles/row | instruction-count floor |
| --- | ---: | ---: | ---: |
| rays (the auxiliary pass) | 7.6 .. 13.7, typical ~10 | 13,450 .. 24,300 | 3.9 |
| vegetation (canopy + trunks + grass) | 5.0 .. 5.9 | 8,900 .. 10,460 | 2.4 |
| rest (row scaffolding, dissolve memcpy + mix) | 0.24 .. 0.36 | -- | not counted |

Three things follow.

1. **The rays are the largest single term in `decor`, and the estimate holds.**
   7.6..13.7 measured against a 6..12 estimate: the auxiliary pass was the piece
   to instrument, and it was measured, not guessed, at the top of its band.
2. **The two bracketed jobs are each about 2.4x their instruction-count floor.**
   That is where the 8..10 ms of unattributed `decor` went -- not into a fourth
   job and not into the row scaffolding, which is 0.24 ms. A 1-cycle-per-
   instruction count on this scalar row code underestimates by ~2.4x (branch,
   load and cache costs), and that factor is now measured rather than assumed.
3. **A cross-fade is visible in the same report.** During a dissolve the
   vegetation pass runs twice, which the counters show directly: rows dissolving
   105 of 135 gave veg 9.61 ms with 240 passes and rest 3.37 ms, against 5.7 ms /
   135 passes / 0.26 ms for the settled windows. A bare cycles-per-row average
   would have been the mean of two different amounts of work.

#### Attempted: gating the profiles by their support (2026-09-15, branch `perf/flower-decor`)

`garden_decor_profile()` returns exactly 0 outside its radius, so the light and
the shadow are each a compact support, and the span the loop walks is their
union -- up to 3.5 radii where each profile owns part of it. A gate that
evaluates a profile only inside its own reach is exact (**both arms byte-identical
to the previous revision over 120 frames x 135 rows x 7.8 MB**, harness
`tools/flower_decor_identity.c`), and on the host it removes **35.6% of the
profile evaluations** (4,304,780 -> 2,772,750 over the same 120 frames, counted
with `-DGARDEN_COUNT_PROFILE`).

On the device it is **not measurable**. The switch was alternated once per
60-frame window inside one binary (`SPLIT3 gate=`) so the pairs differ only by
the gate, 41 pairs: rays delta mean **-0.19 ms**, median -0.03, with the
*unchanged* vegetation pass as a control at mean -0.09 ms. The effect is inside
the noise, and a ~0.7 ms effect is what the removed instruction count predicts
(12,767 evaluations a frame at ~14 instructions).

What that means: **the decorative-ray pass is not bound by the profile
arithmetic.** Removing a third of it moves nothing, so the per-pixel cost is
dominated by the rest of the pipeline (the dither, the pack, the load/store) or
by stalls -- consistent with the pass running ~60..150 cycles per visited pixel
against a ~70-instruction body. Micro-optimising this loop further is a dead end;
the way to move `rays` is to vectorise the whole per-pixel pipeline, not to shave
its arithmetic.



The existing `SPLIT decor` is garden minus pixels and includes vegetation as
well as these rays, so it must not be read as isolated auxiliary-ray timing.
SPLIT3 (above) is what isolates them: `rays=` is a bracket around
`garden_decor_row` alone, and `veg=` around the vegetation pass; `rest` is what
neither bracket covers. Host ratios must not be scaled from the PIE main-background
time: the host uses scalar code where the device uses SIMD.

## `shade` を割る: 一番大きい単項の中身と、そこにあった死んだ呼び出し（旧 flower-shade.md）

2026-09-19。FLOWER の `shade` は SPLIT の単項で最大（1 ヒット 748 サイクル、
1,876 ヒット/フレーム、5.81 ms）だったが、**一度も割られていなかった**。
割った結果、中身の 29% が `rgbd` で、その中に**結果を捨てるだけの呼び出しが1本**あった。

### 1. 割る前に分かっていたこと

`flower_parts.h` の `normal()` に「22 ms 中 1.16 ms、置き換える価値なし」という実測注記があり、
`shade` の残りが何かは誰も測っていなかった。オブジェクトを読むと `shade` は 654 命令で、
`.literal.shade` の再配置が指す呼び出し先は **`sqrtf` / `__divsf3` / `rgbd`（3箇所）** だった。
`normal()` の `1.0f/sqrtf(d)` が平方根と除算を両方とも関数呼び出しにしている
（その2つが何であるかは §6 で逆アセンブルした。当時の思い込みとは違った）。

### 2. 計器

`prof_norm`（`normal()` を囲む）と `prof_rgbd`（`rgbd` を囲む）を足し、SPLIT2 へ
`shade: norm=… rgbd=… rest=…` として出す。`rest = shade - norm - rgbd` なので
三つ足すと SPLIT の `shade=` になる。どちらも `#ifdef ESP_PLATFORM` の TEMPORARY 計器で、
`rgbd` 側は `#define rgbd rgbd_timed` を `shade` の前後だけで有効にして囲う
（`rgbd` は3つの return 経路から呼ばれるので、呼び出し側を3箇所書き換えるより安全）。

実機 COM3、home の FLOWER、種 0（鐘のある種）、60 フレーム窓:

```
shade=5.81 (748 cy)  |  norm=1.176 (1863 calls, 151 cy)
                        rgbd=1.666 (1863 calls, 214 cy)
                        rest=2.968
```

**`rgbd` が `normal()` より重い。** 予想していたのは逆だった。

### 3. なぜ `rgbd` が 214 サイクルなのか

`rgbd` は 135 命令で、`garden_dither` を **2 回**呼んでいた（オブジェクトの再配置に
2 本の `callx8` が出る）。ソースを見ると 1 本目は死んでいる:

```c
int d=garden_dither(sh_px,sh_py);
if(sh_grain) {
    ...
    d=garden_dither(sh_px+(int)fx,sh_py+(int)fy);   // 1本目の結果はここで捨てられる
```

葉・草・花糸（`sh_grain`）の画素はすべて、**使わない dither を1回まるごと払っていた**。
加えて `garden_dither` は `garden.c` にあり、`flower.c` からは翻訳単位をまたぐ呼び出しになる。
中身は算術4つで、その周りに `entry` / `retw` と引数の受け渡しが付く。

### 4. 置いたもの

- 死んだ呼び出しを消した。`d` の代入を両方の腕へ1回ずつに分ける。**同じ式、同じ順序。**
- `garden_dither` の本体を `garden.h` へ `static inline` として移した。コピーではなく**移動**で、
  `garden.c` 側の2箇所もヘッダのものを使う。定数4本は1箇所にしかないので食い違えない。

どちらも画素を動かさない変更である。

### 5. 結果（実機 COM3、同じ種・同じショット・同じ計器つきビルド）

| | 前 | 後 |
| --- | ---: | ---: |
| `rgbd` | 1.67 ms（214 cy/call） | **1.09 ms（139 cy/call）** |
| `norm` | 1.18（151 cy） | 1.13（145 cy） |
| `rest` | 2.97 | 2.87 |
| `shade` | 5.81 ms（748 cy/hit） | **5.10 ms（651 cy/hit）** |
| `decor` | 9.5–9.9 | **7.9** |
| `total`（= PERF の `kernel`） | 31.9–32.6 | **29.2–29.4** |
| PERF `fps` / `draw` | 26.4–27.4 / 35.3–36.6 ms | **28.2–28.4 / 33.9–34.2 ms** |

> **2026-09-19 訂正。** この表の下2行（`decor` / `total` / `fps`）は
> **実行をまたいだ比較で、成立していない。** §9 で分かったとおり、この
> シーンの `decor` は種と view が同じでも位相で動き、`rays` は 4,700〜7,067 cy/行、
> `veg` は 8,768〜9,461 cy/行 と実行間で振れる。私の変更は `flower.c` の
> `rgbd` と `garden_dither` のリンケージだけで、`rays` の幾何には触れていない。
> したがって「kernel −2.7 ms、fps +1.5」は**取り下げる**。
>
> 成立しているのは**呼び出し単位・ヒット単位の行**だけである:
> `rgbd` 214 → 139 cy/call は、後続の別 run でも 138〜149 cy で再現し、
> 変更前の 210〜219 cy とは観測されるドリフト（±5%）よりはるかに離れている。
> `shade` 748 → 651〜676 cy/hit も同様。`garden_dither` のインライン化が
> `garden.c` 側の2箇所に効いたかどうかは、**測れていない**。

**ビット一致の証明**: `tools/flower_frame_dump.c` で HEAD のツリーと作業ツリーの
両方を建て、5 種 × 3 時刻 = 15 フレームを PPM で比較し**全バイト一致**。
`test_flower`（60 ポーズ × 全種）、`test_garden`、`tools/pie/test_kernels.py` も PASS。
実機は `smoke --cycles 20` / `test_settings` / `capture_home` / `kasane_input_device_test` PASS。
静的 DIRAM 増分 0、flash +328 B。

### 6. `__divsf3` と `sqrtf` は何をしているのか（逆アセンブル、2026-09-19）

`flower.c` のコメントは「`sqrtf` は88命令のソフトウェアルーチン、`normal()` はその上に
soft-float の除算を重ねる」と書いていた。**この石では両方とも違う。**

- **`__divsf3` は libgcc からリンクされていない。** map は
  `__divsf3 = 0x40002274` で、そこは ROM のサンク表。実体は ROM 0x40056124 で、
  **30 命令・分岐ゼロ**の FPU 列 —— `div0.s`（逆数シード）→ `nexp01.s` / `maddn.s`×13 /
  `mkdadj.s` / `addexpm.s` / `addexp.s` → 最後に `divn.s` 1 段。flash は 0 B。
- **`sqrtf` は 6 命令のラッパ**で、`__ieee754_sqrtf`（picolibc、flash 164 B）を呼ぶ。
  こちらは **33 命令・分岐ゼロ**で、`sqrt0.s` シード → 同じ Newton 部品 → やはり
  最後に `divn.s`。
- **`shade` 自身の算術も FPU 命令**である。688 命令のうち `madd.s` 55、`mul.s` 37、
  `olt.s` 17、`trunc.s` 13、`sub.s` 6、`msub.s` 5、`add.s` 4 で、
  **`__mulsf3` / `__addsf3` への再配置は1本も無い。**

ESP32-S3 の FPU は**単精度のみ**（`float f(a,b,c){return a*b+c;}` → `madd.s` 1本、
`double g(a,b){return a*b;}` → `call8 __muldf3`）。除算と平方根に1命令は無く、
あるのはシード（`div0.s` / `sqrt0.s` / `rsqrt0.s`）と Newton の部品だけなので、
`a/b` と `sqrtf` はその系列を回す関数への呼び出しになる。

#### そこから出た結論2つ

**整数平方根は遅い（実機 A/B、同一バイナリ、窓ごとに交互）。**
`g_flower_fixed_sqrt` は `flower_isqrt_q` が書かれて以来**ツリーの誰も反転させていなかった**ので、
一度も計られていなかった。交互に倒した3組:

| | `sqrt` | `bell:sqrt` |
| --- | ---: | ---: |
| `sq=0`（`sqrtf`） | 115–124 cy | 157 cy |
| `sq=1`（`flower_isqrt_q`） | **191–195 cy** | **178–179 cy** |

16 反復の整数ループ＋両端の float 変換が、分岐ゼロ 33 命令の FPU 列に負ける。
backlog のこの項目は「取り分が小さい」ではなく**逆方向**として閉じる。
スイッチと実装は残すが、既定 0 の理由は絵ではなく**速度**であるとコードに書いた。

**逆平方根はまだ開いている。** `1.0f/sqrtf(d)` は Newton 系列2本と窓呼び出し2回で、
`rsqrt0.s` なら系列1本・呼び出しゼロになる。平方根の合計支出は
`bell:sqrt` 1.55 + `norm` 1.13 + `sqrt` 0.68 = **3.36 ms（kernel の 11.5%）**。
正しい NR 系列は Xtensa ISA リファレンス側で、この石の TRM には無い。
**記憶で書かないこと** —— ただし ROM の除算列と picolibc の平方根列が手元にあるので形は借りられる。
書くなら実機で `1.0f/sqrtf(x)` と密に掃引比較し、最大 ULP を報告してから使う。

### 7. レジスタ間の移動は消せるか

`shade` の静的 688 命令のうち **`l32r` 175 + `wfr` 161 = 336（49%）が定数の実体化**である。
`l32r` がリテラルプールから汎用レジスタへ読み、`wfr` がそれを FPU レジスタへ移す。
ただし材質の if 連鎖は12本が排他なので、**実行される経路はこの半分以下**:
共通部分（`sh_grain` から局所座標まで）が 162 命令で `l32r`/`wfr` 29、
そこに材質1本（ROSE なら 17 命令・12）と最後の式（13 命令・4）が乗る。
**典型的な経路で約 216 命令・うち約 45 が定数の実体化（21%）。**
（`-O2` では行の帰属がスケジューリングで混ざるので、行ごとの数は目安である。）

消す機構は**ある**。`lsi ft, as, off`（出荷中のツールチェーンでアセンブル確認済み）は
メモリから FPU レジスタへ直接ロードするので、`l32r`+`wfr` の2命令が1命令になり、
汎用レジスタを経由しない。定数表をベースレジスタ1本で指せばよい。

**が、取り分は小さい。** 45 のうち半分を畳んで約 22 命令、`rest` の 368 cy に対して
おそらく 25〜40 cy、kernel 29 ms に対して **0.2〜0.3 ms（約1%）**。
手書きアセンブラで `shade` の算術を書き直す代償としては割に合わない。
同じ効果は C からも届く —— 材質ごとの係数を `static const float` の表にして
ポインタ経由で読めば GCC は `lsi` を出し、ついでに12本の if 連鎖も表引きになる。
そちらなら `flower_frame_dump` のビット比較で守れる。

### 8. 次に残っているもの

`shade` の内訳で残るのは `rest`（約 340〜370 cy/hit）で、実行経路 約190 命令に対して
約 1.9 cy/命令 —— 既知の良い列（`sqrtf` が 39 命令で 106 cy、2.7 cy/命令）と同程度なので、
**異常な待ちがあるわけではない。速くするには命令を減らすしかない**、というのが計器の答えである。

### 9. `lsi` を実際にやってみた結果（2026-09-19）: 差は出なかった

§7 の見積もりどおりに実装した。ROSE / VIOLET / BLUE / HERB / RED / INK と既定の
7 本の腕を、`longitudinal` に対して 1 次の係数表 `flower_materials[16]` へ畳み、
ポインタ経由で読む。GCC は期待どおり `lsi` を出した。

| | 前 | 後 |
| --- | ---: | ---: |
| `shade` 静的命令数 | 688 | **587** |
| うち `l32r` | 175 | 130 |
| うち `wfr` | 161 | 117 |
| うち `lsi` | 0 | **29** |
| flash 合計 | — | **−344 B**（表 576 B を入れてなお） |

**42 フレーム（全14種 × 3時刻）がビット一致。**

そして **時間は動かなかった。** 両アームを1バイナリに入れ、SPLIT の窓ごとに交互に倒して
同じ run・同じ位相で 7 対を取った結果:

| | `rest` 平均 | `shade` 平均 |
| --- | ---: | ---: |
| 表引き | 3.056 ms | 675.7 cy/hit |
| 従来の腕 | 3.064 ms | 677.1 cy/hit |

**0.2%。** 同じ run の中で `shade` は 665〜697 cy/hit を行き来しているので、
この差は分解できない。

理由の候補は2つあり、**どちらかは確かめていない**。
(a) 節約した命令が FPU の待ちに吸われていて、クリティカルパス上に無い。
(b) 測った種 0 は鐘を持つ種で、その花弁の多くは GOLD / CORONA / CHECKER ——
**表が覆っていない腕**を通る。つまりこの run は表そのものの薄いテストだった可能性がある。
USB 診断から種を固定する口が無いので、(b) を切り分けるには種を選べる probe が要る。

結論として**表は残す**が、理由は速度ではなく **flash −344 B と分岐6本の削減**である。
スイッチ `g_flower_material_table` は対照として残す。
そして §7 の「0.2〜0.3 ms 取れるはず」という見積もりは、**実測に否定された**。

### 10. この節で分かった測定上の落とし穴

**このシーンでフレーム単位の合計（`total` / `decor` / PERF の `fps`）を実行間で比べてはいけない。**
種と view を揃えても、`rays` は 4,700〜7,067 cy/行、`veg` は 8,768〜9,461 cy/行 と
位相で振れる。比べてよいのは**事象あたりに正規化された値**（`shade` cy/hit、
`rgbd` cy/call、`sqrt` cy/call）で、しかもそれでも実行間で ±5% は動く。
20〜70 cy の変化を主張するなら、**同一バイナリの窓ごと交互**しか手が無い ——
整数平方根（§6、115 対 193 cy）がきれいに出たのは、その手順を踏んだからである。

### 11. では何が重いのか（`rest` をもう一段割る、2026-09-19）

§9 が「命令を 22 本抜いても時間が動かない」と言ったので、命令数は答えではない。
`rest` を 3 つに割って残りを引き算した。種 0、14 窓、値は全窓でほぼ同一:

| 区間 | cy/hit | 割合 | 中身 |
| --- | ---: | ---: | --- |
| `other` | **169** | 25% | 前口上（V 2個 = 6 `wfr`）、材質のロード、分岐、**最後の9積3変換**、計器自身（約15 cy） |
| `norm` | **147** | 21% | `sqrtf` + `__divsf3` —— **不透明な呼び出し2本** |
| `rgbd` | **142** | 21% | ディザ —— **不透明な呼び出し1本** |
| `mat` | **126** | 18% | `light` と材質の腕（この種は鐘なので GOLD / CORONA の実算術） |
| `lite` | 59 | 9% | 法線から出る内積2本、`rim`、`spec` の16乗 |
| `geom` | 33 | 5% | Petal の局所座標（ロード＋内積2本＋積2本） |

（合計 676。`shade` の実測は 676〜697 で、計器の 5 括りぶん約 15 cy を含む。）

#### 読み取れること

**1. メモリは遅くない。** `geom` は Petal を DRAM から読んで内積を2本取って **33 cy**、
約 2.2 cy/命令。ロードが詰まっているという仮説はここで消える。

**2. 重いのは呼び出しである。** `norm` 147 + `rgbd` 142 = **289 cy、`shade` の 42%** が
3本の不透明な呼び出しに乗っている。呼び出しは3つのコストを同時に払う ——
本体、窓 ABI のレジスタ入れ替え（float は汎用レジスタで渡る）、そして
**スケジューリングの壁**。`geom` は `norm` と独立なのに、`sqrtf` を跨いで
重ねることができない。

**3. だから §9 で命令を抜いても動かなかった。** 命令列にはすでに待ちの隙間があり、
抜いた 22 本はその隙間に吸われていた。**スループットではなく遅延が律速**で、
効くのは「命令を減らす」ではなく「**壁を取り除いて独立な仕事を重ねられるようにする**」。

#### 順番に効くはずのもの

1. **`rgbd` をインラインにする（142 cy）。** → **やった。`shade` 643 → 529 cy/hit（−18%）。**
   `always_inline` を付けただけ。窓の往復が消え、ディザの整数ハッシュと最後の9積（FPU）が
   重なった。呼び出し overhead だけなら 20〜30 cy のはずで、**実測はその 3〜4 倍**
   —— §11 の「壁を取り除けば独立な仕事が重なる」がそのまま出た形である。
   1,870 ヒットで約 0.8 ms/フレーム。42 フレームがビット一致、DIRAM −32 B。
   今日の `shade` 全体では 740 → 529 cy/hit（−28%、約 1.6 ms/フレーム）。
   （この数字は同じ種 0 の cy/hit どうしの比較である。フレーム合計と fps は
   §10 の理由で比較していない。）
2. **`normal()` の逆平方根（147 cy）。** → **やった。効かなかった。§12。**
3. `other` の 169 cy のうち、最後の9積3変換がどれだけかはまだ分けていない。
   ただし 1 が効いた以上、その一部はもう回収されている。

**なお §11 の計器（`norm` / `rgbd` / `lite` / `geom` / `mat` の5括り）は外した。**
答えは出たし、括りだけで約 15 cy/hit 掛かる。表は上に残してあるので、
次に割りたくなった人はこのコミットを revert すれば同じ計器が戻る。

> **注意（§12 で分かったこと）。** 上の「643 → 529 cy/hit」は、`rgbd` のインライン化と
> **5括りの計器の撤去を同時に**行った差である。`PROF_FENCE` はコンパイラ障壁なので、
> 計器は自分の実行時間だけでなく**スケジューリングも止めていた**。
> したがって −114 cy のうちインライン化ぶんが何 cy かは**分離できていない**。
> 分けるには `always_inline` を切り替えられる2ビルドが要るが、それは §10 の理由で
> 実行間比較になり、この規模の差には使えない。**現状の正しい言い方は
> 「計器を外しインライン化した状態で 529、その前は 643」**である。

### 12. 逆平方根も効かなかった（2026-09-20）

§11 の 2 番を実装した。`shade` の法線だけに閉じた `shade_normal()` を置き、
`sqrtf` と `__divsf3` の呼び出し2本を、**呼び出しゼロ**の列
（よく知られた整数シード＋1/sqrt の Newton 2段、`rsqrt0.s` は使わない ——
正しい NR 系列が ISA リファレンス側にあり記憶で書けないため）に置き換えた。
近似は**色にしか効かない**: `normal()` は `flower_species.c` が花弁の軸を作るのにも
使っていて、そちらを近似すると幾何が動く。

- **精度（ホスト、実測）**: `d` を 1e-12〜1e6 で掃引して最大相対誤差 **4.71e-06**（約18ビット）。
- **絵（ホスト、実測）**: 14 種 × 3 時刻の 1,360,800 画素中 **11 画素（0.0008%）** が違い、
  最大 **8/255**（RGB565 の3ビットチャネルの1量子化ステップ、つまり丸めの境界）。
  §6 の整数平方根が謳っていた「0.0〜0.2%、うち数画素は大きく」より桁で良い。
- **速度（実機、同一バイナリの窓ごと交互、7 対）**:

| | `shade` 平均 |
| --- | ---: |
| 逆平方根 | 560.9 cy/hit |
| `sqrtf` + `__divsf3` | 557.9 cy/hit |

**差 3 cy。むしろ遅い。** 画素を 11 個動かして得るものが無いので**破棄した**。

#### これで分かったこと

`shade` から仕事を取り除く試みは**2回とも空振り**した ——
§9 で命令 22 本、ここで呼び出し2本ぶんの算術。一方、計器を外して `rgbd` を
インライン化したときだけ大きく動いた（§11 の注意書きのとおり、その内訳は未分離）。

**この関数はもう「中の仕事」では縮まない、と考えるのが素直である。**
`norm` の 147 cy という数字自体、括りの `PROF_FENCE` が周囲のスケジューリングを
止めた状態での値だった可能性が高い。次に `shade` を触るなら、測るのは
**関数の中身ではなく、呼ばれる回数**（1,870 ヒット/フレーム）のほうだろう。


**種の偏りに注意。** ここで測った種 0 は鐘を持つ種で、花弁の多くが GOLD / CORONA の
特別な腕を通る。`mat` の 126 cy はその実算術であって、§9 の表引きが覆う7材質では
別の値になるはずである。**診断から種を固定する口が無い**のが、いま一番効く道具の穴。

### 13. Fable の独立所見と、それで分かった記録の誤り（2026-09-20）

§1〜§12 の全材料（逆アセンブル、括りの表、3つの実験）を渡して Fable に独立に読ませた。
**逆アセンブルで確かめられる指摘が3つあり、3つとも私の記録のほうが間違っていた。**

1. **現在の `shade` は 988 命令**（`rgbd` が3箇所へ展開されたので 688 から増えている）で、
   `callx8` は **2本だけ**（`sqrtf` と `__divsf3`）。`<rgbd>` シンボルはもう無い。
2. **前口上は `wfr` ではない。** §11 の表は「V 2個 = 6 `wfr`」と書いたが、実際は
   `s32i.n a3,a1,4` → `lsi f0,a1,4` の**同一アドレスへの store 直後 load が3組**。
   構造体を値で渡す ABI の産物である。
3. **コンパイラはすでに鎖の隙間を埋めている。** `mul.s f1,f0,f0`（0xb）と
   `madd.s f1,f2,f2`（0x1b）の間に `l32r` / `slli` / `l32i` / `sub` が挟まり、
   `spec` の16乗（`mul.s f0,f0,f0`）は5命令おきに置かれて、その隙間に `geom` の
   `lsi` が入っている。`olt.s`→`bf` も10命令空いている。

#### 律速の読み直し

Fable の主張は **「重いのは呼び出しではなく、直列の依存鎖である。呼び出しは鎖を
直列化する箱にすぎない」**。§11 (2) の書き方は半分ずれている、という指摘で、
論拠が強い:

- **`callx8` は `PROF_FENCE` より強い障壁**である。in-order コアは callee 実行中に
  caller の命令を1本も進められない。したがって §12 の「fence が `norm` の 147 を
  膨らませた」という疑いは、**実験2（呼び出しを鎖に置き換えても動かない）を説明しない**。
  fence 説が正しければ、呼び出しを消した時点で周囲が重なって何かは縮んだはずである。
  縮まなかった以上、**置き換えた鎖のレイテンシが元と同じ長さだった**が残る説明。
- 実験1で消した `l32r`/`wfr` は**鎖に乗っていない詰め物**で、消しても鎖は縮まない。
- 実験3が動いたのは、呼び出しを消したことで**独立な整数ハッシュが鎖の隙間へ入った**から。

#### 欠けている数字

この読みを確定させるのは、**この石での依存発行レイテンシ** —— `mul.s`/`madd.s` の
依存、`mul.s`→`rfr`、`trunc.s`→`rfr`、`olt.s`→`bf`、`wfr`→`mul.s`、`lsi`→`mul.s`、
`s32i`→同アドレス `lsi` の8ケース。**リポジトリにも esp32s3-hw-mcp の実測にも無い**
（あちらは PIE 命令のみ）。測り方は既にある —— 依存2000命令と独立2000命令の差を取る、
`experiments/pie-timing` と同じ流儀。

#### 手順として残す2つ

1. **切除アーム（ablation arm）を先に流す。** 領域 X を最適化する前に、X の*結果*を
   定数に置き換えたコピーを同一バイナリに置いて窓交互で測る。得られるのは
   **回収可能コストの上限**で、0 なら X のどんな実装も効かない。
   **実験2は偶然これになっていた**（法線の呼び出し2本の回収可能コスト ≈ 0）。
   §7 の見積もりが §9 で否定された類の空振りは、これを手順にすれば事前に潰せる。
2. **`noipa` のコピーで、コンパイル時の選択を実行時に切り替える。**
   `shade_inl` / `shade_call` / `shade_prof` を `__attribute__((noipa))` で作り
   （`-O2` の `-fipa-icf` が畳むので `noipa` は必須）、呼び出し側をグローバル1本で選ぶ。
   これで `always_inline` の帰属も括りの歪みも、実行またぎ無しで割れる。
   残る交絡は配置だけで、定義順を入れ替えた2本目で符号が変わらないことを見れば足りる。

#### 規模感

`shade` は 529 cy × 1,870 hit ≈ **4.1 ms**、kernel 約29 ms の 14%。
ミス側（`ray_row` の約6,900 visit のうち約5,000が miss）は 2.6〜4.0 ms で**同じ大きさ**、
しかも `depth` とシルエットのビット比較で守れるぶん**証明が易しい**。
`decor` 7.9〜9.9 ms と `veg` 約5 ms はそれぞれ `shade` より大きい。

## 行の中にあった、行に依存しないもの（2026-09-20）（旧 flower-row-invariants.md）

5体のエージェントが各モジュールを独立に調査し、候補をFableに査定させた結果の実装記録。
Fableは5体の候補を並べ替えたうえで、**誰も出さなかった2つを足した**。その2つが大きく、
5体が最上位に置いた候補は実機で測定限界以下だった。

数字はすべて実機実測。ホストの比率はホストの比率として別に書く。

### 1. 植生の行不変量を1フレーム1回に（採用、−3.3〜3.7 ms/frame）

`garden_vegetation_row` は135行すべてで、幹ハッシュ3本、樹冠ハッシュ7本と
`garden_motion` 7本と `garden_recip` の除算7本、草ハッシュ18本と `garden_noise` 18本と
`garden_motion` 18本を計算し直していた。**どれもyを含まない。** `GardenVeg`（garden.h）に
移し、`garden_prepare_layout` で1フレーム1回取る。

スロットが2本あるのはレイアウトのクロスフェードが同じ行で2つのseedを描くから
（`garden_row_blend`）。1本だと、すでに倍の仕事をしているその行でだけ総入れ替えになる。
prepareが見ていないseedを渡された呼び出しはスタックに derive して落ちる — 古い値段であって
間違った絵ではない。

| | veg cy/row | veg ms/frame |
|---|---|---|
| 行ごとに再計算（`g_garden_veg_hoist=0`） | 11,219 | 6.30 |
| 1フレーム1回（出荷） | 4,636 | 2.61 |

fritillaria、非クロスフェード窓のみ、同一バイナリの交互窓。valley/daffodilでも同順
（10,163 → 4,325）。**2.4倍。**

ピクセルは動かない: seed対24 × mix 5段 × 時刻4 × 全行 = 1,550万画素がクロスフェード込みで
ハッシュ一致（`g_garden_veg_hoist` の両側で）。

代償は `GardenFrame` が448 → 1,244バイト。`tools/test_garden.c` の
`sizeof(GardenFrame)<=512` を1280へ上げた。**この上限を上げるのは2度目で、1度目は機能が
フレームカウンタに耐えず512へ戻っている** — 今回は先に実機の数字が出てから上げた。
静的DRAMは±0（この構造体は解放可能なflowerシーンブロックの中にある）。

### 2. bellの高さ窓を平方根なしで事前判定（採用、−1.9 ms/frame）

`bell_hit` は6本の緯度帯それぞれで判別式と平方根を取り、根を得てから
`v=o[1]+d[1]*z` が帯の `[lo,hi]` に入るかを見て捨てていた。ホストの census
（`tools/test_bell_reject.c`）はこれを **訪問あたり判別式≥0が4.50〜5.35回に対し採用1.00〜1.11回**
と数える。平方根の8割は計算して捨てている。

根が帯に入るかは平方根を取らずに決まる。`v` はzについて affine なので `[lo,hi]` はzの区間
`[zl,zr]` であり、`f(z)=a z²+2b z+c` がそこに根を持つのは

- `f(zl)` と `f(zr)` が異符号、または
- 同符号で、頂点が区間内にあり、そこでのfが零の向こう側

の場合に限る。頂点判定に除算は要らない: `f'(z)/2 = a z + b` が区間で符号を変えることと
`-b/a ∈ [zl,zr]` は同値なので `(a·zl+b)(a·zr+b) ≤ 0` でよい。零の向こう側という条件は
aの符号によらず `a·f(zl) > 0` にまとまる。`disc≥0` は直前の行で既に手元にある。
`p->inv_d1` は `petal_reciprocals` にある。余裕（slack）はゼロ。

ホスト: 14種 × 24位相で **生き残った平方根 554,942本のうち 461,888本（83.2%）が不要、
誤棄却0本**。

実機（daffodil、同一バイナリの交互窓）:

| | bell cy/visit | bell ms/frame | うちbell:sqrt ms |
|---|---|---|---|
| 全帯で平方根（`g_bell_window=0`） | 832 | 6.39 | 2.09 |
| 高さ窓あり（出荷） | 580 | 4.47 | 0.39 |

**bell_hit内の平方根時間が81%減。** ホストが言った83.2%とほぼ一致していて、これが
「ホストの比率が実機の時間に翻訳された」と言える数少ない例になっている。

### 3. `garden_mix` を強制インライン（採用、−0.18 ms/frame、ただしビルド間比較）

Fableは5体のうち1体が持ち出した `rgbd` の類推を否定した — 整数の中の整数には埋めるべき
浮動小数点ストールが無いので、取れるのは `l32r`+`callx8`+`entry`+`retw` だけ。その通りだった。

veg cy/row（fritillaria、veg巻き上げ有り、非クロスフェード窓）:
インライン 4,636（4,535〜4,799、12窓）/ 呼び出し 4,952（4,885〜5,019、5窓）。
**−316 cy/row = −0.18 ms/frame。**

インライン化は実行時に切り替えられないので**これだけはバイナリ間比較**で、
CLAUDE.mdが言う命令キャッシュ配置の15%の中に入る差である。両ビルドの分布が重ならない
ことと、`veg=0` 側でも同じ向き・同じ大きさ（11,219 → 11,484）に動いたことが根拠の全部。
0.18 msを主張の強さ以上に使わないこと。

### 4. 楕円体スパンの当たり区間（実装し、測定し、外した）

5体すべてがこれを最上位に置き、Fableも1位に据えた。代数は正しい:

```
d(dx) = (q4²−q0q2)dx² + 2(q4q5−q2q3)·dy·dx + (q5²−q1q2)dy² + q2
```

`Q = Σ_j (1/r_j²) b_j b_jᵀ` は正定値なので (x,z) 主小行列式 `q0q2−q4²` は厳密に正、
つまり先頭係数は厳密に負。下に凸でない放物線が非負になるのは**単一の閉区間**か、
どこでもないかのどちらか。両端点は同じ根号を共有するので**平方根は1本**で足りる
（調査側は2本と書いていた。Fableの訂正）。

ホストの検査は期待通りだった: 14種 × 24位相で **楕円体の画面ボックス2,633,607画素のうち
1,076,747画素（40.9%）が walk せずに済み、取りこぼし0**、56フレームがピクセル完全一致。

**実機では何も起きなかった。** span の cy/visit は窓ごとに122〜138を行き来し、
`g_span_narrow` の0/1と相関しなかった。理由は後から見れば明らかで、
**この区間が取り除くのはループの中で最も安い画素**だからである — 除外される画素は
`b`,`c`,`d` を作って `if(d<0) continue` するだけの十数サイクルで、
40.9% × 8,000画素 × 約18サイクル ≈ 0.25 ms。一方この区間自身が
petal×row ごとに平方根1本と除算1本（約270サイクル × 538 = 0.6 ms相当の母数）を払う。
差し引きはゼロ付近で、`span` の窓間の揺れの下に沈む。

**「40.9%の画素を落とす」は本当で、「40.9%速くなる」ではない。**
Fableが唯一「未計測の穴」として挙げていたのが、まさにこの
「生き残った訪問のうち深度で負けるものの割合」＝除外される画素が何をしていたか、だった。
穴のほうが正しかった。

コードとその専用テストは外した。残すのは上の代数と、この段落である。

### 何が分かったか

- **5体が独立に同じ候補を1位にしても、それは正しさの証拠にならない。**
  このリポジトリで3体が一致して全員間違った例（PIEの索引ロード）に1件加わった。
- **Fableが足した2つが、5体が挙げた5つの合計より大きかった。**
  差は視点で、5体は「この関数の中で何が重いか」を見て、Fableは
  「この値はなぜここにあるのか」を見た。行不変量も捨てられる平方根も、
  重い命令ではなく**置き場所の間違い**である。
- **ホストの比率は実機の時間ではない。** 同じ日に、83.2%が81%として現れ（bell）、
  40.9%が0%として現れた（span）。違いは落とす対象の単価で、それはホストの比率が
  決して含まない情報である。

## ray_row の行ごとの span 狭め（棄却。楕円体の当たり区間）（旧 tools/flower-rejection/README.md）

Preserved 2026-09-08 after `git checkout -- main/scene/flower.c` discarded it.
It first went to `.cache/preserved/`, which is gitignored and would not have
survived `git clean -xdf`; it is tracked here instead, which is the only place
on this machine that actually is durable.

### What it is

Substituting b and c into `d = b*b - q2*c` leaves a quadratic in `dx` whose
three coefficients are constant along a row, so the columns where an ellipsoid
can be hit are one interval, found with a single sqrt per petal-row instead of
a discriminant per pixel.

`rejection.c.frag` replaces the `for(int x=p->xmin;x<=p->xmax;x++)` line in
`ray_row`. Bells keep the full span: `bell_hit` intersects six conical bands and
its miss set is not one interval in dx.

### What was established, and is not worth re-establishing

- **Bit-exact.** 280 frames x 7 species, 0 differing pixels, max channel step 0.
  The per-pixel `d<0` test still runs; the interval is widened one column each
  side so float rounding in the closed form cannot drop a column the exact test
  would have kept. `test_bitexact.c` is the harness and it is the expensive part.
- **Removes 27-50% of per-pixel visits** (`measure_visits.c`): snowdrop 50.2,
  valley 43.3, sunflower 40.4, daffodil 39.6, crocus 35.8, tulip 33.3,
  calla 27.0. One sqrt buys 5-7 removed visits.
- Ellipsoid `d<0` misses are 51-72% of visits; bell misses 59-64%.

### Why it lost, and what would change that

On device it cost 20 ms of `kernel=` (66.1-67.1 -> 84.7-87.2, fps 11.6 -> 9.5).
The visits it removes are the cheap ones -- the ones that fall out on `d<0`
immediately -- and one sqrt plus one reciprocal per petal-row cost more than
they did. On x86 it was a wash, which is why the host could not decide it.

It was measured against a specific balance, and two parts of that balance are
changing:

1. 34.5 ms of garden pixel loop sat in front of it. After the PIE kernel that
   becomes ~3 ms, so ray_row's share of `kernel=` roughly doubles.
2. `ray=` did not track (it stayed flat while `kernel=` rose 20 ms), so the
   30-42 ms figure it reported was wrong; the corrected estimate is ~13 ms.

Re-measure with `kernel=` alone, which has never contradicted itself. The
number to beat is a fall in `kernel=`; anything else is noise at this size.

### Measured 2026-09-08, against hits rather than visits: it is worth ~0.2 ms

Both sides of the paragraph above are wrong, and the arithmetic that settles it
did not need the board.

`ray_row` was measured at 30.25 ms with **6,600 visits and 1,957 hits** a frame,
so a hit costs about **3,400 cycles** and a `d<0` miss costs about **18** -- the
miss path is `dx`, `b`, `c`, `d` and a compare, all hardware-FPU multiply-adds
with no sqrt in them. Removing 27-50% of the visits therefore removes

    6,600 x 0.4 x 18 = 43,000 cycles = 0.18 ms

out of 30 ms. **A reduction in visits is not a reduction in time.** The visits
this file removes are the cheapest pixels in the function, and the time is in
the 1,957 that are not removed at all.

The 20 ms it was measured to *cost* does not survive either. `measure_visits.c`
counts 186-1,178 ellipsoid part-rows a frame; the fragment adds one `sqrtf`, two
float divides and two roundings to each. Even at a pessimistic 1,000 cycles that
is 1.7 ms, not 20. So the old device figure is out by a factor of ten and the
explanation attached to it ("one sqrt cost more than the visits it removed") is
right in direction and out by two orders of magnitude in size: the real trade is
about 0.2 ms against about 0.3 ms.

Both numbers came from the `ray=` counter during the period when it was known
not to track. Nothing here is worth re-running; it is kept because the closed
form is correct and someone may want it for a different reason.

The include paths were also broken -- `../main/` resolved to `tools/main/` after
this directory was created around the files, so neither program had been
runnable since the day they were preserved. Fixed, and `measure_visits.c` now
also reports part-rows, which is the number that made the cost side decidable.
`test_bitexact.c` still does not build: it includes a generated
`flower_ref_body.h` that was never preserved with it. Left alone, because the
thing it would prove -- that the narrowing is bit-exact -- is not in doubt and
is not worth 0.2 ms.

**Where the time actually is:** `shade()` and the two software `sqrtf` calls per
hit. `sqrtf` on this target resolves to an 88-instruction Rust routine behind a
two-level call, and `normal()` adds a soft-float divide to it.

## FLOWER の実測内訳（2026-09-19、home 静止、`sq=0 gate=1 tweaks=1 canopy=1`）（旧 backlog.md）

次に何を狙うかはこの表から選ぶ。出所は実機 COM3 の `garden: SPLIT` / `SPLIT2` / `SPLIT3`
（60 フレーム窓、ms/frame）。種とカメラのショットで動くので、2 つの窓を併記する。

| 項 | 種 10 / 29.9 fps | 種 13 | 内訳 |
| --- | ---: | ---: | --- |
| total（= PERF の kernel） | 21.60 | 23.71 | |
| ├ pixels（PIE ベクタ半分） | 6.36 | 6.45 | 既に PIE |
| ├ decor | 6.50 | 7.36 | veg 4.18 / rays 2.11 / rest 0.20 |
| └ ray（花そのもの） | 8.74 | 9.90 | span 7.31 + scan 1.13 + pre 0.11 + rest 0.19 |
| 　└ span の中 | | | shade 3.75（1,440 hits、624 cy/hit）、sqrt 0.63、visit 残り 2.90（6,834 visits × 102 cy） |

**単項で最大は `shade`**、次が `veg`（135 行 × 7,431 cy/行）。
`shade` は 2026-09-19 に初めて割った —— [flower-perf.md](flower-perf.md)。
死んだ `garden_dither` 呼び出しを消し、`garden_dither` をヘッダの `static inline` へ移して
**kernel 32.0 → 29.3 ms（種 0、−8.4%）、fps 26.4–27.4 → 28.2–28.4**。
残りは `shade` の `rest`（約 370 cy/hit、依存チェーン待ちかロードかは未測定）と
`norm`（145 cy、逆平方根の Newton 系列で上限 0.9 ms）。`veg` はまだ割られていない。
`visits` の 79% は miss（6,834 visits に対し 1,440 hits）。
