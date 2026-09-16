# compiler_builtins の会員はなぜリンクに入るのか（cgu.13 = 10,244 B の場合）

## 結論

`cgu.13` は `__fixdfsi` の参照では落とせない。我々の C にある参照を **2 本とも消しても
1 B も動かなかった**（下の実測）。理由は 2 つある。

1. マップの "Archive member included to satisfy reference by file (symbol)" は
   **会員ごとに 1 本しか出さない**。そこに `pocket_bridge.c.obj (__fixdfsi)` の 1 行しか
   無いことは「その 1 本だけが理由」を意味しない。`__fixdfsi` の参照元は
   クロスリファレンス表で **18 本**ある（main の .c が 12 本、`quickjs.c.obj`、
   ESP PHY のバイナリブロブ `phy_analog_cal.o`、そして compiler_builtins 自身の
   別会員 cgu.04/09/14/15）。1 本消しても次の 1 本が代表として出るだけ。
2. そもそも cgu.13 の配置 10,244 B は `__fixdfsi` ではない。
   リンク後の ELF では `__fixdfsi` は `A 0x400022d4`（絶対シンボル = ROM。
   `esp_rom/esp32s3/ld/esp32s3.rom.libgcc.ld` が定義している）で、
   cgu.13 の `.text.__fixdfsi` は "Discarded input sections" にある。
   つまり呼び出しは ROM の実装に行き、cgu.13 側の同じ名前の関数は GC で捨てられている。
   アーカイブ会員の取り込みだけが起きて、その理由の関数は使われない。

## 実測（基準 `/workspace/pjs-vm/build_base` = `bd0fa43`）

`main/pocket/pocket_bridge.c` の `take_options()`（`(int32_t)ms`）と、
`main/pocket/pocket_av.c` の同じ式（`main/pocket/pocket_av.c:79`）を
一時的に定数へ置き換えてビルドした（挙動は壊れてよい、リンクを見るためのスタブ）。

| ビルド | 消した参照 | bin | flash | DIRAM | 配置合計 | compiler_builtins | cgu.13 | マップの理由行 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `build_base` | — | 2,142,864 | 1,557,328 | 123,308 | 1,820,667 | 46,329 | 10,244 | `pocket_bridge.c.obj (__fixdfsi)` |
| `build_stub` | bridge のみ | 2,142,848 (−16) | 1,557,324 (−4) | 123,308 (±0) | 1,820,663 (−4) | 46,329 (±0) | 10,244 (±0) | `pocket_av.c.obj (__fixdfsi)` |
| `build_stub2` | bridge と av | 2,142,848 (−16) | 1,557,320 (−8) | 123,308 (±0) | 1,820,658 (−9) | 46,329 (±0) | 10,244 (±0) | `pocket_capture.c.obj (__fixdfsi)` |

減ったのは消したリテラル（4 B × 2）だけで、`cgu.13` も `compiler_builtins` 合計も動かない。
`build_stub2` の ELF でも `__fixdfsi` は `A 0x400022d4` のままである。

## cgu.13 の 10,244 B は誰が持っているか

配置されているのは C 名のラッパーではなく Rust の `libm_math` 関数である
（クロスリファレンス表の定義元と、メモリマップの配置バイトから）:

| 配置 B | 関数 | 表に出ている参照元 |
| --- | --- | --- |
| 4,027 | `rem_pio2_large` | cgu.14, cgu.15 |
| 2,394 | `cosf` | cgu.09, cgu.08 |
| 1,764 | `cbrt` | cgu.12, cgu.00 |
| 1,282 | `cos` | cgu.09, cgu.05 |
| 375 | `k_sin` | （cgu.09 側の同名定義と対） |
| 50 | `__umodti3` | core cgu.08, cgu.03, cgu.15 |
| 19 + 19 | `log`, `log1p` | `quickjs.c.obj`（と picolibc の `atanh`） |
| 352 | 匿名 rodata | — |

つまり cgu.13 を落とすには、`__fixdfsi` ではなく
**compiler_builtins 内部の連鎖（cgu.00/01/04/05/09/12/14/15 と core cgu.08）**と
**quickjs.c の `log`/`log1p`** を先に切る必要がある。`__fixdfsi` の側からは何も動かない。

## 同じ罠

`cgu.04`（`__gedf2`）や `cgu.08`（`__divdf3`）も、マップの理由行が 1 本しか出ないという
同じ理由で「我々だけが理由」に見えている。会員を落とせるかどうかを判断するときは、
理由行ではなくクロスリファレンス表で**その会員が定義する全シンボルの参照元**を見ること。

## 副産物（この枝には入れていない）

`(int32_t)ms` を `(int32_t)(int64_t)ms` にすると、直前のガードが既に計算している
`(int64_t)ms` を使い回すので `__fixdfsi` の呼び出しが消える（値は一致。
ホスト総当たり 1,302,394,077 点で不一致 0、結果列のハッシュも一致、
受理域 1..30000 は網羅）。ただし得られるのは 4 B のリテラルだけで
cgu.13 には効かないので、この枝ではコードを変えず知見だけ残す。
