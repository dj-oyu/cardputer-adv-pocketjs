# App control v1 (2026-10-22)

旧 platform/native-app-control.md を末尾の節に吸収した。

Firmware and broker normative shared contract. Native endpoint available at HOME;
never a guest bridge session. No arbitrary source/path/raw serial.

## Transport

GS (0x1d), ASCII `C`, lowercase hex(body+CRC32), LF. Legacy GS `B` unchanged.
Reuse bounded bridge codec/CRC and usb_console_frame writer. Body header:
u8 version=1, u8 kind, u16 payload_length, u32 control_session, u32 command_id.
All integers little endian. CRC32 reflected 0xedb88320 over header+payload.
Max frame496/body492/payload480/wire995. No JSON dependency in firmware.

Kinds: CATALOG=1 OPEN=2 START=3 TAP=4 STATUS=5 SUSPEND=6 STOP=7 CLOSE=8;
response kind=128. Requests exactly60 bytes:
offset0 boot_id:u64; 8 run_id:u32; 12 seq:u32; 16 lease_fence:u32;
20 ttl_ms:u32; 24 app_id:char[32] (ASCII,NUL padded,max31);
56 arg:u8; 57 reserved[3]=0.
CATALOG uses seq as descriptor index (0-based), other fields zero, session/id0.
It returns current boot identity and descriptor/count; no serial open by list_apps.
OPEN uses known boot, run0, seq0, nonzero random session and nonzero lease fence,
ttl1..90000, command_id0, app_id empty, arg0. Repeated identical OPEN is idempotent,
never extends TTL; another session/fence is busy until cleanup completes.
START uses arg0=fresh(default),1=resume, registered app_id. Fresh discards dormant
guest, creates new run; resume requires same app and instance, else explicit rejection.
TAP arg:1 accept,2 back,3 up,4 right,5 down,6 left. No held or physical edges.
SUSPEND explicitly preserves guest/run or returns unsupported; STOP confirms guest
destruction. HOME_READY alone proves neither. CLOSE only succeeds after stop.
STATUS command_id0 reads session/run state; nonzero queries that command's ledger.
STATUS is read-only and never applies/replays input. Echo request id in header.
For START/TAP/SUSPEND/STOP/CLOSE positive command_id and increasing seq from1.
Identity includes boot/session/fence/run; START fresh requests run0, resume known run.
STOP/CLOSE remain permitted as bounded cleanup after command budget exhausted.

Response payload exactly120 bytes:
0 boot:u64;8 run:u32;12 command_seq:u32;16 fence:u32;
20 result:u8;21 state:u8;22 original_op:u8;23 arg:u8;
24 next_seq:u32;28 remaining_ttl_ms:u32;
32 catalog_index:u16;34 catalog_count:u16;36 flags:u32;
40 app_id:char[32];72 title:char[48] (NUL padded ASCII).
flags bit0 available; bit1 suspend supported; bit2 action.tap supported.
CATALOG descriptors bind actual launch sources; unsupported build-gated vmprobe
descriptor must be unavailable and START rejected. Unknown id=unknown_app.

Results: ok0, accepted1, consumed2, unknown_app3, unsupported4, stale_boot5,
stale_session6, stale_lease7, stale_run8, out_of_order9, conflict10, queue_full11,
blocked12, busy13, outcome_unknown14, limit15, expired16, invalid17,
teardown_failed18. States: no_session0,ready1,running2,suspended3,stopped4,needs_review5.
Accepted means admitted to finite owner queue; consumed means owner delivered to
defined action path, not JS/render/save completion. Recheck blocked/identity at
consumption. Same command id+identical bytes returns cached outcome; changed bytes
conflict. Sequence high-water survives ledger lifetime; no evicted command accepted.

## Limits and lifecycle

One session, request inbox4, dedup ledger64 entries/no eviction during session,
max60 user commands/actions per plan (reserves lifecycle slots),10 commands/s,
TTL<=90000ms, appid<=31 ASCII, no session extension. Broker one outstanding wire
command; retransmission is STATUS only after lost ACK. Never auto replay/reopen/
relaunch after disconnect/restart; unknown evidence=outcome_unknown.
Device TTL always tears down active or suspended controlled guest from UI owner.
Cancellation/timeout stops new input, requests bounded STOP then CLOSE under the
original authority/cleanup agreement; unconfirmed teardown/port close=needs_review.
Console flood does not evict ledger/ACK state; ACKs share serialized frame writer.

Broker retains existing auth/immutable plan/lease/jobs/one serial worker and
principal-bound bounded log cache. Add test=app_session to prepare/execute workflow,
list_apps (cached catalog with freshness, never opens device), app_command,
read_app_events (cursor,oldest_seq,next_cursor,has_more,gap,dropped_count,expiry).
Separate bounded control events from logs; 128 control events and256 log events,
per-line<=1024B,total logs<=262144B,retention<=existing bounded log TTL.
Existing redaction applies. Untagged console attribution=observed_during_run.
Plan fixes app/start/allowed actions/max_commands/rate/duration/log delivery and
end=stop/teardown including dormant eviction. No per-key approval.
Use common mock/host client for local.stress and ordinary local.hello; preserve
legacy benchmark parsers/quality gates, no quality-pass claim from transport.

## Review amendments (normative, supersede earlier offer wording)

control_session is DEVICE issued: CATALOG response header returns current offer.
Initial offer is random nonzero, increments on close/TTL/end; exhaustion/wrap
refuses new OPEN until reboot. OPEN must match offered value; only an ACTIVE
identical OPEN is idempotent. Never reopen a retired offer or reset its ledger.
Retain stopped ledger for STATUS until next new OPEN. Closed/expired requests
cannot acquire another guest. Match existing ledger id/content before current
run validation, so START(run0) duplicates remain queryable after actual run1.

CATALOG special index0xffff returns metadata: reply app_id[32] contains RAW
firmware app_elf_sha256 bytes (sole exception to ASCII), title[48] holds version.
Descriptor count is still present. Cache canonical descriptor snapshot plus
firmware hash/version/time. If no previously observed cache, prepare explicitly
binds source=unobserved/expected=null; handshake validates and records actual
firmware/catalog, without claiming prior firmware verification. Observed cached
binding is immutable in plan and must match handshake before START.

Non-frame console stdout/stderr escapes raw GS as visible `\\x1d`. Only framed
writer may emit raw GS. Thus log bytes are not a command/ACK channel; CRC alone
is not authentication. Preserve framed legacy B semantics and add host tests.
Inbox-full has a reserved rejection response path; a silent drop is not an
explicit queue_full acknowledgement. Control ledger never shares log capacity.

## ネイティブ実装の挙動と検査（旧 platform/native-app-control.md）

上の wire 契約の firmware 側の実装メモ。2026-10-27 に通常 main（`45a06e0`）へ app-control と USB console を抽出し直したときの記録のうち、挙動と検査の手順だけを残した（当時の PR の積み方・配置手順は削除）。以下は元の head の仕様と検証履歴で、現在の head の受入結果ではない。

- **名前空間と所有**: native の `C` は HOME で使え、ゲストの `B` ブリッジがリセット・中断しても生きる。既存の入力タスクが両方の名前空間を解析し、ゲストの生成・再開・中断・tick・破棄は UI の所有者だけが行う。
- **カタログ**: 前面アプリ 11 個の ID を、実際に埋め込まれたソースと安定した registry manifest に結びつける。エディタ・動的ワーク・プレイヤー・オーバーレイは利用不可と広告し、これらの型付き起動は未実装。VM PROBE は診断ワークロードを含むビルドでも利用不可と広告する。公開の起動が HELLO に落ちることはない。
- **fresh / resume**: `fresh` は休眠ゲストを追い出して新しい run を作る。`resume` は現在の制御セッション内の SUSPEND の後に同じゲスト世代・run を保つ。新しい broker セッションは管理外の休眠ゲストに接続できず、初回 `start=resume` は broker が明示的に未対応とする。BACK は既存の action 経路で中断しうる。STOP は制御中のゲストを破棄し、HOME_READY から成功を推測しない。別インスタンス（同じ app ID でも）の物理・ゲスト起動は制御入力を無効にする。後始末は無関係な置換ゲストの破棄を拒み `needs_review` を報告する。
- **上限**: firmware は利用者の操作を最大 60 件と有界な後始末を 64 エントリの台帳に収める。broker の計画は START が 1 操作を使うので対話操作を 59 件に絞る。キューは 4 リクエスト＋満杯拒否用の別の mailbox 1 つで、安定した `queue_full` の受領を返す。両方を超える敵対的な連打は ACK を失いうるので、クライアントは STATUS を聞き、証拠が無ければ `outcome_unknown` と報告し、入力を再送しない。拒否・消費の受領はコンソール出力と別で、ログの洪水で追い出されない。
- **TTL**: UI はコマンドの間と、ブロックしうる所有者の呼び出しの後に固定のデバイス TTL を確認する。実行中のゲストのターンは先取りできない（既存のゲストのウォッチドッグが担う）。期限切れは制御中のゲストを破棄し、デバイス発行のセッション offer を退役させ、古い OPEN/START の再送を防ぐ。再接続・再起動・再送はしない。シーケンスと受領の保証はファームの再起動をまたがない。
- **GS のエスケープ**: フレーム外の stdout/stderr は生の GS を可視の `\x1d` にするので、ゲストの文字列が native の ACK や旧フレームを偽造できない。生のフレームを書けるのは `usb_console_frame` だけ。CRC は破損の検出で認証ではない。認可・不変の計画・lease fencing・ログのマスクは broker にあり、firmware に二つ目の認証は無い。

### ホストの検査

```sh
python tools/test_app_control.py
python tools/test_usb_console.py
python tools/test_session_dispatch.py
python tools/test_app_control_broker_interop.py --broker-source /path/to/esp-device-broker-mcp
```

1 つ目は実際の native コントローラと共有の B/C パーサを、最適化・strict aliasing・ASan/UBSan でコンパイルし、ライフサイクル・同一性、キューとシーケンスの上限、ACK 喪失と STATUS、重複と衝突、レートとコマンドの上限、ブロックするターンをまたぐ TTL、置換されたゲスト世代、旧 B との隔離を調べる。interop 検査は broker の Python エンコーダ／デコーダを実際の C のファーム状態に対して実行する（シリアルのハードウェア無し）。既存のベンチマークのパーサと品質ゲートは独立で、転送の成功はベンチマーク品質の成功を意味しない。GCC の `-fstack-usage` はコンパイラ推定のスタックで、実測の最大値ではない。ファームは独立にレビューし、ビルドし、実機で検証してからマージ・書き込みする（ESP-IDF は 6.0.1 固定）。
