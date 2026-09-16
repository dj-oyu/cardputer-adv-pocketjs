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

## 5. 測っていないこと

- サイズの候補（soft-float / libm を消したときの減少量）は**推定すらしていない**。
  §4 の 46,381 B は「いま入っている量」であって「削れる量」ではない。
- `__divdi3` = 223 **命令**は命令数であってサイクルではない。
- 実行頻度はコードから読んだ見込みで、実機で数えたものではない。
- Rust 側（`libpocketjs_idf_ui_core.a`）は上流の成果物なので、本稿の対象外。
  ここを動かさずに compiler_builtins を減らせるかは未検討。
