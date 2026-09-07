# ioprobe — `pocket.io` の実装を実機で確かめる

`main/pocket_io.c` が**実際に強制している**ことだけを一つずつ叩いて、期待するコードと突き合わせる。
各行は `OK <名前> -> <結果>` か `HUH <名前> -> <結果>` を出す。`HUH` は実装か本READMEのどちらかが間違っている。

## 何も繋がっていない機体で安全である理由

- 外部ピンを**Highに駆動しない**。GPIOは `ext.int`（G4）を**入力**として読むだけ。
- 予約アドレスは開かない。開くのは `0x50` だけで、EXT/Groveに何も付いていなければNACKで終わる。
- SPIは `ext.spi`（G40/G14/G39, CS=G5）に4バイト送るだけ。microSDのCSはG12で触らないので、カードが刺さっていても無視される。
- UARTは `ext.uart`（TX=G13）に4バイト出すだけ。何も繋がっていなければどこにも届かない。
- IRは基板上のLED（G44）を38kHzで約14.6ms光らせる。目には見えず、機体の外に影響しない。
- **共有I²Cバスは1回も予約アドレスで開かれない。** キーボード・IMU・コーデックの通信は止まらない。

## 走らせ方

このアプリは `main/shell.c` の一覧には入っていない（このセッションは `main/pocket_io.c` と `main/pocket_io.h` 以外を触らない約束）。走らせる方法は2つ:

1. **Playground に貼る** — Playgroundのコードエディタに `ioprobe.js` の中身を貼って実行する。出力は Console 画面に出る。
2. **埋め込む** — 一覧に載せたい人が3行足す:
   - `main/CMakeLists.txt` の `EMBED_TXTFILES` に `"../apps/ioprobe/ioprobe.js"`
   - `main/shell.c` の `apps[]` と `app_details[]` に1行ずつ
   - `main/main.c` の `shell_app()` switch に1分岐
   （`test_settings.py` と `capture_home.py` は押下回数を数えるので、行を足すなら同じ変更の中で直すこと。）

ソースは3,006バイト。§14の「ソースのバイト数がゲストヒープを食う」制約に収まるよう、説明はすべてこのREADMEに置いてある。

## 各チェックが証明していること

| 行 | 期待 | 何の証明か |
| --- | --- | --- |
| `0x34 keyboard` / `0x69 imu` / `0x18 codec` | `PERMISSION_DENIED` | 共有バス上の3ドライバのアドレスは**open時に**断られる。IMUは `motion.c` が0x68/0x69のどちらでも取るので両方断る（0x68も同じ扱い） |
| `0x00 i2c-reserved` | `PERMISSION_DENIED` | I²C自身の予約域 `0x00-0x07` / `0x78-0x7F` も通らない |
| `hz 250000` | `INVALID_ARGUMENT` | §10の `100000|400000` は範囲ではなく2択。丸めずに断る |
| `port "lcd"` | `NOT_FOUND` | ポート名はホストが列挙したものだけ。パネルやFlashのピンは `ports()` に**そもそも載っていない**（§10の「予約して公開しない」） |
| `empty 0x50` | `IO_ERROR` または `TIMEOUT` | 何も居ないアドレスへの転送が失敗しても、バスは戻る。IDFのI²Cドライバが NACK をどちらで返すかはビルド依存なので両方を正解にしてある |
| `300B write` | `LIMIT_EXCEEDED` | 1転送256バイト（`maxTransferBytes`） |
| `timeoutMs 5000` | `INVALID_ARGUMENT` | 共有バスの待ちは50ms（`maxWaitMs`）が上限。**§14の提案値100msではない** — キーボードがその裏で待つから |
| `after close` | `CLOSED` | close済みハンドルのメソッドは世代不一致で断られる |
| `grove.g1 vs grove` | `BUSY` | `grove`(I²C, G2/G1) と `grove.g1`(GPIO) はピンを共有する。2つ目は黙って再設定せず断る |
| `ext.int reads 0/1` | — | G4を入力で読む。closeでフローティング入力に戻す（`closeState`） |
| `read, none sending` | `TIMEOUT` | §10の「空配列をEOFと混同しない」。受信0バイトは空配列ではなくTIMEOUT |
| `spi 4B` | `ok 4B` | 送信長と同じ長さが返る。MISOがフローティングなので中身は不定 |
| `spi 1024B in 1ms` | `TIMEOUT` | 4MHzで1024バイトは2.05ms。**転送前に**長さと clock から所要時間を計算して断る（`maxHoldMs`=20） |
| `ir 5kHz carrier` | `INVALID_ARGUMENT` | キャリアは20〜60kHz |
| `ir NEC lead-in` | `ok` | RMTで実際に送出し、完了割込みが `pocket_api_complete()` を打ってJSタスクがPromiseを解決する |

## 出力例で見るべきところ

- `io.i2c supported=true available=true` — 5面すべて `supported=true`。`available` はハンドルの空きで、機器の返事ではない。
- `i2c refuses 0x00-0x07,0x18,0x34,0x68,0x69,0x78-0x7f, waits <=50ms, scan=false` — 拒否リストと待ち上限が
  コメントではなく `capability.limits` に出ている。`scan=false` は「バス走査は提供しない」の明示。
- `port ...` の10行 — 公開されているポートの全部。ここに無いピン（LCD・Flash・USB・電源・strapping・I²S・SDのCS）は
  名前ごと存在しない。
