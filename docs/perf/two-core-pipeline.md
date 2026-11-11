# 2 コアのフレームパイプライン（設計、P1 まで実装）

[性能と描画の文書](README.md) / [1 フレームの内訳](frame-breakdown.md)

状態: **P0 の探針、P1b（表示タスク、すぐ join、`CONFIG_POCKET_DISP_TASK` 既定 n）、P1a の 1〜6 まで実装（§7）。** 実測は [frame-breakdown.md](frame-breakdown.md) §8（P0）と §11（P1）。CPU 0 に置くと描画＋転送が +0.9 ms（+9.6%）延びた。IMU を読み手がいるときだけ読み、キーボードを INT ピンで門にした後は +0.41 ms。残りは LCD の SPI 完了割り込み（1 フレーム約 29 回、CPU 0）で、割り込みを CPU 1 に移すと −0.09 ms になり、合格の基準（+0.15 ms）を満たす（frame-breakdown.md §11）。送信をレジスタ直書き（`lcd_spi.c`、割り込みなし）にすると、表示タスクで 9.13 ms、inline 9.21 ms（キューの inline は 9.62 ms）で、表示タスクを使うビルドの既定にした。同じコアなら +0.10 ms。以下の本文は 2026-11-10 にコードを読んで作った（HEAD 881887f、ビルド・測定なし）。行番号は ±数行の目安。数値は「実測（出典）」か「推定（式）」を付けてある。未確認の前提は §6 にまとめた。

フレーム N の描画・転送を別のタスク（CPU 0）で回している間に、UI タスク（CPU 1）がフレーム N+1 の JS を進める。JS が描画の読む状態に触れる所でだけ待ち合わせる（join）。

## 0. 結論

- **新しい画素バッファは要らない。** 追加の DRAM は表示タスクの分だけで、静的に約 4.7 KB（推定: stack 4,096 B ＋ TCB 約 0.4 KB ＋ セマフォ約 90 B ＋ job 約 120 B）。UI の stack を 32,768 → 28,672 B に縮めれば、差し引き約 +0.6 KB。
- **同期は「所有権のトークン 1 つ ＋ `disp_join()`」。** 表示中のフレームの入力（Shiki の前面、Kasane の 2 つの bank、strip、SPI）は、表示タスクが読むだけで、UI は書かない。制御状態（`submitted`/`active`/`pending`/`has_committed`）は、UI だけが join の中で書く。ロックで守る共有の可変データは作らない。
- **以前の読みの訂正**:
  - `V.patch/replace` の BUSY は `presenting` ではなく、`core_begin` の `building||submitted||repairing`（`ksn_core.c:271`）から来る。`V.stats` は BUSY を返さない。
  - レンダラは core を `const` でしか読まない。core に書くのは `prepare_frame`/`presented`/`failed`/`defer_repair` だけなので、この 4 つを UI 側に移せば、core は表示中に変わらない。
  - 表示は遅れない。フレーム N の表示が始まるのは、今と同じく JS N の直後。frame-breakdown.md §4 の「1 フレーム遅れる」は、この設計では起きない。
- **重ねるフレームは許可リストで絞る**（§1.3）。DERBY・STRESS・BIG WAVE・MEGADEMO の通常のフレームだけ join を遅らせ、それ以外はすぐ join する（今と同じ意味）。
- 1 フレームの見込み（推定）: T = max(D, loop_other + tick_pre + js_pre) + js_post + 後処理（D は描画＋転送）。STRESS 約 11.4〜11.7 ms、DERBY のレース約 8.5〜9.9 ms。命令キャッシュの取り合いと、CPU 0 の他のタスクの影響は入っていない。

## 1. 所有権のモデル

### 1.1 相とトークン

`disp_state`（`atomic_uchar`）は次の順に移る。

- `IDLE`: UI がすべてを持つ。
- `RUNNING`: 表示タスクが job を実行している。
- `DONE`: 表示タスクは終わったが、UI がまだ join していない。

その後 `IDLE` に戻る。UI から見ると、`IDLE` 以外はすべて「表示相」。

### 1.2 状態ごとの持ち主

| 状態 | 表示相で表示タスクがすること | 表示相で UI がしてよいこと | 守る仕組み |
| --- | --- | --- | --- |
| Shiki のフレームの前面 `[0,count)` とヘッダ | 読む | 読むだけ。書く `merge()`（`pocket_shiki_frame.c:743`）は join の後 | `commit_impl:756` の先頭で join |
| 後ろ `[tail_base, …)` と `tail_*`、`building` | 触らない | 読み書き | `tail_base ≥ count`（§5 P1a の 5b） |
| `candidate_color` / `candidate_sig` / `sig_ready` | 読む | 書かない | 5a・5c |
| `committed_*` / `pending` / `has_committed` / `repair_required` / `committed_lost` | 読む | join の中（`present_result:1186`）と、join 後の commit でだけ書く | join |
| `half_carry` / `image_band_buf` / `image_band_*` | 読み書き | 触らない | 5c・5d |
| Shiki のヒープ（ロックなし）、`slots[]`、`shiki_vm`、JIT のコードと登録 | 触らない | 専有 | 5d、ヒープに `NOT_DISPLAY` の表明 |
| Kasane の 2 つの bank、image の登録表 | 読む | 読むだけ（`core_begin` は `submitted\|\|repairing` で BUSY） | `submitted/repairing/presenting` を join まで立てたままにする |
| core の制御状態（`active/submitted/repairing/full_redraw/repair_*/transaction/outcome`） | 読む | 書くのは handoff の前の `prepare_frame` と、join の中だけ | レンダラから変更の呼び出しを抜く（P1a） |
| core の `invalidated*` | 触らない | 書いてよい | `prepare_frame`（`ksn_core.c:961-988`）が `repair_*` へ移してから handoff する |
| `ksn_view_host`、`kasane_state`、ksn_cache | 触らない | UI の専有。JS から見える読み出しは join の後 | §2.3 |
| `ksn_render.c` の static と prof の累計 | 専有 | join の後に読む | A/B のスイッチは join 済みの場所で切り替える |
| strip `shared[]`、`tx_*`、`half_*`、SPI | 専有 | 呼ばない | `board_strip_next/board_present*/tx_reap` に持ち主の表明を置く |
| host の重ね描き（text、pet、capture、`paint.c`、notice） | 読まない（`job.host_overlays=false`） | 自由 | 適格条件で「handoff の時点ですべて無効」を求める |
| `board_capture` | 遅延モードでは無効 | `board_capture(true)` の前に join | capture 中は UI の上でその場で実行する |
| pump の類 | 触らない | 自由 | Kasane の入口が join する |
| ゲストのヒープ、JS の値 | 触らない | 専有 | 表示の経路は JS の参照を持たない |
| システムのヒープ | 確保しない | 自由 | デバッグ: alloc hook で、表示タスクから呼ばれたら abort |
| ログ・統計 | `job.*` に書くだけ。`ESP_LOG` を呼ばない | join の中で合算し、ログは join の外で書く | 30 フレームのログを `present_frame` の冒頭（join の直後）へ移す |

### 1.3 重ねてよいフレーム（handoff のときに UI が判定する）

次のすべてを満たすフレームだけ、join を遅らせる。

- フォアグラウンドのゲストである。
- 続きのターンでも、Back のターンでもなく、停止の要求も出ていない。
- `app_tick_body` の末尾の、通常の present である。
- schema、provider、pixel・video・grid の resource、animation block、modal、notice がどれも無い。
- text の編集、pet の alert、録音、`board_capture` のどれも動いていない。
- 前の join で IO のエラーが出ていない。

DERBY の `SK.resource({maxSegments:768, spare:512})` と STRESS の `P.resource()` は、どちらも image モードの proc surface なので、この条件を満たす。

## 2. 同期の仕組み

### 2.1 選んだ形

```c
bool       disp_start(disp_job *job, bool deferred); // UI。IDLE→RUNNING。!deferred は直後に join
bool       disp_inflight(void);                      // UI。state != IDLE
ksn_result disp_join(void);  // UI だけ。IDLE なら atomic load 1 回で戻る。そうでなければ done を待ち、
                             // IDLE にして present_finish(job) を UI で実行する
```

- **UI → 表示タスク**: 表示タスクの task notification（index 0）で起こす。
- **表示タスク → UI**: static の 2 値セマフォ `done` で知らせる。UI の notification の index 0 は `vm_wake`（`main/vm/vm_wake.c:37-49`）が使っているので使えない。index を増やすと、全タスクの TCB が太る。
- job は static に 1 つ置く。今 `present_frame` の stack にある構造体（`app_session.c:2535-2556`）を job へ移す。

### 2.2 退けた案

| 案 | 退けた理由 |
| --- | --- |
| mutex（優先度継承） | 表示タスクが取る前に UI が取れてしまう。両タスクが同じ優先度なので、継承の利点もない |
| event group | 遅く、足りる機能はセマフォと同じ |
| spinlock / portMUX | 割り込みを止めたまま、数 ms の描画をまたげない |
| atomic / seqlock で共有の値を守る | 共有の可変の値を job への写しで無くしたので、要らない |
| UI が spin して join を待つ | CPU 1 を空回しで焼く。まず block で作り、測ってから考える |

### 2.3 join する場所

**A. JS から見える入口**（先頭で `disp_join()`）
- Shiki: `commit`（`:756`）、`resource`、`createSurface`。
- `kasane` 名前空間のメソッド（`pocket_kasane.c:2717-2723, 2803`）、scene と schema のメソッド、`animation.*`。
- `pocket_kasane.c` が他のモジュールに出している関数のうち、状態を読むもの。
- **join しないもの**: `beginFrame`、`draw*`、`pocket_proc_native_*`、`P.heap()`、`pocket_kasane_invalidate(_bands)`。

**B. `app_tick_body`**
- 表示相なら `prior_submission` は false とみなす。
- `needs_present` の門は、handoff の後に新しい無効化があるときだけ join する。
- 続きのターンで `has_submission()` / `proc_pending()` を読む前に join する。
- **`present_frame()` の先頭では必ず join する。** 表示が JS より長い場面では、ここで待つのが律速になる。

**C. 遷移と解体**
- `app_stop`、`app_suspend`、`app_resume`、`app_start*`、`app_overlay_tick`、`app_vm_prepare_stop` の先頭で join する。
- `pocket_kasane_reset` は `presenting` の間 BUSY を返すので、必ず先に join する。
- 表示タスクは起動時に 1 回だけ作り、壊さない。

**D. UI ループの直接描画**
- `tick_run` のモーダル 3 種の前と、`board_capture(true)` の前で join する。

**E. 転送の失敗（`KSN_IO`）**
- join の中で `present_finish` を今どおり走らせ、同じ内容を同期で最大 3 回やり直す。
- それでも失敗したら、そのセッションの残りは遅延モードを止める。
- **意味が変わるのはここだけ**で、LCD の転送が続けて失敗したときに限る。

### 2.4 不変条件

- I1: `disp_state != IDLE` の間、UI は §1.2 で「表示タスクが読む」としたものへ書かない。
- I2: 表示タスクは、ヒープ・ログ・JS・`paint.c`・host の重ね描き・`invalidated*` に触れない。待つのは SPI の結果キューだけ。
- I3: core の制御状態、`pending/has_committed`、`host->presenting` を書くのは UI だけで、handoff の前か join の中に限る。
- I4: JS から見える値は、join 済みの状態か、join の前後で変わらないものだけ。
- I5: `board_*` の present 系を呼ぶのは、表示相なら表示タスク、そうでなければ UI。

### 2.5 費用・優先度の逆転・WDT・ISR（推定）

- **1 フレームの費用**: CPU 時間は 6〜10 µs、遅れは最大で約 50 µs（16.67 ms の 0.3%）。
- **デッドロック**: 待つ向きは「UI が表示タスクを待つ」の一方向だけ（I2）なので起きない。
- **優先度**: 表示タスクは優先度 5 から始め、音（sfx 7、mp3dec 6）を遅らせない。
- **WDT**: 表示タスクは notification か SPI の待ちで block する。UI の「idle が 500 ms 走れていなければ 1 tick 譲る」（6d04053）はそのまま要る。join は 1 s で timeout のエラーを出し、そのあとも待ち続ける。
- **ISR**: SPI2 の ISR は `app_main`（CPU 0）が登録するので、CPU 0 にあるはず（未確認）。

### 2.6 違反を安く見つける（`CONFIG_POCKET_DISP_ASSERT`、既定 n）

1. **持ち主のタグ**: `xPortGetCoreID()` を比べ、`ASSERT_UI_JOINED`、`ASSERT_DISPLAY_OR_JOINED`、`ASSERT_NOT_DISPLAY` を要所に置く。
2. **入力のチェックサム**: handoff のときに前面と bank の FNV を取り、join で比べる（推定 50〜100 µs）。
3. **影の再描画**: join の直後に同じフレームをハッシュだけ取る port で描き直し、送った画素のハッシュと比べる。実機の本当のタイミングで画素が一致するかを確かめる唯一の手段。
4. **host の「遅い join」**: `disp_start` は job を覚えるだけにして、`disp_join` で実行する。最も意地悪な順序が決まった形で再現できる。加えて、pthread の表示スレッドと `-fsanitize=thread` で試す。

## 3. 表示タスクのコア・優先度・stack

- **CPU 0 に固定、優先度 5、stack 4,096 B、`xTaskCreateStaticPinnedToCore` で .bss に置く。** 描画は PIE を使い、コプロセッサを使うタスクはコアに固定されるので、固定は必須。
- **stack の見積もり**: 最も深い呼び出しの鎖で約 1.3〜1.8 KB、割り込みとコプロセッサの退避域を足して約 2.5 KB（推定）。主な関数のフレームは `render_rects_body` 912 B、`render_group` 640 B（いずれも ELF の `entry` から）。`uxTaskGetStackHighWaterMark` で空きが 1 KB 以上あることを確かめる。
- **UI の stack**: 未使用の最小は実測 23,788 B（vm-L2-results.md:818）。28,672 B に縮めても約 19.7 KB の余裕がある（推定）。全アプリで最高水位を取り直してから、別のコミットで縮める。
- **CPU 0 の他のタスク**: Wi-Fi のリンク中は表示が 6〜8% 延びる見込み（類推）。sfx が CPU 0 を占める割合は未測定。
  - **実測（P1、frame-breakdown.md §11）**: CPU 0 では、STRESS の描画＋転送が +0.9 ms 延びた（描画 +9%、転送 +0.3 ms）。`input_task`（CPU 0、優先度 6、I2C）が 1 秒に約 130 ms 走るが、表示タスクを優先度 6 にしても 0.15 ms しか戻らない。残りの原因は未特定。
- **CPU 1 に置く変種**（`CONFIG_POCKET_DISP_CORE=1`）: 重なるのは転送の待ち（実測 2.8〜5.2 ms）だけ。STRESS は約 15.7 ms、DERBY のレースは 11.6〜14.3 ms の見込み（推定）。CPU 0 だと表示が 10% 以上延びるときや、命令キャッシュの取り合いが大きいときの退避先。

## 4. 命令キャッシュ

- **事実**: 描画の主なコードはすべて flash にある（`render_rects_body` 7,021 B、Shiki の `render_band` 3,162 B、`render_group` 2,248 B など）。IRAM に置くと DRAM を 1:1 で食う。
- **見立て（推定）**: 描画が触るのは 6〜10 KiB。JS から見ると、使える命令キャッシュが 32 KiB から 22〜26 KiB に縮む。ミスが 15% 増えると、JS は約 8.5% 遅くなる（0.57 × 0.15。STRESS で約 0.6 ms）。
- **決めるための測定**:
  1. 実機なしで、`tools/r5sim/icache.py` を 24 KiB 相当（6-way × 128 set）で回す。
  2. P1b の後、同じバイナリで `sync` と `defer` を窓ごとに巡回させ、JS の CPU 時間（JS − join の待ち）と描画時間を比べる。
  3. CPU 0 で描画の真似をする妨害タスクを窓ごとに入れ切りし、JS の伸びを見る。
- **判断**: JS の CPU 時間が +10% 以下、描画が +15% 以下なら進める。+25% を超えたら、(a) `render_band` だけを IRAM に置く（DRAM −3.2 KB）、(b) CPU 1 の変種、の順に試す。

## 5. 段階

共通の確認:
- host: STRESS の digest `de4a66c5`（`tools/test_stress_app.c:330`）、DERBY の先頭 `f2d667e3`（`tools/games/test_derby_host.c:1261`）、`tools/kasane_contract/run.sh`。
- 実機: `FRAME_BD` に `disp=inline|sync|defer` を足し、60 フレームの窓ごとに巡回させる。

| 段 | 中身 | DRAM | 合格 | 中止 |
| --- | --- | --- | --- | --- |
| **P0** 探針 | join の場所に印だけ置き、`js_pre`・`js_post`、場所ごとの回数、適格条件の項目ごとの回数、UI stack の最高水位を測る | 0 | レースと STRESS で `js_pre` が JS の 70% 以上、適格が 95% 以上、T の予測が 13 ms 以下 | `js_pre` が 50% 未満のアプリは、CPU 1 の変種が本命 |
| **P1a** 整理（タスクなし） | 下の 1〜7 | 数 B | digest が変わらない、契約試験が通る | digest が動く |
| **P1b** 表示タスク、すぐ join | `disp.c` を足し、`inline` と `sync` を巡回 | +4.7 KB（UI stack を縮めれば +0.6 KB） | `sync` の描画＋転送が `inline` +0.15 ms 以下。表示 stack の空き ≥ 1 KB。音の途切れ 0。WDT の警告なし（10 分）。影の再描画が一致 | 描画＋転送が +10% 超 → CPU 1 の変種 |
| **P2a** join を遅らせる | `beginFrame` の BUSY を緩める、join の場所を有効に、重ね描きを切る、IO の規則、3 モードを巡回 | 0 | host の遅い join で digest が変わらず TSan 0 件。実機で表明・チェックサム・影の再描画が 0 件。1 フレームの仕事が STRESS ≤ 12.5 ms、DERBY レース ≤ 11 ms、WIDE 2 ≤ 15.5 ms | §4 の判断 |
| **P2b** 適格を広げる | notice・animation・text の重ね描きを 1 つずつ | 0〜数十 B | 同上 | — |
| **P3** commit の前倒し | join の前に `band_signature` と `frame_damage` を済ませる | 0 | STRESS で 1 フレームの仕事 −0.5 ms 以上 | digest が動く |
| **P4** | §4 の (a) | −3.2 KB | 配置を変えた A/B で比べる | 逼迫判定の回数が増える |

P1a の中身:
1. `render_rects_body`（`ksn_render.c:1946`〜）から `ksn_core_presented/failed/defer_repair` の呼び出し（約 20 か所）を抜き、結果を返り値にする。
2. `ksn_view_host_present_complete`（`ksn_view.c:190`）を begin / run / end に分ける。
3. `pocket_kasane_present_complete`（`pocket_kasane.c:3042`）を prepare / finish に分ける。
4. `present_frame`（`app_session.c:2514`）を `present_prepare`、`present_run(job)`、`present_finish(job)` に分け、ログの塊を外に出す。
5. Shiki の直し:
   - (a) `tail_color` を足す。
   - (b) `:663` の base を `(has_committed || pending) ? candidate->count : 0` から作る。
   - (c) `sig_ready=false` と、draw ごとの `image_band_valid=false; carry_drop()` を `merge` に寄せる。
   - (d) `half_carry` の確保（`:570`）を `present_prepare` に移す。
6. `kasane_send`（`:604-635`）の重ね描きを `job.host_overlays` で切る。
7. host に「遅い join」のモードを足す。

## 6. コードだけでは決められないこと

- **R1** 命令キャッシュと SPI0 の取り合い、キャッシュの置換方式（§4 の測定 1〜3）。
- **R2** CPU 0 の負荷で表示が延びるか（音あり・なし、音楽、Wi-Fi、BLE で比べる）。**音なしの STRESS でも +0.9 ms 延びた**（frame-breakdown.md §11）。input の横取りは 0.15 ms 分だけで、残りは割り込みか I2C ドライバの待ち方か、未切り分け。
- **R3** SPI の ISR がどのコアにあるか。**CPU 0（`app_main` が初期化）で、表示タスクを CPU 0 に置いたときの上乗せの残り（約 0.4 ms）の正体だった。** `CONFIG_POCKET_LCD_ISR_CORE` で移せる（frame-breakdown.md §11）。
- **R4** 表示タスクの stack の深さ、コプロセッサの退避域、`StaticTask_t` の大きさ。
- **R5** DRAM の余裕: レースの空き約 42 KB から 4.7 KB を引くと、逼迫を抜ける条件（36,864 B）までの余裕は約 0.4 KB（推定）。UI stack を縮める交換が前提になる。
- **R6** まだ数えていない入口（`pocket_shape.c`、`pocket_derby.c`、`pocket_overlay.c`、`pocket_grid/pixel/video` ほか）。
- **R7** 表示経路の副作用（`ksn_font.c`、静的アセットの image port など）。
- **R8** PIE を 2 つ目のタスクで使えるか（コードには「owner task only」とある）。
- **R9** 続きのターン・park・Back の保存ターン・強制停止と表示相の組み合わせ。
- **R10** 待ち方との噛み合わせ（`last_frame_us` に join の待ちが入る）。
- **R11** 見込みの値（キャッシュと CPU 0 の影響を含まない）。P0 の実測から引き直す。

## 7. 実装の記録

### 7.1 P1a（2026-11-11）

§5 の P1a の 1〜6 を入れた。どれも inline で動く間は振る舞いを変えない（host の検査で確かめた）。

| 項目 | 入れたもの | コミット |
| --- | --- | --- |
| 1 | `render_rects_body` は `const ksn_core *` と準備済みの frame を受け取り、失敗の記録（defer repair／failed）を値で返す。中で使う 5 つの helper も `const` にし、core を書かないことをコンパイラに保証させた | aa84cfd |
| 2〜4 | `ksn_render_begin/run/end`、`ksn_view_host_present_begin/end`、`ksn_runtime_present_begin/end`、`pocket_kasane_present_begin/end`。begin と end が core を変え（UI）、run（描画と completion の finish）は読むだけ。`app_session.c` は advance・begin・end・集計・ログを UI に残し、`present_run`（描画・送信・`board_present_frame_end`）だけを表示タスクに渡す | aa84cfd |
| 5a〜5d | `tail_color`、tail の起点を pending の frame の後ろにも、`sig_ready=false` と image band・carry の無効化を `merge` へ、`half_carry` の確保を `pocket_proc_prepare_render`（present の前、UI）へ | 6c5cc69 |
| 6 | `board_host_overlays`: pet・マイク・文字欄・notice の重ね描きを止めるスイッチ（今は常に on） | 6c5cc69 |

- host: `test_kasane_dma_overlap`、`test_dma_completion_owners`、Kasane の一式（`KASANE_DMA_OVERLAP` あり・なし）、Shiki の proc・turn・api、STRESS の digest `84a31336`（4 行・8 行 × 範囲あり・なしで変更前と同じ）、DERBY の全 tier、board の half・transport・overlap、session dispatch。
- 実機（c474b9d、測定用のコミットは戻した）: STRESS と DERBY で誤りなし。STRESS の描画＋転送は inline 9.01〜9.23 ms、表示タスク 9.12〜9.50 ms。DERBY のレースの FIELD では周期 16.4〜16.5 ms。
- 残り: 7（host の「遅い join」）は P2a の仕組みと一緒に入れる。

### 7.2 P2a: A の入口を QuickJS で網羅する（2026-11-11、エンジン側のみ）

§2.3 A は「join する関数を手で並べる」形だった。並べ漏れはその関数が表示タスクと競合するだけで、host でも実機でもまず見つからない。そこで、同梱の quickjs-ng（このリポジトリで直接変更している。上流には出さない）に、**ファームが作ったネイティブ関数は呼ばれる前に join のフックを呼ぶ**という印を入れた。join しない関数（§2.3 A の除外）だけを手で外す。

- 印: `JSObject.u.cfunc` の `cproto` を 7 bit にして空いた 1 bit、`JSCFunctionDataRecord` の `data_len` の最上位 bit（data は 127 個まで。超えると RangeError）。オブジェクトの大きさは変わらない。
- 付け方: 印は「印を立てたスタックフレーム」から直接作られた関数にだけ付く（`rt->cfunc_marking` と `rt->cfunc_mark_sf`）。`JS_MarkCFunctions` / `JS_SetCFunctionMark` は呼び手のフレームで立て・下ろし、印のある関数の呼び出しは自分のフレームで立てる。したがって面が実行中に作る関数（cancel トークン、購読の `close` など）には付き、その呼び出しが走らせる JS（Kasane の scene の build など）と、その JS が作る・初めて使う組み込み（`Promise.finally` の thunk、`Array.prototype.map` など）には付かない。
  - 最初の版は「数を足す」形で、印のある呼び出しの中で初めて使われた組み込みにまで永久に印が付いた（Fable のレビュー F1）。重なりが黙って消える種類の誤りなので、フレームで限る形に直した。
- 遅延生成: 関数表（`JS_SetPropertyFunctionList`）の関数は最初に読まれたときに作られるので、登録の時点の印を残し、作るときはその印に**置き換える**（読み手の印は使わない）。autoinit は未使用だった id 3（`JS_AUTOINIT_ID_PROP_JOIN`）、遅延リストは `len` の 1 bit。入れ子の `JS_OBJECT_DEF` にも伝わる。エンジンの遅延 intrinsic（`Map` など）は常に印なし。遅延名前空間（`pocket_api_lazy`）も貢献者ごとに登録時の印に置き換える。
- 呼び出し側: `js_call_c_function` と `js_call_c_function_data` の 2 か所（ほかの経路は iterator の近道だけで、印のある関数は近道を通らない）。印の無い関数の費用は分岐 1 つ（推定。未計測）。
- 外す: `JS_SetCFunctionJoin(func, false)`。
- 検査: `tools/vmtest/cfunc_join.sh`（asan）。直接作った関数・data 関数・関数表・getter・入れ子・実行中に作った関数・`call`/`bind`/`map` 経由・例外の後で数が 0 に戻ること・エンジンの関数には付かないこと。フックの呼び出しを消す変異、遅延リストの印を消す変異は、どちらも検査が落ちる。`tools/vmtest/run.sh` の 88 件は変わらず通る。
- 外す面（`app_session.c` の `TRY_NOJOIN`）: 表示相の状態に触れない面を調べた結果（pocket の土台、memory、random、storage、app、fs、av、imu、io、net、ble、input、text、bridge、workspace、derby、shiki-debug、shapes）。kasane・capture・pet-hub・overlay は外さない。av の `outputSource`・`playbackSource` は Kasane に届くので、av 自身が印を付ける。`kasane.shiki` は `beginFrame`・`draw*`・slot/region/list・`heap`・`sin/cos` を外し、`commit`・`resource`・`createSurface` は join する。載っていない面は join する（安全側）。
- `disp_start` / `disp_join`（`main/hal/disp.c`）: start は表示タスクに渡してすぐ戻り、join は待ってから finish を UI タスクで走らせる。タスクが無いときはその場で走らせ、finish は join まで待つ。フックは `disp_join`。present はまだ `disp_run`（同期）なので、今は何も重ならない。

#### 配線の前に直すこと（Fable のレビュー、2026-11-11）

- F6: finish は「確認」（`present_end`、結果・統計・時刻を静的な job に残す）だけにする。集計・ログ・`ESP_FAIL`・§2.3 E のやり直しは次の `present_frame` / `app_tick` で。フックは任意の JS の深さで走り、エラーを返せない。
- F7: host の重ね描き（text・pet・capture）を、遅延するフレームでは止める（`board_host_overlays` を job ごとに）。それまでは text の `TRY_NOJOIN` は成り立たない。
- F8: `beginFrame` の BUSY を、表示中のフレームについては緩める。そのとき `commit` は join の後・`merge()` の前に `any_pending()||any_repair()` を見直す。`buffers()` が `image_band_buf` を確保するのは `resource` / `prepare_render` だけにする。
- F11: `port`・`stats`・`display_state`・`completion` は `present_frame_body` の局所変数。job から指しているので、遅延すると宙に浮く。静的にする。
- F12: `disp_join` を UI タスク以外から呼んだら止める assert（`CONFIG_POCKET_DISP_ASSERT`）。表示タスクから呼ぶと永久に待つ。
- F13: §2.3 の B・C・D の join（`app_stop` は面の reset とランタイムの解放の前）。
- F14: 待ちに 1 秒の診断、join した場所の計数（重なりが消えた原因を見えるように）。
- F3・F4（済・記録）: `js_call_math_fast` は印のある関数を通さない。GC の finalizer は join しない native の入口なので、表示相が読むものを finalizer で解放しない。

### 7.3 P2a: join を遅らせる（2026-11-11）

- `CONFIG_POCKET_DISP_DEFER`（`POCKET_DISP_TASK` が y のときだけ、既定 y）。`app_tick_body` の末尾の present だけが対象で、`present_deferrable()` が §1.3 を判定する: フォアグラウンド、Back・停止・続きのターンでない、ジョブの待ちが無い、表示タスクで動く、capture・text・録音・pet の alert・通知が無い、pixel・video・grid の保留が無い、Kasane 側（`pocket_kasane_present_deferrable`: schema・provider・video/pixel/grid の resource・animation block・modal・notice が無い）、前に転送の失敗が無い。
- 遅らせたフレームは `board_host_overlays=false` で描き、`present_ack`（join の中）で戻す。集計（`present_account`）は `present_settle()` で、次の present の冒頭・次のターンの冒頭・遷移で行う。
- 次のターンの冒頭（§2.3 B）: まだ表示中で、フレームが条件を満たし続けていれば join しない。そのときは `set_animation_time` と `needs_present` の門を飛ばし、`prior_submission` は false とみなす（表示中のフレームは core から見ると submission）。条件を外れていれば先に join する。続きのターンの `has_submission`/`proc_pending` は settle の後に読む。
- 転送の失敗（`KSN_IO`）を見たら、そのセッションの残りは遅らせない（§2.3 E）。
- FRAME_P0 の行に `defer`（遅らせたフレーム数）、`hook`/`hook_ms`（印のある native の呼び出しで待った回数と時間）、`settle`/`settle_ms`（host 側で待った回数と時間）を足した。
