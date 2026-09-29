# Wi-Fi の自動起動（起動時の時刻同期と、接続サービスの土台）

2026-09-30、ブランチ `vm/wifi-autosync`（`vm/main` 964f634 から）。依頼は「Wi-Fi の自動起動。時刻同期を自動で、将来は背景でオンライン前提のものも」。**実機は使っていない**（別の作業が COM3 を使用中）。この文書の数値は、**実測**（過去に実機で測られ、コードのコメントか文書に記録された値。出典を書く）・**計算**（実測値からの算術）・**推定**（根拠はあるが測っていない）を区別して書く。実機で測る項目は §8 にまとめた。

## 1. 結論

- **常駐のオンラインは既定にしない。** 入れたのは「一過性の自動時刻同期」と「参照カウント付きの接続サービス（ネイティブのみ）」の2層。
- **自動同期**: 資格情報が保存されていて、設定 AUTO TIME SYNC が ON（既定）のとき、**ホーム画面が10秒アイドル**になったら別タスクで 接続 → SNTP → 無線停止。成功したら次は12時間後。失敗は 1分・5分・30分で再試行し、**連続4回の失敗でそのブートは打ち切る**（設定を ON にし直すと再開）。1ブートの試行は最大32回。
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

眠っているアプリ（常駐中断）は除外しない。中断は既に「ホームから無線を上げられる空き」（`APP_SUSPEND_MIN_FREE` 96KiB）を条件にしており、再開は `begin_run()` を通るので必ず譲る。**動いているオーバーレイ**も除外しない（§3.4）。

この状態が**キー入力なしで10秒**続き、方針（§3.2）が「期限到来」と言い、資格情報があり、空きが `AUTOSYNC_MIN_FREE` 以上なら、優先度4（UI タスクの5より下）の `autosync` タスクを立てる。資格情報は NVS を読むので、「ホームを離れたら無効化し、次に必要になったとき1回読む」キャッシュにした（資格情報は Wi-Fi 画面でしか変わらず、それはホームではないので正確）。資格情報が無ければ何も言わずに1分後にまた見る。

ブート直後の忙しさ（フォント・SKK・背景シーンの初期化）は、最初の `HOME_READY` から10秒のアイドルを待つことで避けている。

### 3.2 方針（純粋関数、ホストで試験）

| 定数 | 値 | 根拠 |
| --- | --- | --- |
| `AUTOSYNC_IDLE_MS` | 10 s | 操作中のメニューや、テストスクリプトのキー間隔（最大2.5 s）と重ならない長さ |
| `AUTOSYNC_BACKOFF_*` | 1分・5分・30分 | 依頼の例どおり |
| `AUTOSYNC_MAX_FAILURES` | 連続4回 | 最初＋再試行3回。1回あたり最大約25 sの無線と約48KBのヒープを使うので、届かない網に5回目は払わない |
| `AUTOSYNC_RESYNC_MS` | 12 h | 依頼の12〜24 hの下端。RTC の日差は分単位に達しないが（推定）、長時間稼働で1日2回は安い |
| `AUTOSYNC_RETRY_MS` | 1分 | 中断・無線が他で使用中・空き不足のとき。失敗には数えない。メニューとアプリを行き来するたびに無線を上げ下げしないため |
| `AUTOSYNC_MAX_ATTEMPTS` | 32 / ブート | 何があっても有限にする安全網 |
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

`autosync` タグ: `AUTOSYNC_START free= largest=` / `AUTOSYNC_DONE <ok|failed|aborted|busy> stage= ms= free_before= min_free= free_after=` / `AUTOSYNC_NEXT in_s=` / `AUTOSYNC_GAVE_UP` / `AUTOSYNC_YIELD <why> waited_ms= [LATE] free=` / `AUTOSYNC_DEFERRED low_memory` / `AUTOSYNC_SKIP already synchronized` / `AUTOSYNC_ENABLED`。`net_service` タグ: `NET_ACQUIRE` / `NET_RELEASE` / `NET_ACQUIRE_REFUSED` / `NET_RELEASE_UNMATCHED`。`wifi` タグの既存の `RADIO_INIT ... cost=` / `LINK_UP free=` / `LINK_DOWN free=` と、新しい `LINK_STOPPED while connecting`。SSID は既存の `LINK_START ssid=` に出る（AP が放送する名前で、従来どおり）。**パスフレーズはどこにも出ない。**

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

ホーム画面の空き約274KiB（実測、CLAUDE.md）に対し、64KiB の門は十分に下（計算: 残り約210KiB）。ホームで門が効くのは、眠っているアプリ（96KiB 保証）かオーバーレイ（56KiB 保証）がいるときだけ。**この64KiB は `AUTOSYNC_DONE min_free=` の実測で置き換えること。**

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
