# netcheck — `pocket.net`（Wi-Fiリースと HTTP）の自己検査

`docs/common-api.md` §11 と、§4 の期限・キャンセル契約が実機で仕様どおりかを、1回の
起動で確かめる。実装は `main/pocket_net.c` と `main/wifi_time.c` のリンク部分。

このファイルは `shell.c` にも `main/CMakeLists.txt` にも配線していない（他セッションが
それらを持っているため触っていない）。**Playground に貼って実行する**か、
`EMBED_TXTFILES` に足して使う。ソースは 3,055 バイトで、ゲストの解析コストは実測の
閾値（6.5KB でゲスト107KiB）より十分小さい。

## 実行に必要なもの

- **サーバーは不要。** 唯一の外部通信は `URL`（既定 `https://example.com/`）への
  GET 1回で、これは IANA が例示用に予約しているドメイン。ここを `null` にすると
  デバイスから1バイトも出ない状態で残りの検査が全部走る。
- **Wi-Fi の資格情報はあってもなくてもよい。** 無い場合、`ACQUIRE` は
  `NOT_AVAILABLE` を返し、そこから先（`LEASE` 以降）が出ない。それ自体が
  「アプリにパスワードを触らせない」§11 の確認になる。`SCAN` は資格情報なしでも
  動く（電波を出して周囲を見るだけで、どこにも繋がない）。
- パスフレーズ・URLのクエリ・ヘッダ値・本文は**どの行にも出ない**。

## 出力と、その行が主張していること

USBログの `js` タグに `NETCHK ...` として出る。無線側の `LINK_START` /
`LINK_UP free=` / `LINK_DOWN free=` は `wifi` タグで、リースの前後の空きヒープは
そこで読む。

| 行 | 主張 |
| --- | --- |
| `CAP net.wifi true/true null {...}` | capability が supported で、`limits` が `pocket_net.c` の**強制値**と一致する（§14 の提案値ではない） |
| `CAP net.http ...` | 同上。`maxReadBytes` `maxResponseBytes` `tlsMinFreeBytes` などが公開されている |
| `NOLEASE INVALID_ARGUMENT not-applied` | 有効な lease なしの `request` は拒否。§11 の「有効なlease必須」 |
| `PROFILE NOT_FOUND not-applied` | `profileId` はホスト設定への参照。存在しない名前は NOT_FOUND で、アプリが接続先を作れない |
| `SCAN n=<件> trunc=false ms=<実測>` | 電波を上げ、走査し、下ろすまでが1つの Promise。16件上限、UTF-8にできないSSIDは `trunc` に出る |
| `CANCEL CANCELLED after=<実測>ms` | **`after` がスキャンの所要時間とほぼ同じであること**が要点。キャンセルは即座に reject せず、ネイティブが止まってから settle する（§4） |
| `ACQUIRE NOT_AVAILABLE`（資格情報なし） | 設定画面が接続先を持っていないだけで、アプリの誤りではない。retryable=true |
| `ACQUIRE AUTH_FAILED`（鍵が違う） | 保存された鍵を AP が拒否した。`wifi_time.c` の stage=auth を、鍵を一切露出せずに伝えている |
| `LEASE connected <IPv4>` | `acquire` は IP 取得まで待つ。`address` が埋まっている |
| `PLAIN PERMISSION_DENIED not-applied` | 平文 HTTP はローカルアドレス宛だけ。`example.com` はソケットを開く前に拒否される |
| `SCHEME INVALID_ARGUMENT` | `http://` と `https://` 以外は受けない |
| `METHOD INVALID_ARGUMENT` | GET/POST/PUT/DELETE のみ |
| `BIGBODY LIMIT_EXCEEDED` | 送信本文は 4096 バイトまで。黙って切らずに拒否する |
| `CREDS PERMISSION_DENIED` | URL 内の `user:password` は受け取らない。ログに出さずに扱う方法がないため（§7） |
| `GET status=200 type=text/html trunc=false` | 4xx/5xx を含め status で返し、reject するのは transport と TLS だけ。ヘッダは小文字化済み |
| `GET TLS_ERROR`（時計未同期のとき） | 証明書の有効期間を検査できないので、検証を黙って省略せず TLS_ERROR にする（§11） |
| `GET OUT_OF_MEMORY ... free, ... largest block` | TLS ハンドシェイクに足りる連続領域がない。中断させずに数字で断る |
| `BODY <n> eof` | 本文は `read(1024)` の連続で取る。全量を RAM に置かない。`null` が終端 |
| `ACLOSE disconnected` | `close()` 後の `status()` は例外ではなく disconnected を返す（冪等） |
| `CLOSED INVALID_ARGUMENT` | 返した lease は再利用できない。掴んだままのオブジェクトは無害な抜け殻になる |
| `CHANGE disconnected` | リンクが落ちたときだけ出る。落ちたら再接続せずアプリに伝える |
| `DONE` | ここまで到達＝Promise 連鎖が全部 settle した。1つでも宙吊りなら出ない |

## 見るべき副作用

- `DONE` の直後に `wifi` タグの `LINK_DOWN free=` が出て、`LINK_UP free=` との差が
  リースを持っていた間のコスト。アプリが終了した場合も `app_stop()` →
  `pocket_net_reset()` が同じ経路を通る。
- 途中で **戻る** キーを押してアプリを殺しても、`LINK_DOWN` は必ず出る。出なければ
  リースが漏れている。
