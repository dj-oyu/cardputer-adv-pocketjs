# Kasane の565ブレンドの最適化（量子化表・256スケール・PIE カーネル）

> 吸収した旧文書: `kasane-lut.md`、`kasane-alpha256.md`、`kasane-blend-pie.md`。各節の中の節番号（§N）は旧文書のものを保つ。

ブレンド経路（`ksn_render.c`、PIE は `ksn_blend_pie.c`）に入れた3つの最適化。スイッチの既定値（`ksn_render.c` / `ksn_render.h` で確認）:

| スイッチ（マクロ） | 既定 | 性質 |
| --- | --- | --- |
| `g_ksn_blend_lut`（表の行は `KSN_BLEND_LUT_ROW`） | 1（solid の表、厳密） | |
| `g_ksn_blend_lut_alpha` | 0（TEXT の 16 段の表） | 画素が動く近似なので既定 OFF |
| `g_ksn_scale256`（ホスト検査は `-DKSN_SCALE256_ARM=1` で粗い腕を強制） | 0（厳密な /255） | 255→256 の粗スケールは測った結果で既定にしなかった |
| `g_ksn_blend_pie`（`ksn_blend8_pie`、`ksn_blend_pie.c`） | 1（厳密。8 画素単位の PIE） | 同一バイナリの実機 A/B で render の利益を確認して既定 ON |

## 直接ブレンド連鎖の量子化キー表（境界 4）（旧 kasane-lut.md）

基点 `87eb92a`（`perf/kasane-opt`、`perf/kasane-lut` 上）。2026-09-15。
`docs/perf/kasane-opt-survey.md` の境界 4（per-pixel）の候補のうち、**画素ごとの連鎖を
「量子化したキーの表」に置き換える**版を実装し、測った記録である。

**時間は測っていないし、主張しない。** ホストに `rsr.ccount` は無い（`pie-simd.md` §6.7）。
ここにあるのは ①target `-Os` の `objdump` 命令数 ②`-finstrument-functions` で数えた
起動回数 ③全パラメータ掃引 ④120 フレームの画素比較、の 4 つで、実機の ms は
`g_ksn_blend_lut` を同一バイナリで切り替える A/B でしか言えない。

---

### 0. 結論

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

### 1. 何を表にするのか

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

### 2. パラメータの量子化（正確な定義）

* **solid**: 量子化は無い。行の軸は bayer しきい値 `t ∈ [0,16)` そのもの
  （`bayer4` の 16 値ちょうど）で、実効 α `mul8(colour α, opacity)` と元チャネル値は
  ビルド時に厳密に畳み込む。行の値は `blend` の式そのまま。→ 移動 0 画素。
* **alpha**: 実効 α `a = mul8(mul8(colour α, coverage), opacity)` を
  `level = a >> 4` で 16 段に落とし、行は `a = level*16 + 8`（段の中央値、最大 248）で作る。
  したがって `|a - 行の a| ≤ 8`。8 bit のブレンド値の差は `8*|s-d8|/255 ≤ 8` で、
  5bit チャネルでは ≤ 1 段、6bit チャネル（緑）では ≤ 2 段。`a == 0` の画素は旧経路と
  同じく何も書かない（`blend` の早期 return と一致）。

### 3. 表の大きさ・置き場所・作成コスト

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

### 4. 画素あたりのコスト（target `-Os` `objdump`）

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

### 5. 近似の測定（移動画素と最悪段差）

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

### 6. 判定できないこと（限界）

1. **時間**。命令数と回数だけである（`pie-simd.md` §4.6/§6.7）。
2. **インク密度**。ハーネスの TEXT 被覆率は合成フェイス（`ink()`）で、実フォントより
   濃い。alpha アームの画素数と作成コストの比は、実機の jpfont のインクで決まる。
   §3 の下限（約 720 インク画素/コマンド）はこの依存を明示するための算術である。
3. **帯ごとの再構築**。鍵が変われば作り直す。PATCH で 1 コマンドの色が変わるたびに
   1 行（thin 2,950 命令）を払う。実測で 21 行/フレームなので小さいが、色が
   毎フレーム変わるアニメーションでは効きが落ちる。
4. **グループ経路**。`group_over` の元画素はタイル（画素ごとに違う）なので表にできない。
   `demo.js` の 2 矩形と 2 インスタンスはここを通る。

### 7. 再現手順

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

## スカラー 565 ブレンド/パックの 255 → 256 粗スケール（`g_ksn_scale256`）（旧 kasane-alpha256.md）

対象: `main/ui/kasane/ksn_render.c` のスカラー合成式。境界 4a（per-pixel 面）の前半、
すなわち**8レーンカーネルを書く前に、その算術がどこまでずれてよいか**を測るためのスイッチである。
実機は使っていない。この文書に出る数はすべてホストの道具（`run_models.py` の C モデルと
target の objdump）の出力で、引用した既存文書の値以外に実機の時間は 1 つも無い。

### 0. 何を置き換えるか

スカラー経路は 255 で割る箇所をすべて**厳密**に計算している。コンパイラは
`value*maximum/255u` を魔法の逆数（objdump で `l32r` + `muluh` + `srli 7`）にし、
8レーンのレーンモデルは `(257*(y+1))>>16` の恒等式を使う（`kasane-blend.md` 2.3）。
パネルは 240×135 の RGB565 なので 8bit チャネルの 1 段差はパネルに出ず、この一族では
近似が明示的に受け入れられている。256 スケールは 8レーンカーネルが欲しい形
（16bit の積と 1 本のシフト、逆数不要）でもある。

`g_ksn_scale256`:

| 値 | 意味 |
| --- | --- |
| 0（既定） | すべての `/255` が厳密なまま。変更前の参照経路 |
| 1 | 255 重みの項を 256 重みに置き換える。`scale256(b) = b + (b>>7) = round(b*256/255)`、その後は素の `>>8`。**相補な重みの対**（`b` と `255-b`）は 2 つの丸め積ではなく**1 つの丸めミックス**に畳む |

置き換える場所は 4 つ、いずれも RGB が `pack565` に入る直前の 3 チャネル合成である。

| 場所 | 厳密な形 | 粗い形 |
| --- | --- | --- |
| `blend` の 3 チャネル | `(s*a + d*(255-a) + 127)/255` | `mix256(s,d,a)` |
| `premultiply_over` の RGB 3 本 | `clamp8(mul8(color,a) + mul8(dst,255-a))` | `mix256(color,dst,a)`（`clamp8` 不要） |
| `group_over` の RGB 3 本 | `clamp8(mul8(src,opacity) + mul8(dst8,255-alpha))` | 項ごとに `mul8_256`、`clamp8` は残る |
| `quantize`（dither パックの `/255`） | `v*M/255` と `32*rem > (2b+1)*255` | `(scale256(v)*M)>>8` と `rem > 8*(2b+1)` |

**意図的に動かさないもの: すべての α。** `mul8` による色 α、群タイルの α 累算、
span の coverage スケーリング、タイル α は、いずれも下流で 0/非 0 の判定になる
（`blend` の `if(!a)`、`group_over` の `if(!alpha)`、`group_pixel` の dither provenance）。
そこで 1 → 0 が起きると、それは 1 段差ではなく**合成される画素が消える**ことなので、
粗いアームは `pack565` に落ちる RGB だけに触る。モデルはこれを主張として検査する
（「effective α・返る α・タイルの α チャネルが両アームで一致」= 0 件の不一致）。

### 1. 算術

- `scale256(b) = b + (b>>7)` は `round(b*256/255)` に等しい（全 256 b で検査、0 違反）。
- その補数はそれ自身: `256 - scale256(b) == scale256(255-b)`（全 256 b で検査）。
  これが相補な対を 1 本のミックスに畳める理由である。
- `mix256` は両端で厳密（`a=0` は dst、`a=255` は src）で、常に `max(x,y)` の中に収まる
  （256×256×256 で検査）。だから `premultiply_over` の粗いアームは `clamp8` を落とせる。
- dither の折り畳み: 剰余を 256 分の 1 で数えると同じ判定が `rem > 8*(2b+1)` になる
  （16×256 の (rem,bayer) で検査）。
- 既定アームの重み `a` は `mul8(src_alpha,opacity)`。65,536 組の入力が **256 個すべての値**を
  踏むので、以下の掃引は `a` を直接 0..255 で回せば入力空間の全域になる（モデルが数えている）。

### 2. 掃引（`python tools/pie/run_models.py scale256`）

「動いた画素数」と「最悪段差」は**印字するだけで、主張しない**（近似なのだから 0 でないのが当然）。
主張（`mismatches`）にしているのは §1 の恒等式、既定アームが変更前の式と一致すること、
そして α が動かないことだけである。

```
scale256      round(b*256/255) と 256-scale256(b)==scale256(255-b): 0+0 違反; scale256(255)=256
weights       a=mul8(src_alpha,opacity) は 65,536 組で 256/256 の値を踏む
endpoints     mix256(a=0)==dst, mix256(a=255)==src, mix256<=max(x,y)（256^3 で 0 違反）
dither fold   32*rem > (2b+1)*256 ⇔ rem > 8*(2b+1)（16×256 で 0 違反）
mix channel   全 256 a × 256 src × 256 dst8 = 16,777,216: 動いた 8,208,384 (48.93%), 最悪段差 1 (a=1 で最大)
              重みごとの動いた数は 65,536 中 0..32,640。段差は 1 のみ（2 は 0 件）
mul8 term     全 256 x × 256 b（group_over の項の形）: 動いた 32,064/65,536 (48.93%), 最悪段差 1
group sum     到達可能な (src_alpha,opacity) 65,536 組: 2 項は各 1 と 0 しか動かない（重みごとに総当たり）
              ので、和の最悪は 2（src_alpha=1, opacity=128）
old path      既定アームは 256 opacity × 8 色 × 256 語 × 16 位相 = 8,388,608 回で変更前の参照式と一致: 差分 0
blend thin    24 パラメータ × 全 65,536 語 × 8 位相 = 12,582,912 画素: 動いた 1,690,272 (13.433%), 最悪段差 r/g/b=1/1/1
blend dither  6 パラメータ × 全 65,536 語 × 16 bayer 位相 = 6,291,456 画素: 動いた 1,126,042 (17.898%), 最悪段差 1/1/1
quantize      全 (value 256 × maximum 2 × bayer 16) = 8,192: 動いた 374 (4.57%), 最悪段差 1
group path    3,864,576 タイル/dst 組: α は両アームで一致（不一致 0）; タイル RGB は 11,593,728 チャネル中
              5,336,356 が動き、最悪段差 2; group_over の 565 は 1,638,125 画素が動き、最悪段差 r/g/b=1/2/1
frames        120 枚の全面 REPLACE フレーム（240×135、17 帯すべて）= 3,888,000 画素:
              動いた 140,838 (3.622%)、動いた画素のあるフレーム 120/120、最悪段差 r/g/b=0/1/1
phases        ループの 1 フレームを計器で数えると fill 68, span 140, tile 34, blend 497, read 560
              （120 フレームは fill/span/tile/per-pixel の全経路を通っている）
mismatches=0
```

読み方:

- **画素単位**では `blend` 経路が 13.4%（dither 時 17.9%）、群の経路が 42% 動く。8bit チャネルの
  48.9% が ±1 動くので、565 のフィールドに現れるのはその一部（5bit は 1/8、6bit は 1/4）だが、
  それでも「0.1% 以下」の桁ではない。
- **最悪段差**は `blend`/`quantize` では 1。**群の経路だけは 2** になる。`group_over` の 2 つの重み
  （`opacity` と `255-alpha`）は相補でない（タイルに既に `src.a` が掛かっている）ため、
  2 項の丸めが独立に足し合わさるためである（§1 の `group sum` がその数）。
- dither の有無で差は小さい（13.4% → 17.9%）。`quantize` 自体は 4.57% しか動かないが、
  動く画素の上では dither が段差を増幅も増幅もしない（最悪は 1 のまま）。

### 3. 命令数（target `-Os`、objdump）

```sh
XT=/root/.espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin
for arm in "" "-DKSN_SCALE256_ARM=0" "-DKSN_SCALE256_ARM=1"; do
  $XT/xtensa-esp32s3-elf-gcc -Os -std=gnu11 -Wall -Wextra -Werror -Wno-unused-function \
      -Imain/ui/kasane $arm -c main/ui/kasane/ksn_render.c -o /tmp/ksn_render.o
  $XT/xtensa-esp32s3-elf-objdump -d /tmp/ksn_render.o
done
```

`KSN_SCALE256_ARM=0|1` はアームをコンパイル時に固定する（切り替えの分岐を外して、**走るアームの
命令数**を数えるため）。既定ビルド（実行時スイッチ）の命令数も併記する。

| 関数 | 変更前 = 既定アーム (`=0`) | 粗いアーム (`=1`) | 実行時スイッチ |
| --- | ---: | ---: | ---: |
| `blend` | 50 | **46** | 71 |
| `quantize` | 19 | **13** | 33 |
| `pack565` | 37 | 37 | 37 |
| `group_pixel`（`premultiply_over` を inline で含む） | 103 | **88** | 130 |
| `ksn_render_rects`（`group_over`/`render_group` を inline で含む） | 1062 | **1053** | 1111 |
| `sample` / `covers` / `coverage_runs` / `frame_command` | 57 / 52 / 113 / 144 | 同左 | 57 / 52 / 113 / 147 |

- 1 チャネルあたりの算術は、厳密形の `mul16u`×2 + `addi 127` + `add` + `muluh` + `srli 7`（6 命令）から
  `mul16u`×2 + `add` + `srli 8`（4 命令）へ、つまり **1 チャネル 2 命令減**。`255-a` と
  「`a` の 256 スケール・その補数」の準備が画素あたり 1 回入るので、`blend` の純減は 3 チャネルで 4 命令である
  （`a` は両アーム共通で厳密のまま）。
- `premultiply_over` は相補な対なので `clamp8` も落ち、`group_pixel` は 15 命令減る。
- `quantize` は 6 命令減（`/255` の逆数乗算と `255*q` の戻しが消える）。
- 実行時スイッチは 1 呼び出しあたり 3 命令（`l32r` + `l32i.n` + `beqz.n`、`quantize` の objdump で確認）。
  `blend` では純減 4 命令をほぼ食い、`group_pixel` では 12 命令、`quantize` では 3 命令が残る。
  静的サイズは両アームを 1 バイナリに持つ分だけ増える（`blend` 71、`quantize` 33）。
- **既定アームの命令列は変更前と同一**である（関数単位で比較して `quantize` の `mul16u` 1 本の
  オペランド順だけが違い、意味は同じ。モデルは既定アームが変更前の式と 8,388,608 回一致することも見ている）。

### 4. 既定は 0（厳密経路）である

このスイッチは「ビット一致、または画素の 0.1% 以下が最悪 1 段差で動く」ときだけ既定 ON にしてよい、
という条件で作った。測定結果は:

- 120 フレームの全面比較で **3.622%** の画素が動く（0.1% の 36 倍）。最悪 565 段差は 1。
- 部分ごとでは `blend` 13.4% / dither 17.9% / 群の経路 42%。
- 群の経路は最悪段差 **2** にもなる。

したがって **`g_ksn_scale256 = 0`（厳密形）を既定**とする。粗いアームはスイッチ経由でのみ到達でき、
既定ビルドの画素・命令数は変更前と一致する（契約スイート 48 PASS、既定アームの命令列も同一）。
条件を満たす改良（例: 丸め `+128` を足す形にすれば 8bit チャネルの移動は 48.9% → 8.2% になる）は
このコミットには入れない。測ったのは「素の `>>8`」という指示された形であり、丸め版は別の測定になる。

### 5. テストと再現

```sh
python tools/pie/run_models.py scale256      # §2 の掃引（数秒）
python tools/pie/run_models.py               # 既存 6 モデル + scale256（すべて agree）
bash /tmp/ksn_run.sh                         # tools/kasane_contract/run.sh の複製（sanitizer フラグだけ除去、cd を repo へ）
                                             # → exit 0、PASS 48 行、既定アームなので変更前と同一
```

`run_models.py` は `mismatches=0` を要求する。このモデルは近似を測る側なので、
**動いた画素数を印字しても exit 0 を返す**（主張するのは §1 の恒等式・既定アームの同一性・α の不変だけ）。
`run_models.py` の説明文にもそう書いた。

### 6. やっていないこと

1. **`tools/kasane_contract/run.sh` への結線**。2 アームの画素比較は今は `scale256_model.c` の中
   （同一バイナリで `g_ksn_scale256` を切り替える）で走っている。契約スイートの流儀に合わせるなら、
   群とテキストを含むシーンを 1 本の `test_*.c` にして `run.sh` に足すのが次の段。
   このブランチでは `run.sh` を触らない指示なので、そこは統合側の 1 行に残した。
2. **実機の A/B**。`blend` の純減は 4 命令で、これは `pie-simd.md` §6.3 の配置ノイズ 15% の中にある。
   効くかどうかは同一バイナリ A/B（`g_ksn_scale256` を窓ごとに落とし、対照欄を同時に出す）と
   実機の `KASANE_PAINT` でしか言えない。この文書に実機の数は無い。
3. **8レーンカーネルとの関係**。`ksn_blend_pie.c` は厳密な `/255`（`(257*(y+1))>>16`）を再現する
   カーネルで、その参照も厳密形である。**粗いアームを既定にすると、あのカーネルはスカラーの
   既定と一致しなくなる**。結線するときは「厳密経路 + カーネル」か「粗い経路 + 粗いカーネル」の
   どちらかに揃える必要があり、このスイッチはその選択を 1 バイナリで測れるようにするもの。
4. 丸め版（`+128`）、`mul8` を呼び出し側で巻き上げる版、G チャネルだけ厳密に残す版。
   どれも「測っていない」ので書かない。

## 565ブレンド/パックの PIE カーネル（候補 4a・モデル先行の1本目）（旧 kasane-blend-pie.md）

2026-09-15、ブランチ `perf/kasane-kernel`（親 `origin/perf/kasane-opt` = `1e14ceb`）。
対象は `docs/perf/kasane-opt-survey.md` §5 の候補 4a、境界4（per-pixel）の
`blend` + `pack565` + `quantize`。調査書はこの一族を 1 フレームあたり
blend 5,534 + pack565 11,590 + quantize 12,960 回、約 1.0 M 命令（面の推定 3.1 M の 1/3）と数えている。

**状態:** 記録時はカーネルだけで未結線だったが、いまは結線済み（`g_ksn_blend_pie`、`ksn_render.c` の整列した 8 画素 run から呼ぶ、既定 ON）。以下の数はすべてホストの道具の出力（`run_models.py` / `stalls.py` / `piesim`）。既定 ON にした根拠は同一バイナリの実機 A/B で render の利益が出たこと（`ksn_render.h` のコメント。その ms の数値はこの文書に残っていない）。

### 1. 置いたもの

| ファイル | 何 |
|---|---|
| `main/ui/kasane/ksn_blend_pie.c`（新規） | スカラー参照 + 16bitレーンモデル + 実機用インラインasm（2アーム）+ 入口 |
| `tools/pie/models/blend_pack_model.c`（新規） | 算術の総当たりと、カーネル全体のスカラー参照比較 |
| `tools/pie/run_models.py`（+1行） | `blendpack` をモデル一覧に追加 |
| `tools/pie/test_kernels.py`（+1クラス） | 実 asm を `piesim` で実行しスカラー参照と全画素比較 |
| `tools/pie/piesim.py`（+1命令） | `EE.MOV.U16.QACC`（TRM 1.8.116、ゼロ拡張であって加算ではない） |
| `tools/pie/stalls.py`（1行） | 上の命令を「q オペランドを読むだけ」として扱う |

`tools/pie/` は**このブランチに既にある**（`vm/main` のマージ `64c90b9` で入っている）。
移植コミットは作っていない: 移植元 `perf/pie-opt` との差は `__pycache__` と FIR の追加分だけで、
この作業に要るもの（`piesim.py` / `stalls.py` / `run_models.py` / `models/`）は揃っていた。
ベースラインが緑であることは作業前に確認した（`run_models.py` 全一致、`test_kernels.py` 7 tests OK、
`test_frost.py` PASS）。

### 2. カーネルの形と、再現すべき文

```
ksn_blend8_pie(uint16_t *pixels,int blocks,ksn_rgba src,uint8_t opacity,
               const uint16_t *thresholds)
```

- 16バイト整列した `pixels` から8画素ずつ `blocks` ブロック。src と opacity は run 全体で一定
  （`EE.VLD.128`/`EE.VST.128.IP` は下位4bitを0に固定するので、8画素境界でない run を渡すと隣を読む）。
- `thresholds == NULL` で dither 無し、8レーン与えると dither 有り。中身は呼び出し側の契約で
  `bayer4[y&3][(x+i)&3]`（`ksn_render.c:108` は絶対列で引く）。位相は4列周期なので行の
  8画素ブロックすべてで同じベクトルになる。
- 有効α `mul8(src&255,opacity)` が 0 なら何も書かない（`blend` の早期 return、`ksn_render.c:192`）。
- 入口は C、実体は2本の static 関数（dither 無し／有り）で、それぞれ1つの asm ブロック。
  8個の q レジスタが全部なので、unpack は SAR=12、mix と pack は SAR=4、定数は静的テーブルから
  `EE.VLDBC.16.IP` で1チャネルごとに読み直す。ポインタは毎ブロック先頭で戻す（`mov`）。

再現すべき文（`ksn_blend_scalar_ref` としてソースに置いてある。`ksn_render.c:101-111,190-199`）:

```
a  = mul8(src&255, opacity)                 ; 0 なら dst をそのまま返す
d8 = dst の 5/6bit フィールドの 8bit 展開    ; (v<<3)|(v>>2)、(v<<2)|(v>>4)
c  = (s*a + d8*(255-a) + 127)/255
out= dither 無し: (r>>3)<<11|(g>>2)<<5|(b>>3)
     dither 有り: quantize(r,31,t)<<11|quantize(g,63,t)<<5|quantize(b,31,t)
```

### 3. 恒等式（すべて総当たり、`run_models.py blendpack`）

| 段 | 式 | 掃引 | 結果 |
|---|---|---|---|
| 展開 | `dst*2>>12 == dst>>11`、`(v<<3)\|(v>>2) == (v*33)>>2`、`dst*128>>12 == dst>>5`、`(v<<2)\|(v>>4) == (v*65)>>4` | dst 全 65,536 × 6本 | 0 不一致 |
| a' 選択 | `a ^ ((s<d)?255:0) == (s<d)?255-a:a` | 16,777,216 | 0 |
| /255 | `floor(y/255) == (257*(y+1))>>16` | y=0..65,278 | 0 |
| mix 除算 | `floor((32896+257*Δ*a')/65536) == (Δ*a'+127)/255` | 65,536 | 0 |
| mix 全体 | `min + (Δ*a'+127)/255 == (s*a+d8*(255-a)+127)/255` | 16,777,216 | 0 |
| quantize | v 256 × M∈{31,63} × bayer 16 | 8,192 | 0 |
| T' 折り | `32*rem > (2b+1)*255` ⇔ `rem > floor((2b+1)*255/32)` | 16 × 255 | 0 |
| q<M ガード | 有り／無しで差 | 8,192 | 差 0（冗長） |
| pack | SAR=4 の乗算3本 + ORQ 2本 == シフト形 | 16,777,216 | 0 |

積算器と読み出し（掃引から出した山、主張ではなく実測）:

- mix: 積算器の山 **16,744,321**（2^40 の 1/65,648）、SRCMB 読み出しの山 **255**（16bit 飽和 32,767 の 1/128）、
  `c` の山 255（`VADDS` が飽和しない）。
- dither: 前置きが `P = 257*(255-T')`、積算器の山は 4.2 M 未満、`T' ≥ 7` なので y は負にならない。
- 到達可能な展開値: 赤 32/32・緑 64/64・青 32/32（256 通りのうち）。掃引は 0..255 全部なので上位集合。

### 4. カーネル全体の一致（`run_models.py blendpack`）

入力域の導出: dst は素の `uint16_t` バッファ（`fill565`・群合成・frost が共有）なので全 65,536 語。
src 各チャネルは API が 8bit、TEXT 経路もαだけ `mul8` する（`:262`）ので RGB は呼び出し側のバイトのまま。
opacity は `uint8_t`、dither は bayer4 の16値。

```
kernel thin  1,572,864 画素（24 組 × dst 全 65,536 語）: differing=0 最悪段差 r/g/b = 0/0/0
kernel dith  6,291,456 画素（6 組 × bayer 位相 16 × 全語）: differing=0 最悪段差 0/0/0
kernel runs  331,272 画素（ランダムな複数ブロック呼び出し 2,000 回 + α=0 の早期return 2 種）: differing=0
mismatches=0
```

**近似は使っていない**（すべて恒等式）ので移動画素は 0。この掃引が 0 でなくなれば、モデルは
件数・チャネル・段差を印字する。

### 5. 命令数とストール（`stalls.py` と objdump）

```
python tools/pie/stalls.py main/ui/kasane/ksn_blend_pie.c ksn_blend8_thin
python tools/pie/stalls.py main/ui/kasane/ksn_blend_pie.c ksn_blend8_dither
xtensa-esp32s3-elf-gcc -O2 -fno-inline -Wall -Wextra -Werror -std=gnu11 \
    -DCONFIG_IDF_TARGET_ESP32S3 -Imain/ui/kasane -c main/ui/kasane/ksn_blend_pie.c
xtensa-esp32s3-elf-objdump -d
```

| | 呼び出し全体 | うち EE | 最初の EE まで | ループ本体 | 本体バイト | ストール | 推定サイクル |
|---|---:|---:|---:|---:|---:|---:|---:|
| `ksn_blend8_thin` | 90 | 74 | 11 | 79 命令 | 235 B | 5 | 84.6 |
| `ksn_blend8_dither` | 115 | 97 | 11 | 91 命令 | 271 B | 22 | 113.6 |
| `ksn_blend8_pie`（入口） | 41 | 0 | — | — | — | — | — |

- 前回のカーネル（`garden_decor_mix8`）は1呼び出し73命令・うち37命令が最初の EE までだった。
  ここが 11 なのは、定数が**コンパイル時定数の静的テーブル**で、呼び出し側が毎回組み立てる
  必要がないため（`garden_decor_pie.c` の 37 は light/shadow/d から5本の式をその場で作っていた分）。
- dither の 115 のうち 22 は**1呼び出し1回**の `T' = floor((2b+1)*255/32)` と
  `P = 257*(255-T')` の計算（run 全体で不変）。
- thin の残りストール5本は pack の「フィールド→配置」の2乗算が続く箇所と、その結果を ORQ が
  直後に食う箇所。1命令挟めば消えるが、8レジスタが埋まっていて置き場所が無い。
- dither の 22 本も同じ形（定数の読み込みが軒並み1命令後ろの消費者に当たる）。dither は
  `P` を1本のレジスタに常駐させる分、scratch が thin より1本少なく、同じ組み替えができない。
- `stalls.py` は QR を追うだけで **QACC を見ない**（`tools/pie/README.md` の限界の項）。
  このカーネルは1チャネルにつき `VMULAS.U16.QACC`（QACC を段2で def）→ `SRCMB.S16.QACC`
  （段1で use）が近接するので、TRM 1.7.1 の式では1チャネル1サイクル待つ可能性がある。
  その分はこの表に入っていない（`garden_decor_mix8` と同じ注記）。
- 参考: スカラーの同経路は調査書 §7 の objdump で blend 50 + pack565 35（+ dither は quantize 19×3）。
  8画素で **680 命令（thin）/ 1,136 命令（dither）**。上の 79 / 91 はその 1/8.6 と 1/12.5。
- **時間の主張はしない。** 84.6 / 113.6 はツールの静的モデル（1命令1サイクル + ストア0.6 +
  ストール）で、実機はこの 1.3〜1.4 倍側に出るのが常（`pie-simd.md` §2.6）。実機の数字は
  結線と同一バイナリ A/B の後にしか出せない。

### 6. テスト（`tools/pie/test_kernels.py`、`piesim` で実 asm を実行）

- `test_thin` / `test_dither`: 8〜12ブロックのランダムな dst 語と、境界語（0..7）・
  色／不透明度の端（白255、α=0 に近い組、bayer の16位相）で、全画素をスカラー参照と比較。
  同時に「dst が 16バイト×ブロック数だけ進む」「定数テーブルをちょうど1ブロック分だけ歩く」
  「run の前後16バイトに触らない」も検査する。定数は C の初期化子を `extract_constants` が
  評価したものを使うので、**値と読み順の両方**が掛かる。
- `test_shared_prefix_reads_the_same_unpack`: 2アームは unpack と最初のチャネルの mix を
  共有する（別々に書いてあるので、片方だけ直すと静かに食い違う）ので、両者の**命令列の
  共通前置**が 20 命令以上あることを検査する。20 で止まるのは dither が `P` をレジスタに
  置く分だけ定数の置き場所が違い、そこから先は構成上そろわないため（テストにそう書いてある）。
- `piesim.py` に足した `EE.MOV.U16.QACC` は「置換であって加算でない」ことも別途検査した。

### 7. 結線後に残っている項目

- **群の経路**（`premultiply_over` / `group_over`）は未着手。tile が premultiplied RGBA8 でレーンごとに src が違う版が要り、このカーネルは「run 全体で src 一定」の形。TEXT（αだけレーンごと）と GRADIENT（RGB も）も同じ。
- 結線（整列 8 画素 run をカーネルへ、同一バイナリ A/B の実行時スイッチ `g_ksn_blend_pie`）は完了。実機の `PERF kernel=` の絶対値は未記録。
