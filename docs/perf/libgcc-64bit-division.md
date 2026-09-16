# libgcc ではなく Rust の compiler_builtins — 64bit 除算の在処と 36 箇所

2026-09-16、ブランチ `perf/system-pie`。きっかけは「libgcc のリンクでバイナリが
一気に大きくなるから排除したい」という問い。**測ったら前提が 2 つとも外れていた**ので、
その数字と、代わりに何が大きいのかを残す。すべて現ビルド（`perf/kasane-opt` = `d3fc8ba`、
`build_kasane/cardputer_pocketjs.map` と `nm` / `objdump -dr`）の実測で、新しく実機を
測ったものは無い。

## 1. libgcc は 102 B しか無い

`map` の `Archive member included to satisfy reference by file (symbol)` を読むと、
引き込まれている libgcc のメンバは 2 つだけである。

| メンバ | サイズ | 引き込んだ側（要求シンボル） |
| --- | ---: | --- |
| `libgcc.a(_ffsdi2.o)` | 35 B | `esp_driver_gpio(gpio.c.obj)` (`__ffsdi2`) |
| `libgcc.a(_popcountsi2.o)` | 67 B | `efuse(esp_efuse_utility.c.obj)` (`__popcountsi2`) |

どちらも ESP-IDF 側が要求している。**こちらのコードをどう直しても 0 B も動かない。**

## 2. 64bit 除算のルーチンは Rust の compiler_builtins から来ている

`main/system` の 3 関数を含む C 側 17 ファイルが `__udivdi3` / `__umoddi3` / `__divdi3` /
`__moddi3` を呼ぶが、それらを**定義している**のは `libpocketjs_idf_ui_core.a` の中の
`compiler_builtins-…-cgu.NN.rcgu.o` である（Rust）。サイズと、そのメンバが入っている理由:

| 入力セクション | サイズ | メンバ | そのメンバが入った理由（map の 1 行目） |
| --- | ---: | --- | --- |
| `.text.__udivdi3` | 26 B | cgu.01 | `solar_sail.c.obj` が `__muldf3`（浮動小数）を要求 |
| `.text.__umoddi3` | 26 B | cgu.07 | cgu.03 の `linear_mul_reduction`（libm 経由） |
| `.text.__divdi3` | 135 B | cgu.03 | cgu.00 経由の `fmod` |
| `.text.__moddi3` | 126 B | cgu.10 | **Rust の core 自身**が `__udivti3` を要求 |

つまり 4 つの入口は**すべて別の理由で既にイメージに入っている**。したがって
**C 側の 64bit 除算を 1 つ残らず消しても flash は減らない**（減るのは呼び出しのサイクルだけ）。
同じ理由で `__udivti3` / `__umodti3`（各 50 B、`0x420b64ac` / `0x420b9d80` に配置）と
`specialized_div_rem::u128_div_rem`（2,651 B）も Rust core が保持している。

## 3. C 側の呼び出しは 36 箇所 / 17 ファイル

`objdump -dr` の reloc を関数ごとに数えた（`build_kasane/esp-idf/main/**/*.obj`）。
除算のコスト基準は本リポジトリの実測 `__divdi3` = **223 命令/本**
（[kasane-image-transform-recon.md](kasane-image-transform-recon.md):62）。

| 箇所 | ファイル | 関数 | 呼ぶ補助 |
| ---: | --- | --- | --- |
| 4 | `ksn_render.c` | `image_read` | `__divdi3` |
| 3 | `ksn_core.c` | `sample_tracks` | `__udivdi3` + `__umoddi3` |
| 3 | `sys_device.c` | `sys_device_wait_ticks` | `__udivdi3` + `__umoddi3` |
| 2 | `sys_state.c` | `sys_clock_snapshot` | `__udivdi3` + `__umoddi3` |
| 2 | `pet_hub_core.c` | `pet_hub_packet` | `__udivdi3` + `__umoddi3` |
| 2 | `pet_hub.c` | `clock_read` | `__divdi3` + `__moddi3` |
| 2 | `pet_assets.c` | `pet_assets_tick` | `__divdi3` |
| 2 | `pocket_av.c` | `js_player_method` / `js_player_open` | `__udivdi3` |
| 1 | `main.c` | `ui_task` | `__divdi3` |
| 1 | `ksn_render.c` | `ksn_render_rects` | `__divdi3` |
| 1 | `sys_wall.c` | `sys_wall_evaluate` | `__divdi3` |
| 1 | `sound.c` | `audio_task` | `__udivdi3` |
| 1 | `sound.c` | `sound_capture_level` | `__udivdi3` |
| 1 | `sound.c` | `probe_profile$constprop$0$isra$0` | `__divdi3` |
| 1 | `motion.c` | `motion_poll` | `__divdi3` |
| 1 | `pet_assets.c` | `say` | `__divdi3` |
| 1 | `pet_hub.c` | `alarm_set` | `__divdi3` |
| 1 | `pet_hub.c` | `pet_hub_pump` | `__divdi3` |
| 1 | `mp3_feed.c` | `worker` | `__divdi3` |
| 1 | `mp3_decode.c` | `pocket_mp3_decode` | `__divdi3` |
| 1 | `opus_feed.c` | `dec_task_fn` | `__divdi3` |
| 1 | `pocket_app.c` | `log_record` | `__divdi3` |
| 1 | `pocket_io.c` | `js_method` | `__divdi3` |

（`objdump` は reloc を 2 行で出すので生の行数は 72。上の表は呼び出し単位に畳んだもの。）

**この表の 1 行目は解消済み**（`mp3_decode.c` `pocket_mp3_decode`、§5）。

**消して効くのはサイズではなくサイクル**で、効く順は実行頻度が決める:
`image_read`（画像変換のスパン＝画素に近い）→ `sound` / `mp3` / `opus`（音声のサンプル路）
→ `pet_hub` / `ui_task` / `motion_poll`（フレーム毎）→ `sys_*`（フレーム毎に 1 回、
[system-pie-survey.md](system-pie-survey.md) の推定で 0.2 µs 程度）。

## 4. では何が大きいのか — compiler_builtins の 46,381 B

配置アドレスが非ゼロのセクションを**シンボル単位**で合計すると、`compiler_builtins` は
**46,381 B**（2,210,544 B のイメージの 2.1%）。上位は除算ではなく **libm の数学関数**である。

| シンボル | サイズ |
| --- | ---: |
| `libm_math::rem_pio2_large` | 4,031 B |
| `libm_math::pow` | 3,040 B |
| `specialized_div_rem::u128_div_rem` | 2,651 B |
| `libm_math::sinf` | 2,490 B |
| `libm_math::fma` | 2,459 B |
| `libm_math::cosf` | 2,402 B |
| `libm_math::tanf` | 1,925 B |
| `libm_math::cbrt` | 1,764 B |
| `libm_math::expm1` | 1,337 B |
| `libm_math::acos` | 1,290 B |

これを引き込んでいるのは Rust UI コア自身と、**こちらの float/double 使用**である。
`nm -u` で数えると `main/` の **34 ファイル**が soft-float 補助関数
（`__divsf3` / `__divdf3` / `__muldf3` / `__gtdf2` / `__fixdfsi` …）を参照している。
主なものは、シーンの `flower.c` / `solar_sail.c` / `stars.c` / `wave.c` / `ocean.c` /
`glass_rain.c` / `flower_species.c`、音声の `sound.c` / `mp3_decode.c`、
JS ブリッジの `pocket_av.c` / `pocket_io.c` / `pocket_ui.c` / `pocket_kasane.c` /
`pocket_power.c` / `pocket_clock.c`、時計の `solar_time.c` / `pet_hub.c`。

**効き方は実験でしか分からない。** libgcc / compiler_builtins はどちらもアーカイブの
メンバ単位でしか削れず、メンバは複数の要求元が共有している（`solar_sail.c` が
`__muldf3` を要求したために cgu.01 の `__udivdi3` 26 B も一緒に入っている、という形）。
したがって「float を固定小数へ移したら何 B 減るか」は、**移して map を差分するまで確定しない**。

## 5. 解消済み — `pocket_mp3_decode` の補間（1 サンプルごとの `__divdi3`）

位相 `phase` は出力サンプルごとに 24000 ずつ進み、`rate` を超えるたびに減算される。
補間は `previous` から `sample` への線形で、旧コードは

```c
int out=sample+(int)((int64_t)(d->previous-sample)*d->phase/24000);
```

—— これを **出力サンプルごとに 1〜2 回**（44.1/48 kHz では毎秒 4.4〜9.6 万回）踏んでいた。
`(previous-sample)*phase` は最大 65535×47999 で int32 に収まらないため、コンパイラは
libgcc の `__divdi3`（223 命令）を呼ぶ。

`phase = high*24000 + low` と割ると、`high` の項は 24000 で**割り切れる**ので

```
(previous-sample)*phase/24000
  == (previous-sample)*high + (previous-sample)*low/24000
```

が厳密に成り立つ（`X` が `24000` の倍数のとき `trunc((X+Y)/24000) = X/24000 + trunc(Y/24000)`）。
減算直後は `phase < rate ≤ 48000` なので `high` は 0 か 1、`low < 24000` で
`|previous-sample| ≤ 65535` だから第 2 項の積は最大 `65535*23999 = 1,573,405,665 < 2^31`
—— **全て int32 で厳密、溢れない**。

検証（すべてホスト）:

| 何を | どう | 結果 |
| --- | --- | --- |
| 式の総当たり | `a ∈ [-65535,65535]` × `phase ∈ [0,48000)` の全 6,291,408,000 組を int64 の旧式と比較（`/tmp/mp3sweep/sweep.c`） | **不一致 0**、積の溢れも 0 |
| 実データ | `ffmpeg` で作った 44.1k/48k/32k/24k/22.05k × モノ/ステレオ 10 本を `tools/test_mp3.c` で復号し、**前後の出力ハッシュを比較** | 10 本すべて**完全一致**（`output` サンプル数も同一） |
| 対象オブジェクト | `nm -u` | `__divdi3` が**消えた**（残るのは `__divsf3`/`sinf`/`cosf`/`lroundf` = フィルタ初期化の 1 回だけ） |
| 命令数（`-Os`、`objdump`） | 補間 1 回 | 旧: 223（libgcc）+ 前後 ≈ **233 命令** → 新: `sub`/`mull`/`mulsh`/`saltu` ほか ≈ **12 命令** |
| ファーム | `idf.py -B build_kasane build` | rc=0、`cardputer_pocketjs.bin` **2,210,576 B**（旧 2,210,544 B） |

**flash は減らない**（`__divdi3` の実体は Rust core が保持したまま。§2）。減るのは実行時間で、
その測り方は既存の `MP3DEC mean_us=…`（パケット毎）がそのまま使える —— 実機は未取得。

フィクスチャの作り方（`tools/test_mp3.sh` は引数でファイルを取る）:

```
ffmpeg -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=2" -ac 1 -b:a 128k \
       -write_id3v2 0 -write_xing 0 -id3v2_version 0 m44100.mp3
bash tools/test_mp3.sh m44100.mp3     # MP3_OK … hash=…
```

ID3v2 と Xing を切るのは必須 —— `tools/test_mp3.c` は先頭からフレームを読み、タグや
Xing フレームに当たると `packets>0` の assert で落ちる。

## 6. builtins を誰が生かしているか — cref と 1 回の実験

「builtins を減らせるか」は、リンカの相互参照表（`map` の Cross Reference Table）と、
**我々の参照だけを外したビルド 1 回**で決着する。`-Wl,--gc-sections` はリンク行に入って
いるので、最後の参照が消えた関数のセクションは落ちる。

### 6.1 参照元（cref の実測）

| シンボル | サイズ | 参照元 |
| --- | ---: | --- |
| `pow` | 3,040 B | **`quickjs.c`** ＋ cgu.02 |
| `cbrt` | 1,764 B | **`quickjs.c`** |
| `hypot` | 1,231+652 B | **`quickjs.c`** |
| `fmod` | 835 B | **`quickjs.c`**、`ocean` / `solar_sail` / `stars` / `wave`、picolibc の remainder |
| `sinf` | 2,490 B | 我々の 7 ファイル（flower / flower_species / glass_rain / mp3_decode / solar_sail / sound / stars）＋ cgu.04 |
| `cosf` | 2,402 B | 我々の 4 ファイル（flower / flower_species / mp3_decode / solar_sail）＋ cgu.08 |
| `tanf` | 1,925 B | **`wave.c` だけ**（＋ cgu.10） |
| soft-float 演算（`__muldf3` / `__divdf3` / `__adddf3` / `__divsf3` …） | — | 我々の C **＋ Rust（`pocketjs_core` / `ui_core` / `render_rgb565`）＋ IDF（pm / phy / mesh / i2s / sdmmc）＋ `quickjs.c` ＋ picolibc** |

`pow` / `cbrt` / `hypot` / `fmod` は **JS の `Math.*` の意味論そのもの**（QuickJS）が保持して
いる。soft-float も Rust・IDF・QuickJS が共有している。つまり**この 2 つは減らせない**。

### 6.2 実験（我々の参照だけ外して 1 ビルド）

`sinf` / `cosf` / `tanf` / `fmod` / `sqrtf` を参照している 11 ファイル
（`main/hal/motion.c` / `sound.c`、`main/pocket/mp3_decode.c`、`main/scene/{flower,flower_species,garden,glass_rain,ocean,solar_sail,stars,wave}.c`）で
**その 5 つを `0.0f` にマクロ上書き**して 1 回ビルドした（挙動は壊れる。サイズを測るだけの
実験で、実装はコミットしていない）。

| 何を | 基準 | スタブ後 | 差 |
| --- | ---: | ---: | ---: |
| `compiler_builtins` の配置済みセクション | 46,381 B（70 シンボル） | 42,042 B（67 シンボル） | **−4,339 B** |
| うち `cosf` | 2,402 B | 0 B | **−2,402 B（消えた）** |
| うち `tanf` | 1,925 B | 0 B | **−1,925 B（消えた）** |
| うち `sinf` | 2,490 B | 2,490 B | **±0（残った）** |
| picolibc の libm | 904 B | 904 B | ±0 |
| バイナリ全体 | 2,210,576 B | 2,199,136 B | −11,440 B |

**`sinf` が残った理由が結論そのもの**である: `sinf` の参照元の 1 つ cgu.04 は、我々の
`pocket_av.c` が `__gedf2`（double の比較）を要求したために入っているメンバで、その中身が
内部で `sinf` を呼んでいる。つまり

> 落ちる条件は ①我々の最後の参照が消えること **かつ** ②まだ必要な compiler_builtins の
> メンバが内部でその関数を呼んでいないこと の両方。

`cosf` / `tanf` は両方を満たして落ち、`sinf` は ② で残った。したがって次の一手は
**JS ブリッジ（`pocket_av` など）の double 比較を整数比較へ落とす**ことで、そこまで行けば
`sinf` も連鎖で落ちる見込みがある（同じ理屈で、`__muldf3` / `__divdf3` を要求している側を
1 つずつ外していくと、cgu メンバの数だけ builtins が減る）。

## 7. 測っていないこと


- サイズの候補（soft-float / libm を消したときの減少量）: §6.2 で**三角関数についてだけ**
  1 回測った（−4,339 B）。soft-float 演算（`__muldf3` 等）を消したときに何 B 落ちるかは
  **まだ測っていない** —— Rust・IDF・QuickJS が共有しているので、我々の側を全部外しても
  0 B の可能性がある。§4 の 46,381 B は「いま入っている量」であって「削れる量」ではない。
- `__divdi3` = 223 **命令**は命令数であってサイクルではない。
- 実行頻度はコードから読んだ見込みで、実機で数えたものではない。
- Rust 側（`libpocketjs_idf_ui_core.a`）は上流の成果物なので、本稿の対象外。
  ここを動かさずに compiler_builtins を減らせるかは未検討。
