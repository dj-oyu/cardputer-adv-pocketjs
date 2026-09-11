# vmprobe — L0 の負荷とその競合条件

`docs/quickjs-freertos-vm-spec.md` §5 の基準値を実機で採る一式。ホームの一覧には出ず、
USB の 1 バイトだけで起動する（`main/main.c` の `usb_stroke()`、`main/app_session.c` の
`app_start_test()`）。採取は `tools/vm_l0_capture.py`、結果は `docs/vm-L0-report.md`。

**ソースのバイト数はゲストのヒープを直接食う。** 各ファイルのコメントを短く保ち、理由は
すべてこの README に置く（`CLAUDE.md` の「6.5KB でゲスト 107KiB」）。

## ワークロード（USB の `A`〜`F`）

| 字 | ファイル | 1 フレームで何をするか |
| --- | --- | --- |
| A | `sync_loop.js` | 20,000 回の算術ループ。I/O も確保も無い、素のディスパッチ費用 |
| B | `deep_recursion.js` | 起動時に測った上限の 3/4 まで再帰。C スタックの経路 |
| C | `closures.js` | クロージャ 500 個を作って呼ぶ。`JSVarRef` の経路 |
| D | `promise_chain.js` | 長さ 40 の `.then` 連鎖。ジョブ件数とキュー長 |
| E | `io_wait.js` | `pocket.time.sleep(10)` を自分の `.then` から張り直す。完了遅延 |
| F | `async_generator.js` | `for await` で async generator を 20 回。中断・再開と await の往復 |

## 競合条件（USB の `P`〜`W`）

`P` + マスク（1=UI、2=音声、4=Wi-Fi）。**ワークロードの字とは別で、指定は残る** ——
1 つの条件で 6 本を続けて走らせるため。`condition.js` がワークロードの `frame()` を包み、
`app_session.c` がワークロードの評価の**後**に評価する（`pocketjs_guest_eval` が毎回
`globalThis.frame` を読み直すので、包んだ関数がそのまま呼ばれる）。

| 条件 | 字 | 実際に動くもの |
| --- | --- | --- |
| base | `P` | ワークロードだけ |
| ui | `Q` | `pocket.ui` の画面（rect + text）を作り、**毎フレーム** `setText`。UI コアの tick・レイアウト・draw が毎ターンに入る |
| audio | `R` | `pocket.audio.tone` 440Hz 1 秒を完了から張り直す。合成も I2S も本物で、優先度 7 の音声タスクが優先度 5 の ui タスクを止める |
| wifi | `T` | `pocket.net.wifi.acquire` で走行中ずっとリンクを保持し、3 秒ごとに GET を 1 回、本文を EOF まで読む |
| all | `W` | 上の 3 つ同時 |

条件の側が失敗したときは `VMCOND` の行を出して**続ける**（セッションは落とさない）。採取
スクリプトはこの行を JSONL に残すので、条件が劣化した回は報告の中で見える。

### Wi-Fi の宛先を焼き込んでいない理由

`https://example.com/` で始めたが、実機では成立しない。TLS の握手は**内部ヒープを
30KiB 前後**要求し、ゲストが 95KiB を持ったまま無線が 37KiB を取ると、残りは一桁 KiB に
なる（実測: リンク直後の空きが 51,652 B、要求の直前で 6,024 B）。この状態で握手が通ると
今度は QuickJS 側の確保が落ち、**例外の値が `null` や `[uninitialized]` になってセッションが
終わる** —— これは上流の仕様で、Error オブジェクトを作る確保まで失敗したときに
QuickJS が投げる値。バグではなく、内部 RAM が足りないという事実の見え方。

いまの宛先は**この機体自身のアドレスから作った LAN のゲートウェイ**
（`192.168.1.42` → `http://192.168.1.1/`）。平文 HTTP は private address にだけ許されており
（`main/pocket/pocket_net.c` の `url_check`）、時計の同期も PC 側のサーバーも要らない。
それでも要求は `NET_PLAIN_MIN_FREE`（12KiB）の門で断られることがあり、その回は
`VMCOND http OUT_OF_MEMORY ...` として残る。

### 時計について

平文 HTTP は時計を要らないが、**HTTPS を試すなら先に同期が要る**（`url_check` が
`solar_time` を見る）。同期は設定画面の Wi-Fi 行から行い、RTC はソフトリセットを越えて
残る（実測: 同期のあとハードリセットしても TLS_ERROR は出なくなった）。
