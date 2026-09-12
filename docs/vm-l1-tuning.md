# VM L1 スケジューラ定数の調律 — `VM_JOB_FLOOR` と `VM_JOB_STRIDE`（2026-09-12）

対象: `components/pocketjs_guest/include/pocketjs/vm_sched.h`。
docs/vm-L1-design.md §1（予算の設計）・docs/vm-L1-report.md §8（時計とコアの決定）が
残した2つの疑問（§8.6「追わなかった調律」）を実機で決着させる。数値はすべて実測(device)、
`tools/vm_l0_capture.py`（`CONFIG_POCKET_VM_PROBE=y`、`CONFIG_POCKET_UI_TASK_CORE=1`）。
ワークロードは D `promise_chain`（40件の`.then`連鎖）・F `async_generator`
（`for await` 20回）、条件は base・all、各3反復。

## 0. 決めたこと

| 定数 | 疑問 | 決定 | 変更 |
| --- | --- | --- | --- |
| `VM_JOB_FLOOR` | 8 は時間予算を事実上無効にしているか | **8 のまま** | なし |
| `VM_JOB_STRIDE` | 4→1 の改善は「予算が早く効く」ためか、時計の副作用か | **1 のまま**（既に出荷済み） | なし |

どちらも既存の値を確認しただけで、`vm_sched.h` への変更は無い。新しく足したのは
1本のプローブ（§3）と、それが出す生データだけ。

## 1. 既存の "jobs" サンプルでは測れなかった理由

`tools/vm_l0_capture.py` が読む既存の `VMPROBE S ... jobs ...` は **`app_tick()` 1回**
あたりの実行ジョブ数で、`vm_sched_drain()` **1回の呼び出し**あたりではない。
`main/app_session.c` の `app_tick()` は、前ターンの drain が
`VM_DRAIN_YIELDED` で終わっていれば `pocketjs_ui_turn_continue()`（継続 drain、
`pocketjs_guest_continue()` 経由）だけを呼んで **早期 return** する
（`app_session.c:693-715`）。この経路は `vmprobe_frame_sample()`
（`main/pocket/vmprobe.c`、呼び出し箇所は `app_session.c:770` の1箇所だけ）に
一度も到達しない。したがって継続ターンが実行したジョブ数は、次に
`vmprobe_frame_sample()` に到達したターン（＝次の `frame()` 呼び出しを含むターン）の
サンプルに**合算されて現れる**。

D の場合、実際には「floor で 8 件だけ実行して yield」→「継続ターンで残り 33 件を
1 回で完走」という 2 ターンなのに、既存の "jobs" サンプルは 8+33=**41** という
1個の値しか見せない。これは「41 件が 1 回の呼び出しで実行された（=時間予算が
一度も効いていない）」場合とビットレベルで区別できない。§8.6 の疑いは、この
盲点のせいで確かめようがなかった。

## 2. 足したプローブ: `vm_sched_drain()` 呼び出しごとの `ran`

`drain_jobs()`（`components/pocketjs_guest/src/guest.c`、`pocketjs_guest_frame()` と
`pocketjs_guest_continue()` の両方が通る唯一の関所）に、呼び出しのたびに
`vm_sched_drain()` が返した `ran` を生のまま記録するリングを足した
（`vmprobe_drain_call_record()`、`CONFIG_POCKET_VM_PROBE` 時のみ、容量128）。
`main/pocket/vmprobe.c` の `vmprobe_frame_sample()` が毎ターン
`pocketjs_guest_vmprobe_drain_calls()` で吸い上げ、既存の行族に新しい
`VMPROBE S <seq> drainrun <n> v,v,v...` 行として出す。**既存の行・既存のフィールドは
1バイトも変えていない** — `VMPROBE WINDOW` に `drainrun_drop=%u` を末尾に追加しただけ
（`tools/vm_l0_capture.py` の `WINDOW_RE` は `re.search` で終端アンカーが無いので
影響なし）。継続ターンの `ran` も、他のどの経路の `ran` も、これで一箇所から漏れなく
生サンプルとして見える。

## 3. 疑問A: floor=8 は時間予算を無効にしているか — 実測

`.cache/vm/l1tune_hist_s1f8.jsonl`（出荷値 stride=1 / floor=8、base+all、3反復、
window 0 除外）。`vm_sched_drain()` 呼び出し1回あたりの `ran` の分布:

| ワークロード | 条件 | n | min | 中央値 | p95 | max | ran==8（floor で切られた回数の割合） |
| --- | --- | --- | --- | --- | --- | --- | --- |
| promise_chain | base | 2,360 | 8 | 20 | 33 | 33 | **50.0%**（`[(8,1180),(33,1180)]`）|
| promise_chain | all | 2,336 | 8 | 20 | 33 | 39 | **50.0%**（`[(8,1168),(33,1117),...]`）|
| async_generator | base | 1,440 | 7 | 14 | 16 | 17 | 24.5% |
| async_generator | all | 1,466 | 1 | 13 | 15 | 16 | 19.2% |

**floor は無効になっていない。むしろ D では毎回効いている。** D の呼び出しは
きれいに2山（8 と 33）に割れる — frame() を含むターンの呼び出しは**必ず**
ちょうど8件で止まり（floor が切る）、続く継続ターンの呼び出しが残り33件を
1回で完走する。§8.6 の疑い（「floor の8件に届いていない」）は逆で、
**D の frame() 単体コスト（実測 9.4〜9.5ms、`VM_TURN_BUDGET_US=8000`）が
ターン開始時点で既に予算を使い切っているため、floor の8件目の直後の
時間チェックが毎回・確実に発火している。** floor が「時間チェックを無効化して
いる」のではなく、floor は「時間チェックが本当に発火する地点」を作っている
（floor が無ければ n=0 の初回チェックで即 yield=0 になり、frame() が全く
進まなくなる — これは §6 の 1μs 診断ビルドで再現済み、下記参照）。

F（async_generator）では floor で切られる呼び出しが19〜25%で、残りは
9〜17件の幅に散らばる。1ジョブのコストが高い（実測 0.49ms/件）ため、
floor を越えた後の時間チェック（stride 刻み）が実際に効いている証拠。

**floor=8 を 0 にした場合との比較**（`.cache/vm/l1tune_ts4f0.jsonl` ほか、
floor=0・stride=4、drainrun プローブ追加前の計測。turn/frame()/drain の中央値のみ）:
promise_chain base の turn 中央値は floor=8 で10.55ms、floor=0 で9.28ms
（このセットは stride=4 も併用、単純比較不可だが同オーダー）。async_generator base の
turn 中央値は floor=8 で5.95〜6.01ms、floor=0 で5.98ms。**集計値では floor の
有無による有意差が出ない** — floor のコストは D で最大 0.14ms（8件×0.018ms/件）、
F で最大 3.9ms（設計時の見積り、§1.2）で、いずれもターン全体（9〜14ms・6〜8ms）に
対して小さい。

floor を 0 にした極端な副作用は `VM_TURN_BUDGET_US` を意図的に 1μs まで絞った
診断ビルド（stride=1・floor=0、一時的、コミットせず）で確認した:
n=0 のチェックが毎ターン即座に成立し（`ran=0` で yield）、8ジョブ連鎖が
1件も進まないまま `VM_RUNAWAY_US`（250ms）に達してセッションが強制終了した。
floor は「時間予算がどれほど厳しくても最低限の前進を保証する」という設計どおりに
機能しており、それを外すと本物の飢餓が起きることを実機で確認済み。

**決定: floor=8 のまま。** 効いている（測定で確認済み）、コストは小さい、
外しても集計性能は変わらない一方で、飢餓に対する保証を失う。変える理由がない。

## 4. 疑問B: stride 4→1 の改善は「予算が早く効く」ためか

`.cache/vm/l1tune_hist_s1f8.jsonl`（stride=1）と `.cache/vm/l1tune_hist_s4f8.jsonl`
（stride=4、floorは両方8）を同条件・同反復数で比較（drainrun 生分布）:

| ワークロード | 条件 | stride=1 の分布 | stride=4 の分布 |
| --- | --- | --- | --- |
| promise_chain | base | `[(8,1180),(33,1180)]` | `[(8,1177),(33,1177)]`（**完全一致**）|
| promise_chain | all | `[(8,1168),(33,1117),(34,39),(37,3),(39,9)]` | `[(8,1169),(33,1118),(34,39),(37,3),(39,9)]`（**完全一致**）|
| async_generator | base | 7〜17 に分散、山なし（`(8,353)(14,281)(15,279)(16,218)...`）| 8の倍数寄りに集中、max=20（`(16,835)(8,244)(6,224)(2,60)(20,25)...`）|
| async_generator | all | 1〜16 に分散、なだらか | 6・8・10・12・16 に集中、max=20 |

**D は stride に一切影響されない**（分布がバイト一致）。理由は floor=8 が
たまたま stride=4 の倍数でもあるため: チェック条件 `n>=floor && n%stride==0` は
n=8 で stride=1 でも stride=4 でも等しく真になり、D の呼び出しは常に n=8 で
初めて時間超過を検出して切られる。stride が「次にいつ切るか」を決める前に
floor 自身の境界でチェックが成立してしまうので、粗い stride の出番がない。

**F は stride の効果が直接見える。** stride=1 は毎ジョブ後にチェックするので
切れる点が7〜17の全域に散らばる（きめ細かい）。stride=4 はチェックが4件おきに
しか起きないので、切れる点が8の倍数近辺に寄り、**max が17→20に伸びる**
（=1回のオーバーシュートが stride×単価ぶん大きくなる、という設計コメントの
主張そのもの）。これは report §8.2 が turn 全体の p95/max（6,720→4,184µs、
7,144→4,877µs）で示していた改善を、**呼び出し1回ごとの実測分布で直接裏付ける**。
report は「予算が早く効く」という説明と「時計のコスト」という対立仮説の
どちらかを timing だけで判定していたが、drainrun のデータは時計を全く使わない
ジョブ件数のカウントなので、**時計のコストが理由ではあり得ないことを構造的に
示す**（時計を1回も読まなくても分布は変わるはずが無いのに変わっている＝
チェックの発火点そのものが変わっている）。

**決定: stride=1 のまま**（既存の出荷値を追認）。attribution は確認された
（refute ではなく confirm）: 改善は「予算がより細かい粒度で効く」という
挙動の差であり、時計の読み取りコストの副作用ではない。

## 5. 残した疑問（測定していない）

- **floor と stride の相互作用は D で偶然隠れている。** floor=8 が stride=4 の
  倍数であることが D の結果を stride 非依存にしている。floor を stride の
  倍数からずらした値（例: floor=6, stride=4）にした場合の挙動は測っていない
  — 出荷値を変える提案ではないため対象外にしたが、将来 floor を変えるなら
  この整合は再確認が要る。
- **`VM_JOB_BACKSTOP=64` が実際に effective な条件.** 今回の4ワークロード×2条件
  では backstop（64件天井）に触れた呼び出しは1件も観測していない
  （F の max は stride=4/floor=8 で20件、stride=1で17件、いずれも64を大きく
  下回る）。report §1.2 の「実測の最大 drain 件数68」がどのビルド・どの経路
  （継続ターンを合算した値か、1回の呼び出しか）を指すのか、今回のデータとは
  直接比較できない。

## 6. 測定条件・ビルド

`main/pocket/vmprobe.c` / `main/pocket/vmprobe.h` /
`components/pocketjs_guest/src/guest.c` /
`components/pocketjs_guest/include/pocketjs/guest.h` に drainrun プローブを追加
（本コミットに含む、`CONFIG_POCKET_VM_PROBE` 時のみ、通常ビルドは1バイトも変わらない
— `drain_jobs()` に足した `#ifdef CONFIG_POCKET_VM_PROBE` ブロックのみで、
呼び出し自体は空関数呼び出しにすらならない）。

stride/floor の組み合わせは `vm_sched.h` を一時的に sed 編集→ ビルド → 実機書き込み
→ 採取 → 定数を戻す、をその都度繰り返した（`git diff` で毎回クリーンに戻ったことを
確認済み）。ビルドディレクトリは `build_clk_ts4f0`（このタスク用に再利用、
`sdkconfig.defaults;sdkconfig.vmprobe.defaults`、`CONFIG_POCKET_UI_TASK_CORE=1` は
Kconfig 既定）。

以下は本タスクで実際に焼いた組み合わせ（floor×stride の4通り、drainrun 追加前の
turn/frame()/drain 中央値のみ。floor=8/stride={1,4} は drainrun 追加後に測り直し、
§3-4 の生分布はそちらの数値）:

| stride | floor | promise_chain base turn 中央値 | async_generator base turn 中央値 |
| --- | --- | --- | --- |
| 1 | 8（出荷） | 10.55ms | 5.95〜5.98ms |
| 4 | 8 | 10.55ms | 6.01〜6.02ms |
| 1 | 0 | 9.27〜9.28ms* | 5.97〜5.98ms* |
| 4 | 0 | 9.28ms* | 5.98ms* |

*floor=0 の2行は `VM_TURN_BUDGET_US` は 8000 のまま、drainrun プローブを足す前の
計測（`.cache/vm/l1tune_ts4f0.jsonl` / `l1tune_ts1f0_all.jsonl` / 既存
`clk_ts1f0.jsonl`）。floor の有無で turn 中央値が 9.3ms 前後と 10.6ms 前後に
分かれるのは floor 自体のコスト（D で8件、§3）ではなく、floor=0 計測が別セッション・
別のフォント/ヒープ状態で取られているため の可能性がある — 正確な floor=0/floor=8
の差分だけを見るなら同一セッション内で切り替えて撮り直す必要があり、今回はしていない
（§0 の決定は分布の形— floor が発火しているか — に基づくもので、turn 中央値の
数百μsの差には依存していない）。

## 7. スイート

`vm_sched.h` に変更が無い（stride・floor とも出荷値のまま）ため、タスク指示の
「変更した場合のみ」のスイート一式（`test_settings.py` / `capture_home.py` /
`smoke_device.py` / host `tools/vmtest/run.sh`）は走らせていない。走らせたのは
上記の VM_PROBE 計測ビルドのみで、シェル/エディタ/設定画面の回帰対象ではない。

作業終了時、デバイスは `CONFIG_POCKET_VM_PROBE` を切った素の L1 ビルドでホーム画面に
戻してある。
