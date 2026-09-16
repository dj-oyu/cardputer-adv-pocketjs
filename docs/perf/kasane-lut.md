# 直接ブレンド連鎖の量子化キー表（境界 4）

基点 `87eb92a`（`perf/kasane-opt`、`perf/kasane-lut` 上）。2026-09-15。
`docs/perf/kasane-opt-survey.md` の境界 4（per-pixel）の候補のうち、**画素ごとの連鎖を
「量子化したキーの表」に置き換える**版を実装し、測った記録である。

**時間は測っていないし、主張しない。** ホストに `rsr.ccount` は無い（`pie-simd.md` §6.7）。
ここにあるのは ①target `-Os` の `objdump` 命令数 ②`-finstrument-functions` で数えた
起動回数 ③全パラメータ掃引 ④120 フレームの画素比較、の 4 つで、実機の ms は
`g_ksn_blend_lut` を同一バイナリで切り替える A/B でしか言えない。

---

## 0. 結論

| アーム | 何を表にするか | 画素一致 | 既定 |
| --- | --- | --- | --- |
| `g_ksn_blend_lut`（solid） | RECT / ROUND_RECT / STROKE / `from == to` の GRADIENT の 1 色 | **厳密**（移動 0 画素、最悪段差 0。2.87 億比較＋120 フレーム一致） | **1（ON）** |
| `g_ksn_blend_lut_alpha`（alpha） | TEXT の被覆率で決まる実効 α を 16 段に量子化 | 移動する（24.5% の (a, 元値, 先値) で、最悪段差 2＝緑 6bit） | **0（OFF）** |

既定の規則は「最悪段差 ≤ 1 なら ON」。solid は 0 なので ON、alpha は 2 なので OFF。

**表は 4,112 B の `.bss`**（solid 2,048 B ＋ alpha 2,048 B ＋ 鍵 16 B）で、既存の decode
キャッシュ（`decoded` 5,824 B）に並ぶ。スタックフレームは `ksn_render_rects` **576 B のまま
増えていない**。DRAM は約 334 KiB、ホーム画面の空きヒープ実測 274 KiB（`CLAUDE.md:86`）、
PSRAM は無い（`docs/platform/hardware-constraints.md:14`）ので、表は内部 SRAM の `.bss` に
置くしかない — 4,112 B は空きヒープの 1.5% である。

**画素あたりの命令数は減る**（後述: 169→35、226→35、111→45）。ただし
`pie-simd.md` §4.6 の前例（命令数を削る最適化が 3 回続けて差を出さなかった）に従い、
これは「命令数の勝ち」であって「時間の勝ち」ではない。実機の判定は §7 の手順で。

**表が届かない画素が残る。** 表が効くのは「コマンド内で 1 色」の画素だけで、
per-pixel の色を持つ dither グラジエント（`demo.js` の `axis:'x'` の帯）は届かない。
120 フレームの実測では、両アームを入れても dither グラジエントの 4,320 画素/フレームは
旧連鎖のまま残る（§4 のカウンタ）。**「dither を表に焼く」案は、この描画器では
dither が付く唯一の命令（グラジエント）の色が x の関数だから、1 コマンド 1 色という
前提が成り立たない**、というのが測った答えである。

---

## 1. 何を表にするのか

`blend`（`ksn_render.c:532`）は画素ごとに呼ばれ、チャネルごとに見ると

```
a  = mul8(src&255, opacity)                     コマンド定数（TEXT だけ被覆率で変わる）
d8 = (dst の 5/6bit フィールド) を 8bit に展開   dst から（32 / 64 通り）
c  = (s*a + d8*(255-a) + 127)/255               s はコマンドの色（TEXT は RGB が定数）
out= dither ? quantize(c, 31/63, bayer) : (c>>3)|(c>>2)|(c>>3)
                                                bayer = bayer4[y&3][x&3]（16 通り）
```

の 3 つ（dst の値、コマンドの色と α、bayer しきい値）の純関数である。表はこのうち
**dst の値 32/64 通り**をキーにし、残りを 1 行（128 B = 赤 5bit 32 ＋ 緑 6bit 64 ＋
青 5bit 32）へ畳む。行の内容は `blend_lut_build_row()` が `blend` と同じ式で作るので、
実装がずれる余地は「行をいつ・どのパラメータで作り直すか」だけである。

* **solid アーム**: 1 コマンド 1 色のとき。RECT / ROUND_RECT / STROKE と、
  `from == to` の GRADIENT（`interpolate` は `(c*last + last/2)/last == c` なので全画素
  同じ色を返す）。dither が付くコマンドは 16 行（しきい値ごとに 1 行）を作るので、
  **dither は表の中に入る**。付かないコマンドは 1 行。
* **alpha アーム**: TEXT。RGB はコマンド定数だが実効 α が被覆率で変わるので、
  そこだけ 16 段に量子化して 16 行にする。

## 2. パラメータの量子化（正確な定義）

* **solid**: 量子化は無い。行の軸は bayer しきい値 `t ∈ [0,16)` そのもの
  （`bayer4` の 16 値ちょうど）で、実効 α `mul8(colour α, opacity)` と元チャネル値は
  ビルド時に厳密に畳み込む。行の値は `blend` の式そのまま。→ 移動 0 画素。
* **alpha**: 実効 α `a = mul8(mul8(colour α, coverage), opacity)` を
  `level = a >> 4` で 16 段に落とし、行は `a = level*16 + 8`（段の中央値、最大 248）で作る。
  したがって `|a - 行の a| ≤ 8`。8 bit のブレンド値の差は `8*|s-d8|/255 ≤ 8` で、
  5bit チャネルでは ≤ 1 段、6bit チャネル（緑）では ≤ 2 段。`a == 0` の画素は旧経路と
  同じく何も書かない（`blend` の早期 return と一致）。

## 3. 表の大きさ・置き場所・作成コスト

| 項目 | 値 | 出所 |
| --- | --- | --- |
| 行の大きさ | 128 B（32＋64＋32） | `KSN_BLEND_LUT_ROW` |
| solid 表 | 16 行 × 128 B = 2,048 B | `nm -S`: `blend_lut_solid` 0x800 |
| alpha 表 | 16 行 × 128 B = 2,048 B | `nm -S`: `blend_lut_alpha` 0x800 |
| 鍵（色・不透明度・dither） | 8 B × 2 | `blend_lut_solid_key` / `blend_lut_alpha_key` |
| 合計 `.bss` | **4,112 B** | 既存 `decoded` は 5,824 B |
| スタック | `ksn_render_rects` 576 B → **576 B**（変化なし） | `-fstack-usage` |

作成コスト（`objdump -d` のループ本体、`-Os`）:

| 何 | 命令数 | 行 128 エントリの実測換算 |
| --- | ---: | ---: |
| thin の 1 エントリ | 23 | 1 行 ≒ 2,950 命令 |
| dither の 1 エントリ | 52（`quantize` 19 ＋ 呼び出し 11 を含む） | 1 行 ≒ 6,656 命令、16 行 ≒ **106,500 命令** |
| alpha 表（thin 16 行） | 23 × 16 行 | ≒ **47,200 命令 / TEXT コマンド** |

再ビルドは「色・不透明度・dither」の鍵が変わったときだけ（1 エントリの鍵キャッシュ）。
帯ループは同じコマンドを 17 回通るので、全画面コマンドでもフレームあたり 1 回で済む。
120 フレームの実測で **solid 2,522 行 / 6,362 行（両アーム）**、すなわち約 21 行/フレーム
（PATCH で色が変わるたびに作り直す）。

**割に合う下限**（命令数の算術）: solid の thin は 22 画素、16 行の dither は約 560 画素、
alpha は約 720 インク画素（1 コマンドあたり）で作成コストを回収する。形状は普通これを
超えるが、**インクの薄い実フォントでは alpha は負けうる**（§5）。

## 4. 画素あたりのコスト（target `-Os` `objdump`）

「1 画素あたり＝本体ループ 1 回（`call8` を含む）＋ 呼ばれる out-of-line 関数の本体
命令数の和」で数える（`kasane-opt-survey.md` §5 と同じ数え方）。

| 経路 | 前 | 後 |
| --- | --- | --- |
| 直接 thin（RECT/ROUND_RECT/STROKE） | ループ 25 ＋ `sample` 57 ＋ `blend` 50 ＋ `pack565` 37 = **169** | ループ 19 ＋ `blend_lut_pack` **16** = **35** |
| 直接 dither（グラジエント） | 25 ＋ 57 ＋ 50 ＋ 37 ＋ 3×`quantize` 19 = **226** | 同上 **35**（bayer の行番号はループ内、dither は表の中） |
| TEXT | ループ 24 ＋ `blend` 50 ＋ `pack565` 37 = **111** | ループ 29 ＋ `blend_lut_pack` 16 = **45** |

`ksn_render_rects` 全体は 1,062 → 1,218 命令（両アームを 1 バイナリに入れた分）。
表引きは `sample` を呼ばない（色は表に入っている）ので、削れる分にはその 57 命令も入る。

120 フレームの起動回数（`-O2 -fno-inline -finstrument-functions -DKSN_COUNT_LUT`、
`blend` / `blend_lut_pack` / `blend_lut_build_row` の入口を**カウントした呼び出し地点**）:

| アーム | 旧連鎖の画素（`blend`） | 表の画素（`blend_lut_pack`） | 作った行 |
| --- | ---: | ---: | ---: |
| 0 = 両方 OFF（参照） | 1,258,651 | 0 | 0 |
| 1 = solid ON | 456,797 | 801,854 | 2,522 |
| 2 = solid ＋ alpha ON | 161,280 | 1,097,371 | 6,362 |

最初の全画面 REPLACE フレームだけを見ると: 17,070 → 8,798 ＋ 8,272（solid）→
4,320 ＋ 12,750（両アーム）。**両アームを入れても 4,320 画素が旧連鎖のまま残る。**
これは `demo.js` の dither グラジエント（240×18、`axis:'x'`）の画素数と一致し、
その色が x の関数である以上どの行にも畳めない、という測った理由である
（survey §5 の「dither 画素は 4,320」と同じ数）。

## 5. 近似の測定（移動画素と最悪段差）

`tools/kasane_contract/test_blend_lut.c`。すべて実測値で、主張ではない。

**solid（厳密であることの証明）**

* チャネルごとの全パラメータ走査: 実効 α 0..255 × 元チャネル値 0..255 ×
  thin ＋ bayer 16 段 × 先チャネル値 32/64 = **214,499,328 比較**、
  1,110,016 パラメータ。移動 **0**、最悪段差 **0**。
* コマンド相当のパラメータでの 16bit 全語走査: 不透明度 4 値 × 色 20 組 × 17 段、
  a==0 の行は仕様どおり旧経路に残る（1105 行）。**72,417,280 語**を `blend` と比較、
  移動 **0**、最悪段差 **0**。
* 120 フレーム × 3 アーム: solid アームはハッシュも画素も参照アームと**毎フレーム一致**
  （移動 0、段差 0、ロールハッシュ `1c401d77`）。

**alpha（16 段量子化の近似）**

* チャネルごとの全 (a, 元値, 先値) 走査: **8,355,840 比較**、移動 **2,047,578（24.5%）**、
  最悪段差 **2**（赤 351,746 / 緑 1,344,086 / 青 351,746 の比較で差が出る）。
* 120 フレーム × 32,400 画素 = 3,888,000 画素フレームで、移動 **124,460 画素
  （3.2%、120 フレーム全部で発生）**。段差 1 が 97,110、段差 2 が 27,350、段差 3 以上は 0。
  赤と青（5bit）の最悪は 1、緑（6bit）だけ 2 で、§2 の上界どおり。
* 被覆率経路の算術（`mul8(mul8(colour α, coverage), opacity)`）は 50,653 組で
  `blend` の計算と一致し、段の中央値との差は ≤ 8。

## 6. 判定できないこと（限界）

1. **時間**。命令数と回数だけである（`pie-simd.md` §4.6/§6.7）。
2. **インク密度**。ハーネスの TEXT 被覆率は合成フェイス（`ink()`）で、実フォントより
   濃い。alpha アームの画素数と作成コストの比は、実機の jpfont のインクで決まる。
   §3 の下限（約 720 インク画素/コマンド）はこの依存を明示するための算術である。
3. **帯ごとの再構築**。鍵が変われば作り直す。PATCH で 1 コマンドの色が変わるたびに
   1 行（thin 2,950 命令）を払う。実測で 21 行/フレームなので小さいが、色が
   毎フレーム変わるアニメーションでは効きが落ちる。
4. **グループ経路**。`group_over` の元画素はタイル（画素ごとに違う）なので表にできない。
   `demo.js` の 2 矩形と 2 インスタンスはここを通る。

## 7. 再現手順

```sh
XT=/root/.espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin
cd /workspace/pjs-kasane-lut

# 1. 画素一致・全パラメータ掃引・120 フレーム比較（アーム 3 本、1 バイナリ）
cc -std=c11 -Wall -Wextra -Werror -O2 -fstrict-aliasing -Imain/ui/kasane \
  main/ui/kasane/ksn_core.c main/ui/kasane/ksn_cache.c \
  tools/kasane_contract/test_blend_lut.c -o /tmp/blend-lut && /tmp/blend-lut

# 2. 起動回数（-fno-inline で表引きと旧連鎖を数える）
cc -std=c11 -Wall -Wextra -Werror -O2 -fno-inline -finstrument-functions \
  -DKSN_COUNT_LUT -Imain/ui/kasane main/ui/kasane/ksn_core.c \
  main/ui/kasane/ksn_cache.c tools/kasane_contract/test_blend_lut.c \
  -o /tmp/blend-lut-count && /tmp/blend-lut-count

# 3. 命令数・スタック・型サイズ（target）
$XT/xtensa-esp32s3-elf-gcc -Os -std=gnu11 -Imain/ui/kasane \
  -c main/ui/kasane/ksn_render.c -o /tmp/ksn_render.o
$XT/xtensa-esp32s3-elf-objdump -d /tmp/ksn_render.o | \
  awk '/^[0-9a-f]+ </{n=$2} /^ *[0-9a-f]+:/{c[n]++} END{for(k in c) printf "%6d %s\n",c[k],k}' | sort -rn
$XT/xtensa-esp32s3-elf-gcc -Os -std=gnu11 -fstack-usage -Imain/ui/kasane \
  -c main/ui/kasane/ksn_render.c -o /tmp/su.o   # ksn_render.su が出る
$XT/xtensa-esp32s3-elf-nm -S /tmp/ksn_render.o | grep ' b '

# 4. 既存契約。ASan はこの容器で完了しない（6 回に 1 回 DEADLYSIGNAL の無限ループ、
#    素の HEAD でも再現）ので、/tmp に写した run.sh から
#    -fsanitize=address,undefined の文字列だけ外して使う。リポジトリの run.sh は触らない。
#    写した先は /tmp なので、cd 行だけはリポジトリを指すよう置き換える（それ以外は無変更）。
sed -e 's/-fsanitize=address,undefined//g' \
    -e 's|cd "$(dirname "$0")/../.."|cd /workspace/pjs-kasane-lut|' \
    tools/kasane_contract/run.sh > /tmp/run-nosan.sh
bash /tmp/run-nosan.sh     # 全テスト PASS（exit 0）
```

実機では `g_ksn_blend_lut` を 0/1 で窓ごとに切り替え、`KASANE_PAINT` の `render_ms` を
対照欄と同時に出す（`pie-simd.md` §6.1/§6.2）。画素は両アームで一致するので、
差があればそのまま per-pixel 面の時間差である。
