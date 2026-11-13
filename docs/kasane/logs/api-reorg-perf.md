# Kasane API 整理の計測記録（api-reorg-perf）

[api-reorg.md](../api-reorg.md) §6 の各段の前後で取る指標の記録。1 段につき 1 行を下の表に足す（`tools/r5sim/reorg.sh -a`）。**表の数はすべて r5sim（QEMU）のカウントか、ファームの map の大きさで、実機の時間ではない**（実機の D1〜D3 は別の行で、出典を付けて書く）。

## 1. 道具

`tools/r5sim/reorg.sh [-a] [-n] [<build root>]`（既定 `.cache/reorg`）が、次の 4 つの負荷を `tools/r5sim/fast.sh <dir> -DR5SIM_DEVICE=1 -DR5SIM_APP=<負荷>` で回し、ファームを `idf.py -B <root>/fw -DSDKCONFIG=<root>/fw/sdkconfig build size` で組んで、下の表の 1 行を出す。`-a` で表に足す。`-n` はファームを組まない（その欄は `-`）。初回は 4 つの像とファームを組むので約 6 分、2 回目からは約 1 分。

| 負荷 | 中身 | ターン |
| --- | --- | --- |
| `shiki` | 今の `apps/stress/stress.js`（Shiki で描く STRESS、LV1） | 120、61〜120 を数える |
| `shiki_v1` | `4840504` の `stress.js` と `stress_prog.js` の固定の写し（`tools/r5sim/main/stress_shiki_v1*.js`）。アプリを新しい呼び方へ移した後も、古い呼び方のままの負荷でファームの退行を見るためのもの。**P5（2026-11-13）で終わった**: 呼んでいた `poll`・`cancel`・`features`・`gridFold`・`shiki.grid` などを消したので、負荷と `tools/r5sim/main/stress_shiki_v1*.js`・`-DR5SIM_APP=shiki_v1` を `reorg.sh` から外した。以後の行は、この列が無い（下の「§3.1 の P5 以降の表」）。 | 120、61〜120 |
| `hud` | LCD CATCH（`apps/lcdcatch`）。Shiki なし、保持型（`replace`/`patch`・ref・cache）だけ。ターン 30 で GAME A、以後は `test_lcd_catch_host.c` と同じボット（一番下の星の列へ 1 ターン 1 回の打鍵）がターンの外で `lcdCatch.sky` を読んで `pocket.input` へ鍵を入れる | 240、61〜240 |
| `derby` | DERBY WATCH のデモのレース 1 本。チャンクは `make_app_chunks.py` と `lower_plans.mjs`、Shiki の表は `emit_shiki_rom.mjs` でファームと同じに作る。`pocket.derby`・`pocket.app.load`・`pocket.input` は本物、店・音・乱数の種は `test_derby_host.c` の台本と同じ（1500 点、レース 3、種 `0x2545f491`）。`pocket.time.now()` は 1 ターン 33.3 ms で、ターン 91 で 15 s 進めてデモを始める（450 ターンの待機を省く）。デモの終わり（`DERBY DEMO END`）で止める | 1773（今の像）、場面別は 31 から、全体は 61 から |

出す行（`fast.sh` の出力、`<root>/out/<負荷>.txt`）:

- `R5SIM ccount turns=61..N per_turn=`: 1 ターン（`frame()` と `pocket_kasane_end_turn`）のカウント（[qjs-observe.md](../../vm/qjs-observe.md) §9。1 カウント ≒ 6.54 命令）。
- `R5SIM ccount scene=<場面> turns= per_turn= max= alloc_per_turn=`（`derby` だけ）: ターンを、そのターンの後の場面で分ける。`wait`＝デモの前のパドック、`pad`・`gate`・`photo`・`res`＝デモの中のその場面、レースのターンはカメラで `wide`（`WIDE`、首振りの `WIDE 1〜3` を含む）・`field`・`close`・`vision`・`headon`・`finish`（スロー）、`race`＝最初の `CAM` の前の 2 ターン。
- `R5SIM alloc turns=61..N per_turn= free_per_turn= max= between= live_delta=`: ゲストの確保の回数。ゲストのアロケータの小塊キャッシュは、どの像でも全部の `guest_malloc` を当たり・外れとして数えている（`block_cache_pop`）ので、その和をターンの前後で読む（`pocketjs_guest_alloc_counts`、`POCKET_GUEST_ALLOC_COUNTS` のときだけ `guest.c` に入る。ファームには入らない）。解放はターン 61 からの確保の総数から、生きているブロック数（QuickJS の `malloc_count`）の増分を引いたもの。`between` はターンの外の確保（0 であること）。`realloc` は中で行った確保と解放として数える。ネイティブ側の確保（Kasane・Shiki の区画）は入らない。
- `R5SIM device ... heap_used=`: 120（240、1773）ターン後のゲストの使用量。

## 2. 読み方

- **同じ像なら、2 回走らせて全部の行が同じ。** ただし QEMU を `-icount shift=2,sleep=off` で動かす（`reorg.sh` が `R5SIM_ICOUNT` で `fast.sh` に渡す）。`fast.sh` の既定の `sleep=on` では、起動中に CPU が休む間だけ仮想時計が実時間に従うので、tick 割り込みがターンのどこに入るかが走らせるたびに変わる。STRESS の 60 ターンの平均はそれでも同じだったが、DERBY の場面別（数十ターン）は数カウント動き、`max` は毎回動いた（2026-11-13、同じ像で 2 回: `close` 104,114 と 104,106）。`sleep=off` では 3 回とも全部の数が同じ（STRESS は `sleep=on` と同じ 154,586）。
- **カウントは像の配置で 0.01〜0.03% 動く。仕事が同じでも。** 実測（2026-11-13、同じ `shiki` の負荷）:
  - `4840504` のまま、置き場所だけ違う（親のチェックアウト 154,595、worktree 154,611）。ESP-IDF の `-fmacro-prefix-map` はプロジェクト（`tools/r5sim`）と IDF の中しか写さないので、`main/` と `components/` の `assert` のパスが `.flash.rodata` に絶対パスで残り、その長さで配置が変わっていた。**`tools/r5sim/CMakeLists.txt` でチェックアウトの根を `.` に写すようにした**（`-DR5SIM_PATH_MAP=0` で元の像）。これで worktree と親の数が揃う見込み（親のパスでは未確認）。
  - 計測の道具を足しただけ（ターンの中は同じ、`.flash.text` +464 B、`.flash.rodata` +96 B、DRAM は同じ）: 154,611 → 154,586。
  - パスの写しを入れただけ: 154,586 → 154,569。
  - `shiki` と `shiki_v1`（JS はバイト単位で同じ、埋め込みの名前と配置が違う）: 154,569 と 154,614（+45、0.03%）。`heap_used` の差 12 B はソースの名前（`stress_shiki_v1.js`）の長さ。
  
  QEMU はキャッシュを持たないので、原因は命令の数そのもの（`.flash.rodata` の位置で、未整列のコピー・比較の経路が変わる、と推測。確かめていない）。**段の前後の比較は、同じチェックアウトから組んだ行どうしで、0.05% 未満の差は配置として読む。** api-reorg.md §6 P0 の受け入れ基準 (b)「154,595 と一致」は、この理由で文字どおりには満たせない（下の §4）。
- **`derby` の数は QEMU の空きヒープの上で取る。** DERBY は `pocket.memory.info().internalFreeBytes`（ターンの始めの標本。r5sim でもターンごとに `heap_caps` の実際の値で採る。評価中は実機と同じく `null`）が 18,432 B 未満だと plan を読み込まない。QEMU の空きは実機よりずっと多い（像の DIRAM 96 KB、無線もパネルも無い）ので、実機で起きる「空きが足りず読み込みを待つ」ターンは r5sim には無い。`-DR5SIM_FREE_KIB` の重しは試していない。
- `wide` が DERBY のターンの 43%（759/1773）で、1 ターンが最も重い（STRESS の 1.43 倍）。Shiki のエンジンは r5sim では C のモデルで走る（JIT と PIE が無い）ので、場面どうしの比の描画の部分は実機と違う（qjs-observe.md §12）。

## 3. ベースライン

### 3.1 表（1 段 1 行）

列: r5sim のカウント（`per_turn`）は負荷ごと、`derby` の場面は `wait`/`wide`/`field`/`vision`/`headon`。確保は 1 ターンの回数（`shiki` / `shiki_v1` / `hud` / `derby`）。ゲストは `heap_used` B（同じ順）。像は `shiki` の r5sim の Flash Code B、ファームの DIRAM used と Flash Code B（`idf.py size`）。

| 日付 | sha | shiki | shiki_v1 | hud | derby | wait | wide | field | vision | headon | 確保/ターン | heap_used | r5sim flash | fw DIRAM | fw flash |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 2026-11-13 | `f38b1ef` | 154569 | 154614 | 10459 | 150175 | 45305 | 220725 | 123399 | 142652 | 94344 | 181 / 181 / 4 / 78 | 75372 / 75384 / 47620 / 104220 | 553548 | 181332 | 1722132 |

最初の行は P0 の道具を足したコミット `f38b1ef`（親の `1e66bbf` は `4840504` に文書だけを足したもの。`main/` は `4840504` と同じ）で、2 回走らせて全部同じ。ファームの 2 つの値は `4840504` の親セッションの記録（DIRAM used 181,332・flash code 1,722,132）と一致した（`guest.c` に足した関数は `POCKET_GUEST_ALLOC_COUNTS` の門の中で、ファームには入らない）。

### 3.2 DERBY の場面別（`f38b1ef`、上の行と同じ走行）

| 場面 | ターン | per_turn | max | 確保/ターン |
| --- | --- | --- | --- | --- |
| wait（デモ前のパドック、31〜90） | 61 | 45,305 | 341,872 | 82 |
| pad（デモ） | 72 | 43,871 | 411,752 | 79 |
| gate | 41 | 93,363 | 126,846 | 88 |
| race（最初の CAM の前） | 2 | 88,611 | 158,238 | 161 |
| wide | 759 | 220,725 | 290,560 | 83 |
| field | 145 | 123,399 | 124,617 | 75 |
| close | 98 | 104,114 | 111,235 | 85 |
| vision | 146 | 142,652 | 148,682 | 98 |
| headon | 53 | 94,344 | 100,952 | 72 |
| finish（スロー） | 103 | 113,301 | 148,752 | 73 |
| photo | 111 | 67,822 | 717,042 | 64 |
| res | 152 | 51,873 | 87,834 | 40 |
| 全体（61〜1773） | 1,713 | 150,175 | 717,040 | 78（解放 78） |

デモのレースは `race=1000001 seed=D568DA89`、PICK 7、`order=7,1,5,2,4,6,3,8`。カメラは WIDE 1 → WIDE → FIELD → WIDE 2 → WIDE 3 → CLOSE → VISION → WIDE 3 → HEAD ON → FINISH。予算（8 ms）で `frame()` が持ち越されたターンが 4 つあり、続きは同じターンの中で数えている。`max` の大きいターンは場面の切り替わり（plan の登録、`replace`）。

### 3.3 `pocket.kasane` を最初に読む費用（host、P0 の 4）

`tools/build_kasane_ns_cost_test.sh`（`tools/test_kasane_ns_cost.c`）。ゲストヒープを実機の TLSF の数え方（4 B に切り上げ、最小 12 B）で、GC の後に測る。host の `pocket_api_lazy` は install の時に名前空間を作るので、install の前後の差が「最初に読んだ時」の費用（遅延の getter そのものは入らない。関数の表の項目は QuickJS が最初の使用で作るので、どちらにも入らない）。2026-11-13、実測。

| | `M32=1`（実機と同じ 8 B の JSValue、4 B のポインタ） | 64 bit host |
| --- | --- | --- |
| Shiki の区画が無いセッション | **+1,856 B**、21 ブロック（オブジェクト 6、プロパティ 6、shape 2、atom 7） | +2,996 B |
| Shiki の区画があるセッション（`kasane.shiki` も作る） | **+2,084 B**、25 ブロック | +3,308 B |
| そのうち `grid`（とグローバルの `gridFold` の遅延の口） | +284 B、7 ブロック | +404 B |
| そのうち `video` | +180 B、4 ブロック | +248 B |
| そのうち `pixel` | +180 B、4 ブロック | +248 B |

「そのうち」は、各メンバーを新しいセッションの空のオブジェクトに単独で install した差。`grid`・`video`・`pixel` の 3 つで 644 B。api-reorg.md §3.6 の 1 行目の見積もり（−1〜1.5 KB）は、この 3 つを遅延にする効果としては大きすぎる（実測 644 B。関数は QuickJS が最初の使用まで作らないので、見積もりが数えた関数のオブジェクトは今も作られていない）。`gridFold` の本体（`grid_fold.js` の評価）は読んだ時に初めて走るので、ここには入らない。

## 4. P0 の受け入れ基準に対して

| 基準 | 結果 |
| --- | --- |
| (a) 4 つの負荷が `oom=0 fail=0 bad_present=0` で終わり、2 回で同じカウント | 満たした（`sleep=off` で、場面別・`max`・確保まで全部同じ） |
| (b) `shiki` が 154,595 | **文字どおりには満たせない。** 道具を足す前の像でも、置き場所（worktree）だけで 154,611 になった（§2）。道具を足した後の同じチェックアウトで 154,586（−0.016%）、パスを写した後 154,569。`heap_used` 75,372 と確保の回数は同じ。ターンの中の仕事は変わっていない、と判断した（配置の影響の大きさは §2 の 4 例で 16〜45） |
| (c) sha つきのベースライン | 上の表（`f38b1ef`） |
| (d) 実機 | この行には無い（親セッションが broker で取る） |

計画からの違い:

- DERBY の待機画面は、450 ターンの待機の代わりに、ターン 91 で時計を 15 s 進めてデモを始めた（`test_derby_host.c` の `jump_start` と同じ）。`wait` はターン 31〜90 の 60 ターン（＋デモの終わりのパドック 1 ターン）。
- `derby` と `hud` は `-DR5SIM_DEVICE=1` が要る（ファームのセッションで回す。無いとコンパイルで止める）。
- `hud` の鍵は host と同じく本物の `keymap.c`・`keystate.c`・`pocket_input.c` に通す。ボットとキーの pump はターンの外。
- 確保の回数は大きさ別ではなく回数だけ（大きさ別は従来どおり `-DR5SIM_GUEST_ALLOC=1` の版）。
- r5sim の像にチェックアウトのパスの写しを足した（§2）。
- P0 の 6（トレース）と 7（実機）は行っていない。
| 2026-11-13 | `2ef6480` | 154569 | 154615 | 10459 | 150175 | 45303 | 220729 | 123401 | 142649 | 94335 | 181 / 181 / 4 / 78 | 75372 / 75384 / 47620 / 104220 | 553548 | 181316 | 1694808 |
| 2026-11-13 | `67c2fc6` | 153906 | 153965 | 10375 | 134217 | 44092 | 201082 | 106700 | 116143 | 79636 | 172 / 172 / 3 / 57 | 75748 / 75952 / 47900 / 101108 | 554296 | 174064 | 1696768 |

## 5. P4b（DERBY の走者を `pocket.derby.runners()` へ、2026-11-13）

[derby-runners-native.md](../../apps/derby-runners-native.md) §5。`derby` の場面の行に、ターンの内訳 `js=`（`frame()`）・`shiki=`（その中の Shiki の draw の native、`pocket_shiki_turn` の `total_cy`）・`end=`（`pocket_kasane_end_turn`）・`draws=` を足した（`tools/r5sim/main/r5sim.c`）。下の数は `2ef6480` の worktree で、内訳を足した後の像どうし（`reorg.sh` の行ではない。`shiki`・`shiki_v1`・`hud` はこの段で変わらないので回していない）。

| 場面 | 前（`2ef6480`） | 後（P4b） | 確保/ターン |
| --- | ---: | ---: | --- |
| wait | 45,314 | 45,283 | 82 → 78 |
| gate | 93,370 | 77,714 | 88 → 78 |
| wide | 220,734 | 201,364 | 83 → 70 |
| field | 123,404 | 107,169 | 75 → 65 |
| close | 104,117 | 92,718 | 85 → 71 |
| vision | 142,654 | 116,535 | 98 → 77 |
| headon | 94,343 | 80,103 | 72 → 66 |
| finish | 113,312 | 96,649 | 73 → 63 |
| 全体（61〜1773） | 150,175 | 134,636 | 78 → 66 |

`heap_used` 104,220 → 100,780 B。

## 6. P3（API を足す、非破壊、2026-11-13）

[api-reorg.md](../api-reorg.md) §6 P3。前後とも同じチェックアウト（worktree）と同じビルドの根（`.cache/reorg_before`）で `reorg.sh` を回した（`sleep=off`）。前は `74e3005`、後は `65bbd40`（表の行は `shiki.grid` の直し（下）の前の作業木で取った）。

| 日付 | sha | shiki | shiki_v1 | hud | derby | wait | wide | field | vision | headon | 確保/ターン | heap_used | r5sim flash | fw DIRAM | fw flash |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 2026-11-13 | `74e3005` | 154569 | 154615 | 10459 | 134645 | 45280 | 201361 | 107165 | 116527 | 80105 | 181 / 181 / 4 / 66 | 75372 / 75384 / 47620 / 100780 | 553548 | 181316 | 1696196 |
| 2026-11-13 | `65bbd40` | 153899 | 153960 | 10375 | 134222 | 44103 | 201089 | 106695 | 116135 | 79641 | 172 / 172 / 3 / 57 | 75748 / 75952 / 47900 / 101108 | 554420 | 174048 | 1696816 |

読み方:

- **確保**: STRESS は 1 ターンに `patch` か `replace` を 1 回だけ呼ぶので、181 → 172 の **−9 が 1 回の呼び出しで消えた確保**（ticket・tx・modal の 3 オブジェクトとそれぞれのプロパティの配列、tx への `modal` の追加）。DERBY も 66 → 57（場面ごとに −8〜−10。1 ターン 1 回）。`hud`（LCD CATCH）は 4 → 3: 変化のあるターンにしか `patch` しないので、平均では −1（ターンの最大は 47 → 38）。api-reorg.md P3 の基準「`hud` で 3 回以上」は、この負荷では呼び出しの回数が足りずに数字としては満たさない（1 回あたりは STRESS・DERBY で −9）。
- **カウント**: `shiki_v1`（古い呼び方の固定の写し）−0.42%、`shiki` −0.43%、`hud` −0.80%、`derby` −0.31%（場面は全部下がった）。配置の揺れ（0.05% 未満）より大きい。
- **`heap_used`**（最後のターンの後の使用量、ごみを含む）は +280〜+568 B。確保が減って GC の時機が動いたことと、使い回す tx・modal の 2 つが常駐すること（host の DERBY で GC 後の生きている量 +116 B、下）が混ざっている。評価の後の量は下がった（下）。
- **ファーム**（`idf.py size`）: 表の行の後、`shiki.grid` の getter を `pocket_api_root()` から install の時の kasane の名前空間（借りた参照）へ直した（host の検査の 1 本が `pocket_api_root` を持たずにリンクに失敗したため。毎ターンの経路は同じなので r5sim は回し直していない）。直した後の最終の像: DIRAM 181,316 → 174,064（**−7,252 B**。`.bss` 76,624 → 69,456 の −7,168 B は grid の表 7,200 B から、借りた参照など +32 B。IRAM −84 B は `parse_rect_mode` が `parse_i16` を内側に展開しなくなった分: 315 → 236 B。`mutate`・`parse_i16` は同じ大きさで、3 つとも IRAM のまま）。flash code 1,696,196 → 1,696,768（+572 B）、flash data 785,364 → 785,668（+304 B）、計 +876 B。

host（実測、`-m32`、実機と同じ 8 B の JSValue）:

| | 前 | 後 |
| --- | --- | --- |
| `pocket.kasane` を最初に読む（Shiki なし / あり、§3.3） | 1,856 / 2,084 B | 1,344 / 1,524 B |
| そのうち grid（と `gridFold`）/ video / pixel | 284 / 180 / 180 B | 104 / 0 / 0 B |
| DERBY 評価の後 / 評価のピーク（`run_derby.py --m32`） | 108,976 / 125,968 B | 108,820 / 125,812 B |
| DERBY GC 後の生きている量の最大 | 138,076 B | 138,192 B（+116、常駐の tx・modal） |
| DERBY の確保 / フレーム（race） | 232.6 回、9,105 B | 223.6 回、8,718 B |
| STRESS 評価と最初の present の後（`STRESS_HEAP_LIMIT=163840`） | 62,017 B | 61,646 B |
| HELLO 相当の `mount('hello')` の後（`test_pocket_kasane`、64 bit host） | 37,693 B | 37,519 B |

STRESS の LV3（host、900 フレーム）は、後の像では最初の OOM が `frame`（`replace` の作り直しの経路を通る）、前の像では `read` で起きた（OOM 2 回と 1 回、どちらも `STRESS_HOST PASS`、表示の誤り 0）。ヒープの並びが変わって、負荷が上限に当たる場所が動いた。
## 7. P4b の実機（`74e3005`、broker の app_session、2026-11-13、実測）

同じ像（`74e3005`）で、セッション A（既定の native）と、セッション B（レース中に `up` で旧 JS へ切替、`DERBY AB JS`）を走らせた。デモのレースの種は起動ごとに違うので、比べるのは場面と draws が揃う窓どうし。`KASANE_PAINT turn_ms`（30 フレームの平均）。

| 場面 | native | 旧 JS（同じ像） | 差 |
| --- | ---: | ---: | ---: |
| CAM WIDE 2（draws 約 50、pat 約 40） | 11.52・11.71（B の切替前） | 12.39〜12.88（B の切替後、6 窓） | 約 −1.1 ms（−9%） |
| CAM VISION（draws 33） | 6.80〜6.91（A） | 8.40〜8.66（B） | 約 −1.6 ms（−20%） |
| CAM WIDE 3（draws 27〜30） | 6.99〜7.42（A） | 8.95〜9.41（B） | 約 −1.9 ms（−21%） |
| CAM CLOSE（draws 18） | 5.99〜6.01（A） | 7.00〜7.11（B） | 約 −1.0 ms（−15%） |

- 起動直後の `MEM`: `4840504` は free=44,888・js=95,196、`74e3005` は free=47,316・js=92,124（js −3,072 B）。
- A はレースを FINISH・写真判定まで走り、OOM・FRAMEFAIL なし。FINISH の窓の `shk=` の最大の空き塊は 2,492 B（従来と同じ下限）。
- 旧像（`4840504`）との比較（像が違うので参考）: 待機画面 3.92 → 3.83 ms、CAM WIDE 7.3 → 6.15 ms、CAM FIELD 7.5 → 6.3〜6.4 ms。
- **次の山:** draws が 50 前後になる CAM WIDE 2 は native でも 11〜12 ms。Shiki の engine は 0.5〜0.6 ms なので、残りは JS 側（観客の pat の draw の組み立て）と見られる（推定。r5sim の場面別の内訳で確かめる）。→ r5sim で分けた結果、JS ではなかった。重い窓の増分の 89% は `pocket.derby.ser()` の倍精度の計算で、観客の draw も `ser()` から出る（[derby-ser-cost.md](../../apps/derby-ser-cost.md)）。

## 8. P3 の実機（`67c2fc6`、broker の app_session、2026-11-13、実測）

- STRESS LV1: `turn_ms` 6.19〜6.34（`4840504` は 6.04〜6.40。アプリ未移行なので変わらないのが期待どおり）。起動直後の `MEM free=78,712 largest=31,744 js=44,648`（`4840504` は free=70,072・js=44,936。+8.6 KB は P1 と grid の表を使う時に確保した分）。
- GRID LAB: 3 本の plan を登録（`GRID_APP REGISTER 3..5`）、PIE で `FRAME 48..192`、MODE 0→1。エラー無し。
- MEGADEMO: チャンク 7 本を読み込み、WAKE → NEWS（plans=14 を登録）、エラー無し。
- VIDEO LAB: `FRAME 0..180`、`turn_ms` 1.3、エラー無し。
- 物理の表示（LCD）は見ていない（`board_capture` は転送前のバッファを写すので、ソフトウェアでは確かめられない）。

## 9. P4 の STRESS（`Int32Array` の入力、確保の無いターン、2026-11-13）

[api-reorg.md](../api-reorg.md) §5 の 2・§6 P4。`apps/stress/stress.js` だけを変えた（式 `stress_prog.js` とファームは同じ）。前後とも同じ worktree で `reorg.sh -n`（`sleep=off`）。前は `67c2fc6`、後はその作業木に新しい `stress.js` を置いたもの（カウント、QEMU）。

| 日付 | sha | shiki | shiki_v1 | hud | derby | wait | wide | field | vision | headon | 確保/ターン | heap_used | r5sim flash | fw DIRAM | fw flash |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 2026-11-13 | `67c2fc6` | 153906 | 153965 | 10375 | 134217 | 44092 | 201082 | 106700 | 116143 | 79636 | 172 / 172 / 3 / 57 | 75748 / 75952 / 47900 / 101108 | 554296 | - | - |
| 2026-11-13 | `67c2fc6+dirty` | 142560 | 153964 | 10373 | 134218 | 44093 | 201082 | 106701 | 116143 | 79636 | 156 / 172 / 3 / 57 | 74696 / 75952 / 47900 / 101108 | 554296 | - | - |

読み方:

- **`shiki` −11,346（−7.4%）、`shiki_v1` より 11,404 小さい。** 内訳（同じ作業木で、待機中の VHS も毎フレーム描く変種を回した）: JS 側の書き換えで 144,117（−9,789、−6.4%）、画面外に待機する VHS の draw を `drawMany` の `count` で省いた分が −1,557。ほかの 3 負荷は ±1（配置の揺れの範囲。ファームは同じ）。
- **確保 172 → 156（−16 回/ターン）**: `txt()` の配列、`setRect` の配列、泡の `forEach` のクロージャ、120 フレームごとの `replace` の矢印関数、変わらない HUD の文字列（段階と `native` の行は変わった時だけ `setText`）。残りの 156 回の大半は、試験の負荷そのもの（`load()` の塊、`pocket.fs` の読み取りの Promise）。
- 変えたこと: 入力を `Int32Array`（Q16。`sky` の行数だけ Q0）にし、JS が §2.4 と同じ丸め（0 から遠い方の半分）で換算する。座標が整数の入力（魚・イルカの x、VHS の y）はシフトだけ。夕日の隙間の行は `t >> 2` が変わった時だけ書く。`setRect` は整数 4 つ。

host（実測）:

| | 前 | 後 |
| --- | --- | --- |
| ソース | 11,876 B | 11,870 B |
| 評価のピーク / 評価後に残る（`compile_peak.py`、`-m32` の大きさ） | +49,980 / +22,780 B | +47,240 / +21,792 B |
| 評価と最初の present の後（`test-stress-app-m32`、`STRESS_HEAP_LIMIT=163840`） | 61,715 B | 61,012 B |
| 599 フレーム（LV1・LV2）の画面の digest（64 bit と `-m32`、帯 8 行・4 行） | `31406cd6` | `31406cd6` |
| 900 フレーム（`-m32`、上限 160 KiB） | OOM 11（すべて `frame`）、表示の誤り 0、PASS | OOM 11（すべて `frame`）、表示の誤り 0、PASS |

900 フレームの digest は一致しない: LV3 の最初の OOM が前は 700 フレームの後の `read`、後は 702 フレームの `frame` で起き（ターンの確保が減ってヒープの並びが動いた）、`frame` の OOM は `paint()` を飛ばすので泡の位置がそこから 1 歩ずれる。式への入力・データ領域・HUD の文字は、記録する stub で前後の `stress.js` を 6,000 フレーム走らせ、毎フレーム一致を確かめた（Float64 の入力は §2.4 で換算して比べる。省いた VHS の draw は、前の版で y −40 の画面外）。

## 5. P5（古い呼び方を消した後、2026-11-13）

`shiki_v1` の負荷が終わったので、表の列が 1 つ減った（`tools/r5sim/reorg.sh` の行も 1 列短い）。列: `shiki` / `hud` / `derby` の `per_turn`、`derby` の場面、確保は `shiki` / `hud` / `derby`、`heap_used` は同じ順。比べる前の行は P4 の後の `69c92cc`（ファーム DIRAM 174,064、Flash Code 1,696,768、いずれも同じ作業木の外の worktree で組んだ値）。

| 日付 | sha | shiki | hud | derby | wait | wide | field | vision | headon | 確保/ターン | heap_used | r5sim flash | fw DIRAM | fw flash |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 2026-11-13 | `69c92cc` (P4 の後) | 142560 | 10375 | 134217 | - | - | - | - | - | 156 / 3 / 57 | 74696 / 47900 / 101108 | 554296 | 174064 | 1696768 |
| 2026-11-13 | `69c92cc+dirty` (P5) | 142565 | 10372 | 134220 | 44097 | 201085 | 106691 | 116151 | 79644 | 156 / 3 / 57 | 74696 / 47900 / 101108 | 550204 | 172576 | 1691584 |
| 2026-11-13 | `daecd0c` | 142565 | 10372 | 131453 | 44101 | 194839 | 106691 | 116147 | 79648 | 156 / 3 / 57 | 74696 / 47900 / 101108 | 550204 | 172576 | 1691600 |

- **カウント**: `shiki` +5（+0.004%）、`hud` −3、`derby` +3。どれも配置の揺れ（0.05% 未満、§2）の内で、確保の回数と `heap_used` は同一。JS の仕事は変わっていない（消したのは呼ばれていない入口）。
- **大きさ（map、`idf.py size`）**: ファームの DIRAM −1,488 B（`pocket_overlay.c` の `items[24]` の `.bss` −1,440 B が主で、残りの 48 B は内訳を取っていない）、Flash Code −5,184 B。r5sim の像（`shiki`）の Flash Code は −4,092 B。
