# appcheck — `pocket.app` / `pocket.time` / `pocket.log` の自己検査

`docs/common-api.md` §5（ライフサイクル・フレーム・時刻）と §7 の「ログと診断」が
実機で仕様どおりかを、1回の起動で確かめる。実装は `main/pocket_app.c`。

このファイルは `shell.c` に配線していない（別セッションが同じファイルを持っている
ため触っていない）。**Playground に貼って実行する**か、`main/CMakeLists.txt` の
`EMBED_TXTFILES` に足して使う。ソースは2.8KB で、ゲストの解析コストは実測の閾値
（6.5KB でゲスト107KiB）より十分小さい。

## 出力と、その行が主張していること

USBログの `js` タグに `APPCHK ...` として出る。

| 行 | 主張 |
| --- | --- |
| `CAP app/time/log ...` | 3つの capability が supported かつ available で、`limits` が `pocket_app.c` の強制値と一致する |
| `FRAME function` | このアプリは `globalThis.frame` を書いていないのに存在する。§5 の「ホストが frame を用意する」 |
| `WALL unsynced null` / `WALL network <ms>` | SNTP 成功前は `unixMs` が null。`solar_time.c` のデモ日時を時計として渡さない |
| `SLEEP ~150ms frames=0` | start hook が返した Promise が解決するまで Starting のままで、onFrame は1回も来ていない |
| `CANCEL CANCELLED not-applied` | cancel トークンで reject し、待ちが完了していないので outcome は not-applied |
| `TIMEOUT TIMEOUT not-applied` | `timeoutMs` は呼出受付からの全体期限。sleep より短い期限は黙って延ばさない |
| `BADARG INVALID_ARGUMENT` | Promise を返すメソッドの引数エラーは throw ではなく reject（§4） |
| `FIRST t=... d=0` | 最初のフレームの `deltaMs` は0。以後は実測の間隔 |
| `FRAME30 delta=33` | 30fps。`deltaMs` は単調時計の差 |
| `LOG debug bytes=256 trunc=true` | 398バイトの行が256バイトの文字境界で切られ、`truncated` が立つ |
| `LOG LEVEL INVALID_ARGUMENT` | level は debug/info/warn/error のみ。同期関数なので throw |
| `LOG CONSOLE n/m` | `console.log` が §7 の同じリングに入っている（n>0） |
| `LOG FLOOD dropped=N` | 毎秒4096バイトの上限を越えた分は捨てられ、破棄数で報告される |
| `METRICS heap=... js=... fps=30 io=null` | `device.metrics()` はキャッシュ済みの値を返し、取得不能な `ioDropped` は null |
| `AFTER EXIT` | `exit()` はターン境界で効くので、呼んだ後のリスナーは最後まで走る |
| `STOP back frames=30` | stop hook がゲストの生存中に走り、reason は `back` |

`STOP` が最後の行。出なければ hook が200msを越えたか、そもそも登録されていない。

## 分かっていて仕様と違うところ

- `reason` は常に `back`。`main.c` の終了経路が1本しかなく、`replace` / `shutdown`
  を名乗る根拠がない。
- `exit()` はアプリ側からの正規終了経路を `main.c` が持たないため、失敗したターン
  として着地する。エラー行は `pocket_app_reset()` が `APP EXITED` に書き換える。
- Promise の job はフレームの**後**に排出される（`pocketjs_guest` の設計）。§5 の
  「Promise job → frame」の順とは逆で、sleep の `then` は次のフレームで走る。
