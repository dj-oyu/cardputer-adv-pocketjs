# Wi-Fi の自動起動（起動時の時刻同期と、接続サービスの土台）

2026-09-30、ブランチ `vm/wifi-autosync`（`vm/main` 964f634 から）。依頼は「Wi-Fi の自動起動。時刻同期を自動で、将来は背景でオンライン前提のものも」。**実機は使っていない**（別の作業が COM3 を使用中）。この文書の数値は、**実測**（過去に実機で測られ、コードのコメントか文書に記録された値。出典を書く）・**計算**（実測値からの算術）・**推定**（根拠はあるが測っていない）を区別して書く。実機で測る項目は §8 にまとめた。

## 1. 結論

- **常駐のオンラインは既定にしない。** 入れたのは「一過性の自動時刻同期」と「参照カウント付きの接続サービス（ネイティブのみ）」の2層。
- **自動同期**: 資格情報が保存されていて、設定 AUTO TIME SYNC が ON（既定）のとき、**ホーム画面が10秒アイドル**になったら別タスクで 接続 → SNTP → 無線停止。成功したら次は12時間後。失敗は 1分・5分・30分で再試行し、**連続4回の失敗でそのブートは打ち切る**（設定を ON にし直すと再開）。1ブートの試行は最大32回（アプリへ譲った中断は数えない。§11.3）。
- **アプリが優先**: 「アプリ・別画面・オーバーレイ・診断が動いている間は始めない」と「ゲストを作る直前に中断して、無線を止め、ヒープが戻るまで待つ」の**両方**を入れた（§3.3）。ゲームがホームの無線と同時に存在することはない。
- **接続サービス** `net_service_acquire(reason)` / `net_service_release(reason)`。時刻同期が最初の利用者。`pocket.net` の lease とは**無線を共有しない**が、**所有者付きの停止**にして、互いのリンクを止められないようにした（§4）。
- 静的 DIRAM は **+224 B**（計算: クリーンな HEAD とのマップ比較）。**動的なコストの本体は実機で測る**（ピーク、そして「一度無線を上げると約4.8KiB戻らない」がブートごとに必ず払われるようになる点、§5.3）。

## 2. 読んだ事実と、依頼文との食い違い

| 項目 | 依頼文の前提 | コードの事実 |
| --- | --- | --- |
| 同期済みの印 | `solar_time_set_synchronized(true)` | 手動同期は `wifi_time.c` から `sys_clock_set_synchronized(true)` を直接呼ぶ。`solar_time_set_synchronized()` はそれへの転送だけ（`scene/solar_time.c:7`）。自動同期も `sys_clock_set_synchronized(true)` を**成功時に1か所だけ**呼ぶ。`false` はどこからも呼ばない（従来どおり） |
| リンクの失敗 | — | 接続に失敗したリンクは `WIFI_TIME_LINK_DOWN` で終わる。`FAILED` は「一度上がって失われた」だけ。サービスは「保持者がいるのにリンクが無い」を FAILED と読む |
| 停止の競合 | — | `pocket_net_reset()` / `pocket_net_suspend()` は、自分の lease が無くても `wifi_time_link_state()==CONNECTING` なら `wifi_time_link_stop()` を呼んでいた。リンクの持ち主が増えると、**アプリの終了が他人のリンクを止める**。今回、所有者付きにした（§4） |
| 接続中の停止 | — | `associate_and_wait()` は15秒を1回で待っており、接続中に停止を頼んでも最大15秒止まらなかった。100 ms 刻みに分け、リンクの停止要求を見るようにした（手動同期の振る舞いは不変） |
| Wi-Fi 画面の表示 | — | リンクはアドレスを得ると `status.state=OK` を書き、Wi-Fi 画面はそれを「CLOCK SET FROM NTP」と表示する。**`pocket.net` の lease の後でも同じ表示になる既存の誤表示**（今回は直していない）。自動同期は SNTP の結果で status を上書きし、中断したときは IDLE に戻す（`wifi_time_status_settle()`） |

## 3. 自動時刻同期（`main/pocket/net_autosync.c`）

### 3.1 いつ始まるか

`ui_task` が毎フレーム `net_autosync_poll(eligible, key)` を呼ぶ。**eligible** は次のすべて:

- JS アプリが動いていない、画面がホーム、ホームのモーダル（works／フォルダ／ファイルピッカー）が無い、エラー表示が無い
- オーバーレイが**起動しようとしていない**（`overlay_starting()`、今回 `ui/overlay.c` に足した getter）
- 背景音楽が鳴っていない（約62KB を持ち、締め切りのある唯一のもの。無線の影響は未測定なので避けた）

~~眠っているアプリ（常駐中断）は除外しない。~~ **2026-09-30 のレビューで変更: 眠っているアプリがいる間は同期しない**（方針が `AUTOSYNC_APP_ASLEEP` で1分ずつ延期する。§11.2）。`main/main.c` の eligible の式は変えておらず、判定は `net_autosync_poll()` が `app_dormant_id()` を読んで行う。**動いているオーバーレイ**も除外しない（§3.4）。

この状態が**キー入力なしで10秒**続き、方針（§3.2）が「期限到来」と言い、資格情報があり、眠っているアプリが無く、空きが `AUTOSYNC_MIN_FREE` 以上かつ最大連続ブロックが `AUTOSYNC_MIN_LARGEST` 以上なら（§11.2）、優先度4（UI タスクの5より下）の `autosync` タスクを立てる。資格情報は NVS を読むので、「ホームを離れたら無効化し、次に必要になったとき1回読む」キャッシュにした（資格情報は Wi-Fi 画面でしか変わらず、それはホームではないので正確）。資格情報が無ければ何も言わずに1分後にまた見る。

ブート直後の忙しさ（フォント・SKK・背景シーンの初期化）は、最初の `HOME_READY` から10秒のアイドルを待つことで避けている。

### 3.2 方針（純粋関数、ホストで試験）

| 定数 | 値 | 根拠 |
| --- | --- | --- |
| `AUTOSYNC_IDLE_MS` | 10 s | 操作中のメニューや、テストスクリプトのキー間隔（最大2.5 s）と重ならない長さ |
| `AUTOSYNC_BACKOFF_*` | 1分・5分・30分 | 依頼の例どおり |
| `AUTOSYNC_MAX_FAILURES` | 連続4回 | 最初＋再試行3回。1回あたり最大約25 sの無線と約48KBのヒープを使うので、届かない網に5回目は払わない |
| `AUTOSYNC_RESYNC_MS` | 12 h | 依頼の12〜24 hの下端。RTC の日差は分単位に達しないが（推定）、長時間稼働で1日2回は安い |
| `AUTOSYNC_RETRY_MS` | 1分 | 無線が他で使用中・空き不足・アプリが眠っているとき。失敗には数えない |
| `AUTOSYNC_ABORT_RETRY_MS` | 5分 | 中断（アプリ・画面へ譲った）の後。試行にも失敗にも数えない（§11.3） |
| `AUTOSYNC_MAX_ATTEMPTS` | 32 / ブート | 有限にする安全網。中断は数えない（§11.3） |
| `AUTOSYNC_CONNECT_WAIT_MS` | 20 s | 外側の上限。リンク自身の期限は15 s（`CONNECT_TIMEOUT_MS`、3回まで再接続） |
| `AUTOSYNC_SNTP_WAIT_MS` | 10 s | 手動同期の `SNTP_TIMEOUT_MS` と同じ。100 ms 刻みで中断を見る |

手動同期（Wi-Fi 画面）で既に時計が合っていれば、最初の試行は無線を使わずに成功扱いにし、次は12時間後。

### 3.3 アプリとの優先関係（なぜ両方か）

候補は (1) アプリ起動時に同期を中断して無線を止めてからゲストを作る、(2) アプリが動いている間は同期を始めない。**(2) だけでは足りない**: 同期は最大約25 s 続くので、その間にメニューから起動されたアプリは、無線が約48KB（§5.1）を持ったままゲストを作ることになる。**(1) だけでも足りない**: アプリ実行中に始めれば、ゲームのネイティブ heap（DERBY WATCH の最小空き 33,928 B、実測、`docs/apps/derby-watch.md`）に48KBの無線は入らない。だから両方にした。

`net_autosync_yield(why)` は「中断を頼み、同期タスクが無線を落として（リンクの `wifi_time_busy()` が偽になるまで待って）終わるまで、最大3 s ブロックする」。同期が動いていなければ即座に戻る（原子変数の読み1回）。呼ぶ場所はすべて `main/main.c`:

| 場所 | why | 理由 |
| --- | --- | --- |
| `begin_run()` の先頭 | `app` | 前景アプリの起動、眠っているアプリの再開、`workspace.run` の後継。前景ゲストはすべてここを通る |
| `enter()`（ホーム以外へ） | `screen` | エディタ類はゲストを作り、Wi-Fi 画面は到着時にスキャンを始める（無線が使用中だと BUSY になる） |
| 診断文字（USB）の実行前 | `diagnostic` | ゲストを作るか、ヒープを測る |
| `overlay_tick()` の前、`overlay_starting()` のとき | `overlay` | オーバーレイの起動は空きで門を掛け、断ると人が選び直すまで REFUSED のまま残る |

`main/app_session.c` は**触っていない**。ゲスト生成の共通点（`app_start_test()`）に入れる案もあったが、オーバーレイの門は `app_start_test()` より前で空きを測るので、そこでは遅い。呼び出し側に置けば4か所で済み、スケジューラの中核に手を入れずに済む。

中断の遅延は「刻み（最大100 ms）＋無線の後片付け（`esp_wifi_stop`/`deinit`/netif の破棄）」。後者は未測定（§8 (d)）。3 s を超えたら `AUTOSYNC_YIELD ... LATE` を出して先へ進む（ゲストは空きの少ない状態で作られ、失敗すれば通常の `START_FAILED` になる）。

### 3.4 オーバーレイが動いているとき

DESK CLOCK は時計なので、同期の恩恵が最も大きい。オーバーレイは起動後に `OVERLAY_FREE_FLOOR`（56KiB = 無線を上げる費用、実測 2026-09-07）以上の空きを残すことが保証されており、その床はまさに「オーバーレイがあっても無線を上げられる」ためにある（`ui/overlay.c`）。そこで動作中のオーバーレイは除外せず、代わりに `AUTOSYNC_MIN_FREE`（64KiB）で門を掛けた。**確信は低い**: 同期中に無線が約48KB を取ると、オーバーレイのゲストに残る伸び代は（空き−48KB）になる。オーバーレイが OOM で FAULTED になるなら、オーバーレイ動作中は同期しない方に倒す（§8 (c)）。

### 3.5 止め方

設定の **AUTO TIME SYNC**（OFF/ON、NVS `home/autotime`、既定 ON）。OFF にすると実行中の試行にも中断を頼む（待たない）。ON に戻すと方針を初期化するので、「このブートは諦めた」後にもう一度試させる方法でもある。

### 3.6 ログの印

`autosync` タグ: `AUTOSYNC_START free= largest=` / `AUTOSYNC_DONE <ok|failed|aborted|busy> stage= ms= free_before= min_free= free_after=` / `AUTOSYNC_NEXT in_s=` / `AUTOSYNC_GAVE_UP` / `AUTOSYNC_YIELD <why> waited_ms= [LATE] free=` / `AUTOSYNC_DEFERRED low_memory free= floor= largest= block=` / `AUTOSYNC_DEFERRED app_asleep <id>` / `AUTOSYNC_SKIP already synchronized` / `AUTOSYNC_ENABLED`。`net_service` タグ: `NET_ACQUIRE` / `NET_RELEASE` / `NET_ACQUIRE_REFUSED` / `NET_RELEASE_UNMATCHED`。`wifi` タグの既存の `RADIO_INIT ... cost=`（2026-09-30 に末尾へ ` largest=` を追加） / `LINK_UP free=` / `LINK_DOWN free=` と、新しい `LINK_STOPPED while connecting`。SSID は既存の `LINK_START ssid=` に出る（AP が放送する名前で、従来どおり）。**パスフレーズはどこにも出ない。**

`min_free` は `heap_caps_monitor_local_minimum_free_size_start/stop` による**その試行の区間の本当の最小値**で、§5 のピークを実機で置き換えるための数字。MEGADEMO のトレースビルドが同じ監視を使っているときは「(shared monitor)」と付く。

## 4. 接続サービス（`main/pocket/net_service.c`）

```c
void net_service_init(void);                       // ブート時、1回
esp_err_t net_service_acquire(const char *reason); // 保持を1つ数え、必要ならリンクを起こす
esp_err_t net_service_release(const char *reason); // 1つ返す。最後の1つでリンクを止める
net_service_state_t net_service_state(void);       // OFF / CONNECTING / UP / FAILED
unsigned net_service_holders(void);
void net_service_describe(char *out, size_t size); // "time=1 bg=2"
bool net_service_listen(fn, user); void net_service_unlisten(fn, user);
void net_service_pump(void);                       // UI タスクが毎フレーム。変化1回につき1回通知
```

- 理由の名前ごとに数える（最大4名）。同じ名前の二重 acquire は2と数え、release も2回要る。**対応の無い release は拒否**して記録する（数を負にして他人の無線を止めることはしない）。
- 数えることとリンクの起動・停止は、1つの mutex（静的領域、84 B、map で確認）の中で行う。起動は `wifi_time_link_start_for()` が停止要求を消すので、ロックの外で行うと「acquire が数えた直後に release が0と見て停止 → その停止が起動に消される → 誰も持たないリンクが残る」が起きうるため。
- 保持者がいるのにリンクが失敗したら FAILED。再試行はしない（保持者が acquire し直すと起こし直す）。最後の release の直後、まだ後片付け中のリンクへの acquire は `ESP_ERR_INVALID_STATE`（すぐ消えるリンクに数えない）。
- 通知は `net_service_pump()` を回すタスク（UI タスク）の上で呼ぶ。リンクのタスクからは他人のコードを呼ばない。

**`pocket.net` の lease との整合**: `wifi_time.c` のリンクに所有者（`WIFI_TIME_LINK_APP` / `WIFI_TIME_LINK_SERVICE`）を付け、`wifi_time_link_stop_for(owner)` は**所有者のときだけ**効くようにした。`pocket_net.c` は1行も変えていない — 従来の `wifi_time_link_start()` / `wifi_time_link_stop()` が APP 所有の版になっただけ。両者は従来どおり1つのロック（`running`）で排他で、後から来た方が `ESP_ERR_INVALID_STATE`（アプリには BUSY、retryable）を受ける。今回の自動同期はゲストを作る前に必ず譲るので、両者が同時に存在することは無い。**共有（アプリの lease がサービスのリンクに相乗りすること）は今回やっていない**。必要になるのは §7 の常駐サービスで、要る変更は §7.3。

`pocket.net` への副作用は1つ: 接続中の `acquire` を取り消したとき（cancel・timeout・アプリ終了）、従来は接続を最後まで待ってから（最大15 s）落ちたが、今は100 ms 刻みで止まり、状態は DOWN で終わる。アプリから見える結果（取り消されたという reject）は同じ。

## 5. ヒープ

### 5.1 同期の一過性のピーク（見積もり）

| 内訳 | 値 | 種別 |
| --- | --- | --- |
| `esp_wifi_init` | 27,192 B | 実測（2026-09-07、`pocket_net.c` の `NET_RADIO_MIN_FREE` のコメント） |
| netif とイベントループ | 約21,000 B | 実測（同上） |
| 無線を上げる床 `NET_RADIO_MIN_FREE` | 56 KiB | 実測に余裕を足した値（同上） |
| 接続・DHCP・SNTP 中の動的 RX/TX バッファ、タスク stack（`autosync` 4 KiB、`wifi_link` 4 KiB） | 8 KiB 程度 | **推定**。stack の 8 KiB は計算、動的バッファは未測定 |
| 自動同期の門 `AUTOSYNC_MIN_FREE` | 64 KiB | 上の和（床＋推定） |
| 自動同期の門 `AUTOSYNC_MIN_LARGEST`（最大連続ブロック） | 16 KiB | **暫定（推定）**。§11.2 |

ホーム画面の空き約274KiB（実測、CLAUDE.md）に対し、64KiB の門は十分に下（計算: 残り約210KiB）。ホームで門が効くのは、オーバーレイ（56KiB 保証）がいるときだけ（眠っているアプリがいるときは門より先に延期する。§11.2）。**この64KiB は `AUTOSYNC_DONE min_free=` の実測で置き換えること。**

### 5.2 無線を止めた後

`esp_wifi_stop`/`deinit`/netif 破棄の後に戻る量は、既存の記録では「一度無線を起動すると約4.8KiB は戻らない」（`linkRetainedBytes` 4,915 B、実測、`pocket_net.c`。`esp_netif_deinit()` が IDF v6.0.1 で `ESP_ERR_NOT_SUPPORTED` のため、LWIP のタスクと緩衝が残る）。

### 5.3 この変更で**毎ブート**払うようになるもの

従来、Wi-Fi を使わないブートは4.8KiB を払わなかった。自動同期が ON で資格情報があるなら、**最初のホームアイドルで必ず払う**。DERBY WATCH の最小空き 33,928 B（実測）から単純に引けば約29KB（計算）。さらに LWIP の常駐分がホームのアイドル時点の空きのどこに置かれるかで、ゲームが起動時に要る**連続領域**が割れる恐れがある（MEGADEMO は起動時に10KBの連続領域が取れずに失敗した前例がある）。**これが今回の変更の実害の候補の筆頭**で、§8 (c) で測る。悪ければ、選択肢は (i) 同期の前に何もしない（受け入れる）、(ii) ブート直後、ホームの背景を描く前に同期して LWIP を低位アドレスに置く、(iii) 既定を OFF にする。決めるのは測ってから。

## 6. 設定の行と押下回数の契約

- `main/ui/shell.c` の `settings[]` の**末尾**（index 6、VOLUME の後）に `AUTO TIME SYNC`（`toggles` = OFF/ON、NVS キー `autotime`）を足した。既存の行の index は1つも動かない。
- `LOADED` / `VALUE` の行は NVS キーから作られるので、末尾に ` autotime=%u` が付く。`test_settings.py` の正規表現 `background=(\d+) fps=(\d+) sound=(\d+)` は先頭一致なので影響しない。マーカー文字列は1バイトも変えていない。
- `tools/test_settings.py`: 行6を開いて値を反転 → `VALUE ... autotime=` を確認 → 元に戻す、を追加（装置の設定は元の値で終わる）。後始末の `finally` の「上へ」の回数を5→7（最後の行が index 6 になり、5回では WI-FI の上で Enter して Wi-Fi 画面へ出てしまう）。
- `tools/capture_home.py`: 設定の移動は行0〜2だけなので押下回数は不変。代わりに、計測窓の間に `AUTOSYNC_` 行が出たら `RADIO` として記録に残す（10秒アイドルで同期が走ると、その PERF は静かなホームの値ではない）。
- 実機テストは後段で実行する（本作業では未実行）。

## 7. 将来の「背景でオンライン前提」

### 7.1 形

常駐中断（`docs/vm/app-suspend-design.md`）の一段先として、アプリが眠っていても動く JS の背景サービスが `net_service_acquire("bg:<appId>")` を持ち続け、常時接続を要求する。今回 JS 面（capability・API）は作っていない。

### 7.2 メモリを食うアプリの起動をどう扱うか

**決定（2026-09-30、ユーザー）: B を採る。優先度は「常時接続 < 画面を占有している前景アプリ」。** 前景アプリが起動するとき、その必要量（マニフェストの宣言、または前回実行の実測ピーク）に対して、空きが余裕を含めて足りるなら常時接続と共存させ、足りなければ A（起動前にリンクを明け渡し、前景の終了後に張り直す）に落ちる。どの場合も、前景アプリの起動が常時接続のために断られること（C）はない。今回入れた自動時刻同期は、この規則の最も強い形（常に A）で動いている。**実装は、JS の背景サービスの API と、マニフェストの必要量の宣言を作るときに行う（今回は未実装）**。判断に要る実測は §7.4。以下は決める前の候補の比較で、記録として残す。

（候補。決めるのはユーザー — 決定済み: B）

| 案 | 内容 | 利点 | 欠点 |
| --- | --- | --- | --- |
| A. 起動前に明け渡す | 前景の起動時にサービスのリンクを一時停止（保持者には FAILED/一時停止を通知）、前景の終了後に張り直す | 今回の自動同期と同じ規則で、ゲームは常に無線無しの heap を得る | 背景サービスは前景アプリの間オフライン。再接続に数秒（未測定）と、張り直しのたびの一過性ピーク |
| B. 宣言量で判定 | マニフェストに前景アプリの必要量（または前回実行の実測ピーク）を持ち、「空き−必要量 ≥ 余裕」なら共存、足りなければ A に落ちる | 軽いアプリの間は接続が続く | 必要量の宣言・記録の仕組みが要る。見積もりが外れると前景が START_FAILED |
| C. 起動を断る | 常時接続の保持者がいる間、重いアプリは NO ROOM | 実装が最小 | ゲームが起動できない。利用者から見て原因が遠い |
| D. 能力で分ける | `pocket.net` を使うアプリ（lease を共有する）だけ共存、それ以外は A | ネットを使うアプリには自然 | lease の共有（§7.3）が前提 |

### 7.3 共有に要る変更（D か B を選ぶ場合）

`pocket_net.c` の lease をサービスの保持者にする: `js_acquire` → `net_service_acquire("app")`、`lease_release` → `net_service_release("app")`、失敗経路での release、`LINK_LOST` をサービスの FAILED として両方へ通知、サービス側の再接続の方針（バックオフ）。`wifi_time.c` のリンクはサービスだけが持つ形になる。

### 7.4 判断に要る実測

1. 接続を保持したままの空き（`LINK_UP free=`）と、通信量で動的バッファが増えたときの最小（アイドル10分、HTTP 1回ずつ）
2. `esp_wifi_stop` 後に戻る量（`LINK_DOWN free=` と保持前の差。既知の4.8KiB がそのままか）
3. ゲームごとのネイティブ最小空きと最大連続領域を、無線保持あり／なしで（DERBY WATCH、BIG WAVE、MEGADEMO）
4. 張り直し（A）にかかる時間と、その間のピーク

## 8. 実機で測る項目と手順

前提: 資格情報を保存した機体。`idf.py -B build_wifiauto -p COM3 flash monitor` のログを保存する。SSID/PSK をログ・文書・コミットに書かない（ログの `LINK_START ssid=` は伏せてから共有する）。

| | 項目 | 手順 | 見るもの |
| --- | --- | --- | --- |
| (a) | ホームの空き（自動同期あり／なし） | ブート→ホームで放置。AUTO TIME SYNC ON と OFF で1回ずつ。`python tools/memlog.py --map build_wifiauto/cardputer_pocketjs.map --port COM3 --check` をブート直後・`AUTOSYNC_DONE` の直後・5分後に | ON の `idle_free` が OFF より約4.8KiB 低いか（§5.2）。`AUTOSYNC_DONE free_after` |
| (b) | 同期中のピークと戻る量 | (a) の ON のログ | `AUTOSYNC_START free=` → `min_free=` の差（一過性ピーク）、`RADIO_INIT cost=`、`free_after` と `free_before` の差（戻らない量）。`AUTOSYNC_MIN_FREE` の推定8KiBをここで置き換える |
| (c) | 同期中・同期後のゲーム | ①同期後（`AUTOSYNC_DONE ok`）に DERBY WATCH・BIG WAVE・MEGADEMO を起動。②`AUTOSYNC_START` を見てすぐ各ゲームを起動。③DESK CLOCK を ON にして放置し同期させる | `START_FAILED`・OOM・`OVERLAY_STOPPED FAULTED` の有無、各ゲームのネイティブ最小空きと最大連続領域（DERBY は `internalFreeBytes`、MEGADEMO は起動時の10KB）。OFF のブートとの差 |
| (d) | 中断の挙動と遅延 | `AUTOSYNC_START` を見てから、接続中（1〜2 s 後）と SNTP 中（`LINK_UP` の直後）に Enter でアプリを起動。Wi-Fi 行で Enter も | `AUTOSYNC_YIELD app waited_ms=`（`LATE` が出ないこと）、続く `AUTOSYNC_DONE aborted`、アプリの `RUN ... ESP_OK`。1分後の再試行 |
| (e) | ブート時間 | ブートから `HOME_READY` までと、最初の10 s の fps（`PERF`）を ON/OFF で | 同期はアイドル10 s 後に始まるので `HOME_READY` までは不変のはず。同期中の `PERF` の低下 |
| (f) | 時刻が合うこと | 同期前後で `pocket.clock` か SOLAR SAIL の表示、または `SYNC`/`AUTOSYNC_DONE ok` 直後の `date` 相当 | UTC が合う。`sys_clock` の synchronized が立つ（天体が DEMO から実時刻に切り替わる） |
| (g) | 乱数の観察 | 同期の前と後に `pocket.random.seed()` を数回 | 値が毎回違うか（RF 有効の副産物の観察。設計の根拠にはしない、common-api.md §8.1） |

あわせて `python tools/test_settings.py --port COM3`（行6の反転と復元、`finally` の7回上）と `python tools/capture_home.py --port COM3` を流す。

## 9. 検証（今回やったこと）

- **ホスト試験** `bash tools/build_net_autosync_test.sh`（WSL、ASan/UBSan、`tools/test_net_autosync.c`）: 実物の `net_service.c` と `net_autosync.c` の方針・試行を、偽の無線（接続・失敗・後片付けの時間を台本で）と偽の時計で。資格情報の有無、アイドル待ち、空きの門、バックオフ（1/5/30分）と連続4回での打ち切り、成功でのリセットと12時間の再同期、中断・BUSY は失敗に数えない、試行の上限32、接続中・SNTP 中・直前の中断とその遅延（刻み＋後片付け以内）、SNTP のタイムアウト（10 s ちょうど）と開始失敗と最終エラー、**`set_synchronized` が成功時だけ1回**、SNTP を release より前に止めること、release 後に無線が落ちてから戻ること、参照カウントの対称性・二重 acquire・release 過多の拒否・名前表の満杯・失敗後の再 acquire・後片付け中の acquire 拒否・アプリのリンクがあるときの拒否、状態通知の1回性。全件通過。わざと壊した版（成功以外でも同期印、SNTP 中の中断確認なし、バックオフの添字、無線停止を待たない）では、それぞれ該当の検査が落ちることを確認した。
- **既存のホスト試験**: `tools/kasane_contract/run.sh`（全体 EXIT=0。途中の `run_megademo_app_host.py` は WSL の git が Windows で作った worktree を読めず止まるので、スクリプトの注記どおり `git show HEAD:apps/kasane/proc_megademo.js` を `.cache/kasane_megademo_app/` へ先に置いた。今回の変更とは無関係）、`tools/test_overlay.c`（`main.c` の形を読む検査を含む）、`tools/test_menu_rows.c`、`tools/test_codeedit.c` が通る。
- **ビルド** `idf.py -B build_wifiauto build`: 警告なし（既存の `flower.c` の未使用変数を除く）。`nm` で `net_autosync_poll` / `net_autosync_yield` / `autosync_attempt` / `net_service_acquire` / `overlay_starting` / `wifi_time_link_stop_for` がバイナリに入っていることを確認。
- **DIRAM**（`tools/memlog.py`、クリーンな HEAD のビルドとのマップ比較）: 172,204 → 172,428 B（+224）。`net_service.c.obj` +160（mutex の静的領域・表）、`net_autosync.c.obj` +51、`heap_caps.c.obj` +16（区間最小の監視関数が IRAM に入った）、`wifi_time.c.obj` +4（所有者）。flash +3,648 B。
- **試験していないもの**: FreeRTOS のタスク・`net_autosync_poll()` のアイドル判定・`net_autosync_yield()` の待ち・`wifi_time.c` の所有者付き停止と刻み待ち（ESP-IDF 依存でホストに載らない）。§8 で見る。

## 10. 確信の低い点

- `AUTOSYNC_MIN_FREE` の8KiB（動的バッファ）は推定。
- 動作中のオーバーレイと同期を共存させた判断（§3.4）。
- 毎ブート4.8KiB 失う件（§5.3）がゲームの連続領域にどう効くか。
- 中断の後片付けの時間（3 s の上限に対して）。
- 同期中の CPU（Wi-Fi ドライバのタスク）がホームの fps とキー応答に与える影響。
- デバッグ用のプローブ（`KASANE_*_PROBE`、`CONFIG_KSN_DEVICE_PROBE`）はホームで重い処理を走らせるが、譲らせていない（出荷ビルドでは無効）。

## 11. 独立レビューへの対応（2026-09-30）

ブランチ `vm/wifi-review-fixes`（`vm/main` ff7d809 から）。Fable（SLOW）の独立レビュー（総合「条件付き」）の指摘5件を、コードと ESP-IDF v6.0.1 のソースで確かめてから直した。**実機は使っていない**（COM3 は別の作業が使用中）。表記は §冒頭と同じく実測・計算・推定を分ける。

| # | 重大度 | 指摘 | 検証 | 対応 |
| --- | --- | --- | --- | --- |
| 1 | 重大 | UP の後に AP が落ちると、SNTP を待つ autosync タスクの足元で link タスクが `esp_netif_sntp_deinit()` を呼ぶ | **正しい**。さらに2つ見つけた（下） | `wifi_time.c` に SNTP 用の mutex。autosync は落ちたリンクを1刻みで検出 |
| 2 | 中 | 空きの門が合計だけで、最大連続ブロックを見ない。眠っているアプリがいても同期する | 前半は正しい。後半の「≒49KiB」は背景の二重計上の可能性（下） | 連続ブロックの門（暫定16KiB）と、眠っているアプリがいる間の延期 |
| 3 | 低 | 中断が32回の上限に数えられる | **正しい**（1日で上限に届く） | 中断は試行にも連続失敗にも数えない。中断後は5分空ける |
| 4 | 低 | 3 s の yield が尽きた（LATE）後、Wi-Fi 画面のスキャンが `SCAN BUSY` | **正しい**（`begin_scan()` は `latest.state` しか見ない） | 無線が他で使用中ならスキャンを待たせ、空いたら始める |
| 5 | 低 | SSID が操作なしで毎ブートのログに出る | 正しい（`LINK_START ssid=`、`wifi_time.c`） | **変更しない**。ログを共有するときは伏せる運用（§8 の前提と同じ） |

### 11.1 指摘1: リンク喪失と SNTP の後始末

**確かめた事実。** `esp_netif_sntp_deinit()`（`components/esp_netif/lwip/esp_netif_sntp.c:144-163`）は、自前のロック無しに `s_storage=NULL` → `sntp_stop` → `vSemaphoreDelete(storage->sync_sem)` → `free` を行う。`esp_netif_sntp_sync_wait()`（`:165-178`）はその `sync_sem` に `xQueueSemaphoreTake` でブロックする。autosync は 100 ms 刻みで待ち、link タスクは `BIT_LINK_LOST` で抜けて `tear_down()` の先頭で deinit する。待ち手のいるセマフォの削除は FreeRTOS では未定義。手動同期（`sync_task`）は同じタスクで deinit するので起きない。Fable の記述どおり。

**レビューに無かった2つ**（同じ根から）:

- **開始との競合**: autosync が `UP` を見てから `esp_netif_sntp_init()` を呼ぶまでに AP が落ちると、init（`s_storage` を確保してから書き込む）と link の deinit（`s_storage` を見て解放する）が並行しうる。解放済みへの書き込みか、link の後始末の後に SNTP が動き出す。
- **イベントループ**: SNTP の同期コールバックは既定のイベントループへ `esp_event_post` する。`tear_down()` は最後にそのループを削除する（`wifi_time.c` が作った場合。現状は常にそう）。SNTP がループより長生きすると、`esp_event_post` の NULL 検査と削除の間の競合になる。だから「link の後始末では SNTP に触らず、autosync に任せる」だけでは足りない — ループを消す前に SNTP が止まっていることが要る。

**採った案（(c) の変形）。** `wifi_time.c` に静的 mutex `sntp_lock`（`StaticSemaphore_t`、84 B、map で確認）を置き、`tear_down()` の deinit と、新しい `wifi_time_link_sntp_start/wait/stop()` の3つをすべてその中で行う。autosync はこの3つを ops に使う。

- deinit はロックを取るので、待ちの刻み（100 ms）が終わるまで待つ → **待ち手のいるセマフォは削除されない**。link の後始末の遅れは最大1刻み（計算）。
- start と wait はロックの中で `link_state==UP` を確かめる。link タスクは状態を UP 以外にしてから `tear_down()` に入る（`LINK_LOST` は FAILED、停止は DOWN）ので、UP を見た start は deinit より前に終わり、その deinit が後始末する。UP でなければ何も作らない → **後始末の後に SNTP が生まれない**。
- stop（deinit）は冪等（IDF が `s_storage==NULL` を見て何もしない）。link が先に後始末していれば何もしない → **二重解放にならない**。
- mutex は `running` の CAS に勝ったタスクが最初に作る。作成は CAS で直列化され、削除しないので、作成の競合も古いハンドルも起きない。
- autosync 側（`autosync_attempt()`）: 各刻みの後に `net_service_state()!=UP` を見て抜ける（SNTP の10 s を待たない）。status は「CLOCK SET」（link が UP で書いた OK が残る）でも「NO ANSWER FROM NTP」でもなく、**link の切断が残した段階（ASSOC、"NETWORK WENT AWAY"）で FAILED** にする。結果は FAILED（連続失敗に数える）。

**却下した案。**

- **(a) SNTP は始めたタスクだけが deinit し、link の後始末は SNTP に触らず、フラグで autosync に知らせる**: 上のイベントループの件で不足。link の後始末が「SNTP が止まるまで待つ」必要があり、結局は待ち合わせの仕組み（フラグ＋待ち、または mutex）が要る。フラグの待ちは上限と取り消しを別に書くことになり、mutex より長く、正しさの論証も長い。
- **(b) sync_wait をリンクの状態と一緒に待つ（イベントグループで両方を待つ）**: IDF の `sync_sem` は `esp_netif_sntp.c` の static の中にあり、外から別の待ち物と束ねられない。同期コールバック（`sntp_set_time_sync_notification_cb`）を自前に差し替えて自前のイベントグループで待つ手はあるが、IDF の `esp_netif_sntp_init()` が自分のコールバックを登録し直すので、IDF の内部の手順に依存する。また (b) だけでは link の deinit が他タスクから走る事実は消えない。
- **(c) の素朴形（sync_wait の全体、10 s をロックで包む）**: link の後始末が最大10 s 止まり、その間 `wifi_time_busy()` が真のまま → yield が3 s で LATE になる。刻みごとに取り直す形にして、待たせるのを最大100 ms にした。
- **link の後始末から SNTP の deinit を外し、`sync_task` の終わりにだけ置く**: イベントループの件で不可（autosync の SNTP がループより長生きする窓が残る）。

**ホスト試験**（`tools/test_net_autosync.c`）: 偽の SNTP を IDF v6.0.1 の振る舞い（1つの storage、deinit はロック無しでセマフォを消す）＋ `sntp_lock` の規則として書き、偽の無線に「UP の後に AP が落ちる」（`lost_after`）と「UP を見た直後、SNTP 開始の前に落ちる」（`lose_at_sntp_start`）を足した。検査: 待ち手のいるセマフォの削除0回、init 1・deinit 1・残り無し、二度目の init 無し、同期印0回、保持0、無線停止、喪失から1刻み＋後片付け以内に戻る、status が ASSOC の FAILED、続く試行が普通に成功する。**わざと壊した版**: (i) autosync の喪失検出を戻す → status の検査2件が落ちる、(ii) 偽の後始末がロックを無視する（＝修正前の `wifi_time.c`）→「待ち手のいるセマフォの削除 1回」で落ちる、(iii) 両方（＝修正前の全体）→ 3件落ちる。**限界**: ロックそのもの（`wifi_time.c`）はホストに載らない。偽はその規則を写したもので、試験が確かめるのは autosync 側の振る舞いと、規則が守られたときに不変条件が成り立つこと。

### 11.2 指摘2: 最大連続ブロックと、眠っているアプリ

**(i) 連続ブロック。** `esp_wifi_init` は1つでも確保に失敗すれば断り、それは FAILED として連続失敗に数えられる（4回でそのブートは終わり）。合計は足りても断片化した heap では、4回の無駄な無線起動で自動同期が止まる。方針の入力に `largest_bytes` を足し、`AUTOSYNC_MIN_LARGEST` 未満は `LOW_MEMORY`（延期、数えない）にした。

値は **16 KiB、暫定（推定）**。根拠: この経路で確保される既知の最大の塊はタスクの stack（`autosync` と `wifi_link` 各 4 KiB、計算。イベントループのタスク 2,304 B、Kconfig の既定）で、ドライバ自身のタスク stack はライブラリの中で決まり数 KiB と推定。そのどれにも余裕があり、アイドルのホーム（空き約274KiB、実測）では断らない値として置いた。MEGADEMO が10KBの連続領域を得られなかった前例（§5.3）がある heap でだけ効く。**`RADIO_INIT` の行の末尾に `largest=`（`esp_wifi_init` 直前の最大連続ブロック）を足した**ので、実機で「どの largest で断られたか／通ったか」が取れる。定数は `net_autosync.h` の1か所。

**(ii) 眠っているアプリ。** `app_dormant_id()`（`app_session.h`、眠っていなければ ""）を `net_autosync_poll()` が読み、方針が `AUTOSYNC_APP_ASLEEP` で1分ずつ延期する（数えない、ログ `AUTOSYNC_DEFERRED app_asleep <id>`）。再開か退去で眠りが終われば、次の1分で普通に始まる。`main/main.c` と `main/app_session.c` は触っていない。**`main/main.c` の `net_autosync_poll()` の直前のコメント「A kept app does not disqualify it ...」は古くなった**（振る舞いは poll の中で変わる）。main.c を触らない制約のため、この文書で訂正し、コメントの書き換えは次に main.c を触る変更に回す。

**Fable の数字の検証（計算）**: 「96 KiB − 背景 30.6 KiB − 背景フレーム 16 KiB ≒ 49 KiB」は、SOLAR SAIL の scene_mem（30,671 B）と FLOWER のフレーム（16,384 B）を足しているが、`scene_mem.h` によれば背景は同時に1つ（FLOWER は scene_mem 7,845 B ＋フレーム16 KiB ≒ 24 KiB）。最悪は SOLAR SAIL の約30KiB で、ホームに戻った直後の空きは約66KiB（計算）。門64KiB の**すぐ上**で、無線を上げると10KiB 台が残る。結論（眠っている間は同期しない）は変わらない。96KiB は中断の時点の値で、ホームへ戻ってから他に何が確保されるかは未計上。

### 11.3 指摘3: 中断を数えない

`autosync_policy_record(ABORTED)` は `attempts` を1つ戻し、連続失敗も触らず、次を `AUTOSYNC_ABORT_RETRY_MS`（5分）後にする。BUSY は従来どおり試行に数え、1分後（上限32が有限性を保つ）。

5分の根拠（推定・設計値）: 中断のたびにアプリの起動が「1刻み＋無線の後片付け」だけ遅れ、無線を上げる一過性のピークを払う。1分だとメニュー→アプリ→メニューの往復のたびにそれが起こりうる。5分なら1日の上限は288回（計算: 1440分÷5）で、実際にはホームで10 s 放置した直後にアプリを起動した場合にしか中断は起きない。一方でアプリ中心のブートでも数分の空きがあれば時計が合う。4.8KiB の恒常的な消費（§5.2）は最初の起動で払い済みで、回数では増えない（既存の記録からの推論）。

ホスト試験: 中断は試行0・連続失敗不変・5分空く。失敗1回→中断→連続失敗は1のまま。1日（1分ごとに見て、始まるたびに中断）で打ち切りにならず、ちょうど288回。BUSY は32回で打ち切り。修正前の版では6件落ちる。

### 11.4 指摘4: LATE の後の Wi-Fi 画面

`enter()` の `net_autosync_yield("screen")` が3 s で LATE になると、無線のロックが残ったまま `wifi_ui_open()` の `begin_scan()` が走る。中断した試行は status を IDLE にしてから release するので、`latest.state==RUNNING` の分岐を通らず `wifi_time_scan_start()` が `ESP_ERR_INVALID_STATE` → `SCAN BUSY ESP_ERR_INVALID_STATE`。

`wifi_ui.c` の変更は最小: `begin_scan()` が、表示中の同期ではない理由で `wifi_time_busy()` なら `scan_waiting` を立てて戻り、見出しは `WAITING FOR RADIO`。`wifi_ui_dirty()` が毎フレーム、`latest` の更新の後でロックが空いたのを見てスキャンを始める。手動同期の実行中に C-r した場合の従来の振る舞い（`SYNC RUNNING`）は変えていない。ホスト試験は試行の側だけ（後片付けが `AUTOSYNC_DOWN_WAIT_MS` を超えると、試行は ABORTED で戻り、無線はまだ保持中、その間の次の試行は BUSY で何も起こさない）。`wifi_ui.c` はホストに載らないので実機で見る。

### 11.5 検証

- ホスト試験 `bash tools/build_net_autosync_test.sh`（WSL、ASan/UBSan）: 既存の全件と、§11.1〜11.4 の新しい台本がすべて通過（`NET_AUTOSYNC_OK`）。わざと壊した版6種（§11.1 の3種、連続ブロックの門を外す、眠っているアプリの門を外す、中断を数える）で、それぞれ該当の検査が落ちることを確認。
- 既存: `tools/kasane_contract/run.sh`（WSL の git が worktree を読めないため、`git show HEAD:apps/kasane/proc_megademo.js` を `.cache/kasane_megademo_app/proc_megademo_baseline.js` へ先に置いた）、`tools/test_overlay.c`、`tools/test_menu_rows.c`、`tools/test_codeedit.c`。
- ビルド `idf.py -B build_wifireview build`: 新しい警告なし（既存の `flower.c` の未使用変数のみ）。map で `wifi_time_link_sntp_start/wait/stop` と `sntp_lock_storage` が入っていることを確認。
- DIRAM（`tools/memlog.py`、ff7d809 のクリーンなビルドとの map 比較、計算）: 172,428 → 172,524 B（**+96**）。`wifi_time.c.obj` +88（`StaticSemaphore_t` 84 B とハンドル）、`wifi_ui.c.obj` +1。flash +528 B。

### 11.6 実機で測る項目（人の作業を含む）

前提は §8 と同じ（資格情報を保存した機体、ログは SSID を伏せてから共有）。

| | 項目 | 手順 | 見るもの |
| --- | --- | --- | --- |
| (h) | UP の後の AP 喪失 | **人の作業**: `LINK_UP` が出てから2秒以内に AP（ルーターかテザリング）を切る。数回、タイミングを変えて | `Guru Meditation`・panic・`CORRUPT HEAP`・assert が無いこと。`LINK_LOST reason=` → `AUTOSYNC_DONE failed stage=assoc`（sntp ではない）、`ms=` が喪失から1 s 以内、`LINK_DOWN`。Wi-Fi 画面が「FAILED AT assoc / NETWORK WENT AWAY」 |
| (i) | 門の値 | (a)(b) のログ | `AUTOSYNC_DONE min_free=`（`AUTOSYNC_MIN_FREE` の推定8KiB を置き換える）と `RADIO_INIT ... largest=`（`AUTOSYNC_MIN_LARGEST` の暫定16KiB を置き換える。断られた例があればその largest） |
| (j) | ゲームの連続領域 | DERBY WATCH を自動同期 ON（同期後）と OFF で起動 | `internalFreeBytes` の差、MEGADEMO の起動時10KB の連続領域が取れるか（§8 (c) の続き） |
| (k) | 眠っているアプリ | アプリ（IMU CAL など再開フックのあるもの）を Back で眠らせ、ホームで10 s 以上放置 | `AUTOSYNC_DEFERRED app_asleep <id>` が1分おき。無線が上がらない。アプリを再開または別アプリで退去した後、次の1分で `AUTOSYNC_START` |
| (l) | yield の分布と LATE | §8 (d) を繰り返す | `AUTOSYNC_YIELD app waited_ms=` の分布、`LATE` の有無、yield 中のホームの停止時間。LATE が出たら直後に Wi-Fi 画面へ入り、見出しが `WAITING FOR RADIO` → `SCANNING` になり `SCAN BUSY` が出ないこと |
| (m) | 中断の後の間隔 | ホームで10 s 放置 → `AUTOSYNC_START` を見てアプリ起動、を繰り返す | `AUTOSYNC_DONE aborted` の後の `AUTOSYNC_NEXT in_s=300`。何度繰り返しても `AUTOSYNC_GAVE_UP` が出ない |
| (n) | 空きの記録 | 同期の前後 | `python tools/memlog.py --map build_wifireview/cardputer_pocketjs.map --port COM3 --check` を同期の前と `AUTOSYNC_DONE` の後で |

### 11.7 確信の低い点

- `AUTOSYNC_MIN_LARGEST` の16KiB（推定）。ドライバの内部の最大確保は測っていない。
- `sntp_lock` の正しさは、「link タスクは UP 以外の状態を書いてから `tear_down()` に入る」という順序に依存する（`wifi_time.c` の現状の2経路で確認。経路を足すときはこの順序を保つこと、コメントに記した）。
- IDF 側に残る競合: `sync_time_cb` は `s_storage` を2度読むので、deinit と tcpip スレッド上のコールバックが重なると NULL 参照の余地がある（IDF の内部、手動同期にも同じくある。今回の変更の外）。
- 5分の中断間隔は設計値で、使い方の実測に基づかない。
