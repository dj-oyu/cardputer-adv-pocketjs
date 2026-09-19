# `shade` を割る: 一番大きい単項の中身と、そこにあった死んだ呼び出し

2026-09-19。FLOWER の `shade` は SPLIT の単項で最大（1 ヒット 748 サイクル、
1,876 ヒット/フレーム、5.81 ms）だったが、**一度も割られていなかった**。
割った結果、中身の 29% が `rgbd` で、その中に**結果を捨てるだけの呼び出しが1本**あった。

## 1. 割る前に分かっていたこと

`flower_parts.h` の `normal()` に「22 ms 中 1.16 ms、置き換える価値なし」という実測注記があり、
`shade` の残りが何かは誰も測っていなかった。オブジェクトを読むと `shade` は 654 命令で、
`.literal.shade` の再配置が指す呼び出し先は **`sqrtf` / `__divsf3` / `rgbd`（3箇所）** だった。
`normal()` の `1.0f/sqrtf(d)` がソフトウェアの平方根とソフトウェアの除算を両方呼んでいる。

## 2. 計器

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

## 3. なぜ `rgbd` が 214 サイクルなのか

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

## 4. 置いたもの

- 死んだ呼び出しを消した。`d` の代入を両方の腕へ1回ずつに分ける。**同じ式、同じ順序。**
- `garden_dither` の本体を `garden.h` へ `static inline` として移した。コピーではなく**移動**で、
  `garden.c` 側の2箇所もヘッダのものを使う。定数4本は1箇所にしかないので食い違えない。

どちらも画素を動かさない変更である。

## 5. 結果（実機 COM3、同じ種・同じショット・同じ計器つきビルド）

| | 前 | 後 |
| --- | ---: | ---: |
| `rgbd` | 1.67 ms（214 cy/call） | **1.09 ms（139 cy/call）** |
| `norm` | 1.18（151 cy） | 1.13（145 cy） |
| `rest` | 2.97 | 2.87 |
| `shade` | 5.81 ms（748 cy/hit） | **5.10 ms（651 cy/hit）** |
| `decor` | 9.5–9.9 | **7.9** |
| `total`（= PERF の `kernel`） | 31.9–32.6 | **29.2–29.4** |
| PERF `fps` / `draw` | 26.4–27.4 / 35.3–36.6 ms | **28.2–28.4 / 33.9–34.2 ms** |

**kernel −2.7 ms（−8.4%）、fps +1.5。** `shade` の取り分は 0.7 ms で、残りは
`garden.c` 自身の2箇所（`garden_row` と decor）がインライン化された分である ——
`decor` が 9.9 → 7.9 ms と落ちているのがそれで、`-Os` では同一翻訳単位でも
外部リンケージの関数を呼び出しのまま残していたことになる。**この 1.6 ms は
`shade` を測りに行って見つけたもので、`shade` の中には無い。**

**ビット一致の証明**: `tools/flower_frame_dump.c` で HEAD のツリーと作業ツリーの
両方を建て、5 種 × 3 時刻 = 15 フレームを PPM で比較し**全バイト一致**。
`test_flower`（60 ポーズ × 全種）、`test_garden`、`tools/pie/test_kernels.py` も PASS。
実機は `smoke --cycles 20` / `test_settings` / `capture_home` / `kasane_input_device_test` PASS。
静的 DIRAM 増分 0、flash +328 B。

## 6. 次に残っているもの

`shade` の `rest`（2.87 ms、約 370 cy/hit）が単項として最大のまま。中身は
2 つの内積、`spec` の 4 乗 2 回、局所座標と 2 つの内積、材質の if 連鎖、
最後の 9 つの積和と 3 つの float→int 変換である。370 サイクルは浮動小数の
演算数（約 45）に対して 8 cy/op で、**依存チェーンの待ちか `petals[petal]` の
ロードのどちらか**を疑うべきだが、どちらかはまだ測っていない。

`norm` の 145 cy は `sqrtf`（106 cy と別の場所で実測）と `__divsf3` が大半。
`rsqrt0.s` / `maddn.s` / `nexp01.s` / `mksadj.s` は**出荷中のツールチェーンで
アセンブルできることを確認した**（`xtensa-esp32s3-elf-gcc -c`）ので、逆平方根の
Newton 系列を書く道は開いている。ただし正しい系列は Xtensa ISA の
リファレンス側にあり、この機体の TRM には無い。**記憶で書かないこと。**
取り分の上限は 145 → 30 cy として 1,876 ヒットで約 0.9 ms。
