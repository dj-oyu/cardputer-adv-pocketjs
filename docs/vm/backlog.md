# VM 作業状態（backlog）

未完了の TODO・未検証の項目だけを置く。決定・事実は各設計文書・台帳に残す（ここに複製しない）。終わった項目は消す。

## L2（移動しない VM スタックと中断・再開）

出典: `docs/vm/vm-L2-design.md`。状態は 2026-09-23 時点。

未完了の項目は無い（2026-09-23）。L2c は実機統合まで終わり、`CONFIG_POCKET_VM_YIELD` は既定 y。TCO と FAIR は互換性の判断として n のまま残している。経緯と実測は vm-L2-results.md §8、設計は vm-L2-design.md §11。


## L3（相対参照と移動可能スタック）

出典: [vm-L3-design.md](vm-L3-design.md)、台帳 [09-relocation-entries.md](vm-ledger/09-relocation-entries.md)。
開発は `vm/l3a-refs`。状態は 2026-09-23 時点。

| # | 項目 | 出典 | 状態 |
| --- | --- | --- | --- |
| 1 | **着手の関所（仕様 §12.2-3）が開いていない。** 「移動しないセグメントの分割・空き結合・再利用でメモリが足りるなら L3/L4 を実装しない」の判定は実機で未実施。ホストの4方式比較（台帳07 §7）は本人が代替にならないと断っている。L3a を `vm/main` へ戻す条件にこの実機計測を入れる | design §1.1、G12 | 未実施（L3a と並行） |
| 2 | G7〜G11 の関所（移動後の一致、クロージャ共有、旧番地検出、4種の失敗、Test262） | design §8 | 実施中 |
| 3 | **実機の呼び出し元は作ったが、一度も走らせていない。** `guest_continue_impl()` の再開直前、USB `&` で武装、停止時に `VM_RELOC` を出す。`tools/vmtest/device_reloc.py` が駆動するが**未実行**（COM3 が別ラインと共有）。実機で `VM_RELOC` を観測するのが次の一歩で、`max_us`（最悪の1移動）が停止時間の初めての実測になる | results §3、§3.1 | 未実施（書き込み待ち） |
| 4 | D55 の制限（床の下に外側の JS 活性が在ると移動を拒む）を緩められるかは未検討。`JS_SF_MAY_YIELD` が同じ入れ子を別の理由で扱っている | design §2.1 | 検討中（L3b） |
| 5 | L3b（誰がいつ動かすと決めるか）は未着手。L3a は「動かせること」までで、動かす理由は L4 が持つ | design §1、D54 | 未着手 |
| 6 | **L3 とは独立の不具合**: `--fail-alloc 1500` で `resolve_variables`（`quickjs.c:37723`）の `dbuf_put` が heap-buffer-overflow を起こす。確保失敗が `dbuf` の伸長で起きたときの後始末の経路で、**パース中**（`JS_Eval` → `js_create_function`）なのでフレームもセグメントも関与しない。L3a の作業中に `--fail-alloc` の掃引で見つけたが、**RELOC を1行もコンパイルしない素の `asan` ビルドで同一アドレスに再現する**ので L3 以前から在る。上流に同じ形（backlog #6 の backtrace UAF）の修正があるので、上流の確認から入る | 本セッションの掃引、`.cache/vmtest` | 未着手 |

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
| 16 | 関数ソースの複写（`js_strndup` で全関数の本文を保持）をやめる。`CONFIG_POCKET_VM_STRIP_FN_SOURCE`（既定 y）。toString は上流の「ソース無し」の形を返す。実機: hello −2,780 / imucal −4,740 / companion −2,576 / pet −4,124 B（ゲスト）、smoke 20 周 OK。Test262 部分集合の退行 0、toString ディレクトリで新たに 5 ファイル×2 モードが落ちる（計算名・private メソッド）。vmtest は `-keepsrc` 変種、`expected-keepsrc/`、`--fail-alloc` 番号をトレースで再配置 | [kasane-guest-memory.md](../kasane/kasane-guest-memory.md)、結果は vm-L2-results.md §6 | 完了 |
