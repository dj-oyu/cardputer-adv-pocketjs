# VM L0 報告（phase 0 / `vm/p0-foundation`）

対象: [quickjs-freertos-vm-spec.md](quickjs-freertos-vm-spec.md) §5（L0）と、L1/L2 の判断材料。2026-09-12 時点、HEAD `d9ef1f9` + 未コミットの L0 作業。

**実機ではまだ何も測っていない。** 書き込みもシリアルポートも使っていない。以下の数値は、ビルド成果物の静的な値（実測(build)）、WSL 上のホスト実行の値（実測(host)）、コード読解の結果、推定、のいずれかで、それぞれ明記する。

要点:

- quickjs-ng とゲストのリポジトリへの取り込みは完了し、コミット済み。ファームの DIRAM はバイト単位で変わらない。
- 計測プローブ（既定で無効）、ホストの判定基盤、アロケータ比較、台帳 6 本が揃った。**実機の値が無いので、L0 の完了条件はまだ満たしていない。**
- L2 について最も重要な事実: **既存の async/generator 機構はフレームのデータをヒープへ移すだけで、C の再帰は取り除かない。** 「C スタックを JS の深さから切り離す」には、call 系 opcode と `done:` の構造を新しく作る必要がある。

## 1. できたこと

| 項目 | 状態 | ファイル |
| --- | --- | --- |
| quickjs-ng 0.14.0 とゲストの取り込み | コミット済み（`10f5185` 無改変の取り込み → `6de51f4` immutable-buffer パッチ → `d9ef1f9` リンク先の切り替え） | `components/quickjs-ng/`、`components/pocketjs_guest/` |
| L0 プローブ（`CONFIG_POCKET_VM_PROBE`、既定 n） | 未コミット | [main/Kconfig.projbuild](../main/Kconfig.projbuild)、[sdkconfig.vmprobe.defaults](../sdkconfig.vmprobe.defaults)、[main/pocket/vmprobe.c](../main/pocket/vmprobe.c) / `.h`、`quickjs.c` の `VM_PROBE` ブロック 4 つ + [quickjs-vmprobe.h](../components/quickjs-ng/quickjs-ng/quickjs-vmprobe.h)、`app_session.c` / `main.c` / `pocket_api.c` / `main/CMakeLists.txt` の `#ifdef` 部分 |
| 実機用ワークロード 6 本（USB の `A`〜`F` だけで起動し、ホームの一覧には出ない） | 未コミット | [apps/vmprobe/](../apps/vmprobe/) |
| 実機の採取スクリプト | 未コミット | [tools/vm_l0_capture.py](../tools/vm_l0_capture.py) |
| ホストの判定基盤（guest.c を写した vmrun、コーパス 23 本、Test262 部分集合、時間の基準、確保トレース、`--force-yield` の入口） | 未コミット | [tools/vmtest/](../tools/vmtest/)（手順は [README](../tools/vmtest/README.md)） |
| アロケータ比較（IDF の tlsf 実物 / estalloc / naive） | 未コミット | [tools/vmalloc/](../tools/vmalloc/) |
| 台帳 | 未コミット | [vm-ledger/01〜06](vm-ledger/) |

取り込みで挙動が変わっていないことの根拠:

- 取り込み前後で `idf.py size` の DIRAM は 115,292 B、IRAM は 16,384 B で同じ（実測(build)）。
- `.bin` は 64 B 小さくなった（2,539,504 → 2,539,440 B）。原因は、`__FILE__` から作られる assert の文字列に入るソースのパスが短くなったこと。DIRAM には影響しない。
- 取り込んだパッチ済みの `quickjs.c` は、旧来のビルド時生成物（`prepare_quickjs.py` が作ったもの）と `diff -q` で一致した。

レビューで直した欠陥は 9 件。主なものは次の 3 件。

- `sdkconfig.h` を無条件に include していたため、ホストテスト 4 本がビルドできなくなっていた。
- `deep_recursion.js` は固定の 400 段で、実機では毎フレーム例外になる内容だった。
- 台帳 06 の bench 表は、`replay.c` の計測の不具合から出た値だった。

別のレビュー（Fable）が挙げた台帳の欠落 13 件は、すべて `quickjs.c` と照合して事実と確認し、台帳に反映した。

ホストで再現するコマンド（WSL、`/mnt/c/devs/m5stack/cardputer-adv-pocketjs-vm`）:

```bash
bash tools/vmtest/build.sh all && bash tools/vmtest/run.sh && bash tools/vmtest/run.sh --variant o2
python3 tools/vmtest/test262.py -j 8        # 初回は --fetch
python3 tools/vmtest/timing.py
bash tools/vmalloc/build.sh && bash tools/vmalloc/run_all.sh   # トレース採取は台帳06 §4
```

## 2. ホストで得た基準値

特記のないものは**実測(host)**（x86-64、WSL、gcc 13.3）。ホストでは JSValue が 16 B、ポインタが 8 B で、実機（8 B / 4 B）より多く確保する。**バイト数の絶対値を実機の予算判定に使わないこと。**

| 項目 | 値 |
| --- | --- |
| プローブ無効ビルドの DIRAM | 115,292 B で `build_vm` と同じ（実測(build)）。flash は assert の `__LINE__` がずれる分だけ数バイト違う |
| プローブ有効ビルドの DIRAM | 117,052 B、+1,760 B（`vmprobe.c.obj` +1,736、`quickjs.c.obj` +16。実測(build)、`.cache/memlog/memory.jsonl`） |
| コーパス | 23/23 一致（ASan、-O2、`--force-yield`）。L0 には yield の入口が無いので、`--force-yield` はまだ何もしない |
| Test262 部分集合 | 7,501 pass / 194 fail / 0 skip。失敗はすべて未対応の機能: `Promise.allKeyed` 系 174、末尾呼び出し 9、`using` 7、`Promise.try` 4。[test262-baseline.txt](../tools/vmtest/test262-baseline.txt) |
| 時間の基準 | 8 本のベンチ、各 30 回の中央値・p95・最大。[timing-baseline.txt](../tools/vmtest/timing-baseline.txt) |
| 20 KiB スタック上限での再帰の深さ | ホスト o2 で 29 段、`map` のコールバック経由で 11 段。実機は約 50 段と**推定**（ELF 上の `JS_CallInternal` のフレームが `entry a1, 0x150` = 336 B + alloca）。実機の段数は未計測 |
| 空のランタイム | `qjs_malloc_size` 98,680 B。160 KiB の 6 割にあたる |

コードから確定している性質（実測ではない）:

- **循環ゴミの自動回収が起きない。** GC 閾値の初期値は 256 KiB で、ゲストの上限 160 KiB より大きい。firmware はどこでも `JS_SetGCThreshold` / `JS_RunGC` を呼ばない。ホストでは `gc_threshold_device.js` が、2 オブジェクトの循環 265 個で OOM になる。
- **上流の不具合。** バックトレースを組み立てる途中の OOM で use-after-free が起きる（ASan で検出。[known/oom_backtrace_uaf.js](../tools/vmtest/known/oom_backtrace_uaf.js)）。
- **8 バイト境界が保証されない。** 実機の tlsf は 4 バイト境界で、JSValue や double が 8 バイト境界に乗る保証はアロケータ層のどこにも無い。実害（Xtensa で落ちるのか、遅くなるだけか）は未確認。
- **ゲストの上限の記述が食い違っている。** ソース上は 160 KiB（`app_session.c`）だが、`CLAUDE.md` は 144 KiB と書いている。

アロケータ比較（[台帳06](vm-ledger/06-allocator-baseline.md)、実測(host)）:

- 最小プールの大きさで、tlsf と estalloc の差はほぼ 1 KiB 未満。どちらが小さいかはトレースによって入れ替わる。naive はすべてのトレースで明確に悪い。
- estalloc の探索は全トレースで最大 3 段で、線形の fallback には一度も落ちなかった。
- estalloc の `est_calloc` には乗算オーバーフローの検査が無い。サイズ上限の検査は `assert` 頼み。
- 「160 KiB プールの最大空きブロック」は、今日の taffy の確保可否とは別の量。ゲストは専用プールを持たず、共有ヒープから確保している。

## 3. 実機で測る手順（ユーザーが実行する）

実機はプローブ入りのファームに書き換わる。このファームで変わるのは、USB の `A`〜`F` と `VMPROBE` のログ行が加わることだけ。

**`build_vm_probe/cardputer_pocketjs.bin` は 04:29 のもので、レビューの修正より前の古いファーム。そのまま書き込まないこと。** 下の `flash` コマンドがビルドし直してから書き込む。

```powershell
. 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1'
cd C:\devs\m5stack\cardputer-adv-pocketjs-vm
# -D SDKCONFIG は省かない。省くとプローブが共有の ./sdkconfig に入り、他の build_* もすべてプローブ入りになる
idf.py -B build_vm_probe -D SDKCONFIG=build_vm_probe/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.vmprobe.defaults" -p COM3 flash
python tools\vm_l0_capture.py --port COM3                    # A〜F を各15秒。結果は .cache\vm\l0.jsonl
# C スタックの値だけを取るとき: 実機をリセットしてから
python tools\vm_l0_capture.py --port COM3 --workload B --out .cache\vm\l0-B.jsonl
```

所要時間（推定）は、差分ビルドが数分、書き込みが約 1 分、採取が約 2 分。ばらつきを見るなら採取を 3 回繰り返す（`--append`）。終わったら、普段の手順（本体ツリーの `build_api`）で通常のファームに戻す。

見る値:

| 行 / ワークロード | 見るもの | 何の基準になるか |
| --- | --- | --- |
| `VMPROBE STATIC` | `sizeof_jsvalue`（8 のはず）、`sizeof_stackframe`、`sizeof_varref`、`opt` | 仕様 §11 の仮置き（JSValue 8 B、フレーム 32〜64 B）の確認 |
| A `sync_loop` | `frame` の中央値と最大値 | 素のディスパッチの費用。L2 で確認地点を足したときの性能低下の比較元 |
| B `deep_recursion` | 早期終了の警告が出ないこと、`stack_hw`、`frame` | C スタック。**到達した段数はログに出ない**（未計測のまま） |
| C `closures` | `frame`、`js_used` | JSVarRef の経路 |
| D `promise_chain` / F `async_generator` | `jobs` の中央値と最大値、`qpeak_max`、`frame` | ジョブ件数と最大キュー長。L1 の件数予算の元 |
| E `io_wait` | `lat` の最小値・中央値・最大値 | 完了通知からの遅延。L1 で減らす対象 |
| 全ワークロード | `heap_free`、`heap_largest`、`js_used/js_limit` | 最大空きブロックを taffy の段（29,648 / 59,296 B）と比べる。これが実機での本当の値 |

読むときの注意:

- **`frame` はフレーム全体の時間。** 対象の `pocketjs_ui_turn` は、`frame()` の実行とジョブの drain（`pocketjs_guest_frame`）に加えて、UI コアの tick と draw も含む（`.cache/pocketjs/.../pocketjs_ui_qjs/src/ui_qjs.c:858-862`、本報告の作成時に確認）。
- **`stack_hw` は起動以来の最小値。** 測定中に限った値ではなく、プローブ自身の flush（384 B のコピーと printf）も含む。ホーム画面の描画の方が深ければ、JS 側の値は隠れる。
- **`lat` はハンドラが走るまでの時間ではない。** 測っているのは、完了が記録されてから resolve/reject を呼ぶまで。`.then` のハンドラが実際に走るのは、その後の drain のとき。
- **L0 の完了条件のうち、実機側は次の 3 点が欠けている。** 実機は窓ごとの最小値・中央値・最大値しか出さず、**p95 が無い**。UI・音声・通信との**競合条件を固定していない**。窓の中央値は最大 48 標本の近似値である。

## 4. 台帳の要点と未確認事項

| 台帳 | 要点 |
| --- | --- |
| [01 呼び出し経路](vm-ledger/01-call-paths.md) | 通常の JS 呼び出しは C の再帰になる。フレームを解放する経路（`done` か `done_generator` か）は、コンパイル時の `b->func_kind` で決まる。C から JS への再入は ToPrimitive、getter/setter、`prepareStackTrace` を通じてどこからでも起きるので、有限のリストにはならない。**opcode の種類ではなく、実行時のネイティブ深さで判定する必要がある。** |
| [02 フレームのポインタ](vm-ledger/02-frame-pointers.md) | フレームを指すポインタは 3 系統ある: `rt->current_stack_frame`、呼び出し先の `prev_frame`、切り離されていない `JSVarRef.stack_frame`。`JS_CallInternal` の C ローカル変数（`sp`、`pc`、`var_buf` など）も、フレームへの 2 組目の生ポインタになっている。**GC はフレームを走査しない**（値は参照カウントだけで生きている）。 |
| [03 ジョブと割り込み](vm-ledger/03-jobs-interrupts.md) | 1 ティックの順序は「resolve でキューに積む → `frame()` → drain」。`frame()` が例外を投げたティックは drain しない。`app_session` の割り込みハンドラが上書きするので、guest.c 側の epoch 方式の割り込みは死んでいる。**割り込まれた `await` の Promise は、永久に pending のまま残る。** |
| [04 opcode の確認地点](vm-ledger/04-opcode-checkpoints.md) | `js_poll_interrupts` は 14 箇所（opcode 内は goto 系と if 系の 7 箇所）。call、return、yield、catch、反復系の opcode には確認が 1 つも無い。正規表現も割り込みを投げる（`js_regexp_exec`）。 |
| [05 確保](vm-ledger/05-allocation.md) | `guest_realloc` は常に malloc+コピー+free で、slack は常に 0 に見える。`JSFunctionBytecode` は自分自身を指すポインタを持つ。`js_array_buffer_update_typed_arrays` は、移動したあとで参照を貼り直す既存の前例になる。 |
| [06 アロケータ基準](vm-ledger/06-allocator-baseline.md) | §2 の通り。 |

**残っている未確認事項**（詳細は各台帳の「未確認」節）:

- `js_check_stack_overflow` の閾値の意味。
- 反復系ヘルパー（`js_for_of_next` など）と `js_function_apply` / `JS_EvalObject` / `JS_IteratorClose` の本体に、割り込みの確認があるかどうか。
- async generator が、割り込まれたときに Promise を reject せず飲み込むかどうか。
- `sizeof(max_align_t)` の実機での値。
- 144 KiB という数字の出どころ。
- 実機で本当に GC が 1 回も走らないか（ログでの確認）。
- estalloc の `assert` がこのビルドで有効かどうか。

**本報告の作成時に解消した項目:** 03 の「`pocketjs_ui_turn` の実装が見当たらない」。本体は `pocketjs_ui_qjs/src/ui_qjs.c:818` にあり、`pocketjs_guest_frame` を呼んでから UI の tick と draw を行う。03 の fact 62 の順序は正しい。台帳自体はまだ直していない。

**台帳の食い違い4件**（04 の箇所数、06 のトレースの内訳、`vmprobe.h` のコメント、vmtest の README）は、コミット前に直した。

**行番号の基準:** 台帳の file:line は `015fc62`（プローブの差分を当てた取り込み版）の `quickjs.c` を指す。

## 5. L2 設計への含意（async/generator の機構を一般化できるか）

### 事実（台帳の追記時に `quickjs.c` と照合済み）

既にあるもの:

- `async_func_init` は、引数・変数・オペランドスタック・`var_refs` を 1 つのヒープブロックに置き、`JSStackFrame` を `JSAsyncFunctionState` に埋め込む。
- 再開の入口は `sf` からインタプリタのレジスタを組み直し、`cur_sp = NULL` を「実行中」の印にする。
- 中断は 6 つの opcode が行う。いずれも `done_generator` を経て `cur_pc` / `cur_sp` を保存する。
- 解放時には `close_var_refs` が走るので、`JSVarRef.pvalue` は宙に浮かない。
- GC は、所有する GC オブジェクトを経由してだけ辿られる。

一般化を妨げるもの:

1. **C の再帰が残る。** `async_func_resume` は現在の C スタックの上で `JS_CallInternal` を呼び、再開したフレームの中の JS→JS 呼び出しも C で再帰する。中断できるのは最も内側のフレームだけ。
2. **解放の経路がコンパイル時に決まる。** `done:` と `done_generator` のどちらを通るかは `b->func_kind` で決まる。通常の関数を中断すると、`done:` を通ってフレームが破棄される。
3. **保存される状態が足りない。** `JSStackFrame` は `this`、`new_target`、`flags`、`caller_ctx` を持たない。再開時の `new_target` は `JS_UNDEFINED` 固定なので、constructor には使えない。
4. **`cur_func` の持ち方が違う。** 通常のフレームは借用し、async のフレームは dup で所有する。
5. **`prev_frame` の連鎖が古くなる。** 再開時に繋ぎ直すのは 1 フレームだけ。中断した連鎖の底は、既に消えた C スタック上の `sf_s` を指す。しかも backtrace などの walker は、連鎖が 1 本であることを前提にしている。
6. **戻り値の受け渡しが driver 固有。** `FUNC_RET_*` と `cur_sp[-1]` による取り決めは、generator や Promise の driver ごとに違う。
7. **割り込みは捕捉できない例外として実装されている。** 仕様 §3-6 の「中断は例外ではない」とは相容れない。
8. **GC の対象にならない。** GC オブジェクトとして登録されていない VM スタックはマークされない。値が壊れることは無いが、循環参照は漏れる。
9. **生ポインタの問題。** `JS_CallInternal` の C ローカル変数と `sf_s` 自体が、フレームを指す生ポインタの元になっている。L2a で移すべきものは alloca のブロックだけではなく、`JSStackFrame` 本体も含む。

### 判断と推定（Fable の分析。検証していない）

- async の機構は、**フレームの表現と再開の入口の雛形としては使える。** `cur_sp == NULL`、解放時の close、C フレームを跨いで中断しない、という不変条件はそのまま守るべき。
- **呼び出し構造の雛形にはならない。** 仕様 §7 の完了条件「C スタックが深さに比例しない」を満たすには、call 系 opcode が VM フレームを積んで同じループを続け、`done:` が VM フレームを pop する構造が要る。これは新しい作業になる（L2b）。
- **L2a（セグメント）だけでは C スタックは減らない。** 仕様 §12 の「L2a だけで任意中断を提供しない」と整合する。
- 流用できるのは `async_func_*` の約 150 行と、再開の入口の約 20 行（**推定、未計測**）。

### 本報告の見立て

L2 の価値は、長いジョブの中断のほかに、20 KiB スタックでの再帰の上限（ホストで 29 段、実機で推定約 50 段）を外すことにもある。後者の効き目は、実機の段数を測るまで評価できない。

## 6. L1 に着手する前に決めること

1. **L0 を完了扱いにする条件。** 実機の値が 1 回取れたら完了にするか、p95 と競合条件（UI・音声・Wi-Fi）の固定まで求めるか。後者を選ぶなら、プローブの出力に p95 を足す作業が要る。
2. **L1 の予算と上限の値**（仕様 §11「L0 完了時に記録」）。ジョブの件数予算、時間予算、応答遅延の上限、RAM 増分の上限を決める。UI（30 fps = 33 ms）と音声の要件から、ユーザーが決める値。
3. **割り込みハンドラを 1 本にする。** 今は `app_session` の 250 ms の壁時計 deadline だけが生きていて、guest.c の epoch 方式は死んでいる。L1 の予算判定をどちらに載せるか決める。
4. **drain の制御点。** `drain_jobs` は取り込み済みの guest.c にあるので改変できる。一方、`frame()` → drain → UI の順序を決めているのは、取り込んでいない `pocketjs_ui_qjs`（共有の `.cache`、読み取り専用）。L1 で順序を変えるなら、`pocketjs_ui_qjs` も取り込む必要がある。
5. **runtime の所有タスク。** 初期構成のまま `ui_task` にするか、専用タスクにするか。専用タスクにすれば、そのスタック分だけ DRAM が減る。
6. **L0 で見つかった既存の不具合の扱い**（VM の改造とは独立していて、`main` にも効く）:
   - GC 閾値が上限を超えている件。`JS_SetGCThreshold` を 1 行呼べば済む。
   - OOM 時の use-after-free。
   - 割り込まれた `await` の Promise が pending のまま残る件。

   それぞれ、`main` 側で直すか、L1 で扱うか、記録だけにとどめるかを決める。
7. **コミットとタグ。** L0 の作業は `vm/p0-foundation` にコミットと push を済ませた。`vm/main` へ `--no-ff` で戻す時期を決める。`vm-L0` のタグは、実機の値が揃ってから打つのが仕様どおり。
