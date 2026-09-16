# 565ブレンド/パックの PIE カーネル（候補 4a・モデル先行の1本目）

2026-09-15、ブランチ `perf/kasane-kernel`（親 `origin/perf/kasane-opt` = `1e14ceb`）。
対象は `docs/perf/kasane-opt-survey.md` §5 の候補 4a、境界4（per-pixel）の
`blend` + `pack565` + `quantize`。調査書はこの一族を 1 フレームあたり
blend 5,534 + pack565 11,590 + quantize 12,960 回、約 1.0 M 命令（面の推定 3.1 M の 1/3）と数えている。

**このコミットは結線しない。** `ksn_render.c` は1行も動かしておらず、`main/CMakeLists.txt` にも
足していない（呼び出し側が「どの8画素を任せるか」を決めるのが次のコミット）。実機も触っていない
（`/dev/ttyACM0` は別セッション）。ここにある数はすべてホストの道具の出力である。

## 1. 置いたもの

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

## 2. カーネルの形と、再現すべき文

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

## 3. 恒等式（すべて総当たり、`run_models.py blendpack`）

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

## 4. カーネル全体の一致（`run_models.py blendpack`）

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

## 5. 命令数とストール（`stalls.py` と objdump）

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

## 6. テスト（`tools/pie/test_kernels.py`、`piesim` で実 asm を実行）

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

## 7. やっていないこと（次のコミット）

1. **結線**（`ksn_render.c` の per-pixel ループから、整列した8画素 run をカーネルへ。
   `ksn_render_rects` の `covers` を区間へ畳む作業（候補 4b）と噛み合う）。
2. **群の経路**（`premultiply_over` / `group_over`）。調査書 4a の後半で、tile が
   premultiplied RGBA8 なので**レーンごとに src が違う**版が要る（このカーネルは
   「run 全体で src 一定」の形）。TEXT（αだけレーンごと）と GRADIENT（RGB も）も同じ。
3. **実行時スイッチ**（`g_ksn_pie_blend` 相当）と同一バイナリ A/B。結線と同じコミットで入れる。
4. 実機の `PERF kernel=` と絵。この文書の数はすべてホストの道具の出力である。
