# PIE 最適化の一本化 — この枝に入っているものと検証状態

枝 `perf/mp3-fir` の読み方。目的は**散らばった PIE 最適化を一本にまとめ、成果を一覧できるようにすること**で、
この文書はその一覧と、**どこまで検証したか**の記録。

**この文書を書いた環境に実機は無い**（`/dev/ttyACM0` 不在）。したがってここに新しいミリ秒・fps の主張は
無い。数字はすべて**引用**で、出所（誰が・いつ・どの枝で）を付ける。この枝自身の実機計測はまだ無い。

---

## §1 この枝に入っているもの

基点は `origin/vm/main`（実機が走らせているファーム系統、bd0fa43）。そこへ4つの系統を載せてある。

| 系統 | 入っているもの | 主なファイル | この枝での検証 |
| --- | --- | --- | --- |
| vm/main | 実機が走らせているファーム本体（VM の L0〜L2、pocket、UI、scene） | `main/` 全体 | ファームのビルド rc=0（§2）。**実機での確認はこの枝では未** |
| MP3 FIR の PIE カーネル | `fir8_pie`（8出力をまとめる 174 命令/8出力、0 ストール）と補間の int32 化 | `main/pocket/fir_pie.c`, `fir_pie.h`, `mp3_decode.c` | ホスト: `test_kernels.py` の `FirKernel` 緑、実 MP3 10 本で `block==scalar`（§2） |
| garden の装飾光線 PIE（`perf/pie-opt` の9本） | 8レーン混合 `garden_decor_mix8`、グループ幅の実行時スイッチ（既定8）、行の整列位相グリッド、`SCENE_AB` の腕 | `main/scene/garden_decor_pie.c/h`, `main/scene/garden.c`, `main/ui/shell.c`, `tools/test_garden_decor_pie_ab.c` | ホスト: 契約スイープ緑（§2）。実機の幅4→8の効果は §3 の引用（T2 着地時） |
| 容量削減（`perf/size-census`） | cref census と削減手順（「我々だけが理由でリンクに入っている」ところから潰す） | `docs/perf/builtins-census.md`, `docs/perf/flash-size-method.md`, `tools/size/` | 手順と計器のみ。**削減そのものは未実施** |
| システム API の調査（`perf/system-pie`） | システムAPIに PIE の余地があるか／libgcc と 64bit 除算の出所 | `docs/perf/system-pie-survey.md`, `docs/perf/libgcc-64bit-division.md` | 分析のみ（実装なし） |
| FLOWER / board（`perf/flower-decor` の性能19本） | 下の §4。canopy カーネル、装飾プロファイルの support gate、整数平方根、board の転送キュー | `main/scene/canopy_pie.c/h`, `main/scene/fixed_sqrt.h`, `main/scene/flower.c`, `main/scene/garden.c`, `main/hal/board.c/h`, `main/ui/shell.c`, `tools/flower_*`, `tools/pie/models/canopy_model.c`, `tools/pie/models/disc_model.c`, `docs/flower-*.md` | ホスト: byte 一致の系を含めて緑（§2）。実機の数字は §3 の引用（incoming の計測） |

`perf/flower-decor` が持ってきた文書は `docs/flower-decor-cost.md`・`docs/flower-optimisation-options.md`・
`docs/flower-fixed-point-pipeline.md` の3本で、**`docs/` の直下に置いてある**。この枝の `docs/` は
2026-09-15 に「主線3本＋参照5領域」へ組み替え済み（`docs/README.md`）で、そこでは同じ内容が
`docs/perf/pie-simd.md` §2・§7 と `docs/scenes/flower.md` に吸収されている。動かしていないのは、
3本が互いを `docs/flower-decor-cost.md` の名前で参照しているため（`docs/README.md` の「ファイル名は
動かさない」と同じ理由）。索引は `docs/README.md` に張ってある。

---

**三角関数の LUT 化（`perf/trig-lut` の2本、2026-09-16 追加）**: `main/scene/fxmath.c` を
65 エントリ表＋5次補正（`fx_core` **194 命令**）から **Q31 218 エントリ表＋2次補間（`fx_core` 137 命令、
−29%）** へ。`fx_tanf` の除算（soft-float の `__divsf3`）もビットトリック種＋Newton 3 回の逆数近似に
置き換え、**`scene/fxmath.c.obj` の未定義シンボルは 0**（fxmath.o からの libcall が消えた）。
代償は表 260→872 B（bin +496 B）。精度は libm 比 0.957 ulp（Q31 の床で、表を 2,275 エントリに
しても 0.987 … 表では多項式の 0.27 に届かない）、シーン 84,661,200 画素の比較で**差は 14 画素**
（flower 5 / solar_sail 9、すべて 1 段の陰影差、輪郭の移動 0）。線形補間では同じ規則
（動く画素 ≤0.1% かつ最大段差 ≤1）に入らず、9,104 B/2,276 エントリでも 32 画素・段差 11 だったため
**2次補間が採用条件**。掃引表と決定の記録は [trig-lut.md](trig-lut.md)、生成器は `tools/gen_fx_lut.py`。

**VM の中断・復帰（`codex/vm-improve` の3本、2026-09-16 追加）**: QuickJS VM の中断と遅延復帰
（L2c stage 3a）＋ `tools/vmtest` の整備（segment growth / tco / async audit / build failures /
callbench / device runaway / runner retry、`vmrun.c` の再試行）。`main/` は CMakeLists・
Kconfig.projbuild・app_session・main.c・pocket_app・vmprobe の6ファイル。この枝での検証は
ビルド rc=0 と `tools/vmtest/run.sh --variant o2` の **68 passed / 0 failed**（bin +512 B、
実機は未確認）。設計と実測の記録は `docs/vm/vm-L2-design.md` / `vm-L2-results.md`。

## §2 この枝で実際に走らせた検査（実機なし・2026-09-16）

ファーム:

```
. /opt/esp-idf/export.sh && idf.py -B build_all build
```

- **rc=0**。`cardputer_pocketjs.bin` = **2,146,272 B**（0x20bfe0、app パーティション 3 MiB の 32% 空き）。
- `memlog`: DIRAM **123,340 B** / flash **1,560,388 B**（`--only-changes` の前回記録は c66f401）。
- ビルド後に `git checkout -- dependencies.lock`（ビルドが触る）。

ホスト（すべて gcc、`main/scene/*.c` を直接リンク）:

| 検査 | 結果 |
| --- | --- |
| `python3 tools/pie/test_kernels.py` | **9 tests OK**（11.1 s）。`decor: 0/544 pixels differ`、canopy・fir の8レーンも一致 |
| `python3 tools/pie/run_models.py` | **全モデル一致**（ocean / wave / blend / accel / garden / fir / canopy / disc）。disc の掃引は 66,502 ペアで整列比較の誤判定 0（素朴な飽和語は 2.71% 誤り） |
| `python3 tools/pie/stalls.py`, `test_piesim.py` | rc=0、9 tests OK |
| `bash tools/test_mp3.sh /tmp/mp3fix/*.mp3` | **MP3_OK 10/10**、`fir=block==scalar`（実 MP3 10 本で PIE FIR == スカラー）。出力ハッシュは前回記録 `/tmp/mp3fix/after.txt` と一致 |
| `tools/test_flower.c` | **FLOWER_OK**（60姿勢×14種、strip 等価、block 再利用、LOGFMT_OK）。※既定の判断は §4-3 |
| `tools/flower_decor_identity.c`（5アーム） | **default / OLD / GATE0 / TWEAK0 / PIE0 が byte 完全一致**。120 フレーム × 135 行 = 7,776,000 B、md5 `4c5945f44b9c…`（incoming の byte proof をこの枝で再現） |
| `tools/test_garden_decor.c` | `GRAZING_OK … protected core unchanged`（芯の不変条件）、`DECOR_OK` |
| `tools/test_garden.c` / `test_garden_random.c` / `test_garden_transition.c` | `GARDEN_OK` / `GARDEN_RANDOM_OK` / `TRANSITION_OK` |
| `tools/test_garden_decor_pie_ab.c` | `DECOR_PIE_AB_OK`（契約は全呼び出しで成立、行では 129,600 画素中 16 画素、緑が最大1段） |
| `tools/flower_isqrt_norm_test.c` | 16bit/32bit/Q14 全域で相対誤差 6.1e-5（**14.0 exact bits**）、単調性 0 違反 |
| `tools/flower_sqrt_lanes.c` | 16bit レーンは B<=8、32bit レーンで B<=14（**B=8 が白画素の出る点**という incoming の説明をこの枝で再生） |
| `tools/flower_sqrt_variants.c`（xtensa `-O2`、`objdump` の静的カウント） | `sqrt_restore` 22 / `sqrt_restore_nobranch` 27 / `sqrt_restore_unroll3` 35 / `sqrt_newton` 51 / `sqrt_restore_unroll16` 92 命令 |
| `python3 tools/test_flash_budget.py` | rc=0 |

`tools/flower_sqrt_lanes.c` は `-Wall` で `-Warray-bounds` を1つ出す（8レーンを `uint16_t` に
memcpy する意図的な型パンチ）。出力は正しく出るが、既知の雑音として記録しておく。

---

## §3 実機で測られた数字（すべて引用。この枝では未再測）

出所は `perf/flower-decor` の各コミットメッセージと、そこで更新された `docs/`。
計測は 2026-09-15、dj-oyu、Cardputer ADV（vm-L1-60-g8779e3e 系のバイナリ）。同じバイナリの中で
スイッチを隣接窓ごとに反転させ、対照（触っていない項）を同時に出す方法。

| 項目 | 実測値 | 出所 |
| --- | --- | --- |
| board の転送キュー（非同期化） | **−6.3 ms / 44.7 ms のフレーム**、`async=` を1窓ごとに反転 | `perf/flower-decor` e282c8e（2026-09-15） |
| canopy の PIE カーネル | **差なし**（decor −0.42 ms、対照 veg −0.75 ms、154 ペア）＝「exact, free, no speed」 | fe5a1b8、`docs/perf/pie-simd.md` §7 |
| 装飾プロファイルの support gate | **差なし**（rays 平均 −0.19 ms / 中央値 −0.03、対照 veg −0.09 ms）。プロファイル評価は 4,304,780 → 2,772,750（−35.6%、12,767/フレーム）。両アームは 120 フレーム byte 一致 | aef8636、`docs/flower-decor-cost.md` |
| support 判定の unsigned 1 比較＋canopy の恒等画素スキップ | rays **平均 −1.46 ms** / 中央値 −1.02、decor −1.31 / −1.15、対照 veg +0.02、rest +0.13（46 ペア）。デコレータループ 143 命令のうち 14% | 1ec48bd、`docs/perf/pie-simd.md` §7 |
| 固定小数点平方根（B=8） | **時間は動かない**（ellipsoid +0.03 ms、bell −0.08 ms、対照帯 0.3〜0.4 ms、79 ペア）。絵は **0.02% の画素**が動く（B=14 で消える。`tools/flower_sqrt_lanes.c` の C 節） | a3fd110、`docs/flower-decor-cost.md` |
| 装飾の内訳（`decor` を2つに割った） | rays **7.6〜13.7 ms/フレーム**（13,450〜24,300 cy/行）、vegetation 5.0〜5.9 ms、rest 0.24〜0.36 ms。両方とも命令数の床の約 2.4 倍 | 77e95c8、`docs/flower-decor-cost.md` |
| FPS メータの整形 | `fmt` 0.28〜0.32 ms/フレーム → **0.000**（FPS 表示が off のとき整形しない） | 77e95c8 |
| 装飾のグループ幅 4 → 8（T2 の着地） | 到達率 42% → 98%、幅4と幅8で 13.025% の画素が最大 33 動く（近似） | `docs/perf/pie-opt-plan.md` §9・§12 |
| カーネル命令数・ストール | canopy 39 命令・0 ストール（再スケジュール後）、decor 混合 73 命令/呼び出し | `docs/perf/pie-simd.md` §7 |

**この枝を焼いた計測はまだ1つも無い。** 上の数字はすべて incoming の枝での値で、一本化した
バイナリが同じ値を出すかは実機で確かめること（§6）。

---

## §4 flower-decor の性能19本をどう載せたか

範囲は `8779e3e..79ad62f`（`8779e3e` はその枝の ds ブロックの先端、`79ad62f` が先端）。
`git cherry-pick 8779e3e..79ad62f` を実行し、止まったところを commit ごとに捌いた。

**適用 16 本 / no-op 3 本**（no-op は incoming が既にこちら側の別の形で入っているもの）。

### 4-1. 衝突の裁き（両側の意図を残したところ）

| ファイル | 両立させたもの |
| --- | --- |
| `main/scene/garden.c`（装飾ループ） | こちら側の**グループ格子＋8レーンの混合**（整列位相、`g_garden_decor_group`、`garden_decor_mix8`）と、incoming の**support gate**（compact support の外を評価しない）を1つのループに統合。gate は「グループ先頭列で評価し、両方の reach の外なら `goto grp`」として実装（このループはプロファイルをグループ先頭でしか作らないため）。判定は incoming の **unsigned 1 比較**（`g_garden_scalar_tweaks` の切替つき）。**両アームが byte 一致することをホストで確認**（§2 の identity harness） |
| `main/scene/garden.c`（canopy） | こちら側の `garden_canopy_pixel`／カーネル切替／`garden_prof_canopy` カウンタを残し、incoming の `f==0` 恒等スキップ（こちらに既に移植済み）と重複しないよう統合。モデルへの引用だけ incoming の明示的な綴りを採用 |
| `main/scene/garden.h` | こちらの `garden_prof_canopy` と incoming の `garden_prof_vegetation/rays/dissolve`、`g_garden_decor_gate` を全部残し、両側が入れた**重複宣言は1本に整理** |
| `main/hal/board.c` / `board.h` | こちらの**転送キュー＋「未転送のパネルバッファへ直接バイトスワップ」**（109b4ea、7fd965f）を本体として残し、incoming の `board_async_get/set`（PERF が1窓ごとに反転する口）を追加。incoming の本体はまさにこちらの `g_board_swap_into=0` の腕なので、片側を選ばず両方を1バイナリに置いた |
| `main/ui/shell.c` | こちらの `SCENE_AB` の腕（canopy／swap-into／decor 幅のローテーション）を残し、incoming の `async=` の窓反転は `#if SCENE_AB` の反対側（＝既定ビルド）に置いた。SCENE_AB を on にしたときは上のローテーションが A/B なので、混ざらない |
| `main/CMakeLists.txt`, `main/scene/canopy_pie.h`, `main/scene/canopy_pie.c`, `tools/pie/models/canopy_model.c`, `tools/pie/test_kernels.py` | どれもこちら側が**後の改訂**（`31cad81`「クラッシュした版ではなく直したカーネルを積む」、`f3b54b4`「39 命令・0 ストールに再スケジュール」）を持っているので、こちらを採用。`canopy_model.c` はこちらが F 節（f 周りの混合・SAR16 の展開・飽和パック）を追加済み |
| `tools/pie/run_models.py` | こちらの `fir` と incoming の `canopy` の**両方の行**を残し、docstring の「all four」を七つに直した |
| `docs/flower-decor-cost.md` | こちらは 2026-09-15 の docs 組み替えでこの名前を `docs/perf/pie-simd.md` に吸収して**削除**していた（`DU` 衝突）。incoming の後続2本が実測をこのファイルに足す設計なので、パスごと復活させ、索引を `docs/README.md` に張った（§1） |
| `main/scene/flower.c` | こちらの `SPLIT`/`SPLIT2` と、incoming の `SPLIT3`（`gate=`/`tweaks=`/`sq=`/`canopy=`）を1行に統合。incoming の「他のスイッチは触らず1つだけ動かす」測定は、こちらの全スイッチ反転（隣接窓ペアで測る）と両立する形でコメントに残した |

### 4-2. no-op と判断した3本

| incoming | 判断 | 根拠 |
| --- | --- | --- |
| `6e12d10` pie: the canopy blend as a kernel | **no-op** | `8b61301` と **patch-id が同一**（同じ差分）。こちらの `canopy_pie.c` はその後 `31cad81` ＋ `f3b54b4` の改訂版で、incoming 側の版より新しい |
| `2257682` canopy kernel: fix the reset loop | **no-op** | こちらの `31cad81` が同じ修正（走る x ベクトルを q7 に持つ）で、`test_kernels.py` も既に n=1..5 の複数ブロックを回している（incoming が「これが無かったせいで見逃した」と言っている検査そのもの） |
| `79ad62f` board: raise the data/command line before queueing | **no-op** | こちらのキューパスは既に queue の前に DC を上げている（「DC high: these bytes are pixel data and not a command」の註つき）。incoming の本文は `79ad62f` 以前の形 |

`f951b22`（display-race の実験を戻す）は**コメントだけが入った**。incoming が外した計器（overlap カウンタ、
shell の per-frame `esp_log_level_set`）はこちらには元から無く、incoming の `flower.c` 側の変更は
**どの枝にも定義の無い `g_garden_ray_fast` を参照していた**（`git grep` で 79ad62f のツリー全体を
探して0件）ので、その参照は落とした。持ち込めばコンパイルが通らない。

### 4-2b. 持ち込みに伴う補修

- incoming の3文書と `main/scene/garden.c`・`tools/flower_sqrt_variants.c` が `docs/pie-simd.md` を
  指していた。この枝の docs 組み替えで `docs/perf/pie-simd.md` へ移動済みなので、**3箇所を張り直した**。
- `docs/README.md`（索引）に `docs/perf/pie-consolidation.md` と incoming の3文書の行を足した。
- `main/ui/shell.c` の競合捌きで `#endif` を1つ落としたまま1コミット進めてしまったので、
  `--fixup` でそのコミット（`board: queue the panel transfer…`）に畳んだ。ビルドはこの枝で rc=0 を
  確認済み（§2）。

### 4-3. 判断が要った1件（既定を変えた）

`a3fd110` は固定小数点平方根を**実行時スイッチにして既定 1**（B=8）にしていた。この枝では
**既定を 0（exact な `sqrtf`）に戻した**。根拠は3つ:

1. incoming の枝自身が `tools/test_flower.c` に赤い。79ad62f のツリーをそのままビルドして走らせると
   `test_flower.c:130` の解析的検査（`bell_hit` の返す深さが z=0.88±1e-4）で落ちる。こちらへ来る前から赤で、
   こちらの統合で直る種類のものではない。
2. 絵の差の原因が測られている: B=8 では約10有効ビットしかなく、シルエットで深さ判定が反転する画素が出る
   （`tools/flower_sqrt_lanes.c` C 節が B<=8 / B<=14 の差を出している）。
3. **性能上の根拠が無い**: 同じコミットの実機計測が「差は対照帯の1/10」（§3）で、incoming の
   メッセージ自身が「caller が整数を持ち回るまでは 0 にして sqrtf を出せ」と言っている。

算術（`main/scene/fixed_sqrt.h`）・ツール・スイッチ・A/B の flip はすべて枝に残っている。実機で A/B する
ときは `SPLIT3` の `sq=` が窓ごとに切り替わるので、そのまま flip で測れる。B=14 で試すなら
`-DFLOWER_SQRT_BITS=14`。

---

## §5 取り込まなかったもの

- **`perf/flower-decor` の先頭17本（`d4b1d73` から `8779e3e` まで）は入れていない。** 中身は
  ds / kasane（デザインシステム）の作業で、**別の主線**（`vm/design-contracts` で継続中）に属する。
  この枝の目的は「ファームの PIE 最適化を一本にする」ことなので、混ぜると系統が読めなくなる。
  入れたのは性能の19本だけ（§4）。同じ理由で、この枝は flower-decor を merge していない
  （merge すると17本が祖先に入る）。
- `perf/flower-decor` の docs を `docs/perf/` へは動かしていない（§1 の理由）。

---

## §6 実機で見るべき点

焼く前に見るもの（すべてこの枝で緑）: §2 の3層（`test_kernels.py` / `run_models.py` / host の
`test_*.c`）とファームのビルド。

実機（`/dev/ttyACM0`、ESP-IDF の Python 環境）:

1. **`PERF` 行の `async=`** — 転送キューとブロッキングが2秒窓ごとに入れ替わる。`send=` と `draw=` を
   隣接窓で対にする（incoming の −6.3 ms はこの形で測られた）。絵は両アームで同じはず。
2. **`SPLIT3` 行** — `gate=`（support gate）、`tweaks=`（unsigned 判定＋canopy 恒等スキップ）、
   `sq=`（固定小数点 sqrt、既定 0）、`canopy=`（PIE カーネル）、`rayfast=` は**無い**（§4-2）。
   1窓ごとに1つずつ反転するので、隣接窓で対にして、触っていない `veg`/`rest` を対照帯にする。
   `gate=` と `tweaks=` は「差なし〜−1.5 ms」、`sq=` は「差なし・絵が 0.02% 動く」、`canopy=` は
   「exact, free, no speed」が incoming の答え。
3. **`SCENE_AB=1`**（`main/ui/shell.c` のコンパイル時スイッチ）— canopy / swap-into / decor 幅の腕が
   12窓かけて回る。`AB arm=…` 行に窓の canopy サイクルが出る。
4. **物理的な表示確認** — 転送キューの正しさ（バイト列が変わらないという論証まで）と、
   `board_capture` が `pixels` を撮る位置の関係で**ソフトからは確かめられない**（MISO 未配線）。
   パネルに絵が出ることを目で見る。DC ライン（`79ad62f` の主題）もこの部類。
5. **`tools/capture_home.py`** — 実ピクセルと 30fps。`sq=1` の窓で白い画素が出るか（B=8 の既知の差）、
   decor の幅 8 の近似が帯に見えるか（`SCENE_AB` の幅の腕）。
6. **`python3 tools/memlog.py --map build_all/cardputer_pocketjs.map --port <port> --check`** —
   実機の空きヒープも記録して予算検査。
