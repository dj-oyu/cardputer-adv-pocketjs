# ライブラリを生かしているのは誰か（cref census）

リンクマップを3つの視点で突き合わせた調査。目的は「フラッシュを減らすとき、どのシンボルが
**我々の C の参照だけを理由にリンクに入っている**のか」を、推測ではなく数字で特定すること。
計器は [`tools/size/`](../../tools/size/) の2本（`member_bytes.py` / `map_census.py`）。
手順そのものは [flash-size-method.md](flash-size-method.md) にある。

## 1. 方法 — マップの3箇所を突き合わせる

| 視点 | マップ上の場所 | 分かること |
| --- | --- | --- |
| 配置（placement） | `Linker script and memory map` の `.text.X` + `0xADDR 0xSIZE OBJ` | **実際にフラッシュに載っている量**（`-Wl,--gc-sections` があるので配置＝真実）。会員ごとの合計も出せる |
| 取り込み理由（inclusion） | `Archive member included to satisfy reference by file (symbol)` | 各アーカイブ会員が**どの参照のため**に入ったか、その参照元ファイル |
| 相互参照（xref） | `Cross Reference Table`（`--cref`） | シンボルごとの**全参照元**（我々 / IDF / Rust / QuickJS / picolibc に分類できる） |

配置は会員名（`...(cgu.NN.rcgu.o)`）でまとめられるので、「この会員は誰が理由で入り、何バイトか」
が1行で出る。落ちるかどうかの判定は
**(a) 我々の最後の参照が消えること** と
**(b) まだ必要な `compiler_builtins` 会員が内部でその関数を呼んでいないこと**
の両方にかかる。(b) は 1 回のスタブビルドで白黒つく。

## 2. 基準（`origin/vm/main` = `bd0fa43`、素の `idf.py build`）

```
cardputer_pocketjs.bin          2,142,864 B
flash (memlog)                  1,557,328
DIRAM (memlog)                  123,308
配置済み合計                    1,820,667 B / 837 会員
compiler_builtins 合計          46,329 B / 16 会員
libgcc 合計                     102 B（_ffsdi2.o 35 B ← GPIO、_popcountsi2.o 67 B ← efuse）
```

## 3. 取り込み理由の全リスト（compiler_builtins の16会員）

```
10244 B  cgu.13  <- 我々: main/pocket/pocket_bridge.c.obj (__fixdfsi)    ★単独の理由
 7276 B  cgu.14  <- builtins 内部の連鎖
 4163 B  cgu.09  <- builtins 内部の連鎖
 3953 B  cgu.11  <- builtins 内部の連鎖
 3786 B  cgu.10  <- Rust core の __udivti3（我々ではない）
 3633 B  cgu.01  <- builtins 内部の連鎖
 3347 B  cgu.06  <- builtins 内部の連鎖
 2488 B  cgu.02  <- builtins 内部の連鎖
 2248 B  cgu.04  <- 我々: main/pocket/pocket_av.c.obj (__gedf2)           ★単独の理由
 2082 B  cgu.15  <- builtins 内部の連鎖
 1188 B  cgu.08  <- 我々: main/ui/shell.c.obj (__divdf3)                  ★単独の理由
 1158 B  cgu.03  <- builtins 内部の連鎖
  472/283/4/4/4 B  <- 内部の連鎖と、我々の __muldf3 / __floatundidf の極小会員
```

**我々が入口になっている会員は 3 つで 13,680 B。** 内部の連鎖（cgu.14/.09/.11/.01/.06/.02/
.15/.03 = 25,338 B）が誰にぶら下がっているかは、3つの入口を潰したスタブビルド 1 回で分かる
（連鎖ごと落ちれば数万バイトになる）。唯一の外部根は cgu.10（Rust core 自身の `__udivti3`）で、
これは我々が何をしても残る。

## 4. 我々のコードが参照している C 名のシンボル

- **trig**: `sinf` 2,490 B ← flower / flower_species / glass_rain / solar_sail / stars / sound /
  mp3_decode、`cosf` 2,394 B ← flower / flower_species / solar_sail / mp3_decode、`tanf` 1,925 B
  ← **wave.c のみ**。参照元はすべて我々（`--cref` で確認）。ただし `sinf` は cgu.04 の内部呼び出し
  でも参照されているため、cgu.04 が残る限り落ちない（下の実験で確認済み）。
- **picolibc libm**: `remainder` 402 B ← solar_sail.c のみ（我々だけ）。`sqrtf` ← flower /
  flower_species / motion / solar_sail の4ファイル。
- **消せないもの**: `pow` / `cbrt` / `hypot` / `fmod` は `quickjs.c`（JS の `Math.*` の意味論）が
  保持している。soft-float の四則（`__muldf3` / `__divdf3` / `__adddf3` / `__divsf3`）は Rust・IDF・
  QuickJS・picolibc と共有。**64bit 除算（`__udivdi3` 等）は我々が 36 箇所すべて消しても 0 B**
  （40 以上の参照元が居るため）── サイクルの話であってサイズの話ではない。

## 5. 参照元の広がり（inclusion 節の全数）

「我々を参照元に含む会員」は 137、そのうち **137 すべてが我々だけを参照元にしている**。
つまり画像のライブラリはすべて我々のアプリが起点で入っている（ファームとして当然）。
サイズを削るとは、このうち「我々が入口になっている会員」を減らすことである。
上位は `quickjs.c.obj` 302 KB（JS エンジンなので対象外）、`minimp3.c.obj` 19.3 KB、
`libesp_driver_i2c.a(i2c_master.c.obj)` 9.5 KB、`libesp_driver_uart.a(uart.c.obj)` 9.1 KB、
`libesp_http_client.a(esp_http_client.c.obj)` 5.9 KB、`libesp_driver_rmt.a(rmt_tx.c.obj)` 6.2 KB
── これらは**機能**であって余剰ではないので、消すなら機能を捨てる判断（ユーザーの領分）になる。
libm / builtins の family は「値で証明できる書き換え」で機能を保ったまま落とせる側である。

## 6. 分担（並列で着手した枝）

| 枝 | worktree | 入口 | 直接の獲物 |
| --- | --- | --- | --- |
| `perf/size-bridge` | `/workspace/pjs-size-bridge` | `pocket_bridge.c` の `__fixdfsi` | cgu.13 **10,244 B** |
| `perf/size-av` | `/workspace/pjs-size-av` | `pocket_av.c` の `__gedf2` | cgu.04 **2,248 B**（＋`sinf` の解放） |
| `perf/size-shell` | `/workspace/pjs-size-shell` | `ui/shell.c` の `__divdf3` / `solar_sail.c` の `remainder` / `sqrtf` | cgu.08 **1,188 B** / 402 B / 4 ファイル |
| `perf/size-trig` | `/workspace/pjs-size-trig` | シーンと音声の `sinf`/`cosf`/`tanf` | 6,809 B＋呼び先 |

各枝の流儀: 必ず**スタブビルド 1 回で「本当に落ちるか」を先に確かめる**（落ちなければ書き換えない）。
本実装は値で証明できる書き換えに限る（旧式と新式の総当たりで不一致 0、実データのハッシュ一致）。
見た目が動く変更は「動いた画素の割合と最大段差」を数字で出し、採否はユーザーが実機で決める。
実機のミリ秒は、この環境に `/dev/ttyACM0` が無いので主張しない。

## 7. 再現

```bash
# 基準マップ（vm/main の素のビルド）
cp /workspace/pjs-vm/build_base/cardputer_pocketjs.map <scratch>/bd0fa43.map
T=tools/size
python3 $T/member_bytes.py <map> compiler_builtins     # 会員ごとの配置バイト
python3 $T/member_bytes.py <map>                       # 画像全体
python3 $T/map_census.py <map> --library-targets --top 20
python3 $T/map_census.py <map> --member compiler_builtins --top 20
python3 tools/memlog.py --map <map>                    # flash / DIRAM（IDF env が必要）
```

`compiler_builtins` の会員を外したかどうかは、この会員ごとの数字を前後で diff すれば一目で分かる。
