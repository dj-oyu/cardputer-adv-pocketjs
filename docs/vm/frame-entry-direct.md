# frame の直接入口と例外報告

`FRAME_WRAP` の `f.apply(this,arguments)` を削除し、source app の `frame` をホストから直接 `JS_VMCall` する。native apply の再入床で本体の協調中断が無効になっていた問題への変更。QuickJS の入口／native 境界そのものは変更しない。

## 本番への接続と互換性

通常の APPS／Playground は `main/main.c` → `app_start_source()` → `app_start_test()` → `eval_user_source()`。後者は prelude と user.js を評価し、`pocketjs_guest_eval(...,"bind-frame.js")` に `globalThis.frame` を保持させる。`app_tick()` の `dispatch_guest()` → `pocketjs_guest_frame()` → `JS_VMCall` が実行の入口。中断後は `pocketjs_guest_continue()` → `JS_VMResume` で同じ呼出しを続ける。`app_overlay_tick()` も同じ guest API を使う。これは `CONFIG_POCKET_VM_PROBE` 専用経路ではない。

旧 sloppy wrapper が strict な user frame にも渡していた realm global の `this` は、`eval_user_source()` が `pocketjs_guest_set_frame_global_this(guest,true)` で明示する。VM が park 時の receiver を保持するため、ホストの一時参照は call 後に解放できる。**直接 guest API を呼ぶ diagnostic／hello の経路は wrapper を通っていなかった**ので、既定 false のまま `JS_UNDEFINED` を渡す。strict な直接呼出しまで global に変えてしまう初期 WIP の差分は取り消した。sloppy／arrow／bound／Proxy の言語上の receiver 規則は engine のまま。

引数は従来と同じ 2〜4 個（buttons、analog、任意の touches、touch_hits）。park 後も最初の値を保持し、再開のたびに新しい入力で上書きしない。frame が返るまで、その frame が作った Promise job を追い越させない。通常／async 関数、async の await 前後、job の中断を検査する。bound／Proxy で包まれた関数は引き続き native floor を通り、この変更だけでは本体が park しない。

frame のエラーは guest が保持するホスト callback に渡す。初回 call の失敗と、park 後に再開した HOST frame の失敗の両方が対象。park 中／prepare_stop 中には呼ばない。Promise rejection は従来どおり job／rejection の報告経路であり、この callback の対象ではない。watchdog の interrupted は依然捕捉不能で、catch／finally を実行しない。

画面は同じ 128 B の `jsconsole_error()` を使い、message と stack 先頭行を保存する。`String(symbol)` と暗黙の ToString(symbol) は異なるため Symbol の表示は別扱いにする。旧 `e && e.stack` の primitive 上の継承プロパティと falsy の第2引数も維持する。`__pjs_error` は明示呼出しとの互換用に残す。ただし wrapper 自体の関数 identity／stack の余分な wrapper 行、書き換えられた `Function.prototype.apply`／global `String` への依存まで不変とは主張しない。

formatter の getter／文字列変換が失敗しても元の例外を callback から stderr dump へ戻す。**stderr の `js_std_dump_error` 自身も JS を再入して失敗できる**ため、その後にも pending exception を消す。callback 後だけ消していた WIP は、throwing toString／Error.stack getter の試験で O2 と ASan の両方が失敗した。今回の修正後は次の呼出しまで汚染が残らない。画面 formatter 自体が失敗したときは元の例外を保持し、最低限 `frame failed` を表示する。

## Linux の再現手順

```bash
bash tools/vmtest/test_frame_entry.sh o2
bash tools/vmtest/test_frame_entry.sh asan
```

`tools/vmtest/build.sh` の QuickJS オブジェクトと、本物の `guest.c`／`vm_sched.c`／`vm_clock.c`／`block_cache.c`／`jsconsole.c` をリンクする。`frame_entry_source.py` は `app_session.c` の `frame_error`／`eval_reporting`／`eval_user_source` をそのまま抽出し、source 分岐と callback 登録も検査する。**app_session 全体を host に移植したものではない**。module loader と OOM ログだけは stub（module 分岐へ来たら assert）、HAL／LCD／native pump はリンクしない。`nm` で guest、JS_VMCall、JS_VMResume、jsconsole と source entry の実体を確認した。

timer／heap／critical section の shim はこの検査専用。timer は無効、強制 safepoint yield と `JS_VMRequestYield` の実 API を使う。8 ms の割込み競合や ESP32 の性能を測る試験ではない。driver／変更した本番 C は `-Wall -Wextra -Werror`。sanitizer は QuickJS 自体も計装する。

この cloud container では LeakSanitizer が ptrace 環境のため起動できない（別の malloc/free 最小対照でも失敗）。そのため今回の実行は次の指定。LSan 成功とは記録しない。no-PIE はこの環境の ASan 起動配置問題を避けるためで、ファームの設定ではない。

```bash
ASAN_OPTIONS=detect_leaks=0 \
VMTEST_CFLAGS='-fno-pie -no-pie' \
VMTEST_OUT="$PWD/.cache/frame-entry-sanitize" \
bash tools/vmtest/test_frame_entry.sh asan
```

Windows 用 `tools/vmtest/test_frame_entry.ps1` も同じ driver／抽出を使うよう更新したが、今回の cloud 作業では未実行（Python も必要）。以前の WIP の Windows 成功記述を Linux の検証根拠にはしない。

## 2026-10-01 UTC の検査結果（64 bit Linux host）

基準は `origin/vm/main` の `91801cf`、継続開始時の WIP は `92ae35a`。

| 検査 | 結果 |
| --- | --- |
| focused suite O2／ASan+UBSan | 両方 PASS。UBSan は recover 無効 |
| 100／1,000 回 loop | 202／2,002 回 resume、結果 4,950／499,500。strict this 検査付きは 204 回 |
| 旧 FRAME_WRAP を戻す負の対照 | 同じ 100 回 loop で resume は wrapper 側の 1 回だけ。本体の 202 回と区別できる |
| 強制 hook を使わない JS_VMRequestYield | 1 回 park／resume して完走 |
| receiver／引数の GC 保持 | 2／3／4 引数、UINT32_MAX／INT32_MIN、park 中 GC、40 回繰返し PASS |
| job／async | frame → job の順序、count-budget の続き、await 前後の例外が rejection 側へ流れること PASS |
| 任意の throw と callback の失敗 | primitive／Symbol／throwing getter／変換の 18 ケース、同期・再開後、次の呼出しまで PASS |
| 旧 wrapper と画面文字列の比較 | Error／文字列／Symbol／falsy／primitive stack／独自 stack の 11 ケースが byte 一致 |
| watchdog | 同期・再開後とも catch／finally を実行せず停止、PASS |
| source 入口 | prelude lexical、frame 不在、明示 null、top-level error、直接 guest の strict this=undefined、PASS |
| parked teardown | sync／Promise handler／async／async generator × prepare_stop／heap=0+stack=1 の stop／parked destroy、12 ケース PASS |
| frame の確保失敗 | lazy push を事前構築した 51 点 × 通常／強制 yield = 102 点すべて refusal、残留 chain／例外なし。各点で次の frame が動き、破棄時 guest allocator の live block は 0 |
| VM corpus O2 通常／強制 yield | 各 80/80、出力一致 |
| VM corpus ASan+UBSan 通常／強制 yield | 各 80/80、出力一致（LSan は無効） |
| VM lifecycle ASan+UBSan | 900 ケース PASS（LSan は無効） |
| 静的検査 | git diff --check、Python py_compile、bash -n、変更 C の -Werror、PASS |

host allocator の live-block 検査は guest allocator を通る確保の検査であり、すべての libc 確保に対する LSan の代わりではない。コーパスの vmrun は guest wrapper 全体ではなく scheduler を共有する既存 harness なので、production source の検査は focused suite と分けて記録する。

## 別件として残す baseline の lazy OOM

`[].push(0)` を事前構築しない全点掃引は、**変更前 `91801cf` の guest.c でも同じ失敗**を再現した。55 点中の 16 点目で 1 回だけ guest allocation を拒否すると、frame が `ESP_OK` を返すのに pending exception が残る。新しい error callback は一度も呼ばれていない。この枝では修正しない。

```bash
# 既知の baseline 失敗を意図的に再現する。通常 suite の PASS には含めない。
bash tools/vmtest/test_frame_entry.sh o2 --oom-lazy
```

対象は `frame=(b,a,t,h)=>{let objects=[];for(let i=0;i<8;i++)objects.push({i,t,h});done=objects.length}`、3 個の touches／touch_hits 付き。テストの `frame_entry_heap_malloc` が source 評価後から確保を数え、第 N 回だけ NULL を返す。旧 guest の比較は新 API への no-op adapter だけを付け、guest／engine の本体は変更しなかった。この baseline の失敗が残るので「一般的な OOM 安全性がすべて合格」とはしない。

## 統合前に残ること

ESP-IDF build／実機／COM3 は未使用。32 bit ESP32 の heap margin、8 ms timer 競合、Back 保存、Kasane beginFrame／commit の途中 park、場面切替 OOM、display transfer、入力の応答性は実機未検証。module loader と top-level await、app_session 全体の native surface reset もこの host slice の対象外。

既存の `end_guest_turn()` は work_pending のあいだ Kasane を park し、`app_tick()` は続きが終わるまで pump／新しい frame を待たせる。完了した描画には先に display turn を与え、Back は leave turn の特別扱いで保存に進む。これらの source 上の接続は確認したが、wrapper 削除で通常 source app が初めて途中 park するため、複合実機検査を代替できない。host の resume 回数を実機の速度向上率に読み替えない。push／main・vm/main への merge はしていない。
