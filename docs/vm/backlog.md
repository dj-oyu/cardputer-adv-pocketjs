# VM 作業状態（backlog）

未完了の TODO・未検証の項目だけを置く。決定・事実は各設計文書・台帳に残す（ここに複製しない）。終わった項目は消す。

## L2（移動しない VM スタックと中断・再開）

出典: `docs/vm/vm-L2-design.md`。状態は 2026-09-15 時点。

| # | 項目 | 出典 | 状態 |
| --- | --- | --- | --- |
| 1 | 完了条件6後半「アロケータ比較で内部余白・外部断片化を分ける」が未実施（台帳07 は確保履歴に対する内部余白しか出していない。外部断片化は未測定） | design §1.1 #6 | 未着手 |
| 2 | 標準セグメントサイズの512/4096B（D42）はフレーム使用量の実機分布から最終決定した値ではない（台帳07 のオブジェクト分布由来の初期値をD42で見直した後も） | design §7.2 | 未着手（値の再検証） |
| 3 | `flat_caller_sp` を足した `JSAsyncFunctionData` の詰め物が実機で +0B かは `_Static_assert` で確認するまで推定 | design §11.9 | 未着手 |
| 4 | `-recur` 変種で `deep_async_recursion` がどのガード（C スタック検査の RangeError か、ヒープの InternalError）で終わるかは段 A2 で測って期待値に書く | design §10.3, results §4.1 | 未着手 |
| 5 | Test262 に「async 再帰が C スタック検査で失敗する深さ」依存のテストが無いか（D38 で flat の答えが InternalError に変わる影響）は測る必要がある | design §10.3 | 未着手 |
| 6 | `js_async_from_sync_iterator_*` など、ジョブから本体へ至る経路で他にネイティブフレームを挟むものが無いかは未確認 | design §11.9 | 未着手 |
| 7 | L2c 本体（`rt->vm_susp`／`vm_yield:`／`vm_resume:` の実装）そのものが未着手。現在は関所とガード（pass-through）まで着地（`quickjs-vm.c` の `JS_VMResume` は常に例外を返す） | design §11（冒頭の実装状況表）, results §4.2 | 未着手（本体） |
| 8 | D42+D43 後、実機の最大連続空きブロックが taffy 59,296B 段への余裕を 6,240→4,192B まで減らした原因は未確認（総空き量は増えたのに最大ブロックが減った＝配置の問題と推定。1周だけの計測で再現性は未確認） | results §5.4 | 未着手 |
| 9 | `stack_hw_min`（ui タスクの最高到達点）が全アプリで同値なのは「セッションをまたいだ最小値」の可能性があり、読みが未確認。正しければ L2b で浮いた C スタックの回収余地になる | results §5.1, §5.4 | 未着手 |
| 10 | 末尾呼び出し最適化（TCO）は設計下書きのみで未決・未実装。sloppy モードを含めるか、既存コーパス・`budget_probe.sh` に末尾再帰で `RangeError` を固定した項目が無いかの確認、Test262 の見込みの実測、`CONFIG_POCKET_VM_TCO` の既定値決定がすべて未着手 | vm-tco-design.md §4（全体） | 未着手 |
| 11 | L2c 実機統合: `CONFIG_POCKET_VM_YIELD` の実機ビルド、D27 の書き手と leave ターンの `JS_VMClearYield`、guest の 2 ビット・3 起点、`pocket_app_reset` の後ろ盾、FAIR、`VM_FRAME_RUNAWAY_US` の実測、H14 計測、Back 押下時の stop hook 検査の追加 | design §11.8 | 未着手 |
| 12 | 呼び出し経路（L2a/L2b）の速度差は同一バイナリでの切り替え比較でないと交絡し主張できない。実機の速度も未計測 | results §3.3, §3.4 | 未着手 |

## L2 で完了済みの項目（参考）

- G1/G5/G6 判定機構は作成済み（design §1.2）。
- D1〜D43 は design §13 の決定表で「決定済み」と明記されたもの以外、上表に開いたものだけが残る（D6 は明示的に未決のまま design 側に残置）。
- L2c の関所とガード（旧「段1・段2」）は着地済み（results §4.2）。

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
| 3 | 公平モード（`CONFIG_POCKET_VM_FAIR`、既定 off）を既定にするかどうかの再検討。F 型アプリで完了遅延が中央値 3〜7 倍改善する一方、JS から観測できる順序（互換順序）を変えるため、仕様 §12 の既定挙動維持とは相容れない。L2 が実測を根拠に問い直せると位置づけ済み | vm-L1-report.md §9.5 | 未決（build 時選択として温存） |
| 4 | 測定器の穴: `vmprobe_frame_sample()` が継続ターンを標本しないため、L1 有効時の実行中空きヒープ・`js_used` を legacy と直接比較できない。継続経路からも呼ぶ（数行、プローブ専用）だけで埋まる見込み | vm-L1-report.md §5.4 | 未着手 |
| 5 | GC 閾値（初期値 256 KiB）がゲストのヒープ上限（160 KiB）より大きく、firmware がどこからも `JS_SetGCThreshold`/`JS_RunGC` を呼ばないため、循環参照のゴミが実質回収されない。`main` にも効く、VM 改造とは独立の不具合。コード確認済み（2026-09-15、`grep -rn JS_SetGCThreshold main/` に呼び出しなし） | vm-L0-report.md §2 | 未着手（`JS_SetGCThreshold` 1 行で直る見込み） |
| 6 | 上流 quickjs-ng の use-after-free: バックトレース組み立て中に確保が失敗する経路。再現は `tools/vmtest/known/oom_backtrace_uaf.js`。vm-L2-design.md がヒープ枯渇時の到達しやすさを別途記録している（重複ではなく利用箇所） | vm-L0-report.md §2 | 未着手（上流不具合） |
| 7 | 中断された `await` の Promise が永久に pending のまま残る（割り込みが捕捉不能な例外として実装されているため）。L1 は予算切れではこれを作らないが、暴走ガードと `stop_interrupt` の経路では今も起きる | quickjs-freertos-vm-spec.md §7「非対応事項」、vm-ledger/03 事実39-41 | 未着手（L2 の中断設計と併せて扱う想定） |
| 8 | ゲストの確保ヘッダが 1 ブロックあたり 12B 余分（`max_align_t` で得たい 8B 整列は tlsf が4B境界しか返さないため得られていない）。候補: (a) ヘッダを `size_t` 1個(4B)にする、(b) ヘッダを無くし `heap_caps_get_allocated_size()` を使う（`js_free_rt` の費用増、未計測）。コード確認済み（2026-09-15、`guest.c` の `allocation_header_t` は今も `union { size_t; max_align_t }`） | vm-ledger/05-allocation.md §1、vm-L2-design.md §2.2 | 未着手 |
| 9 | `guest_realloc` が常に新規確保+コピー+解放で、その場で伸ばせる場合もコピーが走りピークメモリを押し上げる（台帳07 で `memory_device` +27% の実例）。ヘッダ修正（項目8）と両立可能。効果はワークロード次第で未計測。コード確認済み（2026-09-15、`guest_realloc` は今も malloc+memcpy+free） | vm-ledger/05-allocation.md §1・§6 | 未着手 |
| 10 | `VM_JOB_FLOOR` と `VM_JOB_STRIDE` の相互作用は、floor(8) が stride(4) の倍数であることに隠れて D 型では検証できていない。floor を stride の倍数からずらす値に変える場合は再検証が要る（出荷値を変える提案ではないため現状は対象外）。`VM_JOB_BACKSTOP=64` が実際に effective な条件も未観測 | vm-L1-report.md §10 | 未着手（出荷値変更時のみ必要） |
| 11 | task-allocation-facade: FreeRTOS 接続層（タスク作成・登録・終了のライフサイクル）が未実装。既存ファーム・ビルド設定には未接続 | task-allocation-facade.md §実装範囲・§FreeRTOS接続層の契約 | 未着手 |
| 12 | task-allocation-facade: 「配置を保持した協調的な FP 輪番」は検討中の案のみで未決・未実装。所有者不在時の扱い、通知順序、輪番と優先度/音声期限の優先順位、チェックポイント間隔などが未検証 | task-allocation-facade.md §進行中の議論 | 検討中（採否未定） |
