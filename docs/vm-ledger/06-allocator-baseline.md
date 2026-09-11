# 06 アロケータ基準（§7 L2a・§12 手順3向け）

対象: `docs/quickjs-freertos-vm-spec.md` §7「L2aのメモリ確保: 非移動セグメント方式」、§12手順3「estalloc等を比較対象に、移動しないセグメントの分割・空き結合・再利用を先に測る」。実装は `tools/vmalloc/`（本ドキュメントの作成物）、実行基盤は `tools/vmtest/`（既存のL0基準、`docs/vm-ledger/05-allocation.md` にゲスト側アロケータの事実整理がある）。

**比較する3実装**（いずれも実測(host)。実機の値ではない）:

1. **tlsf** — `tools/vmalloc/vendor/{multi_heap.c,tlsf/}`。ESP-IDF v6.0.1の `components/heap/{multi_heap.c,tlsf/tlsf.c,tlsf/tlsf_block_functions.h,tlsf/tlsf_control_functions.h}` を**そのままコピー**したもの。ゲストが今使っているアロケータそのもの（`guest.c` の `heap_caps_malloc` → `multi_heap_malloc` → `tlsf_malloc`、`docs/vm-ledger/05-allocation.md` 参照)。
2. **estalloc** — picoruby/estalloc、pin revision `dd3988acb10ae5e14f1cbf3991753f12c4dcf9d2`（2026-09-08）、BSD-3-Clause。`tools/vmalloc/vendor/estalloc/`。**ファームウェアには一切組み込んでいない**（比較専用）。
3. **naive** — 本タスクのために書いた単純な明示空きリスト・first-fit・即時結合。`tools/vmalloc/adapter_naive.c`。どこにも由来しない「床」。

結論・推奨は書かない（§12「新たな失敗を期待値の書き換えだけで処理しない」の精神に合わせ、ここでは数字と、数字が測れた／測れなかった理由だけを記す）。

## 1. tlsf: IDF実物をホストで動かすために必要だったパッチ

`tools/vmalloc/vendor/tlsf/tlsf.c` と `tlsf_control_functions.h` に计3箇所、`VMALLOC PATCH` とコメントした変更がある。**いずれもガード付き（マクロで切り替え）で、`VMALLOC_TLSF_ALIGN_LOG2` を定義しなければ IDF そのままの値 `ALIGN_SIZE_LOG2=2`（4バイト境界）に戻る。** 本ツールは `-DVMALLOC_TLSF_ALIGN_LOG2=3`（8バイト境界）でビルドしている。理由は以下の通り。

- **原因**: IDFのtlsfは `ALIGN_SIZE_LOG2=2` を**ハードコード**している（`tlsf_control_functions.h:20`、パッチ前）。README (`tlsf/README.md:17`) も「Currently, assumes architecture can make 4-byte aligned accesses」と明記している。これは32bitポインタ(4B)を前提にした値で、64bitホスト(ポインタ8B)でそのままビルドすると `block_header_t`（先頭が8Bポインタの `prev_phys_block`）が4バイト境界に置かれ、`insert_free_block` の最初の呼び出しで即SEGV（`tlsf_control_functions.h:404`、UBSanは "member access within misaligned address ... requires 8 byte alignment" を出す）。2026-09-12にホストで再現済み（実機ではない。本タスク中に確認、以下すべて検証コマンドは §5 参照）。
- **32bitツールチェインは使えない**: `gcc -m32` は `crti.o` が無くリンク不可（このWSLにmultilibが入っていない。`tools/vmtest/README.md` がvmrunの `-m32` で同じ制約を既に書いている）。`sudo apt-get install gcc-multilib` はパスワードなし実行不可で試せなかった。**つまりIDFのtlsfを実機と同じ4バイト境界のままホストでビルドする手段はこの環境に無い。** 参考として、ESP-IDF自身の `components/heap/test_multi_heap_host/`（Linuxターゲット用ホストテスト）も、`CMakeLists.txt` の `if(${target} STREQUAL "linux")` 分岐で `tlsf.c` を一切ビルドせず別実装 `heap_caps_linux.c` に差し替えている——**IDF自身もtlsfを64bitホストへ移植していない**、という状況証拠。
- **パッチ内容**（3箇所、すべて `tools/vmalloc/vendor/tlsf/` 内にコメントで理由を書いてある）:
  1. `tlsf_control_functions.h`: `ALIGN_SIZE_LOG2` を `VMALLOC_TLSF_ALIGN_LOG2`（既定2、本ツールは3）でオーバーライド可能にした。
  2. 同ファイル: `control_t` のビットフィールド `fl_index_shift`（3→4bit）・`small_block_size`（8→9bit）を拡張。ALIGN_SIZE_LOG2=2用に**値がちょうど収まる幅で**切り詰められていたため（`fl_index_shift` 最大 `5+2=7` は3bitに収まるが `5+3=8` は3bitからあふれて0に折り返る）、3にすると無音で壊れる。この2フィールドはヒープ1個ぶんの内部管理値でブロックレイアウトには影響しない（幅を広げても既定の2では出力が変わらない——後述の整合性チェックで確認）。
  3. `tlsf.c` の `control_construct`: 管理メタデータ総サイズ `control->size` を `ALIGN_SIZE` へ切り上げ。`sl_bitmap` が `unsigned int`(4B)×`fl_index_count`(しばしば奇数)なので4の倍数にしかならず、`tlsf_size(tlsf)` をそのままプール先頭へのオフセットに使う `tlsf_create_with_pool` で8バイト境界が崩れていた。
- **整合性チェック**: `.cache/vmalloc/vmalloc_replay-asan`（ASan+UBSan+LeakSanitizer）で `tools/vmtest/corpus/*.js` 由来の23本と `apps/vmprobe/*.js` 由来の5本、計28トレースすべてを malloc/realloc/free のランダム順で再生し、`multi_heap_check()`（プール全体のブロック鎖・ビットマップ整合性を歩いて検証）が**全通過**。境界確保のfuzz（1MBプール・20万オペレーション・ランダムサイズ1〜2000B）も別途 `multi_heap_check()` 通過とASanクリーンを確認済み（§5のコマンド）。

### アラインメントの監査結果（仕様書「対象のアラインメント」項目）

**これがこの調査でいちばん実務的な発見**: IDFのtlsfは`ALIGN_SIZE_LOG2=2`（4バイト境界）が**実機（32bit Xtensa）でも既定**であり、`multi_heap_malloc()` は `tlsf_malloc()` を素通しで呼ぶだけで追加のアラインメント保証をしない（`multi_heap.c:216`)。ゲスト側の `heap_caps_malloc(total, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`（`guest.c:58`）の `MALLOC_CAP_8BIT` は「8/16bitのデータアクセスを許すメモリ」という意味のケーパビリティフラグであり（`esp_heap_caps.h:33`）、**ポインタのアラインメントとは無関係**。guest.cは自前ヘッダ(`allocation_header_t`、`size_t` 1個、32bitで4B)を挟むが、これも4の倍数のオフセットを足すだけで8バイト境界を作らない。

→ **QuickJSがXtensa上で確保するJSValue/doubleが8バイト境界に乗る保証は、今のアロケータ層のどこにも無い**（コード読解による事実。実機での実害——Xtensa LX7が未整列doubleアクセスで実際に落ちるか遅くなるだけか——は今回検証していない、未確認）。

対照的に、**estallocはPLATFORM_64BITの判定と無関係に既定で8バイト境界**（`estalloc.h:49-56`、`ESTALLOC_ADDRESS_16BIT` を定義しない限り32bit/64bit問わず `ESTALLOC_ALIGNMENT=8`)。L2aでestallocを採用するなら、今のtlsfより強いアラインメント保証を追加コストなしで得られる、という構造的な違いがある（これも推奨ではなく事実の記述）。

## 2. estalloc: 監査結果

- pin: `dd3988acb10ae5e14f1cbf3991753f12c4dcf9d2`（2026-09-08、picoruby/estalloc `main`）、BSD-3-Clause。ライセンス条項は `tools/vmalloc/vendor/estalloc/LICENSE`。**ベンダリングはしていない**（比較専用ツールのビルド内にのみ存在）。
- アラインメント: 上記の通り既定8B、QuickJSの要求（8バイト、doubles/JSValue）を満たす。
- **`est_calloc` に乗算オーバーフローガードが無い**（`estalloc.c:734`: `unsigned int total_size = nmemb * size;` がそのまま `est_malloc` へ渡る）。比較として、本ファームの `guest_calloc`（`guest.c:67-70`）と本ツールの `vm_calloc`（`tools/vmtest/vmrun.c`）はどちらも `count != 0 && size > SIZE_MAX/count` を先にガードしている。**今のところ実害は無い**——`guest.c` はQuickJSの `js_calloc` を `est_calloc` へ直結する設計ではなく、`guest_calloc` が先にオーバーフローチェック込みで `total` を計算してから `guest_malloc`(1引数)を呼ぶ形なので、L2aでこのパターンをそのまま踏襲する限り `est_calloc` 自体は呼ばれない。**もし将来 `JSMallocFunctions.js_calloc` を `est_calloc` に直結する実装に変えるなら、このガード欠如がそのまま露出する。**
- **`est_init`/`est_malloc` のサイズ上限チェックは `assert()` 頼み**（`estalloc.c:467-468`, `estalloc.c:47`, `#include <assert.h>`）。`NDEBUG` が定義されたビルドでは全部no-opになる。本プロジェクトの `sdkconfig.defaults` に `CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_*` の明示指定は無い（2026-09-12確認、grep 0件）ので、実際にNDEBUGが付くかはESP-IDFの最適化レベル既定値に依存し、**このリポジトリのビルドで実際にassertが生きているかは未確認**。
- **OOM時の整合性**: `est_malloc` が失敗する経路（プール終端超過、SIZE_FREE_BLOCKS範囲外）はいずれも `EXIT_CRITICAL` してから `NULL` を返しており、途中状態のブロックを残さない（コード読解）。本ツールのfuzzテスト・28トレース再生でも `est_take_statistics()` 経由の `error_message` 検出は全通過（§5）。
- 実装は素朴なTLSF系（FLI/SLIのセグリゲートリスト＋2段ビットマップ、mruby/c系譜）。`est_malloc` のコード末尾に「Change strategy to First-fit」というフォールバックの線形走査ループがあり（`estalloc.c:594-601`）、README のO(1)表記はこの経路を含んでいない。**本ツールはこの事実を実測で裏取りした**: 表Dの通り、estallocの `malloc_steps_max` は全トレースで3（=FLI直接ヒット/次バケット/ビットマップの3段のうち最大3段目まで、というO(1)経路の中での話）に収まっており、**今回のワークロード（JS実行由来の確保パターン）ではフォールバックの線形走査に一度も落ちていない**。ただしこれは「この23+5本のトレースでは」という実測であり、フォールバックに落ちるサイズ分布を作れば当然O(n)になる（README通り）。

## 3. naive: 実装

`tools/vmalloc/adapter_naive.c`。単一の明示双方向空きリスト、first-fit線形走査、`free()` 時に隣接物理ブロック(前後)を即結合。ヘッダは `{size_t size; int used; block_t *phys_prev,*phys_next,*free_prev,*free_next;}` で64bitホストでは48B/ブロック(パディング込み)——tlsf/estallocよりずっと大きい固定オーバーヘッド。セグリゲーションなしなので `malloc_steps`（=空きリストの走査ノード数）はワークロード次第で2桁に達する（表D、`deep_recursion_device.trace` で75、`try_finally.trace` で71など）。**どこの実装にも由来しない、比較の「床」として書いたコード**であり、estalloc/tlsfのように上流での実績があるわけではない。`naive_check()` がブロック鎖・空きリスト到達可能性・「隣接する空きブロックが2つ連続しない」(結合済みである)不変条件をプール全体で検査し、全トレースで通過。

## 4. 手法

### トレース収集

`tools/vmtest/` のコーパス(23本)を `run.sh --trace` で採取。加えて `apps/*/*.js` のうち **`pocket.*` を一切呼ばない5本**（`ui.*` も呼ばない、= vmrunだけで動く)を `vmrun --profile host --frames 5 --trace ... FILE.js` で個別採取した:

- 含めた: `apps/vmprobe/{async_generator,closures,deep_recursion,promise_chain,sync_loop}.js`
- **除外した**（`pocket.*` または `ui.*` を呼ぶため `ReferenceError` で止まる。2026-09-12に実行して確認、全件 `pocket.*`呼び出し回数>0 または `ui.*`使用を先にgrepで確認済み）: `apps/hello/main.js`（`ui.*`）、`apps/apicheck/{apicheck,capsweep,pickcheck}.js`、`apps/appcheck/appcheck.js`、`apps/bridge/bridge.js`、`apps/companion/companion.js`、`apps/deskclock/deskclock.js`、`apps/imucal/imucal.js`、`apps/ioprobe/ioprobe.js`、`apps/miccheck/miccheck.js`、`apps/mp3play/mp3play.js`、`apps/netcheck/{netcheck,tlscycle,tlshosts,tlsleak}.js`、`apps/opusfit/opusfit.js`、`apps/opusplay/opusplay.js`、`apps/pet/pet.js`、`apps/player/player.js`、`apps/pocketfs/pocketfs.js`、`apps/pocketui/pocketui.js`、`apps/streamplay/streamplay.js`、`apps/textcheck/textcheck.js`、`apps/vmprobe/io_wait.js`。

合計28トレース、`.cache/vmtest/traces/*.trace` + `.cache/vmalloc/apptraces/vmprobe_*.trace`（いずれもgit管理外、再生成コマンドは§5）。

### 指標と定義

`tools/vmalloc/vmalloc.h` の共通インタフェースを3バックエンドが実装し、`tools/vmalloc/replay.c` が1トレースを1アロケータ・1プールサイズに対して1回再生する（1プロセス=1アロケータ=1トレース=1プールサイズ。バックエンドの静的状態をプロセス間で共有しない設計、`tools/vmalloc/run_all.sh` がプロセスをforkして回す)。

- **peak_used**（表A/B列「peak_used」）: `stats()` が返す「生存中の確保バイト数（アロケータ自身のブロックオーバーヘッド込み）」の全実行中最大値。tlsfは `multi_heap_get_info().total_allocated_bytes`、estallocは `est_take_statistics().stat.used`、naiveは全ブロックを歩いて集計。
- **min_pool**（表A）: 二分探索（4096Bから倍々で失敗しなくなる上限を探し、その後64B刻みまで二分）。同じトレースを何度も最初から再生する。**bench_\*系はトレースが280万〜580万行あり二分探索がO(log(範囲))×フル再生になるため対象外**（後述）。
- **min_largest_free / max_largest_free**（表B）: 160KiB固定プールで全オペレーションごとに`stats().largest_free_block`をサンプルした最小値・最大値。**これは `CLAUDE.md` が書いている「taffyノード確保の段差（16以下→2,048B / 17〜33→29,648B / 34以上→59,296B）」に直結する数字**——単一連続ブロックが必要な確保なので、`min_largest_free` がその段の閾値を割り込んだ瞬間はレイアウトのメモリ不足(Rust側は報告せずabort)が実機で起きうる、という意味を持つ。
- **realloc_copy_bytes**: 全realloc呼び出しについて、返ってきたポインタが渡したポインタと変わっていれば(=その場拡張できずコピーが発生した)`min(旧サイズ,新サイズ)`を加算した総和。3実装とも「その場拡張」の余地があるため（tlsf/estallocは物理隣接空きブロックへの拡張、naiveも同様に実装した）、実装依存の数字になる。
- **malloc_steps / realloc_steps**: 1回のmalloc/realloc呼び出しあたりの探索コスト。**naiveとestallocは正確**(前者は空きリストの走査ノード数そのもの、後者は `estalloc.c` にこの計測専用で足したカウンタ——`vmalloc_estalloc_last_steps()`、FLI/SLI/ビットマップ各段のヒットを1、線形フォールバックはノードごとに1)。**tlsfは構造的にO(1)**（ビットマップのclzで非空バケットを一定回数の演算で特定し、走査せずポップするだけ——`tlsf.c`にestallocの`594-601`行に相当する線形フォールバックが無い）。この違いのため、tlsfの「steps」は常に1固定で出力しており、「1回で見つかった」という意味のnaive/estallocの1とは**意味が違う**（tlsfは「そもそも数える走査が無い」という構造的事実、naive/estallocの1は「たまたま1回目で見つかった」という実行結果)。
- **malloc_ns / realloc_ns / free_ns**: `clock_gettime(CLOCK_MONOTONIC)`で計測した壁時計時間。ホストのタイマ分解能・スケジューラジッタをそのまま含む実測値で、比較指標としては steps より粗い(表Dのtlsf `job_throw.trace` 24,590ns / `memory_device.trace` 127,728ns のような外れ値はほぼ確実にジッタ)。3実装が同じ計測方法・同じホストで比較されている、という点だけが意味を持つ。

### 再現コマンド

```bash
# WSLから、/mnt/c/devs/m5stack/cardputer-adv-pocketjs-vm で
bash tools/vmtest/build.sh all
bash tools/vmtest/run.sh --trace                       # 23本 -> .cache/vmtest/traces/
mkdir -p .cache/vmalloc/apptraces
cd apps/vmprobe
for f in async_generator closures deep_recursion promise_chain sync_loop; do
  ../../.cache/vmtest/vmrun-o2 --profile host --frames 5 \
    --trace ../../.cache/vmalloc/apptraces/vmprobe_$f.trace $f.js
done
cd ../..

bash tools/vmalloc/build.sh                             # -> .cache/vmalloc/vmalloc_replay-{asan,o2}
bash tools/vmalloc/run_all.sh                            # -> .cache/vmalloc/results.txt（表A-Dの元データ、約2分）

# 整合性チェック単体（ASan+UBSan+LeakSanitizer、代表トレース1本）
.cache/vmalloc/vmalloc_replay-asan --allocator tlsf     --pool 262144 .cache/vmtest/traces/closures.trace
.cache/vmalloc/vmalloc_replay-asan --allocator estalloc --pool 262144 .cache/vmtest/traces/closures.trace
.cache/vmalloc/vmalloc_replay-asan --allocator naive    --pool 262144 .cache/vmtest/traces/closures.trace

# 1トレース・1アロケータの詳細を見る
.cache/vmalloc/vmalloc_replay-o2 --allocator tlsf --bisect .cache/vmtest/traces/promise_chain.trace
.cache/vmalloc/vmalloc_replay-o2 --allocator tlsf --pool 163840 --sample-every 1 .cache/vmtest/traces/closures.trace
```

## 5. 結果

すべて実測(host)。ホストはJSValue 16B/実機8B、ポインタ8B/実機4B（`tools/vmtest/README.md` と同じ注意）、かつtlsfは前述の8バイト境界パッチ適用済みでの数字——**bytes単位の絶対値を実機の予算判定にそのまま使わない**。段差・順序・傾向の比較として読む。

### Table A: 二分探索で求めた最小プールサイズ（bench_\*を除く、KiB）

| trace | tlsf min_pool | tlsf peak_used | estalloc min_pool | estalloc peak_used | naive min_pool | naive peak_used |
|---|---:|---:|---:|---:|---:|---:|
| builtin_reentry.trace | 152.8 | 129.8 | 152.5 | 143.2 | 234.0 | 202.5 |
| closures.trace | 133.6 | 117.5 | 132.9 | 127.3 | 195.8 | 177.0 |
| deep_recursion.trace | 113.4 | 101.9 | 112.5 | 110.6 | 168.1 | 154.2 |
| deep_recursion_device.trace | 111.2 | 100.6 | 110.4 | 109.7 | 162.0 | 154.6 |
| error_toplevel.trace | 108.6 | 96.8 | 107.1 | 105.4 | 154.6 | 148.6 |
| gc_threshold_device.trace | 171.1 | 152.8 | 169.8 | 169.0 | 252.6 | 251.3 |
| generators.trace | 141.6 | 124.4 | 140.5 | 135.9 | 203.6 | 192.6 |
| job_throw.trace | 108.6 | 96.9 | 107.1 | 105.2 | 154.8 | 148.4 |
| memory_device.trace | 166.1 | 154.3 | 164.6 | 163.0 | 222.0 | 206.6 |
| microtask_order.trace | 148.4 | 133.4 | 148.8 | 148.1 | 218.8 | 215.9 |
| promise_chain.trace | 3355.1 | 2873.1 | 3446.7 | 3431.9 | 5679.1\* | 5645.9\* |
| rejections.trace | 118.0 | 106.6 | 117.3 | 116.6 | 168.2 | 166.7 |
| special_calls.trace | 192.0 | 173.8 | 191.9 | 187.7 | 293.8 | 248.4 |
| sync_loop.trace | 120.2 | 106.3 | 119.9 | 115.6 | 176.1 | 162.9 |
| try_finally.trace | 131.2 | 115.7 | 130.4 | 125.5 | 190.8 | 172.2 |
| vmprobe_async_generator.trace | 109.4 | 98.8 | 108.6 | 107.8 | 159.1 | 152.2 |
| vmprobe_closures.trace | 108.6 | 96.6 | 107.1 | 105.0 | 154.6 | 147.3 |
| vmprobe_deep_recursion.trace | 108.6 | 96.7 | 107.1 | 105.0 | 155.3 | 147.4 |
| vmprobe_promise_chain.trace | 137.6 | 123.2 | 137.9 | 137.2 | 206.8 | 201.1 |
| vmprobe_sync_loop.trace | 108.6 | 95.2 | 107.1 | 103.4 | 154.6 | 145.1 |

\* `promise_chain.trace` のnaiveは既定の二分探索上限4MiBで見つからず(`bisect=FAIL_ABOVE_MAX`)。上限を16MiBに広げて別途実行（47秒、`--bisect-max 16777216`）: `min_pool=5,815,360B`(5679.1KiB) `peak_used=5,781,360B`(5645.9KiB)。

**この表だけからわかる傾向**（数字の並び以上の主張はしない）: 20トレース中17本で `estalloc min_pool < tlsf min_pool`（差はほぼ全て1KiB未満）、残り3本（`microtask_order`・`promise_chain`・`vmprobe_promise_chain`）は逆にtlsfの方が小さい——**一方が常に優位ではなく、トレース依存で入れ替わる**。naiveは全トレースで他の2つより明確に大きく、`promise_chain.trace`のmin_poolはtlsf比+69.3%・estalloc比+64.8%。naiveの48B/ブロックという固定ヘッダと、セグリゲーションなしfirst-fitの断片化の両方が効いている——このトレースの生JS要求ピークは `awk` で独立計算した `app_peak=2,757,914B`（2.63MiB、ヘッダ・アロケータ内部丸め抜き）に対し、tlsfのpeak_usedは2,942,096B(+6.7%)、estallocは3,514,224B(+27.4%)、naiveは5,781,360B(+109.6%)——**naiveのオーバーヘッド比率は他の2つと1桁近く違う**。

### Table B: 160KiB固定プール(`JS_SetMemoryLimit`と同じ値)での適合判定、bench_\*を除く

| trace | tlsf | estalloc | naive |
|---|---|---|---|
| builtin_reentry.trace | OK (used=129.8K minfree=7168B) | OK (used=143.2K minfree=7688B) | FAIL (used=148.1K minfree=1040B) |
| closures.trace | OK (used=117.5K minfree=26624B) | OK (used=127.3K minfree=27768B) | FAIL (used=152.2K minfree=592B) |
| deep_recursion.trace | OK (used=101.9K minfree=47104B) | OK (used=110.7K minfree=48680B) | FAIL (used=150.7K minfree=592B) |
| deep_recursion_device.trace | OK (used=100.6K minfree=49152B) | OK (used=109.7K minfree=50784B) | FAIL (used=149.4K minfree=592B) |
| error_toplevel.trace | OK (used=96.8K minfree=51200B) | OK (used=105.3K minfree=54184B) | OK (used=148.5K minfree=1040B) |
| gc_threshold_device.trace | FAIL (used=143.1K minfree=24B) | FAIL (used=159.3K minfree=32B) | FAIL (used=158.7K minfree=48B) |
| generators.trace | OK (used=124.4K minfree=18432B) | OK (used=135.9K minfree=20024B) | FAIL (used=149.6K minfree=1024B) |
| job_throw.trace | OK (used=96.9K minfree=51200B) | OK (used=105.2K minfree=54184B) | OK (used=148.4K minfree=1040B) |
| memory_device.trace | FAIL (used=135.0K minfree=12800B) | FAIL (used=143.7K minfree=14528B) | FAIL (used=150.1K minfree=592B) |
| microtask_order.trace | OK (used=133.4K minfree=11776B) | OK (used=148.1K minfree=11472B) | FAIL (used=151.4K minfree=448B) |
| promise_chain.trace | FAIL (used=142.7K minfree=0B) | FAIL (used=159.3K minfree=0B) | FAIL (used=150.9K minfree=592B) |
| rejections.trace | OK (used=106.6K minfree=40960B) | OK (used=116.6K minfree=43728B) | FAIL (used=151.6K minfree=448B) |
| special_calls.trace | FAIL (used=136.0K minfree=6656B) | FAIL (used=147.8K minfree=6560B) | FAIL (used=152.9K minfree=176B) |
| sync_loop.trace | OK (used=106.3K minfree=38912B) | OK (used=115.6K minfree=40048B) | FAIL (used=149.1K minfree=1024B) |
| try_finally.trace | OK (used=115.7K minfree=28672B) | OK (used=125.5K minfree=30368B) | FAIL (used=151.6K minfree=448B) |
| vmprobe_async_generator.trace | OK (used=98.9K minfree=51200B) | OK (used=107.9K minfree=52632B) | OK (used=152.3K minfree=592B) |
| vmprobe_closures.trace | OK (used=96.6K minfree=51200B) | OK (used=104.9K minfree=54184B) | OK (used=147.1K minfree=2192B) |
| vmprobe_deep_recursion.trace | OK (used=96.7K minfree=51200B) | OK (used=105.0K minfree=54184B) | OK (used=147.4K minfree=1040B) |
| vmprobe_promise_chain.trace | OK (used=123.1K minfree=22528B) | OK (used=137.1K minfree=22696B) | FAIL (used=159.1K minfree=48B) |
| vmprobe_sync_loop.trace | OK (used=95.2K minfree=51200B) | OK (used=103.4K minfree=54184B) | OK (used=145.0K minfree=4416B) |

**taffy段差との突き合わせ**（CLAUDE.mdの目安、「正確な数字として書かない」指示に従い相対関係だけを書く）: tlsf・estallocとも、160KiBに収まった(OK)トレースは16本ずつだが、そのうち**実行中のどこかで`min_largest_free`が34ノード段の目安(59,296B)以上を保てたものは1本も無い**(tlsf 0/16, estalloc 0/16)。29,648B段(17〜33ノード)以上を保てたのはtlsf 10本・estalloc 11本で、残りの6〜5本は最悪時それ未満まで断片化している。naiveはOKになった6本のどれも29,648Bの段にすら届かない。——**「JS_SetMemoryLimitの160KiBには収まっている」ことと「その実行中いつでも34ノード相当のtaffyレイアウトを組める」ことは別条件どころか、今回の20本のワークロードでは後者を一度も満たしていない**。ヒープ残量が十分でも単一の大きい確保だけがRust側のパニックで落ちる経路が構造的にある、という`CLAUDE.md`の記述と整合する結果だが、これはJS側アロケータの外形挙動だけを見た数字で、taffy自体の確保呼び出しは含んでいない(`tools/uibudget/`が別途その部分を持つ)——**taffyが実際にこのタイミングで大きい確保を要求するかどうかまでは確認していない**（§6）。

**（2026-09-12 レビューで追記）この突き合わせを今のファームの結論として読まないこと。** 理由は2つ。(1) 現行のゲストは専用プールを持たない。`JS_SetMemoryLimit(160KiB)` は QuickJS の論理的な上限で、確保そのものは `guest.c` の `heap_caps_malloc` がシステムの共有ヒープから取る。taffy（Rust UIコア）も同じ共有ヒープから取るので、「160KiBプールの最大空きブロック」は今日の taffy の確保可否とは別の量。この数字が意味を持つのは、L2a で JS 専用の非移動セグメントを切り出し、**かつ**そこから taffy が確保する設計にした場合だけで、後者は今の計画に無い。(2) ホストは JSValue 16B・ポインタ8B で、空のランタイムだけで `qjs_malloc_size` 98,680B（`tools/vmtest/README.md`、実測(host)）＝160KiBの6割を占める。実機（JSValue 8B）では同じトレースの使用量は小さく、最大空きブロックは大きくなるはず（推定、未計測）。また `vmprobe_deep_recursion` は同日、ワークロード自体を直した（固定400段は実機の20KiBスタック上限で毎フレーム RangeError になる。`JS_CallInternal` の Xtensa フレームが `entry a1, 0x150`＝336B＋alloca、`build_vm_probe` の ELF で確認）ため、そのトレースと表 A/B/D の行は取り直した値。

### Table C: 160KiB固定プール、bench_\*（スループット計測用、`tools/vmtest/README.md`が言う「時間計測用」）

| trace | tlsf | estalloc | naive |
|---|---|---|---|
| bench_alloc.trace | FAIL (used=142.6K minfree=0B) | FAIL (used=158.6K minfree=512B) | FAIL (used=154.7K minfree=176B) |
| bench_calls.trace | OK (used=95.0K minfree=51200B) | OK (used=103.2K minfree=54184B) | OK (used=144.9K minfree=5016B) |
| bench_closure.trace | OK (used=95.9K minfree=51200B) | OK (used=104.1K minfree=54184B) | OK (used=146.1K minfree=1856B) |
| bench_generator.trace | OK (used=96.7K minfree=51200B) | OK (used=105.2K minfree=54184B) | OK (used=147.9K minfree=1040B) |
| bench_loop.trace | OK (used=94.7K minfree=51200B) | OK (used=102.9K minfree=54184B) | OK (used=144.3K minfree=5288B) |
| bench_promise.trace | FAIL (used=142.1K minfree=40B) | FAIL (used=159.2K minfree=72B) | FAIL (used=159.1K minfree=32B) |
| bench_proxy.trace | OK (used=97.4K minfree=51200B) | OK (used=105.8K minfree=53200B) | OK (used=148.2K minfree=1024B) |
| bench_sort.trace | FAIL (used=132.7K minfree=13824B) | FAIL (used=141.1K minfree=15808B) | FAIL (used=147.5K minfree=1040B) |

**（2026-09-12 レビューで訂正）** 初版のこの表は `used=0.48K` のような値を載せ、「`peak_used` が小さいうちに失敗している」「メモリ不足ではない可能性」と書いていたが、**これは replay.c の計測の不具合だった**。bench_\* は `--sample-every 4001` で再生しており、(1) 失敗した op では統計を取らずに return していたので、3,052 op 目で失敗した `bench_alloc` の `peak_used` は op 0 の値（空のプール、496B）だけだった、(2) 約2,500行しかない `bench_calls`/`bench_loop`/`bench_proxy`/`bench_sort` は op 0 と破棄後にしか標本を取らず、ピークを一度も観測していなかった。`replay.c` は失敗時にも標本を取るように、`run_all.sh` は1トレースあたり約2,000標本になる間隔に直し、上の表はその再実行の値（実測(host)）。**3本の FAIL は普通に160KiBを使い切っている**: `bench_alloc`・`bench_promise` は使用量が上限近くで失敗、`bench_sort` は使用量132.7K（tlsf）で最大空きブロック13.8KiBのところに、それより大きい単発確保（配列の拡張）が来て失敗している。トレースは `--profile host`（64MiB）で採ったものなので、160KiBでの失敗はトレース採取時の上限と再生時のプールが違うことによる当然の結果で、アロケータの性質ではない。

### Table D: 探索コスト・壁時計時間の最悪値（160KiB実行、1トレース中の最大値）

| trace | tlsf malloc ns max | estalloc steps max / ns max | naive steps max / ns max |
|---|---:|---:|---:|
| builtin_reentry.trace | 3729 | 3 / 2822 | 6 / 2492 |
| closures.trace | 5094 | 3 / 13083 | 7 / 2803 |
| deep_recursion.trace | 4140 | 3 / 1458 | 12 / 2353 |
| deep_recursion_device.trace | 3446 | 3 / 2683 | 75 / 2640 |
| error_toplevel.trace | 2400 | 3 / 1307 | 10 / 2468 |
| gc_threshold_device.trace | 3597 | 3 / 18767 | 25 / 1361 |
| generators.trace | 4604 | 3 / 19309 | 4 / 1253 |
| job_throw.trace | 24590 | 3 / 1657 | 5 / 2882 |
| memory_device.trace | 127728 | 3 / 95599 | 18 / 2670 |
| microtask_order.trace | 4135 | 3 / 3131 | 70 / 2984 |
| promise_chain.trace | 7050 | 3 / 6776 | 15 / 2307 |
| rejections.trace | 4823 | 3 / 19151 | 66 / 2637 |
| special_calls.trace | 24477 | 3 / 11612 | 22 / 3651 |
| sync_loop.trace | 4215 | 3 / 2762 | 4 / 3018 |
| try_finally.trace | 9364 | 3 / 2881 | 71 / 2535 |
| vmprobe_async_generator.trace | 2989 | 3 / 41073 | 19 / 2346 |
| vmprobe_closures.trace | 2914 | 3 / 5334 | 4 / 9237 |
| vmprobe_deep_recursion.trace | 11217 | 3 / 1674 | 10 / 2899 |
| vmprobe_promise_chain.trace | 13522 | 3 / 12312 | 18 / 2476 |
| vmprobe_sync_loop.trace | 3605 | 3 / 2342 | 4 / 1296 |

**steps列だけが構造的な比較として意味を持つ**（ns列はホストのタイマ・スケジューラのジッタを含む生の壁時計時間で、上の一覧の外れ値——tlsfの`job_throw`24,590ns・`memory_device`127,728ns——はいずれもmalloc自体の複雑さでは説明がつかない大きさで、ジッタと見るのが妥当、断定はしない)。tlsfはこの28トレース全体で「steps」概念自体が無い(§4の定義通り常時1)。estallocは全トレースでmalloc_steps_max=3(=セグリゲートリストのO(1)経路の中で最大3段目まで到達、線形フォールバックには一度も落ちていない)。naiveはトレースのブロック数・断片化度に強く連動し、4〜75の範囲でばらつく(`deep_recursion_device.trace`で75、`try_finally.trace`で71)——**これは"床"の実装が持つ性質そのもの**で、セグリゲーションもビットマップも無い以上、空きリストが長くなれば線形に伸びる。

## 6. 制限・未検証事項

- **tlsfの8バイト境界パッチはホスト限定**。実機ビルド（32bitポインタ、`ALIGN_SIZE_LOG2=2`のまま）でのTable A〜Dの数字は別物になる——この調査は**アルゴリズムの相対比較**(段差の付き方、断片化の出方、探索コストの構造)のためのもので、絶対バイト数を実機の予算判定にそのまま使わない。
- **estallocの`assert()`が実際のビルドで生きているか未確認**（§2）。
- **`memory_device.trace`のtlsf `malloc_ns_max=127728`・`free_ns_max=1006413`など一部の外れ値の原因を追っていない**——ホストのスケジューラ・ページフォルト・ASLRのいずれかによる一過性のジッタという推定に留まる（実測ではなく推定）。
- **`bench_*`系の`min_pool`（二分探索）は測っていない**（§5 Table C参照、トレースが数百万行でO(log range)回のフル再生が現実的な時間に収まらないため——`bench_promise.trace`は580万行、フル再生1回で数秒、二分探索は30回前後必要で計算上15分超）。スループット計測用であり、L2aのセグメントサイジングという本来の目的には`bench_*`以外の20トレースで足りると判断した。
- **`naive`はどの実装にも由来しない比較用コードで、上流での実績が無い**。estalloc/tlsfと違い、他プロジェクトでの使用実績による間接的な検証を経ていない。
- **taffyノード確保との突き合わせ（§5 Table B注記)は目安であり、実際にRust UIコアがこの`min_largest_free`推移と同じタイミングで大きい確保を要求するかは検証していない**——2つの独立した確保パターン(JS VM側の本トレース、taffy側の`tools/uibudget/`)を数字の上で並べただけ。

## 7. 成果物

- `tools/vmalloc/vmalloc.h` — 3バックエンド共通インタフェース。
- `tools/vmalloc/adapter_{tlsf,estalloc,naive}.c` — 各アロケータのアダプタ。
- `tools/vmalloc/vendor/` — IDF v6.0.1のtlsf/multi_heap（パッチ3箇所、コメント付き）、estalloc（step計測カウンタ1箇所のみ追加、pin `dd3988acb10ae5e14f1cbf3991753f12c4dcf9d2`）。
- `tools/vmalloc/replay.c` — トレース再生・二分探索・指標計算。
- `tools/vmalloc/build.sh` / `run_all.sh` — ビルドと全組み合わせ実行。
- `.cache/vmalloc/results.txt`（git管理外、`run_all.sh`が生成）— 本ドキュメントの表A〜Dの元データ全144行。
