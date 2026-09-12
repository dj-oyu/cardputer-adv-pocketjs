# tools/vmtest — VM 改造のホスト側判定基盤

`docs/quickjs-freertos-vm-spec.md` の L1〜L5 は、すべてここで取った L0 の基準に対して判定する。§12「ホスト上の差分実行・sanitizer・対象機能の Test262 を使い、各レベルで既存の合格項目を維持する」の実体。

- **コーパス**（`corpus/*.js` と `expected/*.txt`）: 出力がバイト単位で一致しなければ不合格。L1/L2 が守るべき意味論の基準。
- **Test262 部分集合**（`test262-baseline.txt`）: L0 で通った (テスト, モード) の一覧。減ったら不合格、増えるのは構わない。
- **割り当てトレース**: L2a のセグメント寸法を決めるための確保履歴（§7「標準セグメントサイズは L0 の使用量分布から決める」）。
- **時間の基準**（`timing-baseline.txt`）: ホスト -O2 の中央値・p95・最大。**実測(host)** であり実機の数字ではない。

すべて WSL で動かす（Windows 側に gcc は無い）。実機・シリアルポートには一切触れない。

## コマンド

```bash
# WSL: cd /mnt/c/devs/m5stack/cardputer-adv-pocketjs-vm
bash tools/vmtest/build.sh all                 # vmrun-asan と vmrun-o2 を .cache/vmtest/ に作る（Kconfig の既定経路。all-alloca / all-recur / all-flat で他の経路）
bash tools/vmtest/stack_probe.sh 2000 o2       # G1: 1 段あたりの C スタック（L2b 以降は NOT_PROPORTIONAL が正）
bash tools/vmtest/budget_probe.sh o2           # D10: 予算がヒープより先に答えるか（10 項目）
bash tools/vmtest/run.sh                       # コーパス（ASan+UBSan+LSan）
bash tools/vmtest/run.sh --variant o2          # コーパス（-O2）
bash tools/vmtest/run.sh --force-yield         # L1 以降: 全チェックポイントで yield させても出力が同じか（L2: opcode 粒度。L2c まで赤、FORCED-YIELD の節）
bash tools/vmtest/run.sh g5_gaps               # G5: 最長の中断禁止区間を info-<variant>.txt の "#info g5" 行に出す
bash tools/vmtest/run.sh --budget-jobs 3      # L1: 全 drain を 3 件で切っても出力が同じか
bash tools/vmtest/run.sh --trace               # 同時に .cache/vmtest/traces/<name>.trace を書く
python3 tools/vmtest/trace_stats.py .cache/vmtest/traces/closures.trace   # トレースの検証と要約
python3 tools/vmtest/test262.py --fetch        # 固定 revision を .cache/test262 へ（初回のみ、約2分）
python3 tools/vmtest/test262.py -j 8           # 部分集合を実行し基準と比較（ASan で約1分）
python3 tools/vmtest/test262.py --variant o2 --force-yield -j 8
python3 tools/vmtest/timing.py                 # 時間の計測（書き込みは --write）
```

生成物はすべて `.cache/vmtest/`（git 管理外）: `vmrun-{asan,o2}`、`obj-*/`、`actual-<variant>/<name>.{txt,raw,diff}`、`info-<variant>.txt`、`traces/`、`test262-results-<variant>.txt`。

## vmrun

`vmrun.c` は `components/pocketjs_guest/src/guest.c` の挙動を決める部分を**写したもの**で、作り直していない。ただし **drain のループ本体だけは L1 から写しではなく同じもの**で、`components/pocketjs_guest/src/vm_sched.c` と `vm_clock.c` を `build.sh` がそのままリンクする（この 2 ファイルは esp ヘッダを含めない）。つまり予算・yield・継続の判定はファームと同一のコードで検査される。

| 項目 | ゲストと同じ点 |
| --- | --- |
| アロケータ | ブロック前にサイズヘッダ、`usable_size` は要求サイズ（malloc の実バケットではない）、realloc は常に malloc+copy+free。QuickJS は `malloc_size` を `usable_size` で数えるので、上限と GC 閾値が同じ論理サイズで効く |
| 上限 | `JS_SetMemoryLimit(160 KiB)` / `JS_SetMaxStackSize(20 KiB)`（`main/app_session.c` の値）。割り込みハンドラも同様に常時 0 を返すものを入れる |
| 初期化順 | `JS_NewRuntime2` → 上限 → rejection tracker → `js_std_init_handlers` → `JS_NewContext` → `js_std_add_helpers(ctx, 0, NULL)` |
| drain | `vm_sched_drain()` そのもの。キューが空になるまで実行し、**空になった境界でだけ**未処理 rejection を報告。予算で切れた境界では報告せず継続する。ジョブ自体が例外を投げたら dump して即座に失敗を返し、残りのキューと報告は次の drain に回る |
| ターンの順序 | 継続 drain が空になるまで、ホスト側の完了配送（`--host-events` の `host.request`）も `frame()` も呼ばない。`docs/vm-L1-design.md` §2.1 の規則そのもの |
| 報告文言 | `E pocketjs_guest: Unhandled Promise rejection: <reason>`（ESP_LOGE の時刻部分を除いたもの） |

意図した差分（すべて観測結果を変えないか、明示フラグ）:

- eval の後、`frame` が無くても drain する。ゲストは `frame` が無いと `ESP_ERR_NOT_FOUND` で drain せずに返る。`--require-frame` でゲストと同じにする。
- ヘッダにトレース用の連番を持つ。x86-64 の `max_align_t` は 32 B なので、`{size_t, uint64_t}` を足してもヘッダ長は変わらない。
- `$262` は `--test262` の時だけ入れる。トレース時だけ GC 検出用のオブジェクトを 1 個作る（後述）。
- ホストは 64 bit。`sizeof(JSValue)` はホスト 16 B・実機 8 B（NaN boxing）で、ポインタも倍。**同じプログラムでもホストの方が多く確保する**。`-m32` はこの WSL に multilib が無く使えなかった。

終了コード: 0 正常、1 eval 中の未捕捉例外、2 ジョブの例外または未処理 rejection、3 引数・ファイルの誤り、4 ランタイム生成失敗、5 ジョブキューの暴走（`--runaway-jobs`）。

主なオプション: `--profile device|host`、`--heap-limit N[K|M]`、`--stack-limit N[K|M]`、`--frames N`（eval 後に `globalThis.frame()` を N 回、各回の後に drain）、`--fail-alloc N`（N 回目の確保試行を NULL にする。L2c の OOM 検証用）、`--time`、`--stats`、`--module`、`--strict`、`--include FILE`。

L1 の予算まわり:

| オプション | 意味 |
| --- | --- |
| `--budget-jobs N` | 件数モードの予算。N 件走らせたら yield し、残りは次の「ターン」= 継続 drain で片付ける。**時計は一切読まない**（`limit_us<=0` で時間判定が切れる）ので、中断点はプログラムだけで決まる。ホストで時間予算を使うと ASan の下では再現しないので、期待値と比べる検査は全部これ |
| `--force-yield` | `--budget-jobs 1` に加え、L2 からは弱シンボル `vmtest_vm_set_force_yield` 経由で **7 地点の opcode セーフポイント（goto×3・if_*×4）で毎回止める**。L2c が保存・再開を作るまで「止める」= 捕捉不能な `interrupted` なので、**ジョブが死ぬ = コーパスが赤くなるのが正しい**（FORCED-YIELD の節） |
| `--gaps` | G5。JS 実行中の連続する停止機会（セーフポイント／ホストからの進入／最外フレームの復帰）の間隔を VM 側で記録し、最大値と上位 8 件を `#info g5 ...` に出す（`--force-yield` 中は常時オン）。単位は ns（ホスト時計）。理由と読み方は G5 の節 |
| `--runaway-jobs N` | **1 本の論理 drain**（予算で切られた drain + その継続群）が N 件走ってもキューが空にならなければ終了コード 5。**既定は無効**。ファームの判定は時間（`VM_RUNAWAY_US`）が主で件数（`VM_RUNAWAY_JOBS`）は時計が死んだときの受け皿だが、件数モードのホストでは時計を読まないので、写せるのは件数の側。ターン数では**ない** — ターンは backstop でも終わるので、長いだけの正直な drain を暴走と見なしてしまう（vm-L1-design §5.2・§11.1）|
| `--stop-turns N` | N 回の継続ターンでセッションを**終わらせる**（終了コード 0、キューは実行せず破棄）。`app_stop()` がキューを残したまま呼ばれる場合の代役で、設計 §3.2 の検査に要るがそこでは機構を指定していなかったため、ここで足したもの |
| `host.exit()`（`--host-events` に同梱）| `pocket.app.exit()` の写し。停止要求は割り込み経由で届くので、**キューが空になったターンでだけ**読まれる（vm-L1-design §11.2）。`corpus/budget_exit_midchain.js` がその順序を固定する |
| `--host-events` | `host.request(k)` を入れる。k 番目のジョブ境界で「完了が記録され」、**キューが空になったターンでだけ**配送される Promise を返す。`pocket_api_complete()` / `pocket_api_pump()` の縮小模型 |

`#info turns=… max_run_turns=… jobs_dropped=…` は常に出る（`--stats` 不要）。`run.sh` と `test262.py` はどちらも `#info` 行を diff と判定から外す。

### プロファイル

| プロファイル | ヒープ | スタック | 用途 |
| --- | --- | --- | --- |
| `device`（vmrun の既定） | 160 KiB | 20 KiB | 実機の値そのもの。資源の振る舞い（OOM、GC、スタック溢れ）の確認 |
| `host`（`run.sh` の既定） | 64 MiB | 7 MiB | 意味論の確認。上限に当たらないことが前提のコーパスと Test262 |

空のプログラムでホストの `qjs_malloc_size` は 98,680 B（実測(host)、`--stats`）で、160 KiB の 6 割を占める。`device` プロファイルは**実機より余裕が少ない**。実機の空ランタイムの大きさは未計測。

### FORCED-YIELD

`--force-yield` または環境変数 `VMTEST_FORCE_YIELD=1` で、vmrun は**すべてのチェックポイントで中断→ホストへ復帰→再開**させる。L1 のチェックポイントはジョブ境界なので、これは `--budget-jobs 1` と同じ意味になり、L0 の「フックが無いので普通に走らせた」という注記は消えた。

加えて、弱シンボル

```c
void vmtest_vm_set_force_yield(JSRuntime *rt, int on);
```

が定義されていれば呼ぶ。L2 で opcode のチェックポイント（`docs/vm-ledger/04-opcode-checkpoints.md`）を実装したら VM 側でこれを定義し、粒度が opcode まで細かくなる。`run.sh --force-yield` と `test262.py --force-yield` は、その時も「全地点で中断しても出力と合格集合が変わらない」の検査のまま使える（§7 完了条件）。

L1 時点の結果（実測(host)）: コーパス 31 件が `--budget-jobs 1 / 3 / 7 / 16`・`--force-yield`・予算なしで、asan と o2 の両方でバイト一致。Test262 は `--force-yield` で両 variant とも 7,501 pass / 194 fail（基準と同じ、`regressions: 0`）。

**L2 で弱シンボルが埋まった**（`components/quickjs-ng/quickjs-ng/quickjs-vm.c`、`build.sh` がリンクし、`components/quickjs-ng/CMakeLists.txt` にも載せてある）。止まる地点は設計 §7.2 の分類 A の 7 地点だけ。`quickjs.c` 側の差分は、その 7 行の `js_poll_interrupts` → `js_poll_safepoint` の名前替え、スローパス `__js_poll_interrupts` の armed 分岐、frame pop 4 箇所と class-call の復帰 2 箇所の LEAVE フック、で、**既定経路の高速側（カウンタの減算）は無変更**。状態は `JSRuntime` のメンバではなくファイルスコープの 1 ポインタ — メンバにすると `JSRuntime` が 8 B 育ち、それだけで `gc_threshold_device.js`（じわじわ型 OOM）の結果が動いた（元の `quickjs.c` に 8 B のパディングだけ足して同じ落ち方を再現した。実測(host)）。**`JS_SetInterruptHandler` には乗せていない** — あれはセッション終了の述語で、終了と中断を同じ経路に通すと区別が消える（設計 §1.2）。armed 中はすべてのポーリングがスローパスに入るが、ホストの割り込みハンドラは影のカウンタで従来どおり 10,000 回に 1 回だけ呼ぶので、arm しても終了要求が読まれる地点は動かない。

**L2c までこの関所は赤い。** 今の VM が持つ「止まる」は捕捉不能な `interrupted`（設計 §4.1）だけなので、opcode 粒度で止めると最初の後方分岐でジョブが死ぬ。実測(host、2026-09-13、asan): `run.sh --force-yield` は **37 件中 3 件だけ通る**（`error_toplevel` / `job_throw` / `runaway_jobs` — いずれも `#info vm safepoints=0`、つまり分岐を 1 つも実行しないファイル）。残り 34 件は `#info vm force_yield=1 ... stops=1`（`budget_starve` は 10、`rejections` は 2）で落ちる。**通ってしまったら疑うこと**: `info-<variant>.txt` に `#info vm force_yield=1 safepoints=N stops=N` が出ていなければ弱シンボルがリンクされていない（`nm vmrun-asan | grep vmtest_vm_` で `T` が 3 つ）。通常走行（`--force-yield` なし）は 37/37（asan・o2）、Test262 は 7,501 pass / 194 fail / regressions 0 で変わっていない。

### G5: 最長の中断禁止区間

`--gaps`（`--force-yield` 中は常時）で、VM は**停止機会**のたびに時計を読み、直前の機会からの間隔を記録する。停止機会は 3 種: 7 地点のセーフポイント、ホストからの JS 進入（`JS_CallInternal` のプロローグ・ポーリングで `current_stack_frame == NULL`）、最外フレームの復帰（4 箇所の frame pop と、フレームを積まない callable — Promise の resolve 関数・Proxy・bound — の復帰）。ホストにいる間は数えない（`#info vm enters=N leaves=N` が一致していることが、数え漏れが無いことの検査）。**最大値**が G5 で（設計 §1.3: 平均や中央値では完了条件を満たさない）、上位 8 件を `start=<機会>:<関数名> end=<機会>:<関数名>` 付きで出す。

**単位は時間（ns）で、命令数ではない。** §2 の N1〜N4（正規表現・ネイティブ完結・for-in の 1 ステップ・パース）はバイトコードを 1 つも実行しない区間で、命令数では長さが 0 になる。ホストの ns は実機の値ではなく、ASan 版は素の -O2 より一桁遅い。**機構は実機に持ち越せるが数字は持ち越せない**（`vmtest_vm_set_gap_clock` に時計を渡す形なので、実機側は `esp_timer_get_time` を渡せば同じ記録が取れる。未実装）。

`corpus/g5_gaps.js` が N1〜N5 を関数 1 つずつに閉じ込め、各関数の先頭に自明な分岐を置いて区間の始点にその関数名が出るようにしている。実測(host、2026-09-13、同一バイナリ、他に何も走らせていない状態で 3 回走らせた範囲):

| 区間 | 何をするか | asan（3 回の範囲） | o2（3 回の範囲） |
| --- | --- | --- | --- |
| N2 `n2_native_json` | `JSON.stringify` 2 万要素 | 128.7〜137.2 ms | 10.1〜10.7 ms |
| N2 `n2_native_sort_callback` | JS 比較関数付き `sort` 2 万要素 | 78.7〜80.4 ms | 9.6〜10.9 ms |
| N4 `n4_direct_eval` | 2 万文の直接 eval（パース＋直線実行） | 60.9〜61.6 ms | 14.7〜15.3 ms |
| N1 `n1_regex` | `/^(a+)+b$/` に a×18（2^18 回の後戻り） | 14.8〜15.7 ms | 7.2〜8.4 ms |
| N3 `n3_forin` | 非 enumerable 3000 個を 1 ステップで読み飛ばす for-in | 上位 8 件に入らず | 上位 8 件に入らず（8 位は 0.75 ms） |
| N5 `n5_prologues` | 8 段の直線呼び出し | 同上 | 同上 |

`#info g5 max_ns` はいずれの回も asan で `n2_native_json`、o2 で `n4_direct_eval` が始点。同じプログラムでも sanitizer の有無で最長区間の**種類**が入れ替わるので、ホストの順位を実機の順位と読まないこと。

**捉えられるもの**: N1・N2・N4 は「セーフポイント→セーフポイント」の 1 区間として現れ、始点の関数名で区別できる。N3 は区間としては存在するが（`for_in_next` の 1 ステップ）、このサイズでは上位 8 件の閾値（コーパス内の普通のループ区間）より短く、一覧に出ない — 出すには対象を大きくするか、上位 8 件ではなく関数名で絞る出力が要る。**ここで新しく分かったこと**: `sort` の比較関数のように**JS を走らせ続けているのに 7 地点を一度も通らない区間**が実在する（呼び出しのプロローグは確認地点だが停止機会ではない）。設計 §7.6 の「A の 7 地点を通らない長い同期実行が実在するか」への答えは**実在する**で、分類 B（呼び出し地点）が C 再帰の解消だけでなく中断のためにも要る。

**捉えられないもの / 限界**: (1) N5 は固定長で短く、`n5_prologues`（8 段の直線呼び出し）は上位 8 件に入らない。区間として存在はするが、「プロローグが原因」とは区間からは読めない。N3 も同じ理由で一覧に出ない（上の表）。(2) 終点はセーフポイントの**関数名**までで、pc は出していない（スローパスが `pc` を受け取らないため。必要なら 7 地点で `sf->cur_pc` を書く 1 行が要る）。(3) 直接 eval のパース（N4）と実行は同じ区間に溶ける。(4) ホスト時計の分解能とスケジューラのノイズがそのまま乗る。既定経路の速度は `timing.py` で HEAD のビルドと交互に 2 往復して比べ、差は往復間のばらつきの中（bench_loop の中央値 66.95/66.72 ms → 63.74/63.34 ms、bench_calls 64.82/66.04 → 62.90/64.20、他は ±3% 以内）。ただし同じ HEAD ビルドが `timing-baseline.txt`（2026-09-12）より 8〜27% 遅い日だったので、基準ファイルとの直接比較は今日の機械の状態を測っているだけで、この変更の性能を測っていない。**他のプロセス（Test262 の `-j 8` や `idf.py build`）と同時に走らせると数字が 5〜20 倍に膨れた**ので、読む値は必ず単独走行のもの。(5) `--force-yield` 中は各ジョブの最初の停止で記録が終わるので、L2c まで G5 の完全な記録は `--gaps` 単独で取る。(6) 実機では未実装・未計測。

## コーパス

`run.sh` はファイル先頭の `// vmrun-flags: ...` を `--profile host` の後ろに付ける（後勝ちなので上書きできる）。vmrun の終了コードを最終行 `exit=N` として出力に足し、これも diff 対象。`#info` で始まる行（計測値）と `vmrun: note:` 行は diff から外し、`info-<variant>.txt` に集める。cwd は `corpus/` で、ラベルとスタックトレースはファイル名だけになる。

| ファイル | 固定しているもの |
| --- | --- |
| `sync_loop.js` | for/while/do/ラベル付き break・continue/for-in 順序/for-of/switch。後方分岐の全形 |
| `deep_recursion.js` | 7 MiB スタックでの再帰、相互再帰、500 段の finally 巻き戻し、溢れが捕捉可能な RangeError であること、溢れ後の回復 |
| `deep_recursion_device.js` | 同じことを 20 KiB で。組み込み関数（`map`）のネイティブフレームを跨ぐ溢れ |
| `closures.js` | 生存中・切り離し後のフレームを共有するクロージャ、3 段の捕捉、sloppy の mapped arguments、generator/async の中断中フレームを捕捉したクロージャ、eval が作った変数の捕捉 |
| `promise_chain.js` | 3000 段の then、2000 回の await ループ、500 段の reject 伝播、all/allSettled/race/any、ジョブが次のジョブを積み続ける drain、1 つの resolve への 200 本の反応 |
| `microtask_order.js` | then/catch/finally/await/thenable/queueMicrotask の実行順（`frame()` から出力するので drain 完了後の順序そのもの） |
| `generators.js` | next/return/throw、finally 内の yield で止まる return、未開始の generator、yield* の転送、再入禁止、async generator の要求キュー・for await・break・throw |
| `try_finally.js` | 呼び出しを跨ぐ finally、finally による上書き、ループ中の break/continue、スタックトレース文字列（行:列を含む） |
| `special_calls.js` | getter/setter、Proxy（trap 自体が Proxy のものを含む）、direct/indirect/strict eval、bound（new を含む）、constructor・派生クラス・new.target、apply/call/spread、タグ付きテンプレート、型変換コールバック |
| `builtin_reentry.js` | sort の比較関数（安定性、例外、配列の変更、入れ子の sort）、反復系コールバック、JSON reviver/replacer/toJSON、replace 関数、Symbol.replace、RegExp サブクラスの exec |
| `rejections.js` | 未処理のまま / 同じ drain 内で後から処理 / 次の frame で処理（既に報告済み）/ all に吸収 / 非 Error 値。報告の時点と順序 |
| `job_throw.js` | ジョブが例外を投げた drain の早期復帰と、残りが次の drain に回ること |
| `error_toplevel.js` | トップレベルの未捕捉例外。drain されずキューが残ること |
| `memory_device.js` | 160 KiB 下での参照カウント解放、大きな単発確保の OOM が InternalError として捕捉でき回復すること |
| `gc_threshold_device.js` | **現行設定の性質**: 循環ゴミが上限まで溜まること（後述） |
| `budget_frame_boundary.js` | 予算が drain を切っても `frame()` が drain の**中**に入らないこと。eval が積んだ連鎖の最後の出力と `frame 1` の間に境界がある |
| `budget_completions.js` | 継続ターン中に記録された完了が、落ちず・重複せず・要求順に・**キューが空になったターンでだけ**配送されること |
| `budget_starve.js` | 毎ターン 41 件積んで予算 8 件でも、次の `frame()` までに必ず片付くこと（1 フレーム 1 連鎖で遅れない） |
| `runaway_jobs.js` | `f(){Promise.resolve().then(f)}`。キュー長は常に 1、各ジョブは一瞬なので旧来の壁時計ガードには見えない形。終了コード 5、LSan 0 |
| `budget_reject_far_catch.js` | 報告の時点。catch が 70 件先（backstop の外）／ rejection が drain の途中で生まれて 40 件先で捕まる／`frame()` の連鎖内で完結、の 3 形はどれも報告されず、誰も捕まえない 1 件だけが報告される |
| `budget_boundary_exact.js` | 境界そのもので記録された完了。ジョブ内から `k=0`、完了ハンドラからの再入、完了が積んだ連鎖の最中の記録。配送はキューが空になったターンだけ、記録順に 1 回ずつ |
| `budget_teardown_live.js` | キューを残した終了の重い形。await で中断した async、届かない finally、try/finally 内の generator、要求の残った async generator、切断の向こうの thenable、捨てられるジョブの中に catch がある rejection。報告 0 行・`jobs_dropped=1`・LSan 0 |
| `stop_with_queue.js` | キューを残したままのセッション終了。残りは**実行せず**破棄、`#info jobs_dropped=1`、残っていた rejection は**報告しない**（捨てたジョブの中に catch があったかもしれない）、終了コード 0 |
| `bench_*.js` | 時間計測用。出力はチェックサムで、これも意味論の基準になる |
| `g5_gaps.js` | G5 の器。§2 の N1〜N5 を関数 1 つずつに閉じ込め、`#info g5 top[k]` の始点関数名で区間を同定する（FORCED-YIELD の節の表）。diff されるのは各区間の返り値だけ |
| `seg_add_deep.js` | L2a・G6 完了条件#6「セグメント追加」。1セグメントに収まらない数のフレームを積む深い再帰で、各段が自分専用の値を積んだ後の呼び出し結果を読み直す。読み直した値が壊れていれば、下の段の確保が上の段の領域を侵していたことになる |
| `seg_boundary_bigframe.js` | 同「境界越え」。`new Function` で大きな `var_buf` を作る関数と、`apply` で大きな `arg_buf` を作る呼び出し（どちらも `JS_MAX_LOCAL_VARS`=65535 未満）。通常サイズのフレームから呼ばれ、通常サイズのフレームへ返ることを両方向で固定する |
| `seg_return_reuse.js` | 同「返却と再利用」。浅い再帰を何十サイクルも繰り返し、サイクル番号と深さの両方を符号化した値を毎段で読み戻す。後のサイクルが前のサイクルの値を読めば、返却済みの領域が汚れたまま再利用されたことになる |
| `seg_closure_survives.js` | 同「クロージャがフレームを掴んだまま返却される」。捕捉した変数を読むたびの間に無関係な深い再帰（`churn`）を挟み、返却済みのはずの領域がその再帰に再利用されても捕捉値が無事かを見る |
| `seg_generator_frames.js` | 同「generator/async のフレーム」。`done_generator` 経由で中断するフレーム（`alloca` を通らない別経路）の再開のたびに、無関係な深い再帰を挟む。2本の generator を交互に進める形も含め、片方の中断領域がもう片方や無関係な再帰に取り違えられないかを見る |
| `l2b_flat_calls.js` | L2b（設計 §10）。フラット復帰が呼び出し元のローカルをフレーム鎖から組み直す箇所を 1 行ずつ固定する: 既定引数の呼び出しの後で `OP_rest` / `arguments` が読む `argv`・`argc`（D11、宣言より多い引数を native から受けた床を含む）、呼び出し後の `this` / `new.target`、4 つの呼び出し形（plain / method / tail / tail method）の復帰整理、300 段のフラットフレームを跨ぐ例外と finally、generator / async / async generator の床、C 再帰のまま囲った経路（constructor / apply / Proxy / bound / getter / sort / eval）。alloca・再帰・フラットの 3 経路で同じバイトが出る |
| `seg_oom_boundary.js` | 同「境界の直前・直後の OOM」。固定の浅い再帰（device のスタック上限より十分浅い）の中の1段だけが単発の大きな確保をして 160KiB を超える。`memory_device.js` と同じ「余裕を残した単発確保」の形を保ったまま、確保がコールチェーンの途中で起きる点だけを変えている（`known/oom_backtrace_uaf.js` が記録する上流UAFはジワジワ型OOMで踏むため、あえて避けている） |

**期待値の更新規則**: `run.sh --bless` は新しいファイルを足した時にだけ使う。既存の `expected/*.txt` を書き換えるのは、挙動の変更が意図されたもので理由を説明できる場合だけで、その変更だけの commit にする（§12「新たな失敗を期待値の書き換えだけで処理しない」）。

## トレース形式

`vmrun --trace OUT file.js`（`run.sh --trace` なら `.cache/vmtest/traces/<name>.trace`）。1 行 1 レコード、空白区切り。

| 行 | 意味 |
| --- | --- |
| `+ <id> <size>` | malloc / calloc 成功。`<id>` は成功した確保の通し番号（1 から、realloc も番号を 1 つ使う）。ポインタではないので再生できる |
| `- <id>` | free |
| `~ <oldid> <newid> <newsize>` | realloc 成功。ゲストの realloc は常に別ブロックなので id が変わる |
| `! <size>` | malloc / calloc がアロケータで失敗（`--fail-alloc`） |
| `!~ <oldid> <newsize>` | realloc がアロケータで失敗。旧ブロックは生きたまま |
| `# gc <live_bytes> <live_blocks>` | GC（`JS_RunGC`）の開始。その時点の生存量 |
| `# vmtrace 1 file=... heap_limit=... stack_limit=... sizeof_JSValue=... sizeof_ptr=... header=...` | 先頭の見出し |
| `# ready` / `# teardown` / `# end ...` | 評価開始前・破棄開始・終了。`# end` で生存 0 が正常 |

`#` で始まる行はすべて注記で、再生では読み飛ばしてよい。サイズは QuickJS が要求したバイト数（ヘッダを含まない）で、**ホスト 64 bit の値**。

注意:

- `JS_SetMemoryLimit` による拒否は QuickJS がアロケータを呼ぶ前に行うので、トレースには現れない（`!` は `--fail-alloc` とホストの malloc 失敗だけ）。ゲストでも同じく `heap_caps_malloc` は呼ばれない。
- GC の検出は VM を変更せずに行っている。`JS_RunGC` の最初の走査（`gc_decref`）は生存オブジェクト全部の class `gc_mark` を呼び、次の走査は別の mark 関数で呼ぶ。GC 観測用のクラスのオブジェクトを 1 個作り、最初に見た mark 関数で呼ばれた時だけ `# gc` を書く。このオブジェクトの確保 1 回（とクラス登録の分）がトレース時だけ増える。`JS_FreeRuntime` の最後の GC はこのオブジェクトを解放した後なので記録されない。
- `trace_stats.py` は再生して整合（生きていない id の free が無い、id が単調増加、終了時に生存 0）を検査し、確保回数・ピーク生存量・サイズ分布（2 の冪ごと）を出す。

## Test262

- revision: `72faf8ec1445c55149615e8b35187830783aba1a`（2026-09-12 の main）。`test262.py` の `PINNED`。`--fetch` は `harness/` と対象ディレクトリだけを sparse checkout する。
- 対象: `language/statements/{async-function,async-generator,generators,try,for-of}`、`language/expressions/{call,new,async-arrow-function,async-function,async-generator,await,yield,arrow-function}`、`language/eval-code/direct`、`built-ins/Promise`、`built-ins/Proxy/{apply,construct,get,set,has,revocable}`（JS へ再入する trap を選んだ標本）。4,099 ファイル。
- 各ファイルを `onlyStrict` / `noStrict` / `raw` / `module` に従って sloppy と strict の両方（またはどちらか）で走らせる。`async` は `doneprintHandle.js` を足し `Test262:AsyncTestComplete` を待つ。`negative` はエラー名と、parse の場合は相（compile のみで失敗したか）を確かめる。`$262` は `global` / `evalScript` / `gc` / `detachArrayBuffer` / `createRealm` を持つ。`IsHTMLDDA`・`SharedArrayBuffer`・`Atomics` と `CanBlockIsTrue` は SKIP。
- sync テストが意図的に未処理 rejection を残す場合（終了コード 2 で報告行だけ）は合格扱い。報告の時点はコーパスが見る。
- L0 の結果（実測(host)、asan と o2 で同一、2 回実行で同一）: **7,501 pass / 194 fail / 0 skip**。失敗の内訳は未対応機能で、`Promise.allKeyed`/`allSettledKeyed`（174）、末尾呼び出し最適化（9）、`using`/`await using`（7）、`Promise.try` の一部（4）。

## 時間の基準

`timing-baseline.txt`。各ベンチを 30 回ずつ逐次に走らせ、vmrun の `#info time_ns`（eval + drain + frame、プロセス起動とランタイム生成を除く）の中央値・p95・最大・最小を ms で記録。**実測(host)**: Intel Core Ultra 7 258V、WSL、gcc 13.3 -O2。実機（Xtensa、-Os）の値について何も言わない。同じホスト・同じフラグで L0 と比べるためのもので、実行間のばらつき（p95 と中央値の差）より小さい差は結果と呼ばない。

記録時の `quickjs.c` の sha1 は `271d718782c1`。これは作業ツリーの他作業による未コミットの VM_PROBE 追加を含むファイルで、`CONFIG_POCKET_VM_PROBE` 未定義のため該当部分はコンパイルされていない。

## L0 で分かったこと

いずれも実測(host)。実機の値ではない。

- **再帰の深さ**（`#info max_depth`）: 7 MiB スタックで o2 11,187 段 / asan 6,370 段。実機と同じ 20 KiB の上限では o2 **29 段** / asan 16 段、`map` のコールバック経由では o2 11 段。ホストのフレームは Xtensa のフレームと大きさが違うので実機の段数は分からないが、実機の上限（`gc.stack_limit=20*1024`、ui タスクのスタックは 32 KiB）はホストの既定よりはるかに小さい。実機での段数は未計測。
- **実機設定では循環ゴミが回収されない**: quickjs-ng の `malloc_gc_threshold` の初期値は 256 KiB（`quickjs.c` の `JS_NewRuntime2`）で、firmware はどこでも `JS_SetGCThreshold` を呼ばない。一方ゲストの上限は 160 KiB なので、自動の循環回収は上限より先に来ない。`gc_threshold_device.js` のトレースには `# gc` が 1 行も無く、2 オブジェクトの循環 265 個で OOM になる（host プロファイルでは `bench_alloc` で GC が 296 回走る）。上限超過で GC をやり直す経路も無い。実機で同じことが起きるかは未確認だが、閾値と上限の大小関係はコードで確定している。
- **OOM 時の use-after-free（上流の不具合）**: 例外のバックトレースを組み立てる途中でヒープが尽きると、`build_backtrace()` の DynBuf が `js_dbuf_realloc` → `js_realloc` → `JS_ThrowOutOfMemory` を呼び、`JS_Throw` が現在の例外（= 組み立て中の `error_val`、借用参照）を解放し、続く `can_add_backtrace()` が解放済みオブジェクトを読む。ASan で検出（`known/oom_backtrace_uaf.js`、`vmrun-asan --profile device` で再現）。上限にじわじわ近づく OOM で起きうるので、実機でも起きる経路。o2 では何事もなく進むように見える。`known/oom_backtrace_uaf.js` は**1 バイトでも変えると再現しなくなることがある**（ソース長が残りの余裕を変える）。コーパスの OOM は余裕が残る単発の大きな確保に限った。`gc_threshold_device.js` もじわじわ型なので、L2 以降でこれが `build_backtrace` の ASan 報告で落ちたら、まずこの不具合を疑う。

## ビルドの注意

- ASan 版は QuickJS 自体も計装する（`tools/build_pocket_text_test.sh` は QuickJS を計装しない）。L1 以降で変わるのは `quickjs.c` だからで、変更した呼び出し経路の use-after-free を報告させるため。
- `quickjs-vmprobe.h` は `__has_include("sdkconfig.h")` で分岐し、ホストでは `CONFIG_*` が未定義 = 出荷時の既定になる。`build.sh` が `.cache/vmtest/include/` に置く空の `sdkconfig.h` は、この分岐が入る前の名残で、なくても動く。**ただし既定 y のスイッチはこの規則に乗らない**: `CONFIG_POCKET_VM_SEGFRAMES`（L2a、`main/Kconfig.projbuild`）は `build.sh` が `-D` で明示的に渡す。渡さなければホストは firmware が出荷しない方の経路（alloca）を検査することになる。
- **L2a の旧経路（alloca）は `-alloca` 付きバリアントで作る**: `build.sh asan-alloca` / `o2-alloca` / `all-alloca`。コンパイルフラグは同じで define だけが無く、`run.sh --variant asan-alloca`・`stack_probe.sh 2000 o2-alloca`・`test262.py --variant asan-alloca` がそのまま使える。仕様 §12 の「戻せる」はこれで確かめる（L2a 着手前の結果と同一であること）。
- **L2b は 3 経路になった**（設計 §10.2）: `-alloca`（L2a 以前）、`-recur`（segframes、C 再帰のまま = `CONFIG_POCKET_VM_FLATCALLS=n`）、`-flat`（segframes + フラット呼び出し）。**無印の `asan` / `o2` は `main/Kconfig.projbuild` の既定を写す**（`build.sh` の `segframes=` / `flatcalls=` の 2 行が Kconfig と一致していること）。今は既定 y なので無印 = フラット。関所の読み方: G1 はフラットで `bytes_per_call=0.000 … NOT_PROPORTIONAL`、`-recur` で 528.000、`-alloca` で 672.000（いずれも実測(host) o2）。`budget_probe.sh` は最初の 3 項目の期待を `#info vmstack flat=` から決めるので、フラットでは出荷値の走行で `budget_hits>0`（予算が答えている）、`-recur` では 0（C スタックのガードが先）。フラットビルドに `-DCONFIG_POCKET_VM_SEGFRAMES` が無いと `quickjs-vmstack.h` が `#error` で止める。
- **L2a のセグメント**（`quickjs-ng/quickjs-vmstack.h`、ヘッダのみ）: `vmrun --stats` が `#info vmstack seg_size=… depth_max=… live_max=… frame_max=… seg_live_max=… seg_mallocs=… dedicated=… fallbacks=… resident_max~=…` を出す。`--vm-seg-size N[K]` で標準セグメントを変えられる（最初の JS 呼び出しの前にだけ効く）。`live_max` はフレームが実際に使った最大バイト、`resident_max~` はその瞬間にセグメントが占めていた概算（ヘッダと整列の余白込み）。ホストの数字は 64bit の値であって実機の値ではない。
- **L2a のセグメントは asan 版で毒を塗る**: `quickjs-vmstack.h` は ASan ビルドでセグメントの空き領域を `__asan_poison_memory_region` で毒にし、push した分だけ解毒、pop で再び毒にする。返却済みフレームへの生ポインタ（`close_var_refs` が閉じ損ねた `JSVarRef`、死んだフレームを歩くウォーカー、呼び出し先の argv を持ち越した呼び出し元）は、コーパスと Test262 の asan 走行で use-after-poison として鳴る。台帳07 §6 が「確保履歴では検査できない」と書いた、実物のフレームに対する検査がこれ。o2 と実機では何も展開されない。
- **セグメント境界の総当たり**: `VMTEST_VMRUN_FLAGS="--vm-seg-size N" run.sh` でセグメントサイズを変えてコーパスを回せる（期待値は同じ。サイズは観測できてはならない）。L2a 評価では o2 で 16〜1024B を 8B 刻み（`js_vm_stack_configure` が 16 の倍数へ切り上げるので実効は 16 刻み）、asan で 16 / 88 / 136 を回した。
- **pop の鍵はフレームを push したかどうかで、`b->func_kind` ではない**: モジュール本体の関数は `__JS_EvalInternal` が `JS_FUNC_ASYNC` として組むが、`js_inner_module_linking` が hoisting 済み宣言の初期化のために `JS_Call(ctx, m->func_obj, JS_TRUE, 0, NULL)` で通常経路から呼び、`done_generator:` に抜ける（L2a 時点では「モジュール内の直接 `eval`」と書いていたが、H7 の実験で主語が違うと分かった。設計 §10.1。直接 `eval` は `JS_FUNC_NORMAL`）。`func_kind` で pop を決めると、この 1 フレームが積まれたまま残り `JS_FreeRuntime` の assert で落ちる（Test262 `language/eval-code/direct/export.js` / `import.js` が見つけた）。`flags` も opcode がスクラッチに使うので鍵にできない。鍵は `js_vm_stack_holds()`（`sf` が先頭セグメントの生存範囲にあるか）。C ローカルで覚える版は G1 が 528→544 B/段に増えた（実測(host) o2）ので採らなかった。
- **`gc_threshold_device.js` はヒープの残量に敏感で、L2a では条件によって落ちる**: `run.sh --trace` の asan 走行、および `--vm-seg-size 2048` の o2 走行で、OOM を catch した後の `print` 自体が OOM して `null` が未捕捉になる（`cycles-exhaust-heap true` の後に `null` が 2 行、exit=1）。常駐セグメント（ホストで 4,143B）がジワジワ型 OOM の「残り」を変えるためで、メモリ破壊ではない（同じ変更で alloca 版は通る）。通常の `run.sh`（4 バリアント）では通る。`--trace` でトレースを採るときはこの 1 件の FAIL を織り込むこと。
- オブジェクトは `quickjs-ng/*.c`・`*.h`・`build.sh` のいずれかが新しければ作り直す。
