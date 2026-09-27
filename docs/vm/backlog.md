# VM 作業状態（backlog）

未完了の TODO・未検証の項目だけを置く。決定・事実は各設計文書・台帳に残す（ここに複製しない）。終わった項目は消す。

## L2（移動しない VM スタックと中断・再開）

出典: `docs/vm/vm-L2-design.md`。状態は 2026-09-23 時点。

未完了の項目は無い（2026-09-23）。L2c は実機統合まで終わり、YIELD は既定 y を経て 2026-09-27 にビルドオプションではなくなった（下の「ビルドオプションの整理」）。TCO と FAIR は採らなかった実験として同日に削除した。経緯と実測は vm-L2-results.md §8、設計は vm-L2-design.md §11。


## L3 / L4（移動可能スタックとコンパクション）

出典: [vm-L3-design.md](vm-L3-design.md)、[vm-L3-results.md](vm-L3-results.md)、台帳
[09-relocation-entries.md](vm-ledger/09-relocation-entries.md)。開発は `vm/l3a-refs`。状態は 2026-09-25 時点。

**閉じた（2026-09-25、G12 の結果による）。** ホストでは tlsf モデルで比べたすべての方式が動かさない
現行設計に負け（design §10〜§12）、実機の G12（results §15）では:

- 出荷アプリ（hello・imucal・pet・companion、負荷の前後2巡）と Kasane デモで、確保の失敗は 0 件。
- わざと OOM にする負荷では「空き総量 ≥ 要求 > 最大空きブロック」が 168 件出た — **backlog が決めて
  いた字義どおりの基準は満たさない**。決めてあった次の手順（要求が何かを見て §10.3 の退避を検討）を
  取ると、要求は JS の `ArrayBuffer`、分断しているのは JS のオブジェクトで、**セグメントを退かして救えた
  失敗は 0 件**。負の対照（セグメントが分断するヒープ）では 590 件すべてを検出した。

L3/L4 が動かせるのはセグメントだけなので、退避（旧 #5、D58）も含めて採らない。L3a と L4a のコードは
既定 n のまま残す（台帳 09 と毒化は、将来フレームを動かす必要が出たときの出発点）。**再開する条件**:
出荷アプリで G12 の `segfix=1` が出ること（`CONFIG_POCKET_VM_OOMPROBE`、`tools/vmtest/device_g12.py`）。

旧 #2（D8: 駐機中のネイティブ確保が飛地を増やすか）・#3（出荷アプリは駐機しない）・#4（D55 の緩和）は、
動かさない方針では問いにならないので閉じた。

### 完了（参考）

- G12（実機の断片化計測）: results §15。計装は既定 n の `CONFIG_POCKET_VM_OOMPROBE`。

- L3a: 台帳 14 種、差分表による型付き補正、3層の毒化、世代・pin・4種の失敗。コーパス 74 件一致、
  Test262 標準集合と部分集合 7,036 ファイルで対照と同一、負の対照 3 種を検出。実機で 34 回移動・最悪
  235 µs・契約一致・smoke 20 周・予算内（results §3〜§9）。
- L4a と D6 をホストで測り不採用（results §13〜§14、design D57・D59）。
- L3 の作業中に見つけた、確保失敗時のコンパイル経路の不具合群は `vm/oom-truncated-bytecode` で修正し、
  ホストと実機（smoke・`memlog --check`）を通して `vm/main` 経由で取り込んだ（2026-09-25、
  [oom-parse-safety.md](oom-parse-safety.md) §9・§9.1）。

## F 系列（起動床の削減、VM の段とは独立）

出典: [builtin-floor-plan.md](builtin-floor-plan.md)。状態は 2026-09-26 時点。F0（計測と計画）・**F1（ROM atom）・F2（組み込みの索引の遅延）は済み、既定 y**:
アプリごとの `js=` が F 系列の前から実測(device)で 35.6〜38.9 KB 減（plan §12・§13）。計測の穴は §14 で埋め、外れの単価は §15（F2-5）で 2〜3 倍下げた。**F3a（atom の hash 表）・F3b（型付き配列の遅延、既定 y）で
さらに 7.3〜7.7 KB 減**（plan §16・§17、実測(device)）。

| # | 項目 | 出典 | 状態 |
| --- | --- | --- | --- |
| F1-5 | 脱出文字列の回数をアプリごとに数える | plan §14.2 | 済: 起動時 70 回前後、pet のみ 1,960 回で新規 83（キャッシュが 96% を吸う） |
| F1-6 | JS ターンの速さを同一バイナリ内で比べる | plan §14.3 | 済: 実行時の ROM 検索（文字列→キー）は ROM に無い名前より遅くない |
| F2-3 | 外れ 1 回の単価を同一バイナリ内で実測 | plan §14.3 | 済: 8.5 µs（`Object.prototype`）〜30 µs（`Math`）。推定の 10〜30 倍 |
| F2-4 | 全展開の誘発元の計装 | plan §14.2 | 済: 出荷アプリとデモすべてで 0 回 |
| F0-a | 床のうちファーム側（`pocket.*` の面など） | plan §14.1・§16.1 | 済: 約 6.2 KB（`pocket` の土台 3.2 KB、他は各 0.3 KB 以下）。作成直後とホスト計算の差 4.8 KB は `MALLOC_OVERHEAD`（8 B/確保）と `js_std` の helper |
| F0-c | 160 KiB の上限の数え方: QuickJS は確保ごとに 8 B 足すが tlsf の実ヘッダは 4 B（床だけで約 1.8 KB 多く数え、アプリのブロック数に比例） | plan §16.1 | 未着手（上限の意味を変える判断が要る） |
| F3c | Map/Set（1.8 KB）・DOMException（1.4 KB）・WeakRef（0.7 KB）を F3b と同じ仕組みで遅延に | plan §18 | 済（2026-09-27）: 実機で各アプリ `js=` さらに −4.5〜4.9 KB（hello 38,248）、smoke・settings・memlog・stress 通過 |
| F3d | ネイティブが `Uint8Array` を先に作る経路の実機確認（`pocket.fs` の read を使うアプリで） | plan §17.3・`apps/stress/README.md` | 済（2026-09-26）: STRESS TEST が JS より先に `pocket.fs` で読んだ `Uint8Array` の prototype・constructor がグローバルと一致（`STRESS_NATIVE ok`、実機） |
| F0-d | Kasane の使い勝手（STRESS TEST で踏んだ）: `replace` の背景なし・コマンド数超過・半径超過がすべて理由なしの `INVALID_ARGUMENT`、キャッシュのコマンド 48 がビュー合計、インスタンスがシーンの 80 を使う、既定の clip が作成時の bounds、半径 8 まで・円や線が無い | `apps/stress/README.md` | Kasane の線（`feature/kasane`）へ渡す |
| F0-b | Kasane デモの外れ回数 | plan §14.2 | 済: 2.4 回/フレーム（最多は imucal の 8.7） |
| F2-5 | 外れを安くする | plan §15 | 済: 比べ方を直して外れ 1 回 4.0 µs（`Object.prototype`）〜10.5 µs（`Math`）、2〜3 倍。メモリ 0、コード +132 B |
| F2-6 | 外れを定数時間に（記録ごとの RAM 索引 約 0.8 KB、または生成器が flash に焼く索引） | plan §15.3 | 見送り（imucal で 0.1 ms/フレーム未満、推定） |

## S 系列（アプリの常駐中断、2026-09-27〜）

出典: [app-suspend-design.md](app-suspend-design.md)。

| # | 項目 | 状態 |
| --- | --- | --- |
| S-D | 設計の決めること 4 つ | 決定（2026-09-27、設計書 §8）: `resume` 登録アプリだけ／中断アプリの音声は一時停止・music オーバーレイは鳴り続ける／中断中はオーバーレイを起動しない／同じ行で再開・一時停止アイコン |
| S0〜S3 | ゲストの休眠、フック、main.c の経路・一時停止の印、各面の中断・再開 | 実装・実機で確認（2026-09-27、設計書 §11）。無線・SD・マイク・I/O を使っている最中の中断の実機の筋書きは未 |
| S4 | 出荷アプリの参加 | 済（2026-09-27、設計書 §11.1）: IMU CALIBRATION・POCKET PET・PET COMPANION。HELLO と STRESS は終了のまま。`tools/test_app_resume.py` |
| S4a | `tools/test_kasane_imucal.c` が `vm/main` の時点で落ちている（模擬の Kasane が `BUSY`、IMU が無い経路から）。CLAUDE.md の一覧外で誰も回していなかった | 未着手 |
| S5 | music の再生をオーバーレイのセッションから切り離す | 未着手 |

## R 系列（JS のターンの速さ、2026-09-27〜）

出典: STRESS（`apps/stress`）のホストのプロファイル（`tools/vmtest/prof/`）。Kasane の取り込み後、実機の 1 フレームは
描画 6.5 ms・JS のターン 8 ms（LV3 は 11.4 ms）で、ターンが最大の区間になった。

| # | 項目 | 出典 | 状態 |
| --- | --- | --- | --- |
| R1 | 上限の手前での GC の空回り（天井を超えるとオブジェクトごとに GC） | [gc-cap-backoff.md](gc-cap-backoff.md) | 済（2026-09-27）: 実機 STRESS LV3 の turn_ms 11.41 → 8.04 ms、fps 26.4 → 29.4。確保失敗は増えず |
| R2 | 普段のターン（LV1/LV2）の内訳 | [allocator-cost.md](allocator-cost.md) | 計測済み（2026-09-27、実機）: **アロケータが JS のターンの 20.6%**（7.63 ms 中 1.56 ms、1 フレーム約 340 回、malloc 1 回約 1,270 サイクル） |
| R3 | 小さいブロックの再利用とブロック長の直読み | [r3-small-block-cache.md](r3-small-block-cache.md) | 実装済み・**既定 y**（2026-09-27、R3a 解決後）: 実機でアロケータ 1.57 → 0.92 ms、LV1 のターン 7.62 → 7.08 ms |
| R3a | キャッシュ有りで STRESS LV3 に Kasane の `BUSY`（3/3 回、無しでは 0/2）。frame の外の確保失敗の約 2 フレーム後 | r3-small-block-cache.md §6.2 | **解決**: キャッシュでなく `app_tick` の穴。中断された frame() を継続ターンが終えて提出したまま次の frame() を呼んでいた。修正後 実機 3/3 PASS |
| R4 | ターンのサイクルの内訳（性能カウンタ、`CONFIG_POCKET_VM_TURNPERF`） | [turn-cpi.md](turn-cpi.md) | 済（2026-09-27、実機）: **IPC 0.17、サイクルの 70% が flash キャッシュのミス待ち**。flash を QIO にして（既定）LV1 の 1 フレームの JS 10.78 → 7.25 ms、描画 6.61 → 6.16 ms。旧 `turn_ms` 7.1 ms は予算で切られた通常＋継続の平均で、実際の合計は 10.8 ms だった |
| R5 | QIO の後も 57% がミス待ち。ホットな関数の連続配置・一部 IRAM・flash 120 MHz | [r5-icache.md](r5-icache.md) | 模型で評価済み（2026-09-27、QEMU＋キャッシュ模型、推定）: 1 ターンが触るのは 45.5 KiB でキャッシュ超え、衝突は 15% 程度。**並べ替えは実リンクで −0.6%、不採用**。IRAM 8 KiB は**実機で 1 フレームの JS 7.00 → 5.93 ms（−15%）、空き −8.7 KiB。見送り**（払う DRAM に対して戻りが軽い、ユーザーの判断）。候補は `tools/r5sim/candidates/` |
| R4a | `pocket.fs` の open が OOM すると File のスロットが戻らない（`pocket_api_settled` の Promise 生成失敗で File が消え、ファイナライザ無し）。STRESS LV3 60 秒で 474 回 `two files are already open` | turn-cpi.md §4.1 | **解決**（2026-09-27）: `file_settled()` が失敗時にスロットを閉じ、`pocket_api_settled` は失敗を例外で返す。実機の負荷 `^`（OOMPROBE）で修正なし 5/5 漏れ → 修正あり 5/5 OK |
| R4d | 解決した File を受け取る継続が OOM で登録できないと、File が参照を失いスロットが戻らない（ファイナライザ無しの設計） | turn-cpi.md §4.2 | **解決**（2026-09-27）: ファイナライザが印を付け、次の pump が破棄として閉じる。実機の負荷 `^` 段 1 で無し 3/3 漏れ → 有り 5/5 OK |
| R4b | F2 の遅延の索引が、初回の読みの OOM で名前を失う（`fs.open` が "not a function" に、ホストでは `Object.fromEntries`）。`pocket.*` の名前空間も対象 | turn-cpi.md §4.1 | **解決**（2026-09-27）: 失敗時に「済み」の印を戻す。コーパス `lazy_touch_oom.js`（陰性対照で 18 中 13 が消える） |
| R4c | `oom_sweep.sh -n 2500 lazy_builtins.js` の 1,075 番で終了時に `gc_obj_list` が空でない assert（修正前の quickjs.c でも同じ） | turn-cpi.md §4.1 | 未着手 |

## ビルドオプションの整理（2026-09-27）

VM 系の Kconfig を、出荷経路の切り替えではなく診断だけに絞った。**`5db834f` が外す前の最後の木**で、比較が要るときはそこを別の作業ツリーに出す。

- **固定した（常に入る）**: `SCHED` `SEGFRAMES` `FLATCALLS` `YIELD` `LAZY_INPUTS` `STRIP_FN_SOURCE` `ROM_ATOMS` `LAZY_BUILTINS` `LAZY_INTRINSICS` `BLOCK_CACHE`。`#ifdef` は `unifdef` で機械的に畳んだ。F1〜F3 の3つだけは `quickjs.c` 内部のマクロ `POCKET_VM_*` として残り、ROM アトム表の生成器（`tools/vmtest/floor/gen_rom_atoms.sh`、`-DPOCKET_VM_GEN_ROM_ATOMS`）だけが切る — 表は熱心な `JS_NewContext` が作るものを写す必要があるため。`--check` で表がバイト一致することを確かめた。
- **削除した（採らなかった実験）**: `TCO` `FAIR` `CCOUNT`。
- **削除した（決着した計測）**: `ALLOCPROBE`（R2）、`FLOORPROBE`（F 系列）、`CALLBENCH`、`L1_CLOCKBENCH`。R3a の計装（`R3A_*`）は ALLOCPROBE の下にあったので一緒に消えた。
- **残した（診断）**: `SELFTEST` `KSN_DEVICE_PROBE` `PROBE` `RELOC` `OOMPROBE`、および `POCKET_UI_TASK_CORE`。
- **ホスト側**: `tools/vmtest/build.sh` の変種は `asan` / `o2`（と `-reloc`）だけになり、`expected-fair/` `expected-keepsrc/`、`sdkconfig.vm*.defaults` のうち消したオプション用の8本、TCO・callbench・async_audit・lazyfloor/lazyprobe・`device_floor.py` を削除した。
- **確認（ホストと実機ビルド、2026-09-27）**: 出荷構成のファームは `vm/main` の既定ビルドと同じ 2,043,744 B、静的 DIRAM は ±0、flash は −12 B（assert の行番号）。残した診断5つを全部有効にしたビルドも通る。コーパス 79/79（asan・o2・`--force-yield`・`--budget-jobs 3`・`--vm-seg-size 88`・`asan-reloc`）、中断鎖の寿命 900/900、陰性対照 F1 4/4・F2 7/7・F3 4/4・R1 2/2、STRESS・テキスト入力・input・memory のホスト検査は PASS。実機: `smoke_device.py` 20 サイクル SMOKE_OK、`stress_app.py` 3/3 PASS（BUSY 0、LV1〜3 は 29.2〜29.7 fps、LV1 のターン 7.12〜7.14 ms）。
- **この作業の前から落ちていたもの**（2026-09-27 に `vm/fix-host-checks` で直した。ホストだけの変更で、ファームのソースは触っていない）:
  - `budget_probe.sh` の `deep_async_recursion`: F3c（`3ed6eff`、bisect で特定）が床を下げて降下が深くなり（深さ 81 → 158）、降下の後のトップレベルの `"#info max_depth=" + depth` が文字列を確保できず `null` を投げてジョブを流す前に終わっていた。D38 の主張（同期 try に届かない、予算は答えない、ヒープで終わる）は変わっていない。その行を後で走る async 関数の中へ移した。
  - `oom_canary_probe.sh`: 先頭行の `--fail-alloc` しか読まず、F1〜F3 で確保が減った後は番号が走行の終わりより先になって何も注入していなかった。`run.sh` と同じく `// vmrun-rom-lb-li-flags:` を足して読む。
  - **`corpus/oom_callsite_double_free.js` は F1 以降、何も検査せずに通っていた**（`--fail-alloc 1455` に対して走行全体で確保 527 回）。再固定の行がこのファイルだけ抜けていた。失敗の分岐に計装したコピーで掃いて `671`（添字 0、675 が添字 1 で、旧 1458/1462 と同じ間隔）を見つけ、ヘッダに足した後も番号が動かないこと、上流の修正を戻すと ASan で落ちることを確かめた。上の canary の検査（注入するファイルは必ず発火する）が本来これを捕まえるはずだった。
  - `tools/test_session_dispatch.py`: `app_tick` に後から入った呼び出し（メモリ圧、presenter、P0 計測など）の代役が無かった。足したうえで、R3a の場合（継続が中断中の `frame()` を終えて提出したら、次の `frame()` の前に present する）と presenter の失敗の2件を加えた。R3a の修正を外すと落ちることも確かめた。
  - `build_lessons_test.sh` / `build_power_test.sh`: 音の observer の代役と `esp_err.h` の include が無かった。`pocket_clock.c` が Kasane の source を登録するようになった分は、呼ばれたら止まる代役で済ませた（この検査は `wall()` だけを見る）。
  - ビルドオプションの整理の取りこぼし: `pocket_api.c` などの `vm_wake_post()` がもう条件付きでないので、ホストの `vm_wake.h` の代役に空の定義を置いた。`quickjs-vm.c` をリンクしていなかった `build_pocket_capture_test.sh` / `build_pocket_random_test.sh` / `build_class_ids.sh` / `build_js_ledger.sh` / `build_lazy_test.sh` に足した（あわせて `/tmp` の共有キャッシュがヘッダの変更で作り直されるようにした）。`lifecycle.sh` は無印の `asan` に消毒器のフラグを付けていなかった。

## 確保失敗時のコンパイル経路（VM の段とは独立）

未完了の項目は無い（2026-09-25）。ホストの最終検証（[oom-parse-safety.md](oom-parse-safety.md) §9）と
実機の smoke・`memlog --check`（§9.1）を通して `vm/main` へ戻した。修正前の assert や `exit(1)` が実機で
再起動を起こしていたかは推論のまま（smoke は OOM を起こさない）。

## L2 で完了済みの項目（参考）

- flat復帰時の引数等の遅延復元を既定yへ採用。追加Test262 19,307判定が即時復元と失敗詳細まで一致、診断なしhostの68件・ASan・成長36比較・G1/D10、実機smoke20周・故障回復6種・メモリ予算を確認。静的DIRAM/Cフレーム不変、Flash Code +132B。即時復元への設定fallbackを維持（旧項目13、results §3.6〜3.7）。
- 同一バイナリ内のflat/recur比較を実装し実機2回/256標本で測定。同期小関数群ではflatが3.2〜5.4%遅く、Cスタック増加は0B対304B/段。速度改善とは主張せず、スタック制約の解消とのトレードオフとして記録（旧項目12、results §3.5）。
- セグメントサイズをhost6方針×6負荷、実機6方針×7負荷×2回で再検証。保持量と確保コストから512/4096B維持を確定（旧項目2、results §5.5〜5.8）。採取境界の欠落23条件は元を残して再採取し、最終84run/225windowを検証。
- TCO実験実装はhost・実機の深さ/容量、GC/終了/破棄、fallback予算・確保失敗後の所有権と回復を検証。既定挙動維持のためnで確定（旧項目10、vm-tco-design.md §6〜10）。速度改善の主張は含まない。
- G1/G5/G6 判定機構は作成済み（design §1.2）。
- 継続ターンのメモリ採取を再監査し、周期リセットと継続中のwindow出力を修正。実機F/base 2回で継続採取87/86回を確認（旧L1範囲外項目4、results §4.14）。低頻度標本であり真のピーク保証ではない。
- async再帰のflat/recur終了条件を現行ソースで再検証し、既存の変種別期待値とOOM計測が一致（旧項目4、results §4.12）。
- 固定Test262全体53,582本を走査し、async/await・再帰関連8,332ファイルをflat/recurで比較。判定と失敗詳細の差0（旧項目5、results §4.13）。共通失敗・skipは残り、任意プログラムや将来revisionまでの互換性証明ではない。
- HWMはUIタスク生涯値と確認し、現在のCフレーム位置を測る口を追加。flat/recurの実機各2回で0B/段対288B/段を確認（旧項目9、results §4.11）。UIスタック容量は未変更。
- 内部余白と外部断片化を分けた現行6トレース×4方式のhost比較を実施（旧項目1、台帳07 §7）。23完走・1容量不足。実機の断片化や最適セグメントサイズの検証を代替しない。
- H14の中断SEG容量・非SEGフレーム数の計測口を実装し、7起点の検査鎖でhost/device実測済み（results §4.9）。一般アプリの分布や断片化の検証とは別。
- 組込み5種のjob経路とasync-from-syncのnative境界を監査し、追加2コーパスを毎中断GC・FAIRで検証済み（旧項目6、results §4.10）。外部埋込み側の独自jobはこの監査に含まない。
- D1〜D43 は design §13 の決定表で「決定済み」と明記されたもの以外、上表に開いたものだけが残る（D6 は明示的に未決のまま design 側に残置）。
- L2c の関所とガード（旧「段1・段2」）は着地済み（results §4.2）。
- `JSAsyncFunctionData`は実機の`_Static_assert`とELF型情報で104Bを確認（旧項目3、results §4.4）。
- L2cのasync/async-generator所有床とD36保留job、GC・Terminate/Discardは実装済み（旧項目7、results §4.5）。実機統合が済むまでyieldは既定off。

## L1 の範囲外として残った決定と、VM とは独立の不具合

出典: `docs/vm/quickjs-freertos-vm-spec.md`、`vm-L0-report.md`、`vm-L1-design.md`、`vm-L1-report.md`
（時計コスト・コア移行の実測は §8.7/§8.8 に、スケジューラ定数の調律は §10 に統合済み）、
`task-allocation-facade.md`。状態は 2026-09-15 時点。L1 自体は `vm-L1` タグで
完了・マージ済み（`vm-branching.md`）。ここに残るのは L1 の範囲外として送られた決定と、
L1 とは独立に見つかった既存の不具合。

| # | 項目 | 出典 | 状態 |
| --- | --- | --- | --- |
| 1 | `deferred_buttons` が継続ターン中の 2 打鍵を 1 マスクに融合する。受け入れて文書化するか、キュー化する（離鍵フレームの対の作り直しを伴う）か、継続ターン中は最初の 1 つだけ保持するかが未決 | vm-L1-report.md §5.2-1 | 未決（実装は現状維持） |
| 2 | 仕様 §6「未処理ジョブもイベントも無い場合だけ待機する」を、専用タスク化なし（現状: `ui_task` のフレーム待ちを完了通知で早く抜けるだけ）で充足と認めるかどうか。専用タスク化は静的 DIRAM +24〜32 KiB 推定で、L0 の RAM 上限 +8 KiB を超える | vm-L1-report.md §4.1・§3 | 未決 |
| 3 | 公平モード（`CONFIG_POCKET_VM_FAIR`、既定 off）を既定にするかどうか。**2026-09-23に n で確定**: 実機で測り直したところ、L1 §9.5 の「F型で完了遅延が中央値3〜7倍改善」は今の負荷では再現しない（F は完了イベントを持たず、E は drain が予算に収まるため公平モードのコードに到達しない）。観測できるのは費用だけで、仕様 §12 の順序互換を崩す理由が無い。出荷アプリに「予算超過の drain ＋ 待っている完了」が現れたら測り直す | vm-L1-report.md §9.5, vm-L2-results.md §9 | 完了（n で確定） |
| 5 | GC 閾値（初期値 256 KiB）がゲストの上限（160 KiB）より大きく、循環参照のゴミが回収されなかった。`quickjs.c` の比較時キャップ（上限−上限/32）と `guest.c` の初期閾値（上限/2）で修正。回帰は `tools/vmtest/corpus/gc_threshold_{device,near_limit}.js`。生存量が上限の31/32を超えると毎オブジェクト生成でGCする点は未計測（実機） | vm-L0-report.md §2、結果は vm-L2-results.md §4.19 | 完了（host・実機smoke/memlog/benchで確認） |
| 6 | 上流 quickjs-ng の use-after-free: バックトレース組み立て中に確保が失敗する経路。上流 e1c1e416 を移植して修正。同じ関数の CallSite 二重解放（上流 c846cb13）も移植。回帰は `oom_creep_backtrace.js`・`oom_callsite_double_free.js`（再現元の `known/oom_backtrace_uaf.js` も asan で UAF なし） | vm-L0-report.md §2、結果は vm-L2-results.md §4.21 | 完了 |
| 7 | 中断された `await` の Promise が永久に pending のまま残る（割り込みが捕捉不能な例外として実装されているため）。L1 は予算切れではこれを作らないが、暴走ガードと `stop_interrupt` の経路では今も起きる | quickjs-freertos-vm-spec.md §7「非対応事項」、vm-ledger/03 事実39-41 | 未着手（L2 の中断設計と併せて扱う想定） |
| 8 | ゲストの確保ヘッダが 1 ブロックあたり 12B 余分（`max_align_t` で得たい 8B 整列は tlsf が4B境界しか返さないため得られていない）。候補: (a) ヘッダを `size_t` 1個(4B)にする、(b) ヘッダを無くし `heap_caps_get_allocated_size()` を使う（`js_free_rt` の費用増、未計測）。(a) を実機ビルドのみに適用（ホストは `max_align_t` のまま）。実測（2026-09-15、`memlog.py --port --check`、同じ `js=86571` のアプリ）: `app_free` 112,952→128,564B、`app_largest` 62,464→79,872B | vm-ledger/05-allocation.md §1、vm-L2-design.md §2.2 | 完了（(a)の後、#9 で (b) に移行。ヘッダ廃止、usable は `heap_caps_get_allocated_size()` で 1回 316〜454 ns、results §4.20） |
| 9 | `guest_realloc` が常に新規確保+コピー+解放だった。`heap_caps_realloc` に置き換え、usable size を tlsf の実長にした（slack が効くようになった）。実機: 起動後の空き +6.4〜7.0 KiB、realloc のコピー量 −32〜44%、frame max/p99 は不変。js 課金が実長になり +2〜3% | vm-ledger/05-allocation.md §1・§6、結果は vm-L2-results.md §4.20 | 完了 |
| 10 | `VM_JOB_FLOOR` と `VM_JOB_STRIDE` の相互作用は、floor(8) が stride(4) の倍数であることに隠れて D 型では検証できていない。floor を stride の倍数からずらす値に変える場合は再検証が要る（出荷値を変える提案ではないため現状は対象外）。`VM_JOB_BACKSTOP=64` が実際に effective な条件も未観測 | vm-L1-report.md §10 | 未着手（出荷値変更時のみ必要） |
| 11 | task-allocation-facade: FreeRTOS 接続層（タスク作成・登録・終了のライフサイクル）が未実装。既存ファーム・ビルド設定には未接続 | task-allocation-facade.md §実装範囲・§FreeRTOS接続層の契約 | 未着手 |
| 12 | task-allocation-facade: 「配置を保持した協調的な FP 輪番」は検討中の案のみで未決・未実装。所有者不在時の扱い、通知順序、輪番と優先度/音声期限の優先順位、チェックポイント間隔などが未検証 | task-allocation-facade.md §進行中の議論 | 検討中（採否未定） |
| 13 | 上流 quickjs-ng 7955cfd49e（中断中コルーチンが closure 経由でしか届かないと GC に回収される UAF）。JSStackFrame 48B・JSAsyncFunctionData 104B を保つ形（`coro_kind` をパディングに置き、所有者は container_of）で移植（`aa602e1`、本文の訂正は §4.23）。回帰は `coro_closure_gc.js`（修正前は6変種すべて UAF）と `coro_prologue_gc.js`（プロローグ単独のケースは修正前も鳴らず、誤った移植を UBSan で検出するガード。プロローグと本体 closure の混在循環は修正前に6変種すべて UAF）。修正後はいずれもクリーン。修正前後の差は `coro_closure_gc` だけ。実機は Flash +320B、DIRAM・空きは不変 | vm-L2-results.md §4.22、結果は §4.23 | 完了 |
| 14 | 上流の低優先修正（v0.14.0 以降、未移植）: 大きさ計算の整数オーバーフロー a65377157e / e2c45218f6 / e93cca6119 / 252209d99c / 610b90842d、循環 re-export のクラッシュ ef7a3a748b。該当コードはあるが、160 KiB 上限下ではほぼ届かないか、モジュール使用時だけ | vm-L2-results.md §4.22 | 記録のみ |
| 15 | 7955cfd49e 移植の関所で、修正前とバイト一致で落ちていた既存の失敗2件。どちらも VM の不具合ではなかった。`seg_oom_boundary` はテストが名目どおりの形になっていなかった。`new Array(1<<14).fill()` が1.5倍ずつ伸びて 162,500/163,840 B まで詰め、InternalError を作る余地が約1.3KBしか残らなかった。2回目は1回目の Error が catch 束縛に残る分だけさらに狭く、-keepsrc で `null` になった。`Array.apply` の1回確保（`memory_device.js` と同じ）に替え、余裕は約58KB。`tco_guards` は毎中断GCが深さに比例して、予算に対し2乗で伸びる（512K 2.2秒→1M 7.1秒→2M 31秒、7MiB既定で366秒完走・出力一致）。300秒の打ち切りは時間切れで、ハングではない | vm-L2-results.md §7 | 完了 |
| 16 | 関数ソースの複写（`js_strndup` で全関数の本文を保持）をやめる。`CONFIG_POCKET_VM_STRIP_FN_SOURCE`（既定 y）。toString は上流の「ソース無し」の形を返す。実機: hello −2,780 / imucal −4,740 / companion −2,576 / pet −4,124 B（ゲスト）、smoke 20 周 OK。Test262 部分集合の退行 0、toString ディレクトリで新たに 5 ファイル×2 モードが落ちる（計算名・private メソッド）。vmtest は `-keepsrc` 変種、`expected-keepsrc/`、`--fail-alloc` 番号をトレースで再配置 | [Kasane判断台帳](../kasane/decisions.md)、結果は vm-L2-results.md §6 | 完了 |
