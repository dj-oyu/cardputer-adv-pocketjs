# JS のターンはどこでサイクルを使っているか — flash を QIO に（2026-09-27、`vm/turn-cpi`）

R3 の後、STRESS LV1 の JS は 1 フレーム約 7 ms（`turn_ms`）と言われていた。ホストのプロファイル
（`tools/vmtest/prof/`、callgrind）では 1 フレームの JS は x86 で約 35 万命令しかない。実機の 7 ms は
約 170 万サイクルなので、命令あたり 5 サイクル近くどこかで待っている計算になる。R4 はその待ちの正体を
実機のカウンタで測った。

## 1. 計装（`CONFIG_POCKET_VM_TURNPERF`、既定 n、診断）

`app_tick` の通常のターン（`dispatch_guest(false)` と `pocket_kasane_end_turn`）を Xtensa の性能カウンタで
挟む。PM0 はサイクル、PM1 は 30 ターンごとに入れ替わる事象（26 種）。構え方は `main/scene/garden.c` と
同じ（`kernelcnt 0` で割り込みを除く）。30 ターンごとに `TURNPERF <事象> cy= ev= n= us= cont_n= cont_us=`
を出す。継続ターン（8 ms の予算で切られた frame() の残り）は時間だけを足す。

```powershell
idf.py -B build_turnperf -D SDKCONFIG=build_turnperf/sdkconfig `
    -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.turnperf.defaults" build
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

QIO の計装ビルドで各段 60 秒走らせた 1 回目、LV3 の途中から `STRESS_FAIL read PocketError: two files are
already open` が出続けた（474 回）。`pocket.fs` の read の OOM の後で、File のスロットが 2 本とも戻らなくなって
いる。コードで見える経路: `js_open` はスロットを取って `file_wrap` した後に `pocket_api_settled` を呼び、その
中の `JS_NewPromiseCapability` が OOM で失敗すると File を解放する。File には意図的にファイナライザが無い
（`pocket_fs.c` の `file_class_def` の注記）ので、そのスロットはアプリが終わるまで戻らない。QIO が作った不具合
ではなく、ターンが短くなって OOM の落ちる位置がずれただけと見ている（DIO の 60 秒走行と、QIO の既定 20 秒走行は
PASS）。

### 4.1 R4a: 修正（2026-09-27、`vm/fs-open-oom`）

**直したもの 1（fs）**: `file_wrap` と `pocket_api_settled` を `file_settled()` に束ね、Promise を作れなかったら
スロットを番号で探して閉じる。同時に、`pocket_api_settled` が resolve の呼び出しの失敗を握りつぶして
「決して決着しない Promise と、文脈に残った例外」を返していたのを、例外として返すようにした（`pocket_api.h` の
契約に書いた）。`file_wrap` の失敗時に例外値そのものを `settled` に渡していた経路も同じ関数で消えた。

**直したもの 2（VM、実機の負荷を作る途中で見つけた）**: 負荷の最初の版は `fs.open` を満杯のヒープで初めて読み、
そのセッションの間ずっと `fs.open` が `undefined`（"not a function"）になった。F2 の遅延の索引
（`POCKET_VM_LAZY_BUILTINS`）が、項目に「済み」の印を付けてから shape へ入れる順で、`add_property` が OOM で
失敗すると名前が shape にも一覧にも無くなっていた。`js_lazy_touch`・`js_lazy_all`・別名の登録の 3 箇所で、
失敗したら印を戻す（`lazy_undo_done`。確保中の GC が `rt->lazy` を詰め直すので、一覧は（オブジェクト, 表）で
探し直す。`js_lazy_all` の添字も同じ理由で取り直す）。上流の autoinit の同種の穴は以前に直してあった
（`JS_AutoInitProperty` の注記）が、こちらで足した遅延の経路に同じ規則が無かった。**`pocket.*` の名前空間も
この一覧に乗るので、最初の読みがヒープの端に当たったアプリはその API を失っていた。**

| 検査 | 結果 |
| --- | --- |
| ホスト: `--fail-alloc` 1〜3000 の掃引（5 つの遅延の名前を読んで型を見る） | 修正前は 707・708 番で `Object.fromEntries` が `undefined`、修正後は穴 0、ASan クリーン |
| ホスト: 新しいコーパス `lazy_touch_oom.js`（`--profile device`、18 の名前を満杯の縁で初めて読む） | 修正後はすべて function。**陰性対照（修正前の quickjs.c）で 18 中 13 が `undefined`** |
| ホスト: コーパス | asan・o2・`--force-yield` とも 80/80 |
| 実機: `CONFIG_POCKET_VM_OOMPROBE` の USB `^`（ヒープを埋め、0〜23 ブロック返して open を 10 巡、最後に 2 本同時に open） | **fs の修正なし 5/5 `FSOOM leak two files are already open`**、修正あり 5/5 `FSOOM ok`（漏れの拒否 0、File 以外の解決値 0） |
| 実機: 出荷構成 | smoke 20 周・故障回復 6 種、settings、editor draft、`stress_app.py --seconds 60` PASS（29.4〜29.8 fps）、memlog 予算内・DIRAM ±0 |
| ホスト: `pocket_api.c` をリンクする検査 | input・capture・random PASS |

**残したもの**: JS の側で、解決した File を受け取る継続（`await` の再開）自体が OOM で走れない場合は、File は
どこにも届かず、ファイナライザが無いのでスロットは戻らない。これは `file_class_def` の注記が選んだ設計（GC に
書き込み中のファイルを閉じさせない）の帰結で、変えるには API の判断が要る。

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
