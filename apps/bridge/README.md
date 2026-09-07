# bridge — `pocket.bridge` のデモ

`docs/common-api.md` §13 の PC link を、ケーブル1本だけで一往復させるためのアプリ。
実装は `main/pocket_bridge.c` / `main/pocket_bridge.h`、PC側は `tools/pocket_bridge.py`。

## 動かす

```powershell
python tools\pocket_bridge.py selftest                # 実機不要。コーデックとアダプターの検査
python tools\pocket_bridge.py demo --port COM3        # Playgroundへ流し込んで実行し、そのまま応答する
python tools\pocket_bridge.py demo --port COM3 --no-type   # 2回目以降。保存済みのソースを実行するだけ
python tools\pocket_bridge.py serve --port COM3       # すでに動いているアプリに応答するだけ
```

`demo` はホーム → APPS → PLAYGROUND と辿り、C-n で空にし、ソースを打ち込み、C-r で実行してから
`serve` に入る。**打鍵は約50 bytes/秒に絞ってある。** `main/main.c` の入力タスクは5msに1バイトしか
読まず（200 B/s）、UIタスクは1フレーム＝16msに1打鍵しか取り出さない（62/s）。速い方に合わせると
文字が黙って落ちる。2.7 KBのソースで約55秒かかり、C-r が srcstore に保存するので次からは `--no-type`
で済む。

## 期待する出力

`demo` の後半（`[bridge]` はPC側、それ以外は端末のログ）:

```
[bridge] session 1234567890 open
BRIDGE_CAP supported=true available=true maxFrameBytes=496 maxPayloadBytes=480
BRIDGE_READY
BRIDGE_LINK 1234567890
[bridge] host.info -> RESPONSE
BRIDGE_INFO <PCのホスト名> Windows
```

画面には `LINKED <sessionId>` と `HOST <名前>` が出る。ENTER を押すと:

```
[bridge] agent.submit -> RESPONSE
BRIDGE_JOB job-1 accepted
BRIDGE_EVENT 1 running
BRIDGE_EVENT 2 done
```

PC側を起動せずに実行した場合は、10秒後に:

```
BRIDGE_ERROR CONNECT TIMEOUT outcome=unknown
```

`TIMEOUT` で `outcome=unknown` なのが正しい。フレームは送れているので、PCが受け取って何かした
可能性を否定できない（§4）。

## このアプリが確かめていること

| 触っている面 | どこで |
| --- | --- |
| `capabilities.get('bridge.pc')` と `limits` | 冒頭の `BRIDGE_CAP` |
| `connect()` の Promise と `timeoutMs` | `pocket.bridge.connect` |
| `request()` の往復とJSON | `host.info` / `agent.submit` |
| `onEvent()` の購読と `sequence` | `agent.job` |
| `PocketError` の `code` / `outcome` | `fail()` |

ノードは root を入れて8個。`CLAUDE.md` の段差（16以下 → 2,048 B）の内側に収まっている。

## PCアダプターがやらないこと

`tools/pocket_bridge.py` は `host.*` と `agent.*` を**自分で**答える。モデルもシェルも起動しない。
§13 が「APIキーやPCのシェル実行権限をデバイスに複製しない」と言っているのはこの向きの話で、
端末から名前を送ればPC側で何かが実行される、という経路をそもそも作らない。`agent.submit` は
jobId を返してイベントを2本流すだけで、`result` には「このアダプターはモデルを動かさない」と書いてある。

実際のCodex/Claude連携を足すなら、それはPC側のアダプターの仕事で、ディスパッチ表に**明示的に**
1行足す形になる。デバイス側のプロトコルは変わらない。
