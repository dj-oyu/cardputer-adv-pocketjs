# VMの文書

[文書トップ](../README.md) / [過去の調査・実験](../archive/README.md)

仕様・設計にも将来案や当時の値が含まれる。採用範囲・既定値・未確認条件は本文の状態表示を優先する。

## 仕様・設計・作業の入口

| 文書 | 種別 | 内容 |
| --- | --- | --- |
| [quickjs-freertos-vm-spec.md](quickjs-freertos-vm-spec.md) | 仕様 | L0〜L5 の全体仕様。各段の現状と、実装で確定した値 |
| [vm-branching.md](vm-branching.md) | 仕様 | `vm/*` ブランチとワークツリーの運用 |
| [vm-L1-design.md](vm-L1-design.md) | 設計 | L1: ジョブ境界の実行制御と起床 |
| [vm-L2-design.md](vm-L2-design.md) | 設計 | L2: 移動しない VM スタックと中断・再開。決定表 D1〜D43、整列（§2.2） |
| [vm-L3-design.md](vm-L3-design.md) | 設計 | L3: 相対参照と移動可能スタック。決定表 D44〜D55、移動は中断中のみ（§2）、差分表（§3） |
| [vm-tco-design.md](vm-tco-design.md) | 設計・検証記録 | strict末尾呼び出し最適化の実験実装。host/device検証済み、互換性維持のため既定n |
| [task-allocation-facade.md](task-allocation-facade.md) | 設計 | FreeRTOS タスクの配置ファサード（`standalone/fp_ticket`、未接続） |
| [r3-small-block-cache.md](r3-small-block-cache.md) | 設計 | R3: ゲストのアロケータの下に小ブロックの再利用リストを置く設計。確保の大きさの分布（上位 6 つで 92%）とキャッシュ方式ごとの命中率のシミュレーション、置き場所・上限の外に出る量・手放す時点・検査の仕方 |
| [app-suspend-design.md](app-suspend-design.md) | 設計 | アプリの常駐中断: Back でゲストを壊さずに止めて残し、同じアプリを開くと続きから。状態遷移、各面の中断・再開の扱い、API（`suspend`/`resume` フック、`stop("evict")`）、退去の条件、メモリの予算、段と検証。決めること 4 つ |
| [interpreter-stages.md](interpreter-stages.md) | 設計・素案 | インタプリタの時間の段（S0 読み込み〜S4 ターン出口）と、最適化の段（tiered interpreter、T0〜T2）。どの段でも 1 ターンに触るコードを予算に収めて速くする方針、オペコードごとの呼び先と分類、ダンプ判定・TDZ・Math の試作結果、get_var・double の候補、後回しにしたもの（プロファイル・バイトコードの flash 保存） |
| [p1-iram-placement.md](p1-iram-placement.md) | 設計・素案 v2 | P1: 1ターンに触るコードを IRAM に置く。Xtensa の GCC は関数内の hot/cold 分割をしない（実測）。`JS_CallInternal` は冷たい命令を出しても 13〜16 KB 残り、1 KiB あたりの効きは helper 群の半分程度（推定）なので、本体・helper・両方を模型で同じ予算に並べてから実機へ。先に helper の `no_inline`。SLOW のレビューの要約（§8） |
| [builtin-floor-plan.md](builtin-floor-plan.md) | 設計・計画 | F 系列（VM の段とは独立）: ゲストの起動床（実機レイアウトで js=64,420 B、うち組み込みの名前 21.7 KB と何も作っていない索引）を flash へ。F1 ROM atom・F2 遅延索引で 28,684 B（−55%、計算）。捨てた案 4 つ、`atom_array` 33 箇所の台帳、関所と負の対照。道具は `tools/vmtest/floor/` |
| [rom-intrinsics.md](rom-intrinsics.md) | 設計・考察 | F 系列の次: harden（読み取り専用の intrinsic）の下で、組み込みオブジェクトのグラフ全体をビルド時 snapshot として flash に置く。F1〜F3 後の床 24,512 B（context 17,904）のうち約 14.0 KB と、アプリが触ったメソッド 4〜7 KB が対象（実測 host、実機レイアウト）、DERBY で約 20 KB（推定）。参照数は符号で不死を表し（dup +1 命令、free ±0）、`realm` は NULL。壊れるもの 9 項の台帳、候補 4 つの比較、T1 の guard との関係、段 R1〜R4。道具は `tools/vmtest/floor/rom_walk.sh` |
| [ゲストの行番号表を落とす](strip-debug.md) | `CONFIG_POCKET_VM_STRIP_DEBUG`（既定 OFF）と、アプリごとの `option strip-debug`（chunks.txt）: 行番号表なしのコンパイル。アプリごとの効果（DERBY の評価の余裕 +2.7 KB、実機）、失う位置情報、DERBY の首振り（段階 3）には足りないこと、運用案（2026-09-30）|
| [compact-vars.md](compact-vars.md) | 設計・記録 | harden したコンテキストでは局所変数ごとに 2 B（捕まえた変数のスロット番号）だけを持ち、`JSVarDef`（20 B）と変数名の atom を落とす。出荷 16 アプリで −55 KB、DERBY −13.8 KB、MEGADEMO −13.0 KB（実測 host）。失うのは TDZ の文言の変数名 |
| [tdz-elision.md](tdz-elision.md) | 設計・記録 | TDZ 検査（`get_loc_check`・`put_loc_check`・`set_loc_uninitialized`）をコンパイル時のデータフロー解析で消す。DERBY の 1 フレーム 12,375 → 11,506 命令（host 計数）、評価のピーク −2.6 KB、確保なし。規則と正しさの根拠、検査 |
| [harden.md](harden.md) | 設計・記録 | アプリのコンテキストから驚きを消す 4 規則: eval/Function 禁止、prototype の付け替え禁止、組み込みは生まれたときから書き換え不可（凍結せず属性で。遅延の組み込みも鍵付きで生まれる）、species を見ない。override mistake の緩和、shape の対の共有でヒープ +424 B（host）。実機で全 JS アプリの起動を確認、DERBY の JS +0.7%・命令 +0.25%（A/B/A） |
| [c-builtins-zip-fromasync.md](c-builtins-zip-fromasync.md) | 設計 | issue #91: ROM 版に無い `Iterator.zip` / `zipKeyed` / `Array.fromAsync`（上流は JS の bytecode、RD5 で落とした）を C で書き直す設計。zip は `JS_CLASS_ITERATOR_HELPER` に kind を 1 つ、fromAsync は async 関数の `await` と同じ部品（`js_promise_resolve` ＋ `perform_promise_then` に `undefined` の cap）で状態機械。test262 の 3 ディレクトリは今の RAM 版で 350/350、`--harden` で 314（36 件は規則どおり）、ROM 版は 10（理由違い）（実測 host）。flash は正味 ±1 KB 前後、使わないアプリの RAM は 0（推定）。zip を先に |
| [event-loop-design.md](event-loop-design.md) | 設計・素案 | 非同期の合流点を表示ループから外す（#98 の根）: 同期的な 3 層（fs の同期 I/O・pump 段の合流・`frame()` が中心のループ）のなぜなぜ分析、根本原因はターンの過積載。ui_task のままイベントループにし、`frame()` を描画の機会に降格、生存は参照カウント。L1 §4.2 との衝突は段階（E1 は `frame()` 無しのセッションだけ）で避ける |
| [backlog.md](backlog.md) | backlog | L2 の未完了条件、L1 の範囲外として残った決定、VM とは独立の不具合（GC 閾値、確保ヘッダ 12B など） |
| [memory-pressure-api.md](memory-pressure-api.md) | 設計 | VM / host の memory pressure 契約。対象 source と未実装範囲を本文で確認する。 |
| [math-call-fast.md](math-call-fast.md) | 試作 | `Math.sin(x)` など純粋な組み込みを、数値の引数なら native フレームなしで呼ぶ（`js_call_math_fast`）。今の経路（C 関数約18種）、DERBY の Math 163.5 回/フレーム（host）、別名・IC・呼び出し箇所の特殊化との比較。host 検査済み、実機の速度は未測定 |

## 計測・調査・実験の記録

| 文書 | 種別 | 内容 |
| --- | --- | --- |
| [eval-peak.md](eval-peak.md) | 記録・比較 | 評価のピークの構成（pass-1 バイトコード 46%、JSFunctionDef 15%…、host の確保タグ）と下げる手段の実測: 分割（B 案）+18.5〜25.9 KB、事前コンパイル +26 KB・49 ms、パーサの余り返し host −11 KB、ES モジュール、遅延 import。推奨の順序と決めること。§7 製品化した `pocket.app.load`（チャンクの表・書き方・実機で 3 分割 +18.2 KB）、§8 静的 import への引き継ぎ |
| [vm-L0-report.md](vm-L0-report.md) | 記録 | L0 の実機計測（ターン内訳、ヒープ、ジョブ単価） |
| [vm-L1-report.md](vm-L1-report.md) | 記録 | L1 の実測。時計読み出しのコスト（§8.7）、コア移行（§8.8）、スケジューラ定数の調律（§10） |
| [vm-L2-results.md](vm-L2-results.md) | 記録 | L2 の実測と関所の結果（段ごと、host/device の別つき） |
| [vm-L3-results.md](vm-L3-results.md) | 記録 | L3a の実測。コーパス74件バイト一致、枝ごとの踏まれ方、毒の負の対照3種、Test262 7,036ファイル、移動の単価。**実機のサイズは `--gc-sections` で落ちるので測れない**（§2） |
| [oom-parse-safety.md](oom-parse-safety.md) | 記録 | 確保失敗時のコンパイル経路（VM の段とは独立）。全点掃引で見つけたメモリ安全性の穴（closures 71 点・generators 133 点 → 0）と、変数の捕捉が黙ってグローバル読みになる誤コンパイル。`DynBuf` のエラーが余りへの書き込みに効いていなかった根本原因。上流2系統とも未修正 |
| [allocator-cost.md](allocator-cost.md) | 記録 | ゲストのアロケータの値段を実機で測った（`CONFIG_POCKET_VM_ALLOCPROBE`、計測後に削除）: malloc→free 1 組 約 1,800 サイクル、STRESS の JS のターンの 20.6%。経路は `heap_caps` の走査・クリティカルセクション・flash 上の読み戻し |
| [r5-icache.md](r5-icache.md) | 記録 | R5: STRESS のターンを QEMU で記録し、命令キャッシュの模型に通した（`tools/r5sim/`）。触るのは 45.5 KiB、衝突は 15%。flash 内の並べ替えは効かない。IRAM 8 KiB は実機で JS −15%・空き −8.7 KiB で、見送り |
| [turn-working-set.md](turn-working-set.md) | 記録 | 段つきインタプリタの予算の下調べ: STRESS の 1 ターンが触るコードを部品ごとに分けた（`tools/r5sim/turnset.py`、模型の推定）。試作 3 つを入れても 42.5 → 41.8 KiB、インタプリタ（core＋helper）は 21.3 KiB で予算 14 KiB を超え、主因は helper。R5 の模型の行の水増し（45.5 → 42.5 KiB）を修正 |
| [turn-cpi.md](turn-cpi.md) | 記録 | R4: JS のターンを Xtensa の性能カウンタで測った（`CONFIG_POCKET_VM_TURNPERF`）。IPC 0.17、サイクルの 70% が flash キャッシュのミス待ち。flash を QIO にして 1 フレームの JS 10.78 → 7.25 ms（同一配置） |
| [gc-cap-backoff.md](gc-cap-backoff.md) | 記録 | 上限の手前 1/32 の GC の天井が、生存量が天井を超えるとオブジェクトごとに GC を走らせていた（STRESS LV3 で JS 時間の 70%）。天井が起こした GC の後はヒープが伸びるまで待つ。GC 1,085 → 109 回、確保失敗の時点は同一。道具は `tools/vmtest/prof/` |
| [spread-eval-oom.md](spread-eval-oom.md) | 記録 | 文字列のスプレッド・for-of が実機でだけ止まらなかった: `js_string_iterator_next` の `(int *)&idx` が Xtensa（`uint32_t`=`unsigned long`）の strict aliasing で消えていた。修正 `f937388`、同種の6箇所、再現アプリ `POCKET_HEAPPROBE` と変種の表 |
| [aliasing-types.md](aliasing-types.md) | 記録 | spread-eval-oom の同種6箇所と libunicode の正規化配列を、宣言の型を合わせて修正（機械語は修正前と同一＝今のビルドでは誤コンパイルなし）。`-Wno-incompatible-pointer-types` を quickjs-libc.c 以外から外した |
| [vm-ledger/](vm-ledger) | 記録 | QuickJS 内部の台帳 01〜09（呼び出し経路、フレームへの生ポインタ、ジョブと割り込み、opcode チェックポイント、メモリ確保、アロケータ比較、セグメント検査、スラブと最大空きブロック、**09: L2 後のセグメントを指す入口の再監査**） |
