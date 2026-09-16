# フラッシュ削減: 「我々だけが参照元」のライブラリ会員を3つ測って、1つも減らなかった記録

枝 `perf/size-shell`（基点 `origin/vm/main` = `bd0fa43`、ワークツリー `/workspace/pjs-size-shell`）。
基準ビルドは `/workspace/pjs-vm/build_base`。**実機は無い**ので、以下はすべて bin / flash /
DIRAM / 配置バイト / ホスト実行の話で、ミリ秒は主張しない。

ねらいは pie-simd.md 側の perf 作業ではなく **サイズ**である。リンクマップの
`Archive member included to satisfy reference by file (symbol)` と Cross Reference Table から
「我々の main/ 配下のオブジェクトだけが参照元になっている会員」を選び、その参照を消して
`-Wl,--gc-sections` に落としてもらう。**判断は必ずスタブビルドで先に値段を測ってから**行う
（会員が落ちるかどうかは map を読んでも分からない。§4）。

対象3件の結論: **`remainder` は会員が落ちるが差し替えの方が高く、`sqrtf` はビット一致の
差し替えが落ちる量に収まらず、`__divdf3` はフラッシュ 0 バイト。3件とも取り込まない。**

## 1. `ui/shell.c` の `__divdf3`（cgu.08、1188 B とされていた）→ **0 B**

- `nm` は `40002250 A __divdf3` を返す。**ROM のソフトフロートへの絶対エイリアス**で、
  これはリンカスクリプトの代入（`__divdf3 = 0x40002250` 等が並ぶ節）である。map 全体で
  `divdf3` を含む配置行はこの1行だけで、`.text._RNvNtNt...float3div8___divdf3` の実体は
  **Discarded input sections** 側にしか出ない。同じ形のエイリアスが `__divsf3` `__adddf3`
  `__subdf3` `__muldf3` `__extendsfdf2` `__truncdfsf2` `__fixdfsi` `__floatundidf` にもある。
  **つまり我々の double 除算はフラッシュを1バイトも食っていない。**
- cgu.08 の配置 1230 B の中身は `tan`(19+16) + `k_tan`(874+124) + `cosf`(15+4)。参照元は
  tan=quickjs、k_tan=compiler_builtins 内部、cosf=我々の4ファイル。**会員を生かしているのは
  quickjs の tan と k_tan で、我々の cosf は 19 B 分しか寄与していない。**
- スタブ実測: `main/` の12ファイルの double 除算 49 箇所（オブジェクト側では `__divdf3`
  呼び出し 79 箇所）を全部乗算に置き換えたビルド（`build_stub`）でも compiler_builtins は
  **16 会員 48,552 B のまま**、cgu.08 も 1230 B のまま。`__divdf3` を消す価値は無い。

## 2. `solar_sail.c` の picolibc `remainder`（402 B）→ 会員は落ちるが **正味 +80 B**

- スタブ実測（`float f=(float)remainder(...)` を `(float)m` にしただけ、`build_stub`）:
  `libm_math_s_remainder.c.o` 402 B → 0、bin 2,142,864 → **2,142,352（−512 B）**、
  flash 1,557,328 → 1,556,816（−512）、DIRAM 変化なし。**差し替え無しなら確かに落ちる唯一の会員。**
- 本実装を書いて測った: 下の `fold_tau()` は IEEE の `remainder(m, 2π)` と**ビット一致**
  （§2.2 の検証）だが、`main/scene/solar_sail.c` の `eccentric` は 153 B → 661 B（**+508 B**）、
  会員 −402 B と合わせて **bin 2,142,944 = +80 B**。`xtensa` では double の1演算が ROM 呼び出しで
  引数詰め替え込み ~30 B のコードになるため、fold が picolibc の特化ルーチン（386 B）より
  大きい。**フラッシュを減らす目的には合わないので戻した**（枝には入れていない）。

### 2.1 差し替え候補（そのまま使える形で置いておく）

```c
#define TAU_FOLD_HI 6.283185303211212       // 2π（下位26ビットを落とした上位27ビット）
#define TAU_FOLD_LO 3.968374073792802e-09   // 2π − TAU_FOLD_HI、厳密
static double fold_tau(double m) {
    if(m>=-3.141592653589793&&m<=3.141592653589793)return m;
    double h=(double)(long long)(m*0.15915494309189535+(m<0?-0.5:0.5));
    double ah=(h+0.5)*TAU_FOLD_HI,al=(h+0.5)*TAU_FOLD_LO;
    if(m-ah>al)h+=1.0;
    else if(m-ah==al){if((long long)h&1)h+=1.0;}
    else{
        double bh=(h-0.5)*TAU_FOLD_HI,bl=(h-0.5)*TAU_FOLD_LO,d=m-bh;
        if(d<bl)h-=1.0;
        else if(d==bl&&((long long)h&1))h-=1.0;
    }
    return (m-h*TAU_FOLD_HI)-h*TAU_FOLD_LO;
}
```

- 2π を上位27ビットと厳密な尾に割るので、`h` の ±1/2 周期との比較に使う積
  `(h±0.5)*TAU_FOLD_HI` / `*TAU_FOLD_LO` は |h| < 2^24 で**厳密**（有意ビット ≤ 51 ≤ 53）。
- 中間の `m − h*TAU_FOLD_HI` は Sterbenz で**厳密**、最後の尾の減算だけが1回丸める。
  したがって結果は `m − n*2π` を正しく丸めた値そのもので、`n` は半周期との比較で厳密に決まる
  （tie は偶数丸め）。|m| ≤ π は即返しで、`h=0` の退化ケースもそこに畳んである。
- 効くのは `eccentric(m, e)` の呼び出しだけ（`orbit_at` と `satellite_at`）。

### 2.2 検証（ホスト）

`tools/fold_harness.c`（`cc -O2 tools/fold_harness.c -lm -o /tmp/fold_harness`）:

- **133,721,748 点**で libm の `remainder` と比較して **double 一致 100%、float 一致 100%
  （不一致 0）**。内訳は |m| ≤ 2400 の 1/8192 格子 39,321,601 点、一様乱数、|m| ≤ 1e6 の
  乱数、1e-8 級、そして **`(h+1/2)*2π` の近傍を ulp 直下まで動かした敵対的標本 1,695,722 点
  （半整数からの距離の最小は 0 = 厳密な tie を含む）**。
- 前提の直接検査: 分割積の有意ビット数（128bit 整数で計算）は |h| ≤ 2^24 の 13,460 個で
  最大 51 で、**一つも厳密性を破らない**。中間減算は long double（64bit 仮数）と比較して
  **131,038,756 件すべて厳密**。
- 既存の `tools/test_solar_sail.c`: **23,746 回の呼び出しで double 不一致 0**。全 1,200 フレームの
  ローリングハッシュが旧（libm）と新（fold）で一致（`86a78e8375f12985`）、8惑星タイルの PPM も
  md5 一致。**ただし時計を固定してから**（§4）。

## 3. `sqrtf`（flower.c / flower_species.c / motion.c / solar_sail.c）→ 落ちるのは **294 B**、差し替えが収まらない

- 内訳: `.text.sqrtf` + literal = **19 B**（cgu.05、参照元は我々4ファイルのみ）、Rust の
  `libm_math::sqrt::sqrtf` 本体 = **251 + 24 B**（cgu.01）。`RSQRT_TAB` 256 B（cgu.09）は
  f64 の `sqrt` が quickjs の `Math.sqrt` で生きているため残る。
- スタブ実測: `main.c` に `float sqrtf(float)` を定義して我々の呼び出しを全部そこへ向けた
  ビルド（`build_sq`）で **bin 2,142,864 → 2,142,608（−256 B）**、cgu.01 −275 B、cgu.05 −19 B。
- 差し替えの壁:
  - 実機の `sqrtf` は 16bit 固定小数点＋`RSQRT_TAB`＋Newton の整数実装（`objdump` で確認）。
    同じ結果を出すには同じ表（256 B を .rodata に）と同程度のコードが要り、**差し引き 0 前後**。
  - `perf/flower-decor` の `flower_isqrt_q` / `fixed_sqrt.h`（Q形式の整数平方根）は
    同ブランチの実測で **B=8 で 0.022% の画素が変わり、最大段差 246/255**（B=14 で
    32,400 画素中1画素 ≤4/255）。このリポジトリの規約（ビット一致でなければ既定 ON にしない）
    では既定 ON にできない。
  - 表なしの correctly-rounded な実装なら 294 B を下回る余地はあるが、**実機側が correctly
    rounded であることを示す手段がこのコンテナに無い**（Rust のツールチェーンも
    `compiler_builtins` のソースも無い）。示せないまま差し替えると、花の画素演算で 1 ulp 差が
    出たときに「なぜ」を言えない。
- → フラッシュ削減の対象としては見送り。速度側の話（1フレーム約4,000回・約2.1ms）は
  `perf/backlog.md` の既存行のまま。

## 4. この作業で踏んだ罠（次に同じことを測る人へ）

1. **map の「理由」は「根」ではない。** `Archive member included to satisfy reference by file
   (symbol)` に出る記号は、その会員が**取り込まれた最初のきっかけ**にすぎない。会員の配置
   バイトを実際に生かしているのは別の参照であることが多い（cgu.08 は我々の `__divdf3` が
   きっかけだが、中身は quickjs の `tan`。cgu.13 は我々の `__fixdfsi` がきっかけだが、
   その参照を自前定義で先に解決しても 10,346 B のまま、bin も不変）。
2. **ROM エイリアスへの参照は Phantom extraction を起こす。** `__divdf3` `__fixdfsi`
   `__floatundidf` `__extendsfdf2` … はすべて 0x40002xxx の絶対シンボルなので、我々の参照は
   フラッシュ 0 バイトのまま会員を取り込ませる。**「我々だけが参照元」の一覧は、絶対シンボル
   を除外してから読むこと。**
3. **道具が過小に出る。** `tools/member_bytes.py` / `map_census.py` は「名前が行単独で出る
   セクション」だけ数えるので、`.text.sqrtf 0xADDR 0xSIZE member` の1行形式を取りこぼす。
   compiler_builtins は同ツールで 46,329 B（16会員）、1行形式も数える実装では **48,552 B**。
   前後比較は同じ道具でやれば差は正しいが、**絶対値と会員別の内訳は信用しない**こと
   （cgu.05 は 4 B と出ていたが実際は 270 B）。
4. **`sail` シーンのフレームは実時計に依存する。** `solar_sail_prepare` は
   `solar_time_now()`（= `gettimeofday`）で J2000 からの日数を取るので、フレームのハッシュは
   同じバイナリでも run ごとに変わる（実測: 同一バイナリ3回で2種類）。フレーム単位の A/B は
   `/tmp` に複製した `solar_time.c` の `now.tv_sec`/`now.tv_usec` を固定してから。
5. **スタブは「落ちるか」を測るもの。「元が取れるか」は別に測る。** 402 B 落ちることは
   スタブで分かるが、差し替えコードのバイト数は書いてビルドするまで分からない。今回は
   ここで符号が反転した。

## 5. 再現手順

```bash
cd /workspace/pjs-size-shell                     # worktree, branch perf/size-shell
python3 /workspace/knscratch/size/tools/member_bytes.py build_x/cardputer_pocketjs.map compiler_builtins
# 会員の場所と参照元（自分の道具で）
python3 tools/memlog.py --map build_x/cardputer_pocketjs.map      # IDF env 必須
nm -C build_x/cardputer_pocketjs.elf | grep -E ' A __(div|fix|float|extend|trunc)'   # ROM エイリアス一覧
cc -O2 tools/fold_harness.c -lm -o /tmp/fold_harness && /tmp/fold_harness 50000000
```

## 6. 未確認

- 実機のミリ秒、実機の memlog（`tools/memlog.py --port`）は取っていない。
- `sqrtf` を表なしで correctly-rounded に書いた場合のバイト数は測っていない（294 B を
  下回る見込みはあるが、実機側の正しさを示せないため着手していない）。
