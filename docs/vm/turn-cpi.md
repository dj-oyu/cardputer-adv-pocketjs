# JS のターンはどこでサイクルを使っているか — flash を QIO に（2026-09-27、`vm/turn-cpi`）

R3 の後、STRESS LV1 の JS は 1 フレーム約 7 ms（`turn_ms`）と言われていた。ホストのプロファイル
（`tools/vmtest/prof/`、callgrind）では 1 フレームの JS は x86 で約 35 万命令しかない。実機の 7 ms は
約 170 万サイクルなので、命令あたり 5 サイクル近くどこかで待っている計算になる。R4 はその待ちの正体を
実機のカウンタで測った。

## 1. 計装（`CONFIG_KOBAKO_TURNPERF`、既定 n、診断）

`app_tick` の通常のターン（`dispatch_guest(false)` と `kobako_kasane_end_turn`）を Xtensa の性能カウンタで
挟む。PM0 はサイクル、PM1 は 30 ターンごとに入れ替わる事象（26 種）。構え方は `main/scene/garden.c` と
同じ（`kernelcnt 0` で割り込みを除く）。30 ターンごとに `TURNPERF <事象> cy= ev= n= us= cont_n= cont_us=`
を出す。継続ターン（8 ms の予算で切られた frame() の残り）は時間だけを足す。

```powershell
idf.py -B build_turnperf -D SDKCONFIG=build_turnperf/sdkconfig `
    -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.variant.turnperf" build
python tools\stress_app.py --port COM3 --seconds 60 --log <log>
```

## 2. 結果: サイクルの 70% は flash キャッシュのミス待ち（実測(device)、DIO）

STRESS LV1、1,770 ターン。

| 事象 / サイクル | DIO |
| --- | --- |
| 命令の退役（IPC） | **0.17** |
| `I_STALL_BUSY` | **0.70** |
| `D_STALL` | 0.05 |
| バブル（うち分岐 0.07） | 0.08 |
| 窓例外・除算・乗算・L32R | 合わせて 0.05 未満 |

`I_STALL_BUSY` は、この石では flash キャッシュのミス待ちとして読む（[ray-stall-census.md](../perf/ray-stall-census.md)
§2.5: ESP32-S3 の命令キャッシュは LX7 の外にあるので、`I_MEM_CACHE_MISSES` は常に 0 になり、待ち時間は
全部 BUSY に出る）。32 KB のキャッシュでもこうなるのは、作業集合が大きすぎるためである。ホストのコールグラフで
1 フレームに呼ばれる関数を実機の ELF のサイズで足すと **65〜84 KB**（`JS_CallInternal` 単独で 25.5 KB）。
これは推定で、関数単位の上限（実際に触るのは一部）。その上、描画がターンの間にキャッシュを入れ替える。

**解釈の訂正**: `KASANE_PAINT` の `turn_ms`（約 7.1 ms）は通常のターンと継続ターンの平均だった。DIO では
通常のターンが 8 ms の予算に当たって 10.3 ms まで伸び、残りを継続ターンが走らせていた。1 フレームの JS の
合計は **10.8 ms** で、backlog の「LV1 のターン 7.1 ms」はこれを過小に見せていた。

## 3. 打ち手: flash を QIO に（コード 0 行、DRAM 0）

機体の flash は XMC 0x4017、eFuse の flash 種別は quad（`esptool flash-id`）。DIO は 1 クロック 2 ビット、
QIO は 4 ビットなので、ミス 1 回でキャッシュラインを埋める時間がほぼ半分になる。**同じ配置のバイナリ**
（`JS_CallInternal` が同じ番地、`.bin` 同サイズ）で flash モードだけを変えて比べた（実測(device)、各段 60 秒）:

| STRESS | DIO | QIO |
| --- | --- | --- |
| LV1 の 1 フレームの JS（通常＋継続） | 10.78 ms | **7.25 ms**（−33%） |
| LV3 の 1 フレームの JS | 10.74 ms | **7.95 ms** |
| LV1 の通常のターンのサイクル | 2.25 M | 1.51 M |
| `I_STALL_BUSY` / ターン | 1.61 M | 0.88 M |
| IPC | 0.17 | 0.25 |
| `D_STALL` / ターン | 113 k | 63 k（flash 上の定数の読み） |
| 継続ターン / フレーム | 0.50 | 0.16 |
| 描画（`render_ms`、LV1） | 6.61 ms | 6.16 ms |
| 転送（`send_ms`） | 4.08 ms | 3.98 ms（SPI 律速で動かない） |

命令数は変わらない（37.4 万 → 37.8 万/ターン）。短くなったのは待ちだけで、期待どおりの形。

`sdkconfig.defaults` に `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y` を入れた。**既存の `sdkconfig` は DIO の行を持って
いるので defaults だけでは効かない**（命令キャッシュ 32 KB のときと同じ、`sdkconfig.defaults` の注記）。各ツリーの
`sdkconfig` の `CONFIG_ESPTOOLPY_FLASHMODE_*` を書き換えるか、ビルドディレクトリを作り直す。

**確認（計装なしの既定ビルド、実機、2026-09-27）**: `smoke_device.py` 20 周 SMOKE_OK・故障回復 6 種、
`test_settings.py` OK、`test_editor_draft.py` OK（flash への書き込みを含む）、`stress_app.py` PASS
（LV1〜3 は 29.7〜29.8 fps、`turn_ms_med` 6.16 / 6.17 / 6.63）、`memlog.py --check` 予算内・DIRAM ±0。

## 4. 見つけたもの（R4 とは別件）

QIO の計装ビルドで各段 60 秒走らせた 1 回目、LV3 の途中から `STRESS_FAIL read KobakoError: two files are
already open` が出続けた（474 回）。`kobako.fs` の read の OOM の後で、File のスロットが 2 本とも戻らなくなって
いる。コードで見える経路: `js_open` はスロットを取って `file_wrap` した後に `kobako_api_settled` を呼び、その
中の `JS_NewPromiseCapability` が OOM で失敗すると File を解放する。File には意図的にファイナライザが無い
（`kobako_fs.c` の `file_class_def` の注記）ので、そのスロットはアプリが終わるまで戻らない。QIO が作った不具合
ではなく、ターンが短くなって OOM の落ちる位置がずれただけと見ている（DIO の 60 秒走行と、QIO の既定 20 秒走行は
PASS）。

### 4.1 R4a: 修正（2026-09-27、`vm/fs-open-oom`）

**直したもの 1（fs）**: `file_wrap` と `kobako_api_settled` を `file_settled()` に束ね、Promise を作れなかったら
スロットを番号で探して閉じる。同時に、`kobako_api_settled` が resolve の呼び出しの失敗を握りつぶして
「決して決着しない Promise と、文脈に残った例外」を返していたのを、例外として返すようにした（`kobako_api.h` の
契約に書いた）。`file_wrap` の失敗時に例外値そのものを `settled` に渡していた経路も同じ関数で消えた。

**直したもの 2（VM、実機の負荷を作る途中で見つけた）**: 負荷の最初の版は `fs.open` を満杯のヒープで初めて読み、
そのセッションの間ずっと `fs.open` が `undefined`（"not a function"）になった。F2 の遅延の索引
（`QJS_LEAN_LAZY_BUILTINS`）が、項目に「済み」の印を付けてから shape へ入れる順で、`add_property` が OOM で
失敗すると名前が shape にも一覧にも無くなっていた。`js_lazy_touch`・`js_lazy_all`・別名の登録の 3 箇所で、
失敗したら印を戻す（`lazy_undo_done`。確保中の GC が `rt->lazy` を詰め直すので、一覧は（オブジェクト, 表）で
探し直す。`js_lazy_all` の添字も同じ理由で取り直す）。上流の autoinit の同種の穴は以前に直してあった
（`JS_AutoInitProperty` の注記）が、こちらで足した遅延の経路に同じ規則が無かった。**`kobako.*` の名前空間も
この一覧に乗るので、最初の読みがヒープの端に当たったアプリはその API を失っていた。**

| 検査 | 結果 |
| --- | --- |
| ホスト: `--fail-alloc` 1〜3000 の掃引（5 つの遅延の名前を読んで型を見る） | 修正前は 707・708 番で `Object.fromEntries` が `undefined`、修正後は穴 0、ASan クリーン |
| ホスト: 新しいコーパス `lazy_touch_oom.js`（`--profile device`、18 の名前を満杯の縁で初めて読む） | 修正後はすべて function。**陰性対照（修正前の quickjs.c）で 18 中 13 が `undefined`** |
| ホスト: コーパス | asan・o2・`--force-yield` とも 80/80 |
| 実機: `CONFIG_QJS_LEAN_OOMPROBE` の USB `^`（ヒープを埋め、0〜23 ブロック返して open を 10 巡、最後に 2 本同時に open） | **fs の修正なし 5/5 `FSOOM leak two files are already open`**、修正あり 5/5 `FSOOM ok`（漏れの拒否 0、File 以外の解決値 0） |
| 実機: 出荷構成 | smoke 20 周・故障回復 6 種、settings、editor draft、`stress_app.py --seconds 60` PASS（29.4〜29.8 fps）、memlog 予算内・DIRAM ±0 |
| ホスト: `kobako_api.c` をリンクする検査 | input・capture・random PASS |

**残したもの**（→ §4.2 で解決）: JS の側で、解決した File を受け取る継続（`await` の再開）自体が OOM で
走れない場合は、File はどこにも届かず、ファイナライザが無いのでスロットは戻らない。

### 4.2 参照の無い File はファイナライザで手放す（2026-09-27、`vm/fs-orphan`）

§4.1 の残りは「JS が OOM から復帰できない」状況ではなく、アプリは動き続けたまま File 1 個が誰からも参照されなく
なる状況だった（解決済みの Promise に継続を登録する確保が失敗し、File が Promise ごと捨てられる）。アプリを落とせば
`kobako_fs_reset()` がスロットを閉じるが、それを判断するには「File が消えた」ことを知る必要があり、それを知らせる
のがファイナライザなので、スロットだけを閉じれば足りる。

- File にファイナライザを付けた。GC の中では印（`orphan`）を付けるだけで、`fclose`・`unlink`・セクタの解放は
  次の `kobako_fs_pump()`（毎 tick、frame() の前、GC の外）が破棄として行う。
- 以前の「ファイナライザ無し」の理由は「変数を落としたアプリが書き込み中のファイルを予測できない時点で失う」
  だったが、参照の無い File はもう commit も close もできないので、失うものは `app_stop()` で失うものと同じ。
  変わるのは、スロットと同じパスへの BUSY がアプリ終了まで続かなくなることだけ。
- 実機の負荷 `^` に段 1 を足した: open は余裕のあるヒープで成功させ、`.then`（`await` が登録する継続）をヒープの
  端で付ける。**ファイナライザ無し 3/3 回、段 1 でスロットが尽きて `two files are already open`**、
  有り 5/5 回 `FSOOM ok`（継続を登録できなかった回数 `lost` 10〜13、拒否 0）。
- 出荷構成: smoke 20 周・故障回復 6 種、settings、editor draft、`stress_app.py --seconds 60` PASS（29.6〜29.8 fps）、
  memlog 予算内・DIRAM ±0。`filesystem-api.md` の close の項に 1 文足した。

**別件（既存）**: `oom_sweep.sh` を `lazy_builtins.js` に 2,500 点かけると、1,075 番で終了時に
`JS_FreeRuntime: Assertion list_empty(&rt->gc_obj_list)` になる。修正前の quickjs.c でも同じなので今回の変更では
ない。backlog に積んだ。

## 5. 次の手（未着手）

QIO の後もサイクルの 57% は `I_STALL_BUSY`。ミスの**回数**を減らす手が残る:

- **ホットな関数を連続して置く**（リンカの断片で並べる）。32 KB・8 ways では番地 mod 4096 が同じ行が競合する
  ので、ばらばらに散った作業集合は容量より先に競合で溢れる。DRAM は使わない。
- **一部を IRAM に**。IRAM は DRAM と同じプールなので、1 KB ごとにゲストかシステムの空きが減る。
  ray-stall-census §2.8〜2.10 の IRAM の比較は配置のくじに飲まれて撤回されているので、やるなら
  `TURNPERF` の `I_STALL_BUSY` で機構を直接見る。
- **flash 120 MHz**（ESP-IDF では実験的機能。チップと温度の条件がある）。
- 作業集合を実機で直接測るには PC サンプリングが要る（ホストの推定は x86 のコードで、インライン化も違う）。

## 6. main `d3300996` の内訳（2026-11-21、実測(device)）

§1 の計装（`sdkconfig.variant.turnperf`）を、main `d3300996`（fork `ff809dd`、デバッグオフのファーム）で測った。性能の作業の出発点。速度のベースラインは [upstream-v018.md](upstream-v018.md) §9、窓ごとの値は [logs/turnperf-20261121-d3300996.txt](logs/turnperf-20261121-d3300996.txt)。STRESS LV1 は 30 秒で 29 事象が 1 巡、DERBY は 45 秒を 2 回走らせ、窓をアプリのログで場面に分けた（場面の変わり目と起動の窓は捨てた）。

| 事象 / サイクル | STRESS LV1 | DERBY パドック | DERBY レース |
| --- | ---: | ---: | ---: |
| 1 ターンのサイクル | 1.24 M | 0.71 M | 1.27 M |
| 命令の退役（IPC） | **0.50** | **0.29** | **0.43** |
| `I_STALL`（うち `I_STALL_BUSY` = flash キャッシュ待ち） | 0.31（**0.29**） | 0.57（**0.54**） | 0.38（**0.36**） |
| `I_STALL_FAST_L32R` | 0.033 | 0.043 | 0.034 |
| バブル（うち分岐 CTI / レジスタ依存） | 0.18（0.155 / 0.025） | 0.12（0.105 / 0.018） | 0.18（0.156 / 0.025） |
| `D_STALL` | 0.027 | 0.043 | 0.027 |
| 窓例外・除算・乗算 | 合わせて 0.003 未満 | 0.005 未満 | 0.003 未満 |
| 退役＋I＋D＋バブルの和 | 1.01 | 1.02 | 1.01 |

1 ターンあたりの数（実測）:

| | STRESS LV1 | DERBY パドック | DERBY レース |
| --- | ---: | ---: | ---: |
| 命令 | 689,766 | 208,319 | 556,101 |
| `I_STALL_BUSY` のサイクル | 359,524 | 383,396 | 486,548 |
| 成立した分岐と j（`insn_brj`） | 69,719 | 22,222 | 55,412 |
| call・ret（`insn_callret`） | 20,930 | 12,766 | 31,707 |
| `jx`（-Os のディスパッチの数え方、§1 の注記） | 2,196 | 1,604 | 6,717 |
| ロード | 128,850 | 45,766 | 134,411 |
| CTI のバブルのサイクル | 192,669 | 74,032 | 193,302 |

- **最大の項はまだ flash キャッシュ待ち。** ターンの 29〜54%。**1 ターンの待ちのサイクルは場面によらず 0.36〜0.49 M でほぼ一定**で、命令数（0.21〜0.69 M）とは比例しない。ターンの間の描画がキャッシュを入れ替え、毎ターン JS の作業集合を読み直している、という §2 の見立てと合う（推定。PC サンプリングでは確かめていない）。パドックは命令が少ないぶん、待ちの割合が一番大きい（0.71 M サイクルのうち 0.38 M）。
- **次は分岐のバブル（15%）。** 成立した分岐・j・call・ret は 1 ターン約 9.3 万回（STRESS）で、CTI のバブル 19.3 万サイクルは 1 回あたり約 2.1。CLAUDE.md の「成立分岐 × 2」と合う。
- 窓例外（`exr_window`）・除算・乗算・ストアバッファ・バンク競合はどれも 0.5% 未満で、ここに余地は無い。
- **§2〜§3 の値とは比べない。** STRESS はその後で別の負荷になった（2026-11-03 に Kasane の保持シーン 75 コマンドから Shiki の 18 draw へ、11-13 に確保の無いターン。LV1 の `turn_ms` は 6.33〜6.64 → 約 5.72 → 5.31）。今の STRESS のターンには Shiki のエンジン（`SHIKI_TURN total_ms` 約 1.6 ms、JIT のコードは RAM 上）が入るので、IPC は JS だけのターンより高く出ているはず（推定。分けて測っていない）。DERBY の Shiki はパドック約 0.31 ms、レース約 0.42〜0.54 ms。
- 次の打ち手の候補は §5 のまま（ホットな関数を連続して置く、IRAM、flash 120 MHz）。どれも `I_STALL_BUSY` を直接見て判断する。
