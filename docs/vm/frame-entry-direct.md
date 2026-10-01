# frame の直接入口と例外報告

`FRAME_WRAP` の `f.apply(this,arguments)` を削除し、source app の `frame` をホストから直接 `JS_VMCall` する。native apply の再入床で本体の協調中断が無効になっていた問題への変更。QuickJS の入口／native 境界そのものは変更しない。

旧sloppy wrapperがstrictなuser frameにも渡していたrealm globalの `this` を直接callでも明示する。VMがpark時のreceiverを保持するため、ホストの一時参照はcall後に解放する。strict frameでpark前／再開後の両方に `this === globalThis` を検査する。

frame のエラーは guest が保持するホスト callback に渡す。初回 call の失敗と、park 後に再開した HOST frame の失敗の両方が対象。park 中には callback を呼ばない。元の例外は formatter の getter／文字列変換が失敗しても保持し、従来の stderr dump と ESP_FAIL に渡す。画面用 formatter は同じ128Bの `jsconsole_error()` に message＋stack先頭行を保存し、Playground／codeedit の表示経路を維持する。`__pjs_error` は明示呼出しとの互換用として残す。

## ホスト回帰検査

既存 MinGW GCC を使う Windows ホスト検査:

```powershell
./tools/vmtest/test_frame_entry.ps1
```

QuickJS と実際の `guest.c`／`jsconsole.c` をリンクする。新しい timer／heap／critical section の shim はこの検査専用。timer を無効にし、VM の強制 safepoint yield を使うため実機の8msタイミングや割込み競合を測る試験ではない。変更した C と test driver は `-Wall -Wextra -Werror`、vendor は警告を記録する。

2026-10-02 の64bit native host結果: 100回 loop は202回再開、1000回は2002回、結果4950／499500。park 中は画面error報告0回、再開後のErrorは1回で `entry.js` の位置を保持。直接callのError、park中terminateで報告しないこと、文字列変換とstack getter両方がthrowするformatterのfallback／pending exception消去も検査する。

実機・COM3 は未使用。Back保存、Kasane beginFrame／commitの途中park、場面切替OOM、display transferと入力の応答性は実機未検証。wrapper削除により初めて通常APPSがparkし始めるため、この複合検査は統合後の機体利用許可を待つ。hostの再開回数を実機性能の改善率に読み替えない。
