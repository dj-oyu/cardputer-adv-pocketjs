# LCD の送信ドライバ（GP-SPI2 + GDMA を直接使う）仕様 草案

2026-11-04 起草。**草案（監査前）。** 目標は 60 fps に向けて、LCD への送信の待ちと上乗せを減らすこと。数値は各行に「実測」「計算」「推定」「コードの値」を書く。TRM は ESP32-S3 TRM v1.8 の印刷のページ。

## 0. 目的と目標

- **目的:** 帯の描画（CPU）と LCD への送信（DMA）を、帯ごとに互いを待たない形に近づける。1 フレームの「描画と送信」を、`描画の合計 + 送信の合計` に近い今の形から、`max(描画の合計, 送信の合計)` に近い形にする。
- **目標（STRESS LV1、1a の後の実機の値を基準）:** 描画と送信の合計 10.34 ms（実測、`render_ms` 7.38 + `send_ms` 2.90）を 8 ms 前後に（推定）。60 fps の 1 フレーム 16.7 ms のうち、JS のターン（6.45 ms、実測）と合わせて 15 ms 未満。
- **目標でないもの:** 描画の中身（Kasane・Shiki）の変更、`board_strip()` の 1 本の描画用バッファをやめること（呼び出し元 17 か所が 1 フレームの間ポインタを持つ、`board.c` の `tx_buf` の注記）、LCD の画素の形式の変更（RGB444 など）、2 コア化。

## 1. 今の姿（事実）

| 項目 | 今 | 出典 |
| --- | --- | --- |
| 描画用の帯 | `shared` 1 本、240 × 8 行 = 3,840 B、16 B 境界 | `main/hal/board.c:37` |
| 送信用 | `tx_buf` 1 本、3,840 B。`shared` からバイトを入れ替えながら写す（`put_strip`、PIE） | `board.c:67`、`:541-549` |
| ドライバ | ESP-IDF の `spi_master`、`queue_size=1`。帯ごとに `spi_device_queue_trans` と次の帯の前の `spi_device_get_trans_result` | `board.c:397`、`:104-146`、`:567` |
| SPI | SPI2_HOST、80 MHz、mode 0、MOSI 35・SCLK 36・CS 37（GPIO マトリクス経由）、DC は GPIO 34 を CPU が切り替え | `board.c:379-397` |
| 窓 | 上から続けて送る間は RAMWR を開いたまま（`next_row`）。不連続・狭い窓・フレームの最後は窓を開き直す（CASET・RASET・RAMWR）。コマンドの前に送信中のものを回収する | `board.c:511-517`、`:172-178` |
| フレームの終わり | `board_present_finish()` で最後の帯の完了を待ち、その後に表示の確定（ACK）。送信の失敗は Kasane の修復（repair）に返る | `app_session.c:561`、`docs/perf/kasane-dma-overlap.md` |
| 帯 1 本の SPI の時間 | 約 440 µs | `board.c` のコメントの値（未測定） |
| 帯 1 本の線の時間 | 384 µs（3,840 B × 8 ÷ 80 MHz） | 計算 |
| 全画面の送信 | 約 7.4〜7.5 ms（17 帯。最後の帯は 7 行） | 計算と上のコメントの値 |
| STRESS の描画と送信 | `render_ms` 7.38・`send_ms` 2.90（中央値、23 窓） | 実測（docs/kasane/image-rows.md） |

**待ちの仕組み（コードから）:** 帯 N を写して積んだ後、CPU は帯 N+1 を `shared` に描く。描き終えたら、帯 N の送信の完了を待ってから `tx_buf` に写す。1 帯ごとの時間は `max(帯 N+1 の描画, 帯 N の送信)` で、合計は `Σ max(描画_i, 送信)`。送信の時間はどの帯もほぼ同じなので、帯の順を変えても合計は変わらない（帯の重さのばらつきが、そのまま待ちになる）。

### 1.1 今のドライバの中身（ESP-IDF v6.0.1 の `spi_master`、コードを読んだ事実）

パスは `components/esp_driver_spi/src/gpspi/` の下。行番号は v6.0.1。

**帯 1 本の流れ:**

1. **呼び出し側（`board.c:567`）:** `spi_device_queue_trans`（`spi_master.c:1218`）。
   - `check_trans_valid` → `setup_priv_desc`：内部 RAM の DMA 可能なバッファなら写さない。`tx_buf` はこれに当たる。
   - `xQueueSend(trans_queue)` → `spi_bus_lock_bg_request`（ISR を起こす）。
2. **ISR `spi_intr`（`:971`）、前の帯の完了の割り込み:**
   - `spi_post_trans`（`post_cb`）→ `xQueueSendFromISR(ret_queue)`。
   - バス・ロックの調停のループ → `xQueueReceiveFromISR` で次の帯を取り出す。
   - `spi_new_trans`（`:814`）→ `portYIELD_FROM_ISR`。
3. **`spi_new_trans` の中身（毎回すべてやり直す）:**
   - `spi_setup_device`：前と同じ装置なら何もしない。
   - `spi_hal_setup_trans`（`esp_hal_gpspi/spi_hal_iram.c:126`）：割り込みのクリア、線の数、dummy、MISO の遅延、MOSI・MISO のビット長、cmd・addr の長さ、CS の保持を書き、`hal->trans_config` を `memcpy` する。
   - `s_spi_dma_prepare_data`（`:747`）：
     - `spicommon_dma_desc_setup_link`（`spi_common.c:365`）で記述子の鎖を作り直す（4,092 B ごとに 1 個。3,840 B は 1 個）。
     - `gdma_reset` → `spi_hal_hw_prepare_tx`（SPI の送信 FIFO のリセット、`OUTFIFO_EMPTY` のクリア、DMA の送信の有効化）→ `gdma_start`。
   - `spi_hal_enable_data_line` → `pre_cb` → `spi_hal_user_start`（`spi_ll_apply_config` + `spi_ll_user_start`）。
4. **呼び出し側、次の帯の前（`board.c:114`）:** `spi_device_get_trans_result`（`:1270`）。
   - `xQueueReceive(ret_queue)` で眠って待つ → `uninstall_priv_desc`。

**帯ごとの上乗せの出どころ（推定、未測定。S0 で測る）:**

- キュー 3 回：送る、ISR で受ける、ISR から返す。
- ISR の入口と出口。
- バス・ロックの調停。
- SPI のレジスタの十数回の書き込み。
- 記述子の作り直し。
- GDMA のリセットと開始。
- 呼び出し側のタスクの起床。

線の時間 384 µs に対するコメントの値 440 µs の差、約 56 µs がこの合計に当たるかは S0 で確かめる。

**`spi_master` が GDMA に触る口:**

- `spi_common.c:267` の `alloc_dma_chan`：S3 では `gdma_new_ahb_channel` で送信チャネルを確保し、`gdma_connect(..., GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_SPI, 2))` で SPI2 につなぐ。
- 開始とリセットは `gdma_start` と `gdma_reset`（`spi_master.c:743-744` のマクロ）。
- どちらも `esp_hw_support` の公開の GDMA API（`esp_private/gdma.h`）。自前のドライバも、チャネルの確保とつなぎはこの API を使えば、I2S の音や SPI3（SD）とチャネルを取り合わない（Q5）。
- `CONFIG_SPI_MASTER_ISR_IN_IRAM`（既定 y）は `GDMA_CTRL_FUNC_IN_IRAM` を選ぶ。`gdma_start` などは IRAM にある。
- `gdma.c` 自体は、手元の部分的な checkout に無く、未読。

**分割転送（SCT）の API は使えるが、今回の用途に合わない。**

- API は `esp_private/spi_master_internal.h`：`spi_bus_multi_trans_mode_enable`（`:1669`）と `spi_device_queue_multi_trans`（`:1792`）。S3 では `SOC_SPI_SCT_SUPPORTED_PERIPH` が SPI2 だけ。
- 合わない理由:
  - 半二重の装置が要る（今の LCD の装置は全二重の既定）。
  - 呼ぶたびに `heap_caps_malloc(MALLOC_CAP_DMA)` で CONF のバッファを取る。
  - 全区間のデータを積む時に揃えておく必要がある。帯を描きながら足すことはできない（TRM の H2 と同じ。Q3）。
  - 区間の間で CS が上がる（`sct_gap_len`）。CS が上がった後も ST7789 が RAMWR を続けるかは、確かめていない。

**記述子:**

- `spi_bus_initialize` で `max_transfer_sz` から個数を決める（`spi_common.c:338`）。今は `sizeof(shared)` = 3,840 B なので 1 個。
- 帯ごとに `spicommon_dma_desc_setup_link` で中身を書き直す。

**`spi_device_polling_transmit`（コマンド、`board.c:170`）** は `spi_device_polling_start/end`（`:1378`/`:1425`）。割り込みを使わず、完了を回って待つ。

## 2. ハードウェアの制約（TRM）

| # | 事実 | TRM |
| --- | --- | --- |
| H1 | DMA の 1 回の転送（single transfer）の長さは 1〜32 KB。`SPI_MS_DATA_BITLEN` は 18 ビット | p.1108、p.1124 |
| H2 | 送信の途中で GDMA のデータが足りなくなると、SPI は止まらず**最後のデータを繰り返して送る**。`SPI_OUTFIFO_EMPTY_ERR_INT` と `GDMA_OUT_EOF_CHn_INT` が立つ | p.1118 |
| H3 | 記述子は 3 ワード（DW0: size[11:0]・length[23:12]・owner・eof、DW1: バッファの番地、DW2: 次の記述子）。1 個で 1〜4,095 B。送信の記述子は、バースト無しなら番地・長さの整列は不要 | p.360-361、p.364 |
| H4 | 記述子とバッファは内部 RAM `0x3FC88000〜0x3FCFFFFF` に。外なら記述子の検査で `GDMA_OUT_DSCR_ERR_CHn_INT`、チャネルは止まる。owner の検査は `GDMA_OUT_CHECK_OWNER_CHn` が 1 のときだけ | p.363 |
| H5 | `GDMA_OUTLINK_DSCR_ADDR_CHn`（`GDMA_OUT_STATE_CHn_REG` の下位 18 ビット）は、先読みした（まだ処理していない）次の記述子の番地。`GDMA_OUT_DSCR_CHn_REG` も同種の番地 | p.394、p.372 |
| H6 | `GDMA_OUT_DONE_CHn_INT` は、1 個の送信の記述子のデータを送り終えたときに立つ。`GDMA_OUT_TOTAL_EOF_CHn_INT` は EOF 付きの最後の記述子を送り終えたとき | p.367、p.363 |
| H7 | 分割転送（configurable segmented transfer）は GP-SPI2 だけ。CONF の状態で区間ごとにレジスタを DMA から書き換えられ、全体が終わると `SPI_DMA_SEG_TRANS_DONE_INT`。長さの上限は無い | p.1131-1132 |
| H8 | SPI に送るバイトの順を入れ替える機能は無い（ビットの順だけ、`SPI_WR_BIT_ORDER`）。バイトの順を変えられるのは LCD_CAM（パラレル）で、この機体の SPI 配線には使えない | p.1114、p.1083 |
| H9 | マスタの SPI の時計は最大 80 MHz | p.1108 |

**H2 の帰結（この設計の最重要の制約）:** 「描くより先に DMA が走り出し、描画が追いつく前提で長い転送を組む」形は使えない。DMA がまだ書かれていない行に達すると、止まらずに古いデータか繰り返しのデータを LCD に送る。**転送は、送るデータがすべて書き終わってから始める。** 速くできるのは、(a) 転送と転送の間（上乗せ）を詰めることと、(b) 次の帯の写しを、前の帯の送信と重ねて置ける場所（送信用の領域）を増やすこと、の二つだけ。

## 3. 設計

### 3.1 持ち物

- ドライバは GP-SPI2 と GDMA の送信チャネル 1 本を専有する。LCD のために `spi_master` を使わない（`spi_bus_initialize(SPI2_HOST, ...)` も `spi_bus_add_device` もしない）。SPI3（SD・外部、`board_spi3_acquire`）は今のまま `spi_master`。
- ピン・時計・モードは今と同じ（MOSI 35・SCLK 36・CS 37 は SPI2 の CS0 として GPIO マトリクス経由、DC 34 は GPIO、80 MHz、mode 0、半二重の送信だけ）。
- レジスタの扱いは ESP-IDF の HAL（`hal/spi_ll.h`、`hal/gdma_ll.h`）か、`soc/spi_struct.h`・`soc/gdma_struct.h` の構造体で行う。GDMA のチャネルの確保は `esp_private/gdma.h` の確保の API で行い、ほかのドライバ（I2S・SPI3 など）とチャネルが衝突しないようにする。**どちらの API も IDF v6.0.1 のヘッダでの確認が要る（未確認）。**

### 3.2 バッファ

- **`shared`（描画用）は今のまま 1 本。** `board_strip()` も呼び出し元も変えない。
- **送信用の輪（ring）:** `RING_ROWS` 行（1 行 480 B、各行 16 B 境界）。今の `tx_buf`（8 行）を置き換える。案は `RING_ROWS = 12`（+1,920 B）と `16`（+3,840 B）。1 行に 1 個の記述子（12 B）を対応させる。輪の行は内部 RAM の .bss（H4）。
- 帯 j の行は、輪の上で帯 j−1 の行の直後に続けて置く（輪の端で折り返す）。記述子は行ごとなので、折り返しても 1 回の転送を 1 本の記述子の鎖で組める（H3）。`RING_ROWS = 12` なら、帯 j の最後の 4 行は帯 j−1 の最初の 4 行と同じ場所になる（重なり）。

### 3.3 行が空いたかの判定

- 輪の行 r が書いてよいのは、「r を指す、積んだ転送の記述子を GDMA がすでに読み終えた」とき。GDMA は RAM からデータを FIFO へ読んでから送るので、線を通り終える前でも、RAM の行は再利用できる。
- **判定の手段（優先順）:**
  1. 転送の完了（下の 3.4 の ISR が数える、転送の番号）: 完了した転送の行はすべて空き。確実。
  2. 走行中の転送の進み: H5 の「先読みした次の記述子の番地」から、その 2 つ前までの記述子の行を空きとみなす（先読みの分と、処理中の分を余裕として残す）。**この余裕で足りるかは TRM に書かれていない。未決（§7 の Q1）。**
- 2 が使えない（または信用しない）場合、`RING_ROWS = 16` で 1 だけを使う（帯 2 本分の輪。前の前の帯の完了を待つだけで済む）。

### 3.4 転送の開始（深さ 2 の待ち行列）

- 待ち行列は「送信中 1 本 + 待機 1 本」。帯 j を写し終えたら、その転送（行の記述子の鎖、長さ 3,840 B など）を待ち行列に入れる。SPI が空いていればすぐ始め、送信中なら待機に置く。
- **前の転送の完了で、次を始めるのは ISR。** SPI2 の転送完了（`SPI_TRANS_DONE`）か GDMA の `OUT_TOTAL_EOF` の割り込みで、待機があれば GDMA の outlink の番地・長さ・SPI の開始を書く。ISR は IRAM に置き、仕事はレジスタの書き込み数個と、完了した転送の番号を進めることだけ。
- CPU は、帯 j+1 を写す前に待ち行列の空き（待機が空く）を待つ。空かなければ、ISR の完了の印をポーリングで待つ（待っている間 CPU は他にすることがない。`ulTaskNotifyTake` で眠るかは §7 の Q4）。
- CS は転送ごとに上げ下げする（今と同じ。RAMWR は CS の上げ下げをまたいで続く、`board.c:503-509` の注記）。DC は転送の間ずっと 1。コマンドで 0 にするのは、待ち行列が空になってから。

### 3.5 写し（バイトの入れ替え）

- 今の `put_strip`（PIE の入れ替えながらの写し、`board.c` の `pie_swap`）を、1 行ずつ輪の行へ行う。行 r に書く前に §3.3 の判定で空きを確かめる。写しは行の順に進むので、空いた行から順に書け、最後の重なった行だけが待ちになる。
- 狭い窓（`board_present_rect`）は、各行の `x..x+cols` を輪の行の先頭に詰めて写し、記述子の長さを `cols × 2` にする（今の詰め直しと同じ結果のバイト列）。

### 3.6 コマンドと窓

- コマンド（CASET・RASET・RAMWR・MADCTL など）は、待ち行列（送信中と待機）が空になるのを待ってから、CPU 制御の転送（`SPI_W0〜W15`、64 B まで）で DC=0（パラメータは DC=1）として送る。ポーリングで完了を待つ。今の `command()` と同じ順序の保証。
- `next_row` の連続の判定、窓の開き直しの条件、`board_present_frame_end()` は今と同じ意味。

### 3.7 フレームの終わり・同期の呼び出し

- `board_present_finish()`: 待ち行列が空になる（最後の転送が完了する）まで待ち、最初に起きた誤りを返す。Kasane の「ACK の前に送信の完了」の約束は変えない（L2 の「最後の帯を待たない」は、この仕様の範囲外）。
- `board_present_sync()`・`board_present_rect_sync()`: 積んでから finish する（今と同じ意味）。
- `g_board_async = 0`（ブロッキングの経路）は、ドライバの上でも「積んで即 finish」として残す（A/B の器を保つ）。

### 3.8 誤り

- 検出: GDMA の `OUT_DSCR_ERR`、SPI の `OUTFIFO_EMPTY_ERR`（H2。この設計では起きてはならない）、転送の完了の時間切れ（例: 想定の 4 倍、帯 1 本で 2 ms）。
- 起きたら: その転送と待機を捨て、SPI と GDMA を初期化し直し、`next_row = -1`（窓を開き直す）、次の `board_present*` か `finish` が誤りを返す（今の `tx_reap` の誤りと同じ経路で、Kasane の修復に乗る）。
- 数える: 転送の数・誤りの種類ごとの数・待ちの合計時間を、既存の DD のカウンタ（`DD_TX_*`）に載せる。`OUTFIFO_EMPTY_ERR` は試験で 0 であることを確かめる。

### 3.9 変えないもの（呼び出し側から見て）

- 公開の関数と意味: `board_strip`・`board_present`・`board_present_rect`・`board_present_sync`・`board_present_rect_sync`・`board_present_submit`・`board_present_finish`・`board_present_frame_end`・`board_capture*`。呼び出し元（`app_session.c`、`shell.c`、`editor.c`、`codeedit.c`、`tutorial.c`、`wifi_ui.c`、`pocket_workspace.c`、`pickmodal_draw.c`、`pocket_capture.c`、各 probe）は変えない。
- 送る前の上書き（`pet_hub_overlay`・`pocket_capture_overlay`）と画面の取り込み（`PIX` は入れ替え前の画素）は、`shared` の上で今の順に行う。
- `BOARD_ENDIAN_PROBE`（RAMCTRL の切り替え）、`KASANE_P0_BUS_PROBE` の計測点（相当の位置に置き直す）。

## 4. メモリ

| 項目 | 今 | 新（`RING_ROWS=12`） | 新（`16`） |
| --- | --- | --- | --- |
| 描画用 `shared` | 3,840 B | 同じ | 同じ |
| 送信用 | `tx_buf` 3,840 B | 輪 5,760 B | 輪 7,680 B |
| 記述子 | `spi_master` の中、1 個（§1.1） | 12 × 12 B = 144 B | 16 × 12 B = 192 B |
| ISR のコード（IRAM、DRAM と同じ池） | `spi_master` の ISR | 推定 0.3〜1 KB | 同じ |
| `spi_master` の SPI2 の分 | あり（未調査） | 無くなる | 無くなる |

増える .bss は全アプリのゲストの部屋を減らす。`spi_master` の SPI2 の分がどれだけ戻るかは、map で確かめる（§6）。

## 5. 効果の見込み（すべて推定）

| 要素 | 見込み | 根拠 |
| --- | --- | --- |
| 転送ごとの上乗せの削減（キュー・ISR・タスクの起床を、ISR のレジスタ書き込みに置き換え） | 1 帯 約 56 µs → 数 µs、全画面で −0.5〜0.9 ms | コメントの 440 µs と計算の 384 µs の差。実際の上乗せは未測定（§6 の S0） |
| 深さ 2 の待ち行列と輪（`RING_ROWS=16`） | `send_ms` 2.90 の大部分。合計が max(描画, 送信) に近づき、STRESS で −2.5〜2.8 ms | 帯ごとのばらつきの吸収。帯ごとの描画の分布は未測定 |
| 同（`RING_ROWS=12`、重なり 4 行） | 上の約半分（−1.4 ms 前後） | 先に進める量が帯の半分 |

## 6. 検証

- **S0（実機、ドライバの前）:** 今の `spi_master` の帯ごとの時間（積んでから完了まで、ISR の時刻、`KASANE_P0_BUS_PROBE`）と、帯ごとの描画の時間の分布を測る。上乗せと、輪の大きさの効き方を数字にする。
- **host の模型の検査:** ドライバの「輪・記述子・待ち行列・行の空きの判定」を、GDMA と SPI の動きを模した偽物（読み進みの速さ・ISR の遅れを無作為に変える）の上で走らせる。検査すること: (a) 送られたバイト列が、今のドライバの送るバイト列と一致する（全フレーム、狭い窓・コマンドの混じる順を含む）、(b) DMA がまだ読んでいない行に CPU が書かない、(c) 誤り（記述子の誤り・時間切れ）の後に正しく立ち直る。
- **実機:** (a) `OUTFIFO_EMPTY_ERR` と `OUT_DSCR_ERR` が 0、(b) 同じバイナリの A/B（輪の深さを実行時に 1 と 2 で切り替え）で `render_ms`・`send_ms`、(c) 画面の目視（MISO が未配線で、ソフトウェアでは LCD の画素を確かめられない。`board_capture` は入れ替え前の画素）、(d) ホーム・各アプリ・エディタ・オーバーレイ（音楽の帯、通知）・画面の取り込みの動作。
- **既存の検査:** `tools/test_board_dma_overlap.py`・`tools/report_board_transport.py`（`shared`・`tx_buf` の大きさを決め打ち）と CI の board の host 検査（`put_strip` を取り出す、49a6e17）を、新しい形に合わせて直す。
- **`spi_master` との比較:** 古い経路と同じバイナリには入れられない（SPI2 を両方が持てない）。別のビルドどうしの比較になるので、命令キャッシュの配置で 15% 動く（CLAUDE.md）前提で読む。

## 7. 未決（監査で詰めること）

- **Q1:** §3.3 の 2（走行中の転送の進み）: `GDMA_OUTLINK_DSCR_ADDR` は「先読みした次の記述子」。GDMA が記述子 x のデータをどこまで RAM から読み終えたかは、何で分かるか。余裕を何個の記述子にすれば安全か。
- **Q2:** 転送の完了を ISR で知るのは `SPI_TRANS_DONE` と `GDMA_OUT_TOTAL_EOF` のどちらがよいか（GDMA の EOF は FIFO へ読み終えた時点で、線を通り終えた時点ではない可能性）。CS を上げるのは SPI の完了なので、次の転送の開始は SPI の完了に合わせるべきか。
- **Q3:** 転送と転送の間に CPU を挟まずに続けるため、分割転送（H7）で 1 フレームを 1 本の鎖にする案。各区間の CONF で長さを変えられる。ただし H2 のため、鎖の全区間のデータを始める前に書き終える必要があり、帯ごとに追加はできない。採るか。
- **Q4:** CPU の待ちは、ポーリングか、タスクの通知で眠るか（眠ると Wi-Fi などの他のタスクが走れるが、起床の遅れが増える）。
- **Q5:** GDMA の送信チャネルの確保と、ほかのドライバ（I2S の音、SPI3 の SD、`esp_lcd` を使う部分の有無）との衝突。IDF v6.0.1 の `esp_private` の API の安定性。（§1.1: `spi_master` 自身が `gdma_new_ahb_channel`・`gdma_connect`・`gdma_start`・`gdma_reset` を使っている。同じ API で確保すれば取り合いは GDMA のドライバが仲裁する。）
- **Q6:** GPIO マトリクス経由の 80 MHz の半二重の送信の、自前の設定での正しさ（`spi_master` が内部で設定している入力の遅延補償・時計の分周）。
- **Q7:** `RING_ROWS` の既定（12 か 16 か）と、.bss の増加の受け入れ。
- **Q8:** 1 行 1 記述子の GDMA の記述子の取り込みの上乗せ（記述子を読むたびの AHB のアクセス）で、送信の速さが落ちないか。2 行 1 記述子にする手。

## 8. 段階

1. S0 の計測（実機）。
2. ドライバの骨組み: 今と同じ深さ 1（帯の完了を待ってから写す）で、`spi_master` を置き換える。送るバイト列が今と同じこと（host の模型）と、上乗せの削減（実機）を確かめる。
3. 輪と深さ 2（`RING_ROWS=16`、§3.3 の 1 だけ）。同じバイナリの A/B。
4. 必要なら `RING_ROWS=12` と §3.3 の 2（Q1 が解けたとき）。
5. 60 fps の間隔（`VM_DISPLAY_PERIOD_MS`）は別の変更で扱う。
