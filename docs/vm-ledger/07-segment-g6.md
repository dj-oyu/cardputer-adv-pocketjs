# 07 G6: セグメント方式アロケータと参照整合性の関所

対象: [vm-L2-design.md](../vm-L2-design.md) §1.3 の G6（仕様 §7 完了条件 #6「セグメント追加・境界越え・返却で値とクロージャの参照が壊れない」）。
台帳06 の器（`tools/vmalloc/`）に4つ目の実装 `segment` を足し、`replay.c --verify` と `verify_all.sh` を関所にした。2026-09-12〜13、すべて実測(host)。実機の値ではない。

**この文書の主張は2つだけ**: (1) 完了条件 #6 を機械で判定する仕組みが存在し、36トレースで緑になる。(2) その仕組みは壊れたアロケータを実際に赤にする（故障注入5種、全部検出）。「省メモリになる」とは主張しない（§4 の数字がそれを言わせない）。

## 1. 作ったもの

| ファイル | 役割 |
| --- | --- |
| `tools/vmalloc/adapter_segment.c` | L2a の形をした汎用アロケータ。プール（replay が渡す arena）から **16B境界** のセグメントを first-fit で切り出し、セグメント内は **4Bヘッダ・4B粒度** の first-fit + free 時の即時結合。標準サイズに収まらない確保は要求量ちょうどの**専用セグメント**（1ブロック、free で即返却）。空になった標準セグメントは上限付き（既定2本）で保持し、超えた分は返却。ASan ビルドでは返却時に `__asan_poison_memory_region`、free ブロックの中身は `0xDD` で塗る |
| `tools/vmalloc/vmalloc.h` | 統計にセグメント用の欄（内部余白と外部断片化を**別々に**）、任意のフック `owner()` / `segment_at()` / `events()` / `configure()` |
| `tools/vmalloc/replay.c --verify` | 各ブロックに id 由来のパターンを書き、free / realloc の直前に照合。セグメントの**イベント**（追加・返却・再利用。`events()` で毎 op 監視）のたびに全生存ブロックを走査: 同じアドレスでパターンが無傷、4B整列、生きたセグメントの中（キャッシュ済み／生存0のセグメントは不可）、生存ブロック同士が重ならない、全セグメントが16B境界・プール内・互いに素、さらにバックエンドの `check()`。realloc は移動の有無に関わらず `min(旧,新)` バイトの内容保存を照合。最初の違反で停止し `result=BROKEN` / exit 3 |
| `tools/vmalloc/verify_all.sh` | **関所本体。** 36トレースを ASan+UBSan で `--verify` 再生（陽性）し、続けて故障注入5種を同じ検査にかけて**全部捕まることを要求**（陰性）。どちらか欠ければ exit 1 |
| `tools/vmalloc/sweep_segsize.sh` | 標準セグメントサイズの根拠表（§4） |

`replay.c` の既存出力行はそのまま（`run_all.sh` の結果と互換）で、`segment` のときだけ末尾に列が増える。

## 2. 関所の結果（2026-09-13、`bash tools/vmalloc/verify_all.sh`、96 MiB プール、標準セグメント 4096B）

```
G6: all traces verify clean; all faults caught      exit=0   real 1m37.5s
```

36/36 が `verify=OK check=1`。検査が本当に「追加・境界越え・返却を跨いだ」ことは各行のイベント数が示す（抜粋。`verify_sweeps` はイベント駆動の全走査回数、末尾の1回を含む）:

| trace | seg_added | seg_returned | seg_reused | dedicated (境界越え) | verify_sweeps |
| --- | ---: | ---: | ---: | ---: | ---: |
| closures | 33 | 32 | 1 | 1 | 71 |
| promise_chain | 803 | 802 | 49 | 1 | 1,707 |
| budget_teardown_live | 807 | 806 | 1 | 1 | 1,619 |
| special_calls | 38 | 51 | 4 | 15 | 109 |
| memory_device | 29 | 35 | 1 | 8 | 70 |
| bench_sort | 27 | 67 | 2 | 42 | 80 |
| bench_alloc | 2,300 | 2,299 | 590 | 1 | 1,904 |
| bench_promise | 10,025 | 0 | 10,000 | 1 | 692（`--verify-gap` で間引き） |

bench_\* は数百万 op なので全走査を約2,000回に間引いている（free / realloc 直前の照合は全 op で走る）。

**陰性対照**（`closures.trace`、毎 op 走査 `--verify-every 1`）:

| `--fault` | 何を壊すか | 検出 |
| --- | --- | --- |
| `early-return` | 生存ブロックが1つ残っていてもセグメントを返却／キャッシュ | op 1514: `id 1395 (711 B) pattern broken at byte 0` ＋ `lies in a CACHED (empty) segment` |
| `overlap` | 分割の残余ヘッダを4バイト手前に置く（使用中ブロックの末尾と重なる） | op 0: `backend check() failed`（`free footer mismatch`） |
| `misalign` | セグメント先頭を 16k+8 に置く | op 0: `segment 0 base ... not 16-aligned` |
| `pool-overlap` | 7回に1回、切り出した extent を消費し忘れる（2セグメントが同じ番地） | op 238: `backend check() failed`（`gap or overlap before trailing extent`） |
| `compact` | free のたびに次の生存ブロックを穴へ滑らせる（＝移動する。L2 が禁じる操作そのもの） | op 255: `id 253 (72 B) pattern broken at byte 0` |

`overlap` は最初、検査器ではなくアロケータ内の SEGV（ASan）で止まった。生存ブロックとアロケータの**メタデータ**の重なりは、replay がパターンを書くのがアロケータの戻り値の後なので、パターン照合には見えない（ヘッダを上書きしてしまう）。走査のたびに `check()` を呼ぶようにして直した。**検査器が捕まえられなかった故障が1つあって、それで検査器を直した**、という順序を記録しておく。

## 3. 既存3実装に同じ検査をかけた結果

`--verify` はセグメントのない tlsf / estalloc / naive にも使える（走査は 4,096 op ごと）。`closures.trace`、262,144B プール: 3実装とも `verify=OK verify_sweeps=3 verify_errors=0`。**差は出なかった。** これらは realloc の内容保存・非重複・整列（4B）だけの検査で、セグメント固有の項目は掛からない。

## 4. 標準セグメントサイズ（仕様 §7「L0 の使用量分布から決める」）

**決定: 4096 B（既定値、`--seg-size` で変更可）。** 根拠は以下の数字だが、**4096 と 8192 はこの測定では区別できない**（最小プール合計の差 0.35%）。

`sweep_segsize.sh`、非 bench 28トレース、-O2、実測(host):

| seg_size | Σmin_pool (KiB) | 160KiB に収まる | 内部余白 中央値（app 区間） | 外部断片化 中央値（app 区間） | 内部余白 中央値（全区間） | 外部断片化 中央値（全区間） | malloc_steps_max 中央値 / 最大 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1024 | 13,076.2 | 18/28 | 11,562 | 4,800 | 49,740 | 72,256 | 293 / 459 |
| 2048 | 13,004.9 | 19/28 | 13,656 | 2,640 | 62,782 | 68,696 | 229 / 366 |
| **4096** | **12,804.5** | 18/28 | 17,380 | 0 | 69,460 | 57,344 | 210 / 339 |
| 8192 | 12,849.6 | 18/28 | 21,526 | 0 | 81,522 | 40,960 | 168 / 330 |
| 16384 | 12,951.5 | 18/28 | 30,012 | 0 | 83,690 | 32,768 | 130 / 316 |

定義: 内部余白 = 生存セグメント内の空きブロック（ヘッダ込み）の合計、外部断片化 = プールの空き合計 − 最大連続空き。どちらも 160 KiB プールでの各 op 標本の最大値の、トレース間の中央値。「app 区間」は `# teardown` より前だけ。全区間の外部断片化が大きいのは `JS_FreeRuntime` が GC リスト順に解放してセグメントを番地バラバラに返すためで、**アプリ実行中の数字ではない**（app 区間では 4096 以上でゼロ）。この2つの数字は足してはいけない（別の瞬間の最大値）。

読み方:

- 最小プール合計は 4096 が最小だが、2048〜16384 の幅は 1.2% 以内。**サイズをここから決める根拠にはならない。**
- 内部余白は大きいほど増え（4096: 17,380 → 16384: 30,012）、外部断片化は大きいほど減る（全区間 57,344 → 32,768）。トレードオフの中点として 4096 を採る。
- 実機の主要クラス（§3.2: 48 / 80 / 48 / 32、+4B ヘッダで 52 / 84 / 52 / 36）に対し、4096−16（セグメントヘッダ）＝4,080B の詰め込みは JSObject 78個で余り 24B、初期 shape 48個で余り 48B（**計算値、実機未測定**）。8192 なら 157個/12B、97個/28B。どちらも端数は1%台で、この観点でも差はない。
- 160 KiB のプールで 4096 は 1/40。taffy の段差（29,648 / 59,296B の連続確保）を空けるのに要る返却が 8 / 15 セグメントで、16384 なら 2 / 4。**この観点は L2a で JS と taffy を同じプールに置く場合（D6、未決）にだけ意味を持つ。**

**tlsf / estalloc との比較**（同じ28トレース、二分探索の最小プール、bytes）:

| | tlsf | estalloc | segment 4096 | seg4096 / tlsf |
| --- | ---: | ---: | ---: | ---: |
| Σ 28トレース | 13,309,952 | 13,628,800 | 13,111,808 | 0.985 |
| 最小（1トレース） | | | | 0.959（promise_chain） |
| 最大（1トレース） | | | | 1.273（memory_device） |

比が 1 を切るのは大きい5本（promise_chain、budget_teardown_live、budget_honest_long_chain、stop_with_queue、gc_threshold_device）で、小さいトレースの大半は 1.01〜1.12、memory_device は +27.3%（専用セグメント8本: 配列の realloc 成長で旧・新の専用セグメントが同時に存在する）。**「セグメント方式にすれば省メモリ」とは言えない数字**であり、仕様 §7 が「固定プール化だけで省メモリになるとは主張しない」と書いているとおり。

`malloc_steps_max` が 100〜460 なのは、セグメント内 first-fit を全セグメントの空きリストにわたって線形に走るこの実装の性質（estalloc は 3、tlsf は走査なし）。bench_promise では生存セグメント 10,024 本で 20,111。**この `segment` は検査の対象となる「形」であって、性能を詰めたアロケータではない。** L2a の実装がサイズ別リストを持つなら、この列は変わる。

## 5. 実行コマンド

```bash
# WSL
bash tools/vmalloc/build.sh                                  # -> .cache/vmalloc/vmalloc_replay-{asan,o2}
bash tools/vmalloc/verify_all.sh                              # G6 関所（約1.5分）。--quick で bench_* を飛ばす
bash tools/vmalloc/sweep_segsize.sh                           # §4 の表（約1分）
# 1本だけ、故障注入つき
.cache/vmalloc/vmalloc_replay-asan --allocator segment --pool 4194304 --verify --verify-every 1 --fault compact .cache/vmtest/traces/closures.trace
# 既存実装に同じ検査
.cache/vmalloc/vmalloc_replay-asan --allocator tlsf --pool 262144 --verify .cache/vmtest/traces/closures.trace
```

トレースは `.cache/vmtest/traces/`（`tools/vmtest/run.sh --trace`）。本作業ではメインのチェックアウトから36本を写して使った。

## 6. 未検証・限界

- **実物の VM はまだこのアロケータの上で動いていない。** ここで検査したのは確保履歴（malloc/free/realloc の列）であって、フレーム push/pop やクロージャの `JSVarRef` そのものではない。完了条件 #6 を最終的に緑にするには、L2a 実装後に `vmrun.c` のアロケータをこれに差し替え、コーパスを `--verify` 相当の検査で通す必要がある。**今回作ったのは判定の仕組みで、判定そのものは L2a 以降。**
- ホストは JSValue 16B・ポインタ 8B。セグメント内の空きリンク（16B）と最小ブロック（20B）は実機（8B / 12B）より大きい。§4 の数字は相対比較にだけ使う。
- 実機の確保サイズ分布は未取得（§3.2 の注記どおり）。4096 の決定はホスト分布と計算上の詰め込みに拠る。
- 内部余白の「app 区間」は `# teardown` マーカーで切っているが、GC 直後の一時的な空きは含む（意図どおり: それは実行中に起きる）。
- `early-return` の故障はキャッシュに空きがあると返却ではなくキャッシュ化になる。検出は `lies in a CACHED segment` 側で起きた。返却経路（poison）の検出は `pool-overlap` / `misalign` で `check()` と整列検査が先に鳴るため、**ASan の use-after-poison が単独で鳴った実行はこの関所には無い**（開発中に misalign で一度観測したのみ）。
- 実機（COM3）には触れていない。
