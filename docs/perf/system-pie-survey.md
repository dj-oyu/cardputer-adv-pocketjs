# システムAPI（`main/system/`）に PIE の余地があるか — 調査

2026-09-16、ブランチ `perf/system-pie`（親 `origin/perf/kasane-opt` = `d3fc8ba`）。
対象は `vm/design-contracts` が 2026-09-16 に足したシステムAPI・通知ランタイム
（`main/system/sys_{state,clock,wall,timer,notify,ringer,device}.{c,h}`、14 ファイル 728 行）。
[system-runtime.md](../kasane/system-runtime.md) が設計、本書は**分析のみ**で、ソース・挙動・
実機には一切触れていない。したがってここにある数は「読んだ行」「実行したコマンドの出力」
「既存ドキュメントの値」のいずれかで、**このモジュールの実機時間を新たに測ったものは 1 つも無い**。

**結論を先に書く: PIE の余地はほぼ無い。** 理由は算術であって印象ではない —— この 728 行は
4/5/8/9 エントリの表に対する**有界な帳簿処理**で、8 レーンのレーン並列に乗る形が 1 つも無い
（§3）。1 フレームあたりの命令量は §2 の回数と §1 の命令数から **700〜1,500 命令**、
`render_ms` 2.32 ms（240 MHz = 556,800 サイクル）に対して **0.2% 前後**である。
やる価値があるとすれば fps ではなく「JS から毎回叩かれる面の 1 回あたりの値段」で、
そこにある唯一の大きい項は §4 の 64bit 除算 2 本（libgcc で **223 命令/本**）である。

## 1. 命令数（対象ツールチェーン、`-Os`、実測）

`xtensa-esp32s3-elf-gcc -Os` で 7 TU を単体コンパイルし、`objdump -d` で関数ごとに数えた
（`tools/hostshim` の `esp_timer.h` / `esp_err.h` / `vm_wake.h` で自己完結する）。

| 関数 | 命令 | 主な呼び出し元 |
| --- | ---: | --- |
| `sys_wall_evaluate` | 177 | 1 日 1 回のルール評価（`sys_wall_step`） |
| `sys_notify_post` | 134 | JS の通知 post、`sys_timer_step`、`sys_wall_evaluate` |
| `sys_device_step` | **125** | **owner ループ 1 回につき 1 回**（`main.c:765`）＋ `pocket_power_pump()`（`pocket_power.c:108`） |
| `sys_timer_set` | 118 | JS のタイマ設定 |
| `sys_wall_step` | 112 | `sys_device_step` の末尾（毎回。ただし期限が来ていなければ即 return） |
| `sys_clock_snapshot` | **92** | `sys_device_clock_read` / `sys_wall_step` / `solar_time_now` / `pet_hub` |
| `sys_notify_step` | 91 | `sys_device_step`（通知に変更があるときだけ） |
| `sys_power_step` | 81 | 1 秒に 1 回（`SYS_POWER_PERIOD_US`） |
| `sys_ringer_poll` | 71 | `sys_device_take_tone`（`pet_hub_pump`） |
| `sys_device_next_deadline` | 56 | `sys_device_wait_ticks`（フレーム待ち 1 回） |
| `sys_notify_schedule` | 54 | `sys_notify` の状態変化時 |
| `sys_clock_read` | 53 | `sys_device_step`（`sys_clock_take_update` が真のとき）＋ `gettimeofday` 1 回 |
| `sys_device_wait_ticks` | 48 | フレーム待ち 1 回（`main.c:973,986`） |
| 残り 25 関数 | 16〜43 | 購読の登録・解除、poll、deadline 読み |

libgcc の重い補助関数を呼ぶのは 3 関数だけで、`nm -u` と reloc で確認した:

| 関数 | 呼ぶ補助 | 命令数（補助込みの概算） |
| --- | --- | ---: |
| `sys_state.c` `sys_clock_snapshot` | `__udivdi3` + `__umoddi3`（`delta/1000000`, `delta%1000000`） | 92 + 223×2 = **538** |
| `sys_device.c` `sys_device_wait_ticks` | `__udivdi3` + `__umoddi3` | 48 + 223×2 = **494** |
| `sys_wall.c` `sys_wall_evaluate` | `__divdi3`（`local/86400`、符号付き） | 177 + 223 = **400** |

`__divdi3` が **223 命令**という値は本リポジトリの実測
（[kasane-image-transform-recon.md](kasane-image-transform-recon.md):62、
`objdump -d libgcc/_divdi3.o`）からの引用である。このコアの 32bit 除算はハードウェア
（`quou` 16〜18 サイクル、[pie-simd.md](pie-simd.md):233）だが、**64bit は libgcc のループ**
なので値段の桁が違う。

## 2. 回数（コードから読んだ値。この節の数だけが「1 フレーム」に効く）

| 呼び出し | 回数 | 根拠 |
| --- | --- | --- |
| `sys_device_step()` | **1〜2 回 / owner ターン** | `main.c:765`（owner ループ）と `pocket_power.c:108`（power 面が開いているとき） |
| `sys_device_wait_ticks()` | 1 回 / フレーム待ち | `main.c:973,986` |
| `sys_device_clock_read()` → `sys_clock_snapshot` | **JS の `pocket.clock` 呼び出しごと** | `pocket_clock.c:9` |
| 同 | 1 回 / owner ターン | `app_session.c:890` 周辺の pet hub 経路（`pet_hub.c:31`） |
| 同 | 1 回 / 太陽シーンのサンプル | `solar_time.c:16`（flower / solar 系の場面） |
| `sys_notify_active()` | 1 回 / owner ターン | `app_session.c:890` |
| `sys_power_step()` | 1 回 / 秒 | `SYS_POWER_PERIOD_US = 1,000,000` |
| `sys_wall_step()` | 1 回 / `sys_device_step`（期限未到来なら 112 命令の先頭で return） | `sys_device.c:80` |
| 通知・タイマの step | 変更があったときだけ | `sys_device.c:52-61` |

**画素ごと・サンプルごとに走るものは 1 つも無い。** このモジュールの最大の呼び出し頻度は
「owner ターン（≒フレーム）に 1〜2 回」である。したがって定常状態の 1 フレームは
`sys_device_step` 125 ×1〜2 ＋ `sys_clock_snapshot` 538 ×1〜2（pet hub と solar）＋
`sys_notify_active` 23 ＋ `sys_poll` 数本 ≒ **700〜1,500 命令**。240 MHz で 1.0〜1.3 IPC なら
**4〜6 µs / フレーム**、`render_ms` 2.32 ms の **0.2% 前後**である（IPC は pie-simd.md の
スカラー計測からの借用で、この経路で測った値ではない）。

## 3. PIE 適性（なぜ乗らないか）

PIE（`docs/perf/pie-simd.md`）が効くのは「8 本の int16（または 4 本の int32）のレーンに
同じ演算を、分岐なしで、連続したデータへ」当てられる形である。この 728 行を機能ごとに
分類すると:

| 種類 | 例 | レーン並列になるか |
| --- | --- | --- |
| 購読の dirty mask 掃き出し（8 エントリ × topic） | `sys_notify_publish` / `sys_timer_publish` / `publish_clock` / `sys_power_step` の末尾 | **唯一それらしい形**（`pending \|= interest & TOPIC` を 8 エントリへ）。ただし 8 エントリ = 1 命令の世界（`interest`/`pending` は 32bit、記録は 16 B ストライド）で、1 回の掃き出しは数十サイクル。**元が小さすぎて PIE のセットアップ（`EE.VLD.128` + QR の入れ替え）が上回る** |
| スロット探索（`id` 一致＋`strcmp`） | `find` / `notice_find` / `sys_timer_set` / `sys_timer_read` | ならない。分岐多数＋バイト列比較で、レーン内で結果が違う（divergence） |
| 有界な期限と位相の帳簿（4〜9 エントリ、早期 return） | `schedule` / `waiting` / `next_order` / `sys_wall_step` | ならない。制御フローが主で、データは 1 本ずつ別の意味を持つ |
| 64bit の時計算術 | `sys_clock_snapshot` / `sys_device_wait_ticks` / `sys_wall_evaluate` | **PIE に 64bit レーンは無い**。32bit レーンへ割って `EE.VMULAS.S32`/QACC（40bit）で逆数乗算にする道はある（§4-1） |
| 記録の丸ごとコピー（12〜80 B） | `*out=s->power` などの構造体代入、`sys_notify_active` | なる（`EE.LD.128` / `EE.ST.128` の 16 B 対）。ただし libc `memcpy` の 1 回が 20〜30 サイクル、頻度は API 1 回に 1 つ |

`sys_notify.next_order` の O(9²) は `next_order == UINT64_MAX` のときだけ走る救済路で、
定常では 1 命令である。`SYS_NOTICE_SLOTS 9` / `SYS_TIMER_SLOTS 4` / `SYS_WALL_RULES 5` /
`SYS_SUBSCRIPTIONS 8` / 記録は 80 B（`sys_notice`）—— **どの表も QR レジスタより小さく、
8 レーンを埋めない**。

## 4. 候補（PIE ではないものも含めて、大きさの順）

| # | 候補 | 大きさ（推定） | PIE か | リスク | 撤退線 | 実機なしの検証 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | **`sys_clock_snapshot` の 64bit 除算 2 本**（`delta/1000000` と `delta%1000000`）を逆数乗算へ。`delta` は単調増加なので、秒を `2^32` 境界で扱うか、`EE.VMULAS.S32`＋QACC（40bit）で 1/1e6 の逆数乗算にすれば `__udivdi3`/`__umoddi3` の 446 命令が数十命令になる | 538 → 150 命令前後（**呼び出しごと**）。1 フレーム 1 回なら削減は 400 命令 ≒ 0.2 µs で無視できる。**採算はゲストの呼び出し回数が決める**（毎フレーム 100 回読むアプリなら 4 万命令 ≒ 0.15 ms） | 半々（算術だけ PIE の 40bit 積和が使える。レーン並列ではない） | 中。丸め方向と範囲（負の delta、`micros>=1000000` の繰り上げ）が変わりうる。既存の `x≤65152` の `/255` 恒等式のような**総当たりの証明**が要る | 旧経路を runtime 変数で残し 1 バイナリで A/B。証明が崩れたら戻す | `sys_clock_snapshot` の全入力域（`delta` 0〜2^40、`micros` 0〜999999）を旧式と総当たりで一致させる。`run_models.py` に 1 本足す |
| 2 | `sys_device_wait_ticks` の 64bit 除算 2 本（フレーム待ち 1 回） | 494 → 120 命令前後。**1 フレーム 1 回**なので合計 0.4 µs 弱 | 半々（同上） | 低（同じ式の書き換え） | 同上 | 同上（この式は待ちの粒度だけを決める。`sys_device_test` の期限試験が判定する） |
| 3 | `sys_wall_evaluate` の `__divdi3`（`local/86400`、符号付き） | 400 → 250 命令前後。**1 日 1 回 × ルール 5 本**なので合計は無視できる | 半々 | 中（符号付き。負の入力で floor の扱いが動く） | 同上 | `sys_wall_evaluate` の総当たり（`utc` の全域 × offset ±50400 × minute 0..1439） |
| 4 | 記録コピーの `EE.LD.128`/`EE.ST.128` 化（構造体代入と `memcpy`） | 1 回 20〜30 → 4〜8 サイクル。**1 フレームに数回**なので合計 100 サイクル未満 | PIE の得意な形 | 低（アラインメント契約が要る。記録は 80 B で 16 B 整列とは限らない） | 属性/関数を分けて旧経路を残す | 画素ではないので**全フィールドの一致**（memcmp）。`sys_device_test` / `tools/kasane_contract` の該当試験 |
| 5 | 購読スイープの PIE 化 | 8 エントリ × 4 topic。**合計 100 サイクル以下** | 形はなる | 低 | — | 不要（回数がホストで数えられる） |

**1〜5 を全部入れても 1 フレームの削減は 3〜6 µs のうちの 1〜2 µs**（＝0.05% 程度）である。
したがって「この API 群を PIE で速くする」は、**fps を目的にするなら採算が合わない**。
採算が合うのは次のどちらか:

- **JS 面の 1 回あたりの値段**を下げたいとき（`pocket.clock` は JS から任意回数呼べる。
  ゲストが毎フレーム読む前提なら候補 1 が効く。**まず `KASANE_PAINT` のような計器で
  `sys_device_step` と `sys_clock_snapshot` を `rsr.ccount` で括る** —— このモジュールには
  まだ 1 つも計器が無い）
- 待ちの粒度（`sys_device_wait_ticks`）が**起床の遅れ**に効いているとき。命令数を削っても
  フレームレートは動かないが、待ちの上限計算が 494 命令（libgcc 2 本）である事実は
  アイドル復帰の設計に効く。

## 5. 測っていないこと / 主張しないこと

- **実機の時間は 1 つも無い**。`system-runtime.md` の CP14a/CP14b1/CP14b2 は容量と構造の
  記録（`state` 144 B、追加 heap/task 無し）で、時間の実測ではない。§2 の 4〜6 µs は
  命令数 × 回数 × 借用 IPC の**推定**である。
- IPC 1.0〜1.3 は pie-simd.md のスカラー計測からの借用で、この経路で測った値ではない。
- libgcc の `__divdi3` 223 **命令**は命令数であってサイクルではない
  （kasane-image-transform-recon.md の実測は objdump の命令数）。
- 呼び出し回数はコードから読んだ値である。**実機で数えた回数ではない**
  （`g_ksn_prof` のような計器がこのモジュールには無い）。
- `sound_tone()`（`pet_hub.c:106` から呼ばれる）と `board_battery_read()` は
  モジュールの外で、この調査の対象外。前者は音声の合成側なので、別途 PIE の余地が
  あるかは別の調査になる（`pet_hub` は `sys_device_take_tone()` の真偽だけを見る）。
- `EE.VMULAS.S32` と QACC（40bit）で 64bit の逆数乗算を組めるかは**机上**で、
  このコアで組んだ前例は本リポジトリに無い（PIE の実績は int16 8 レーンが主）。
  着手するならまず `tools/pie/piesim.py` に命令を足すところから。

## 6. 次に測るもの（この順）

1. `sys_device_step` と `sys_clock_snapshot` を `rsr.ccount` で括る計器
   （`g_ksn_prof` と同じ形、`KSN_SYS_PROF` で切り、既定 OFF）。ホスト側は回数の契約だけを
   持ち、ミリ秒は実機。
2. JS 面の回数: `pocket.clock` / `pocket.power` を毎フレーム読むアプリを 1 つ作り、
   1 フレームの呼び出し回数を数える（ホストで数えられる）。
3. その回数 × 候補 1 の削減量が 0.1 ms/フレームを超えるなら候補 1 に着手。超えないなら
   **このモジュールには手を入れない**（測ってから決める、が pie-simd.md §4.6 の結論）。
