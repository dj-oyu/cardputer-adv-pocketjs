# ネットワークスタックの資源レビュー（lwIP・esp_netif・Wi-Fi・mbedTLS）

2026-11-12、ブランチ `claude/affectionate-bardeen-d30wqd`。依頼は「ネットワークスタックが DRAM などを無駄にしていないか」。**コードと sdkconfig は変えていない。実機は使っていない。**

数値の種別:

- **実測（実機）**: 過去に実機で測られ、文書かコードに記録がある値。出典を書く。
- **実測（host）**: この調査で、ビルド済みの map / ELF から読んだ値。
- **計算**: 実測値や IDF のソースの定数からの算術。
- **推定**: 根拠はあるが測っていない値（Kconfig の help の「約 1.6 KB」など）。

測ったビルド: **`build_heapmap`**（2026-11-12 12:08 の map / ELF / sdkconfig。通常ビルドとの差は `CONFIG_POCKET_HEAP_MAP=y` だけ）。ネットワーク関連の `CONFIG_*`（242 行）は、手元の 7 つのビルドディレクトリ（`build_chunk` `build_gcstats` `build_heapmap` `build_icab` `build_obs` `build_origin` `build_origin2`）で同一（`diff` で確認）。IDF のソースは `/root/.espressif/v6.0.1/esp-idf`（v6.0.1）。

## 1. 結論

1. **設定だけで消せる「大きな無駄」は残っていない。** IRAM 配置（Wi-Fi・lwIP）、IPv6、DHCP サーバ、SoftAP、AMPDU、Wi-Fi の NVS、PPP、IP 再構成は既に切ってある。lwIP の PCB・ソケットの上限（TCP 16、UDP 16、RAW 16）は `MEMP_MEM_MALLOC=1` なので**静的な RAM を使っていない**（実測（host）: `liblwip.a` の `.bss` は全部で 1,985 B、プールは無い）。
2. **残る静的な DIRAM は約 25 KB で、うち約 18 KB は Wi-Fi のバイナリ（`libnet80211` `libpp` `libphy`）。設定では減らない。** `CONFIG_ESP_PHY_IRAM_OPT` は S3 では何もしない（`esp_phy/linker.lf` と Kconfig の help: esp32c2 だけ）。
3. **効くのは「どこに置かれるか」と「無線を上げている間の量」。** 恒久ブロックの位置（§4）で約 42 KB の連続領域が得られる見込み（計算、§6.6 の地図から）。無線を上げている間の量は、Wi-Fi の静的 RX バッファで約 3 KB 減らせる見込み（推定、§5 の C1）。
4. **§6.6 の 6 個の恒久ブロックのうち、4 個は lwIP のものではない。** カウンティングセマフォ 1 個と pthread の 12 B × 3 個は、Wi-Fi ドライバが「API を呼んだタスクごと」に作るセマフォ（`esp_wifi/esp32s3/esp_adapter.c:222`）と、その pthread のスレッド局所の記録。常駐の `wifi` タスクが持つので消えない。**`esp_netif_init()` を早く呼ぶだけでは、この 4 個は動かない**（§4.2）。
5. **`sdkconfig.defaults` のコメント「nothing in this firmware streams」は古い。** いまは `pocket.net` の HTTP(S) と `opus_net` の HTTP ストリームがある（§2）。バッファを減らす案は、時刻同期だけでなく、この 2 つで確かめる必要がある。

## 2. ファームがネットワークを何に使っているか

| 利用者 | 場所 | 通信 | 同時数 | タスクのスタック |
| --- | --- | --- | --- | --- |
| 時刻同期（自動・手動） | `main/pocket/wifi_time.c`、`net_autosync.c`、`net_service.c` | DHCP、DNS 1 件、SNTP（UDP、`pool.ntp.org`、サーバ 1 つ） | 1 | 常駐の `wifi` 3,072 B と `timesync` 2,048 B（RTC FAST。`wifi-autostart.md` §13） |
| `pocket.net` の HTTP / HTTPS | `main/pocket/pocket_net.c` | `esp_http_client`（バッファ 512 B）、TLS 1.2 クライアント、証明書バンドル | 1 件ずつ（2 件目は BUSY） | `pocket_http` 6,144 B（要求ごとに作って消す） |
| Opus のストリーム | `main/pocket/opus_net.c` | `esp_http_client` の GET を読み続ける | 1 | `opusnet` 6,144 B（再生ごと） |
| スキャン | `wifi_time.c`（`radio_up(false)`） | 802.11 のスキャンだけ。IP は使わない | 1 | `wifi` タスク |

- 3 つとも map に入っている（実測（host）: `.text.http_task`、`.text.net_task_fn`、`.text.opus_net_start`、`.text.esp_crt_bundle_attach` がある）。
- **使っていないもの**: 待ち受け（listen / accept）、UDP ソケット（SNTP と DNS は lwIP の内部 PCB）、IPv6、マルチキャスト、ループバック（自分宛て）、TLS サーバ、エンタープライズ認証、SoftAP、ESP-NOW、mDNS の応答。
- ソケットの同時数は最大 2（`pocket_http` と `opusnet` が重なる場合。計算）。`CONFIG_LWIP_MAX_SOCKETS=4` は表 `sockets[]` が 80 B（実測（host））なので、これ以上削る意味は無い。

## 3. いま何に払っているか

### 3.1 静的な DIRAM（無線を使わないブートでも払う）

実測（host）: `python -m esp_idf_size --archives --format csv build_heapmap/cardputer_pocketjs.map` と、map の節ごとの集計。

| アーカイブ | DIRAM 計 | `.bss` | `.data` | IRAM の `.text` | 主な中身 |
| --- | ---: | ---: | ---: | ---: | --- |
| `libnet80211.a` | 8,571 | 7,640 | 931 | 0 | `g_cnxMgr` 3,944、`s_wifi_nvs` 1,316（NVS を切っても残る）、`g_ic` 692、`gChmCxt` 592 |
| `libphy.a` | 6,432 | 86 | 1,277 | 5,069 | `phy_reg.o` の `.iram1` 2,739、`phy_i2c.o` 966、`phy_init.o` 711、`phy_param` 740 |
| `libpp.a` | 4,704 | 1,234 | 2,640 | 830 | `TxRxCxt` 960、`g_pm` 544 |
| `liblwip.a` | 1,993 | 1,985 | 8 | 0 | `dns_table` 1,104、`arp_table` 240、`loop_netif` 84、`sockets` 80 |
| `libwpa_supplicant.a` | 1,254 | 1,246 | 8 | 0 | `gWpaSm` 852 |
| `libesp_wifi.a` | 800 | 43 | 480 | 277 | `g_wifi_osi_funcs` 480 |
| `libtfpsacrypto.a` | 548 | 388 | 80 | 80 | PSA の `global_data` 184、AES・MPI の静的セマフォ 84 × 2 |
| `libesp_phy.a` | 285 | 43 | 9 | 233 | |
| `libmbed-builtin.a` + `libmbedtls.a` | 180 | 132 | 48 | 0 | |
| `libesp_coex.a` | 97 | 0 | 0 | 97 | |
| `libesp_netif.a` `libesp_event.a` `libesp-tls.a` `libpthread.a` ほか | 101 | 82 | 19 | 0 | |
| **合計** | **24,965** | 12,875 | 5,504 | 6,586 | |

- `esp_http_client`、`tcp_transport`、`http_parser`、`mbedx509` は DIRAM 0（flash だけ）。
- この合計は CLAUDE.md の「リンクするだけで約 37 KiB」を再現しない。37 KiB は空きヒープの差の実測（実機）で、測り方も時期も違う。**差の約 12 KB の内訳は、この調査では分けていない**（Wi-Fi を外したビルドとの比較が要る）。
- IRAM の 6,586 B は、すべて blob の中で節が決まっているもの（`.iram1`）。`CONFIG_ESP_WIFI_IRAM_OPT` `RX_IRAM_OPT` `SLP_IRAM_OPT` `EXTRA_IRAM_OPT`、`CONFIG_LWIP_IRAM_OPTIMIZATION` `EXTRA_IRAM_OPTIMIZATION` は既に全部 n。

### 3.2 最初の `esp_netif_init()` で恒久に残るもの

記録: 約 4.8 KiB（`linkRetainedBytes` 4,915 B、実測（実機）、`pocket_net.c:1392`）。`esp_netif_deinit()` は `ESP_ERR_NOT_SUPPORTED` を返す（`esp_netif/lwip/esp_netif_lwip.c:610-624` で確認）。

ソースから数えた内訳（計算。ブロックの大きさは §6.6 の実測（実機）と ELF の `sizeof`）:

| もの | 大きさ | 作る場所 |
| --- | ---: | --- |
| tcpip スレッドのスタック | 3,584 | `sys_thread_new`（`sys_arch.c:390`）。`CONFIG_LWIP_TCPIP_TASK_STACK_SIZE` 3,072 ＋ `TASK_EXTRA_STACK_SIZE` 512（`esp_task.h:36`。picolibc なので 512 が付く） |
| tcpip スレッドの TCB | 100 | 同上。`sizeof(StaticTask_t)` は 100 B（実測（host）、ELF）。`wifi-autostart.md` §13 の「推定 約 350 B」より小さい |
| tcpip の mbox | 約 160 | `tcpip_init` → `sys_mbox_new`。キュー（16 件 × 4 B ＋ 本体）と 4 B の箱 |
| lwIP の保護用ミューテックス | 84 | `sys_init`（`sys_arch.c:44`） |
| esp_netif の API 用セマフォ 2 個 | 84 × 2 | `esp_netif_init` → `sys_sem_new`（`sys_arch.c:111`）。`api_sync_sem` と `api_lock_sem` |
| lwIP の pthread キー | 12 | `sys_init` → `pthread_key_create`（`sys_arch.c:417`） |
| **小計** | **約 4,110** | |

4,915 との差 約 0.8 KB は未帰属（Wi-Fi のタスク別セマフォ 84 B と pthread の記録 36 B、ヒープの管理分、lwIP のタイマなどと推定）。

### 3.3 無線を上げている間（下ろすと返る）

| もの | 値 | 種別 |
| --- | ---: | --- |
| `esp_wifi_init` | 27,192 B | 実測（実機）、2026-09-07、`pocket_net.c` の `NET_RADIO_MIN_FREE` のコメント |
| netif とイベントループ | 約 21,000 B | 実測（実機）、同上（初回は §3.2 の恒久分を含む） |
| うち静的 RX バッファ | 4 × 約 1.6 KB ＝ 約 6.4 KB | 推定（Kconfig の help の値） |
| うち管理フレーム用の静的 RX バッファ | 5 × 約 500 B ＝ 約 2.5 KB | 推定（同上） |
| イベントループ `sys_evt` のスタック | 2,304 ＋ 512 ＝ 2,816 B | 計算（`esp_task.h:52`） |
| イベントのキュー | 32 件 × 16 B ＝ 512 B | 計算（`sizeof(esp_event_post_instance_t)` は 16、実測（host）） |
| 動的 RX / TX バッファ | 上限 8 個ずつ、1 個はフレームの大きさ | 上限は設定値。実際の量は未測定 |
| TLS のハンドシェイク | 空き −7,172 B、最大ブロックから 15,360 B | 実測（実機）、2026-09-08、`pocket_net.c:59-66`（1 サーバ、1 回） |

## 4. 恒久ブロックの位置（§6.6 の 6 個）

`docs/vm/lv3-heap-exhaustion.md` §6.6 の表を、ソースで照合した。

| ブロック | §6.6 の見立て | ソースで確かめた持ち主 | `esp_netif_init()` を早めると動くか |
| --- | --- | --- | --- |
| 3,584 B（11,792 と 26,780 の間） | tcpip のスタック | 同じ（`sys_arch.c:390`） | 動く |
| 84 B（21,808 と 2,380 の間）、`sys_sem_new` `sys_arch.c:111` | lwIP のセマフォ | esp_netif の `api_sync_sem` か `api_lock_sem`（`esp_netif_lwip.c:592,599`）。`sys_sem_new` を恒久に使うのはこの 2 つだけ（`init_sem` は直後に解放される） | 動く |
| 84 B（2,560 と 11,792 の間）、`xQueueCreateCountingSemaphore` | netconn のスレッド別セマフォ（推定） | **違う。** lwIP のスレッド別セマフォは `xSemaphoreCreateBinary`（`sys_arch.c:514`）。カウンティングを作るのは `esp_adapter.c:222` の `wifi_thread_semphr_get_wrapper` だけ（`grep` で lwip / esp_netif / esp_event / esp_wifi / esp_phy / esp_coex / wpa_supplicant の port を確認）。Wi-Fi の API を呼んだタスクごとに 1 個 | **動かない** |
| 12 B × 3 個（同じ場所）、`pthread_key_create` / `pthread_setspecific` | 同上 | 上のセマフォを指す pthread のキー（`esp_adapter.c:214`）と、`wifi` タスクの値のリストと項目（`pthread_local_storage.c`）。lwIP のキー（`sys_arch.c:417`）が 1 個混じっている可能性はある | Wi-Fi の分は**動かない** |

### 4.1 なぜ Wi-Fi の分は消えないか

`wifi_thread_semphr_get_wrapper` は、セマフォを pthread のスレッド局所に置く。タスクが消えれば pthread の後始末で返るが、`wifi` タスクは常駐（`wifi_time.c:483` の `rtc_task_create`）なので、最初の `esp_wifi_*` の呼び出しで作られたものが、そのブートの間ずっと残る。`esp_wifi_deinit()` でも返らない（キーもセマフォもドライバの外、アダプタ側にある）。

### 4.2 `esp_netif_init()` を早める変更（別作業）への意見

- **効く範囲**: tcpip のスタック、TCB、mbox、ミューテックス、API 用セマフォ 2 個、lwIP の pthread キー。§6.6 の計算どおり、11,792 ＋ 3,584 ＋ 26,780 で約 42 KB の連続領域になる見込み（計算）。
- **効かない範囲**: Wi-Fi のタスク別セマフォと pthread の 3 個（2,560 と 11,792 の間）。これも低い番地へ寄せるなら、**常駐の `wifi` タスクから**、起動の早い時点で `esp_wifi_init()` → `esp_wifi_deinit()` を 1 回通す必要がある（推定。セマフォが作られるのが `esp_wifi_init` の中か、その後の `esp_wifi_set_mode` などの API 呼び出しかは blob の中で、未確認）。費用は起動時の一過性の約 27 KB と時間（未測定）。寄せて得るのは 2,560 ＋ 11,792 ＋ α ＝ 約 14.5 KB の穴 1 個（計算）で、42 KB の方より価値は小さい。**先に esp_netif の分だけ動かして地図を取り直し、この 4 個がまだ大きな穴を分けているかを見てから決めるのがよい。**
- **条件の付け方**: 「自動時刻同期が ON のとき」だけ早めると、OFF の機体で手動 sync・スキャン・`pocket.net` を最初に使ったとき、恒久ブロックはその時点の空きの真ん中に置かれる。**アプリの実行中に初めて使うと、アプリが終わった後も残る。** 条件を「資格情報が保存されている」にする方が、取りこぼしが少ない（資格情報が無ければ、IP を使う経路は動かない）。払うのは、無線を一度も使わないブートでの約 4.1 KB（計算、§3.2）。
- **`esp_netif_init()` に前提は無い**（NVS もイベントループも要らない。乱数は `esp_fill_random` で、IDF のコメントが RF 無しでの使用を認めている。`esp_netif_lwip.c:556-565`）。ホーム画面を作る前に呼べる。
- **代替（採らなくてよい）**: `--wrap=sys_thread_new` でスタックを静的領域に置く方法もあるが、早く呼ぶ方が単純で、同じ効果になる。RTC FAST の空きは 608 B（実測（実機）、`wifi-autostart.md` §13）で、3,584 B は載らない。

## 5. 項目ごとの判定

### 5.1 設定だけで減らせるもの

| # | 変更 | 見込み | 種別 | リスク | 実機での確かめ方 |
| --- | --- | ---: | --- | --- | --- |
| C1 | `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM` 4 → 2（範囲 2〜25） | 無線を上げている間 −約 3.2 KB | 推定 | 受信の取りこぼし（混んだ電波、スキャン中、Opus のストリーム）。AMPDU RX は切ってあるので窓との整合は不要 | `RADIO_INIT ... cost=` の差。自動同期 10 回（`AUTOSYNC_DONE ok` と `ms=`）。`pocket.net` の HTTPS 12 回（既存の 12/12 の試験）。Opus のストリームを 1 曲、アンダーランのログ |
| C2 | `CONFIG_ESP_WIFI_RX_MGMT_BUF_NUM_DEF` 5 → 3（範囲 1〜10） | 同 −約 1 KB | 推定 | スキャンで AP の取りこぼし（プローブ応答が集中する）。接続の失敗 | 同上 ＋ 設定画面のスキャンで見える AP の数を前後で比べる |
| C3 | `CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM` 8 → 4（静的の数以上が条件。C1 と組で） | 受信が集中したときの山の上限 −約 6.4 KB | 計算（4 × 1.6 KB、フレーム最大のとき） | TCP の再送が増える。ストリームの途切れ | `AUTOSYNC_DONE min_free=`、HTTPS の本文 64 KiB の読み出し時間、Opus |
| C4 | `CONFIG_LWIP_TCPIP_TASK_STACK_SIZE` 3,072 → 2,560（下限 2,048） | 恒久 −512 B（2,048 なら −1,024 B） | 計算 | スタックあふれ。tcpip スレッドは DHCP・DNS・SNTP・ソケットの全操作を実行する。**最大使用量は未測定** | 先に HWM を測る（プローブが要る: `uxTaskGetStackHighWaterMark(xTaskGetHandle("tiT"))` を同期・HTTPS・Opus の後に出す）。余裕が 512 B 以上残る値にする |
| C5 | `CONFIG_LWIP_NETIF_LOOPBACK=n` | 静的 −84 B（`loop_netif`）、flash 少し | 実測（host）の `.bss`、効果は計算 | 自分の IP 宛ての通信ができなくなる（使っていない） | `tools/memlog.py --map` で `netif.c.obj` の減少。同期と HTTPS が通ること |
| C6 | `CONFIG_ESP_SYSTEM_EVENT_QUEUE_SIZE` 32 → 16 | 無線を上げている間 −256 B | 計算 | イベントの取りこぼし（スキャン直後に集中） | 値が小さいので、やるなら C1 と一緒に |

- C1〜C3 の価値は、ホームのアイドルではなく、**ネットワークを使うアプリの実行中**と、`AUTOSYNC_MIN_FREE`（64 KiB）・`NET_RADIO_MIN_FREE`（56 KiB）の門にある。門の値は実測で置き換える約束になっている（`wifi-autostart.md` §5.1）ので、C1 を入れたら門も測り直す。
- C1 と C2 は別々に入れて測る（失敗したとき、どちらが原因か分かるように）。

### 5.2 コードかビルドの変更が要るもの

| # | 変更 | 見込み | 種別 | リスク | 確かめ方 |
| --- | --- | ---: | --- | --- | --- |
| K1 | `esp_netif_init()` を起動の早い時点で呼ぶ（別作業で実装中） | 連続領域 約 42 KB（11,792 ＋ 3,584 ＋ 26,780） | 計算 | §4.2 の条件の付け方 | `CONFIG_POCKET_HEAP_MAP` の「ゲストの前」の地図を取り直す |
| K2 | Wi-Fi のタスク別セマフォを早い時点で作らせる（`wifi` タスクから init / deinit を 1 回） | 穴 1 個が約 14.5 KB に | 計算（効果）、推定（方法） | 起動が遅くなる（未測定）。起動時に約 27 KB の一過性 | K1 の後の地図で、要るかどうかを先に判断。やるなら `CONFIG_POCKET_HEAP_ORIGIN` でブロックの番地を見る |
| K3 | `DNS_TABLE_SIZE` 4 → 2、`ARP_TABLE_SIZE` 10 → 4（Kconfig が無い。lwip のコンポーネントへコンパイル定義を足すか、`ESP_IDF_LWIP_HOOK_FILENAME` のヘッダで定義） | 静的 −552 B、−144 B | 計算（`dns_table` 1,104 ÷ 4 ＝ 276 B / 件、`arp_table` 240 ÷ 10 ＝ 24 B / 件。実測（host）の map から） | DNS は同時に引く名前が 2 つまで（NTP と HTTP の 1 つずつで足りる）。ARP は STA なのでゲートウェイと数台 | `tools/memlog.py --map` で `dns.c.obj` `etharp.c.obj` の減少。同期と HTTPS |
| K4 | スキャンの経路（`radio_up(false)`）で `esp_netif_init()` と netif の生成を省く | スキャンだけのブートで恒久 約 4.1 KB を払わない | 計算（量）、推定（可否） | `esp_wifi_init` は lwIP を要らない（`wifi_init.c` は `esp_netif.h` を include するだけで、呼び出しは無い。ソースで確認）が、netif 無しの `esp_wifi_start` ＋ スキャンは**実機で未確認**。K1 を「資格情報があれば常に早く呼ぶ」にするなら、この項目は不要 | 設定画面のスキャン、`SCAN_DONE` |

### 5.3 既に最小のもの、減らせないもの

| 項目 | 状態 | 根拠 |
| --- | --- | --- |
| Wi-Fi・lwIP の IRAM 配置 | 全部 n | sdkconfig。残る IRAM 6,586 B は blob の `.iram1` |
| PHY の IRAM（`libphy.a` 5,069 B） | 減らせない | `CONFIG_ESP_PHY_IRAM_OPT=y` だが、`esp_phy/linker.lf` の `phy_iram` は esp32 と esp32c2 にしか無い。S3 では設定を変えても動かない |
| `libnet80211` の `.bss` 7,640 B（`s_wifi_nvs` 1,316 B を含む） | 減らせない | blob。`CONFIG_ESP_WIFI_NVS_ENABLED=n` でも `s_wifi_nvs` は残っている（実測（host）） |
| TCP / UDP / RAW の PCB の上限 16、listen 16 | RAM を使わない | `MEMP_MEM_MALLOC=1`（`lwipopts.h:105`）。上限であって確保ではない。`tcp_pcb` は 168 B、使うときだけ |
| `CONFIG_LWIP_MAX_SOCKETS=4` | 80 B | `sockets[]`（実測（host））。同時 2 本まで使う |
| `CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=16`（既定 32） | 下げても −40 B | 1 件 4 B。下限 6 |
| TCP の窓と送信バッファ 5,760 B、受信 mbox 6 | 接続中だけ | 確保は pbuf が来たとき。窓を 2,880 B にしても、減るのは受信が溜まったときの山（最大 約 2.9 KB、計算）で、Opus のストリームと HTTPS の速度が落ちる。触らない |
| IPv6、DHCP サーバ、SoftAP、AMPDU、PPP / SLIP、IP 再構成、lwIP の統計、AUTOIP | 切ってある | sdkconfig |
| SNTP | サーバ 1、起動遅延なし | `sntp_servers` 12 B |
| イベントループ | 無線と一緒に作って消す | `wifi_time.c` の `owns_event_loop`。常駐していない |
| mbedTLS の記録バッファ | 動的（`CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`） | 入力 16,384 B を固定で持たない。`SSL_IN_CONTENT_LEN` を下げない判断は `sdkconfig.defaults` のコメントのとおり（相手のサーバを先に決めることになる） |
| mbedTLS: 相手の証明書の保持、TLS 1.3、DTLS | 切ってある | `MBEDTLS_SSL_KEEP_PEER_CERTIFICATE` n ほか |
| `CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA` | 入れても効かない見込み（推定） | 返すのは自分の秘密鍵・DHM・CA の鎖。このファームはクライアント証明書を使わず、CA は鎖ではなくバンドル（flash）で照合する |
| エンタープライズ認証（`CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT=y`） | 既にリンクされていない | `esp_eap_client.c.obj` は flash 24 B（実測（host））。`--gc-sections` が落としている |
| lwIP のスレッド別セマフォ（`LWIP_NETCONN_SEM_PER_THREAD`） | 恒久には残らない見込み | 作るのはソケットの API を呼ぶタスク（`pocket_http`、`opusnet`）。どちらも終わるので pthread の後始末で返る。常駐の `wifi` `timesync` は esp_netif の API（`esp_netif_tcpip_exec`、自前のセマフォ）しか呼ばない（ソースで確認。実機では未確認） |

### 5.4 flash だけが減るもの（DRAM には効かない）

アプリの区画は 3 MB、イメージは 2,542,112 B（実測（host））。余裕は約 0.6 MB（計算）で、今は困っていない。記録だけ残す。

| 変更 | flash の見込み | 種別 | リスク |
| --- | ---: | --- | --- |
| `CONFIG_MBEDTLS_TLS_CLIENT_ONLY=y` | −約 5 KB（`ssl_tls12_server.c.obj` 4,724 B ＋ `esp_ssl_srv.c.obj` 274 B） | 実測（host）の現在の大きさ | サーバは使っていない。`pocket_tls_txfix.c` の wrap がクライアント側だけで足りることを確認 |
| 証明書バンドルを FULL → CMN | `x509_crt_bundle` 67,536 B の一部 | 実測（host）の現在の大きさ。減る量は未測定 | 一部のサイトで検証に失敗する。`pocket.net` は相手を選ばない API なので、勧めない |
| WPA3（SAE）を切る | −約 12.6 KB（`sae.c.obj` 9,103、`sae_pk.c.obj` 2,736、`dragonfly.c.obj` 779） | 実測（host）の現在の大きさ | WPA3 専用の AP につながらなくなる。勧めない |

## 6. 優先順位

| 順 | 項目 | 得るもの | リスク | 確信 |
| --- | --- | --- | --- | --- |
| 1 | K1（早い `esp_netif_init`。条件は「資格情報あり」を推奨） | 連続領域 約 42 KB（計算） | 低 | 高（ブロックの持ち主はソースと実機の両方で一致） |
| 2 | C1（静的 RX 4 → 2） | 無線中 −約 3.2 KB（推定） | 中（Opus のストリーム、混んだ電波） | 中 |
| 3 | C4（tcpip のスタック。HWM を測ってから） | 恒久 −512〜1,024 B（計算） | 中（あふれは即クラッシュ） | 測るまで低 |
| 4 | K3（DNS と ARP の表） | 静的 −約 700 B（計算） | 低 | 高 |
| 5 | K2（Wi-Fi のタスク別セマフォの位置） | 穴 約 14.5 KB（計算） | 中（起動時間） | 低（方法が未確認） |
| 6 | C2、C3、C5、C6 | 各 0.1〜1 KB、または山の上限 | 低〜中 | 中 |

**全部入れても、静的と恒久で減るのは約 1.5〜2 KB、無線中で約 4〜5 KB（計算と推定の和）。** ネットワークスタックに、設定で取り返せる 10 KB 級の無駄は無い。大きいのは K1 の「位置」。

## 7. 実機で測る項目

実機のロックと後始末は CLAUDE.md の「並行作業」に従う。コマンドは [test-commands.md](test-commands.md)。

1. **K1 の後の地図**: `CONFIG_POCKET_HEAP_MAP=y` のビルドで DERBY を起動し、「ゲストの前」の空きの並びを §6.5 と比べる。3,584 B と 84 B のブロックが大きな穴の間から消えたか。Wi-Fi の 84 B ＋ 12 B × 3 がどの穴を分けているか（K2 の要否）。
2. **tcpip スレッドの最大使用量**（C4）: 自動同期、HTTPS の要求、Opus のストリームのそれぞれの後で HWM。プローブのコードが要る。
3. **C1〜C3 の前後**: `RADIO_INIT ... cost=`（`wifi_time.c`）、`AUTOSYNC_DONE ... ms= min_free= free_after=`、`TLSCOST`（`pocket_net.c:407`）、Opus の再生ログ。各設定を 1 つずつ。`sdkconfig.defaults` を変えても既存の `sdkconfig` には効かないので、ビルドディレクトリを新しく切る（`sdkconfig.defaults` の注記のとおり）。
4. **静的な増減**: `python tools/memlog.py --map <build>/cardputer_pocketjs.map --port COM3 --check`。ファイル別の差（`dns.c.obj`、`netif.c.obj` など）と、アイドルの空きを記録する。
5. **lwIP のスレッド別セマフォが残らないこと**（§5.3 の最後の行）: HTTPS を 10 回続けた後の空きが減っていかないこと。既存の「9 回の要求の後も最大ブロック 31,744」（`pocket_net.c:73-76`、実測（実機））と整合するが、セマフォを名指しでは見ていない。
6. **「リンクで約 37 KiB」の内訳**: §3.1 の 24,965 B との差。必要なら、Wi-Fi を外したビルドとの `memlog` の比較で分ける。
7. **回帰**: `test_settings.py`（設定の行は変えないので押下回数はそのまま）、`smoke_device.py`、手動の sync とスキャン（`wifi-autostart.md` §13 の未検証項目と同じ）。

## 8. 確信の低い点

- §3.3 の「静的 RX 1 個 約 1.6 KB」「管理用 1 個 約 500 B」は Kconfig の help の値で、27,192 B の内訳は実機で分けていない。
- K2 の方法（`esp_wifi_init` / `deinit` を早く 1 回通すとセマフォが作られる）は、blob の中の呼び出しに依存していて未確認。
- §3.2 の内訳は、未帰属が約 0.8 KB ある。
- §6.6 の 12 B × 3 個のうち、どれが lwIP のキーでどれが Wi-Fi のものかは、番地では分けていない（トレースの深さ 4 では呼び出し元が見えていない）。
- `build_heapmap` は計測用のビルドで、`CONFIG_POCKET_HEAP_MAP` の分だけ `libmain.a` が通常ビルドと違う。ネットワークのアーカイブには影響しない見込みだが、通常ビルドの map では確かめていない。

## 9. 実機の結果（2026-11-12、Opus）

- **lwIP の核を早く作る条件を「ネットワークが保存されているとき」に広げた**（`5fb4721`、§4 の推奨どおり）。保存されていなければ何も作らない。
- **tcpip スレッドのスタックの最小の空き: 1,716 B / 3,072 B**（実測(device)、`1f2896d`、`NET_STACK app_stop tcpip_min_free=1716`）。使ったのは最大 1,356 B。測ったのは起動後の HELLO（起動時の自動時刻同期を含むと見ている。同期がこの時点までに走ったかはログで確かめていない）。**HTTPS と Opus の流し込みはまだ通していない**（netcheck は broker の起動できるアプリの一覧に無い）。
  - 2,560 B にすると余裕は 1,204 B、2,048 B なら 692 B（計算）。HTTPS・Opus の後の値を見るまで変えない。
- `NET_STACK` は無線を止めたとき（`radio_down`）とアプリの終わりに出る。スレッド名は IDF v6.0.1 では `tcpip`（`lwipopts.h` の `TCPIP_THREAD_NAME`）。
