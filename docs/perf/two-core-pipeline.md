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

## 8. 計画の改訂（2026-11-14、Fable、コード読解のみ。#2 の実装・実測は §8.7）

この節は、§5 の段階表を、2026-11-11 以降の実装（P2a）と 11-13〜14 の実測（HUD の run、`plain_walk`、`box_reuse`、hot/cold、`KASANE_PIECES`）に合わせて引き直したもの。**ビルド・host 試験・実機は使っていない。** 読んだ木は HEAD `eaa9a78` と、作業ツリーの未コミットの変更（`band_bits`、下の #2）。数値には「実測（出典）」か「推定（式）」を付けた。行番号は ±数行の目安。

### 8.0 結論

- **遅延モード（P2a、`CONFIG_POCKET_DISP_DEFER`）は、実機で一度も測られていない。** コードは入っている（§8.1）が、文書に遅延モードの `FRAME_P0`（`defer=` `hook_ms=` `settle_ms=`）の記録もジョブ名も無い。実機の記録は P1（すぐ join）まで（frame-breakdown.md §11、lcd-dma-driver.md §9.14）。この checkout の履歴では `main/hal/disp.c` が `a767306` の 1 コミットに畳まれていて、測定用コミットの有無は履歴からは確かめられない。§2.6 の検査（チェックサム、影の再描画、host の「遅い join」）も未実装。
- **2 コア化の値打ちは、11-10 の見込みから変わった。**
  - 出荷の周期 33 ms では、どの場面も 1 フレームの仕事が 11〜16 ms（推定、§8.3）で周期に収まる。**33 ms のままなら 2 コア化で fps は変わらず、払うのは DRAM（静的 約 +6.6 KB）と危険だけ。**
  - 16 ms では、inline のままでも STRESS・パドック・軽いレースは平均で 16.67 ms を下回る見込み（推定 11.4〜15.9 ms。11-09 の実測は 13.4〜17.5 ms）。**inline で届かないのは JS が長い窓（DERBY の WIDE 2、MEGADEMO の WAKE）と、ログ・GC・続きのターンのフレーム**。2 コア化はそこに 4〜7 ms の余裕を作る（推定）。
  - **重ねた後は、ほとんどの場面で表示側（描画＋転送）が律速になる**（推定: STRESS は表示 6.7〜7.6 ms に対し UI 側 4.6〜4.8 ms）。このとき `js_pre` を削る変更（#6）はフレームを縮めず、縮むのは `js_post`（commit を含む）と、送信が隠していない描画（実測 0.44〜1.5 ms、lcd-dma-driver.md §9.17）だけ。JS が律速の窓（WIDE 2）では逆になる。
- **次の一手は実装ではなく測定**: 同じバイナリで inline / sync / defer を窓ごとに巡回させる 16 ms の変種（`sdkconfig.variant.twocore_ab`）を作り、DERBY と STRESS を 1 回ずつ走らせる（§8.5）。これ 1 本で、(a) 今の木の inline の 16 ms の実力、(b) 遅延モードの最初の実測、(c) 命令キャッシュの共有による JS の伸び（R1）が同時に出る。

### 8.1 各段の現状（コードで確認）

| 段 | 状態 | 確認した場所 | 実機 |
| --- | --- | --- | --- |
| P0 探針 | 済 | `main/hal/frame_bd.c` の `FRAME_P0`（`js_pre`/`js_post`、適格、`nodefer=` の 17 項目） | 実測済（frame-breakdown.md §8） |
| P1a 整理 1〜6 | 済 | `present_run`（`app_session.c:2885`）は `ksn_present_job_run` と `board_present_frame_end` だけ。`board_host_overlays`（`:850-895`） | 実測済（§7.1） |
| P1a 7（host の「遅い join」） | **未実装** | `tools/` に `disp_start` を遅らせて実行する試験は無い。あるのはエンジンの印の検査 `tools/vmtest/cfunc_join.sh` だけ。`tools/test_session_dispatch.c:105` は「`present_defer_block()` は常に 1」で、遅延の経路を通らない | — |
| P1b 表示タスク | 済、既定 n | `disp.c`: 静的タスク、`CONFIG_POCKET_DISP_CORE` 既定 0、優先度 5、**stack は `CONFIG_POCKET_DISP_STACK` 既定 6,144 B**（§0 の 4,096 B ではない） | 実測済。直書きの送信で inline 9.21 / 表示タスク 9.13 ms（frame-breakdown.md §11）。音の途切れ・10 分の WDT・影の再描画は未確認のまま |
| P2a エンジンの印 | 済 | `JS_SetCFunctionJoinHook(join_rt, js_join_hook)`（`app_session.c:1855`）、`TRY_NOJOIN`（`:1861-1948`） | 未測定（印の無い関数の費用「分岐 1 つ」は推定のまま） |
| P2a join を遅らせる | 実装済、**実機未測定** | `present_defer_block`（`:2518`）、`present_frame_body`（`:2934`: `may_defer` なら `board_host_overlays=false` → `disp_start` → join せず戻る）、`present_ack`（`:2911`、確認だけ = F6）、`present_settle`（`:2920`）、`present_account`（`:3011`、`KSN_IO` で `defer_off`）、`app_tick_body` の冒頭（`:2585-2590`）、遷移の `present_settle()`（`:913, 1241, 1417, 1484, 1717, 2315`）、`main.c:920, 988, 990, 1027, 1274` の `disp_join()` | **無し** |
| F8（`beginFrame` の BUSY） | 済 | `pocket_shiki_frame.c:944`（`any_pending() && !disp_busy()`）、`commit_impl:1247` で join 後に見直し、`buffers():336` は `image_band_buf` の確保の前に join | 未測定 |
| F11（job の静的化） | 済 | `pf_display`/`pf_port`/`pf_stats`/`pf_completion`（`app_session.c:2895-2898`） | — |
| F12（持ち主の assert） | 済 | `disp.c:20-33`（`CONFIG_POCKET_DISP_ASSERT`） | — |
| F14（1 秒の診断、join の計数） | 済 | `disp.c` の `join waiting > 1 s`、`frame_bd_join_wait` | — |
| §2.6 の 1（要所の持ち主タグ）・2（入力のチェックサム）・3（影の再描画） | **未実装** | `ASSERT_UI_JOINED` などの語は木に無い | — |
| 3 モードの巡回（`inline\|sync\|defer`） | **形が違う** | `POCKET_FRAME_BD_DISP_AB` は `inline / task5 / task6`（`frame_bd.c:189-191`）。`DISP_DEFER=y` のビルドでは task の窓が遅延になり、**sync の腕が無い** | — |
| P2b・P3・P4 | 未着手 | — | — |

実装が §7.3 の記述と合うことは読んで確かめた。合わない点は 2 つ: stack の既定（上）、`CONFIG_POCKET_DISP_DEFER` は `DISP_TASK` に従属して既定 y（`DISP_TASK=y` にしたビルドは、そのまま遅延する）。

**Shiki の 1 フレームの形（§8.4 の前提）。** 面のバッファは 1 本で、表示中のフレームは前面 `[0, count)`、新しいフレームは後ろ（tail）に積む。`commit_impl` は、join（印）→ `old_box` → `merge()`（tail を前面へ写す）→ `band_signature` → `box_damage` → publish の順（`pocket_shiki_frame.c:1238-1335`）。つまり **commit の仕事はすべて join の後 = `js_post`** にある。

### 8.2 改訂した段階

共通の道具: ブローカーの変種（`sdkconfig.variant.<name>` の Kconfig 断片）。今あるのは `kasane_ab`（`POCKET_KASANE_AB`）と `turnperf`（`POCKET_VM_TURNPERF` ＋ `POCKET_FIXED_RANDOM_SEED`）。測定用コミットを積んで revert する方式は使わない。

| 段 | 中身 | 合格 | 中止 | 実機の測定 |
| --- | --- | --- | --- | --- |
| **M0** 測定の道具（挙動は変えない） | (1) 周期を Kconfig に: `VM_DISPLAY_PERIOD_MS`（`app_session.h:103` の `#define 33`）を `CONFIG_POCKET_VM_DISPLAY_PERIOD_MS`（既定 33）から取る。(2) `DISP_AB` の腕を `inline / sync / defer` にする（優先度 6 の腕は外す: 直書きの後は 5 と 6 の差が −0.08 / −0.06 ms で無い、実測 frame-breakdown.md §11）。(3) `sdkconfig.variant.frame_bd`（`POCKET_FRAME_BREAKDOWN` ＋ 周期 16）、`sdkconfig.variant.twocore_ab`（それに `POCKET_DISP_TASK`・`POCKET_DISP_DEFER`・`POCKET_FRAME_BD_DISP_AB`・`POCKET_DISP_ASSERT`） | 既定のビルドの bin が変わらない（周期の既定 33）。host の一式 | — | なし |
| **M1** 遅延モードの最初の実測 | `twocore_ab` で DERBY（デモのレース 1 本、WIDE 2 を含む）と STRESS LV1。窓ごとに `FRAME_BD`（`js` `rn` `sd` 周期）と `FRAME_P0`（`js_pre` `js_post` `defer` `hook_ms` `settle_ms` `nodefer=`） | 判定ではなく記録。読むもの: ① inline の窓の仕事の和（今の木の 16 ms の実力）、② defer の窓の周期、③ JS の CPU 時間 = `js − hook_ms`（defer と inline の差 = 命令キャッシュの共有、R1）、④ `rn + sd` の sync と inline の差（CPU 0 の費用の再確認）、⑤ `defer / presents`、⑥ 表示タスクの stack の空き | 止まる・WDT・`join waiting > 1 s`・画面の乱れ → 遅延を切って原因を調べる（P2a の検査を先に入れる） | DERBY 1、STRESS 1。持ち主の目視（下） |
| **G1** 判断（持ち主） | M1 の ①〜③ で、2 コア化を続けるか決める（§8.5） | — | — | — |
| **P2a-V** 遅延の正しさの検査 | §2.6 の 2（handoff で前面・bank・`candidate_sig`・`band_bits` の FNV、join で比較）と P1a の 7（host の「遅い join」＋ TSan）を入れる。`CONFIG_POCKET_DISP_ASSERT` の下 | host: 遅い join で STRESS・DERBY の digest が変わらない、TSan 0 件。実機: チェックサムの不一致 0（DERBY 1 レース、STRESS 2 分） | 不一致が 1 件でも出たら、その入口を join に戻す（`TRY_NOJOIN` から外す） | `twocore_ab`（`DISP_ASSERT` 入り）で DERBY・STRESS |
| **P2a** 遅延を「使える」と認める | 上の 2 つの後で、合格の線を引き直す（右） | 16 ms の defer の窓で、1 フレームの仕事（周期 − 待ち）が **STRESS ≤ 10.0 ms、DERBY レース（WIDE 2 以外）≤ 10.5 ms、WIDE 2 ≤ 13.5 ms、パドック ≤ 9.5 ms**（§8.3 の推定＋約 0.8 ms）。JS の CPU 時間が inline の窓の +10% 以下、`rn + sd` が +0.15 ms 以下。`defer` がレースで 95% 以上、STRESS で 90% 以上（続きのターンは設計どおり重ねない、実測 54/60）。10 分で WDT の警告なし。DERBY の効果音に途切れなし（耳）。`memlog.py --port --check` で空きヒープを記録 | JS の CPU 時間 +25% 超 → P4（`render_band` を IRAM、DRAM −3.2 KB）を試し、それでも越えるなら 2 コア化を止める。**CPU 1 の変種は退避先にしない**（重なるのは転送の待ち 2.0〜4.4 ms だけで、inline がすでに 12〜14 ms なら得が小さい、推定） | `twocore_ab` で DERBY・STRESS・BIG WAVE・MEGADEMO |
| **P3'** commit の署名を join の前へ | §8.4 の形（署名とビット表を 2 面にして入れ替える）。#2 が入った後、P2a の合格の後 | 16 ms の defer の窓で STRESS の 1 フレームの仕事 **−0.4 ms 以上**（旧 −0.5。`box_reuse` で commit が縮んだ分を引いた）。digest と `POCKET_PROC_SIG_CHECK` が不変 | −0.3 ms 未満、または digest が動く | 同じバイナリの腕（`g_pocket_proc_sig_early`）を `DISP_AB` と掛ける |
| **P2b** 適格を広げる | notice・animation・text を 1 つずつ。M1 の `nodefer=` で、実際に遅延を止めている項目だけ | P2a と同じ | — | 項目ごと |
| **P4** | `render_band` を IRAM に | P2a の中止の線に掛かったときだけ。配置を変えた A/B（同じカーネルが配置で 15% 動く） | 逼迫判定の回数が増える | — |
| **60 fps の出荷**（別の文書の範囲） | 周期 16 ms を既定に、µs の期限（frame-breakdown.md §6 の `deadline`）、ログのフレーム | 持ち主の判断 | — | — |

M1 を P2a-V より先に置く理由: 時間の測定は検査なしでも成り立ち、G1 で「続けない」になれば検査の実装が要らない。ただし **M1 の結果を「遅延は正しく動く」の根拠にしない**（競合は黙って画素を壊す種類の誤りで、`board_capture` は遅延を止めるのでソフトウェアでは見えない）。M1 では持ち主に DERBY のレースと STRESS の画面の目視を頼む。

M1 で初めて起きる組み合わせ（未確認の前提）:
- **PIE を 2 つのタスクが同時に使う。** 描画（表示タスク、CPU 0）と `pocket.derby` の `ser()`（`CONFIG_POCKET_DERBY_SER_PIE`、UI、CPU 1）。どちらもコアに固定されていて、コプロセッサの状態はコアごと・タスクごとなので成り立つはず（推定）。P1 は表示タスクが描く間 UI が待っていたので、同時には使っていない。R8 は P1 で半分だけ解けた。
- **DRAM。** 静的 +約 6.6 KB（推定: stack 6,144 ＋ TCB・セマフォ 約 0.5 KB）。表示タスクの stack の空きは 3,288〜3,384 B（実測、frame-breakdown.md §11）なので、4,096 B に縮めると空き 約 1.2〜1.3 KB（推定）で、§5 の条件（1 KB 以上）の際。DERBY のレースの空きは逼迫の線（24,576 B）のすぐ上（実測 26.1〜26.3 KB、frame-breakdown.md §8。その後の変更で動いているので取り直す）。R5 は未解決。

### 8.3 1 フレームの見込み（すべて推定。元の値は実測）

元にした実測（30 フレームの窓、**出荷の周期 33 ms**。kasane-text.md 追補、lcd-dma-driver.md §9.16〜9.17、qjs-hotcold.md §13.7）:

| 場面 | JS のターン | 描画 | 転送 | D = 描画＋転送 |
| --- | --- | --- | --- | --- |
| DERBY レース | 5.1〜7.0 | 2.7〜5.2 | 2.0〜4.4 | 5.5〜8.0（窓の対で見た目安。両端の和は 4.7〜9.6） |
| DERBY パドック | 約 3.0 | 約 4.0 | 約 3.5 | 約 7.5 |
| STRESS | 5.2〜5.3 | 4.4〜4.7 | 2.3〜2.9 | 6.7〜7.6 |

仮定（それぞれ未測定）:
- 三つの計時の外（`tick_other` ＋ `loop_other`）は 0.85 ms（実測 0.79〜0.88、frame-breakdown.md §0、11-09。その後は取り直していない）。
- `js_post` は 11-10 の実測（STRESS 1.81〜1.86、レース 1.80〜1.95、パドック 1.37〜1.41）に、ターン全体の縮み（約 ×0.77）を掛けた値: STRESS 約 1.4、レース 約 1.45、パドック 約 1.05。**今の `js_pre` / `js_post` は測っていない**（M1 で出る）。
- 周期を 16 ms にしても、各部の時間は 33 ms のときと同じ（11-05 と 11-09 の比較では STRESS の 3 つの値がほぼ同じだった、実測）。
- 表示タスクを CPU 0 に置く費用は 0〜+0.1 ms（実測 −0.08、直書き、STRESS）。handoff 0.05 ms（§2.5 の推定）。**命令キャッシュの共有による JS の伸びは入れていない**（R1。+10% なら JS 律速の窓で約 +1 ms）。
- WIDE 2 のターンは 11-10 の実測 13.0〜13.4 ms に ×0.77〜0.85 を掛けた 10.0〜11.4 ms（**今の木では未測定**）、D は 5〜7 ms。

式: inline = JS ＋ D ＋ 0.85。2 コア = max(D, `js_pre` ＋ 0.85) ＋ `js_post` ＋ 0.1。

| 場面 | inline の仕事 | 2 コアの仕事 | 律速 | 周期 33 ms | 周期 16 ms（inline → 2 コア） |
| --- | --- | --- | --- | --- | --- |
| DERBY レース | 11.5〜15.9 | 7.1〜9.6 | 表示（軽い窓）／ほぼ同じ（重い窓） | どちらも 30 fps。変わらない | 平均は届く。余裕 0.8〜5 ms → 7〜9.5 ms |
| DERBY レース WIDE 2 | 16〜19.5 | 11〜12.4 | **JS** | どちらも 30 fps | **届かない（51〜60 fps 相当）→ 届く** |
| DERBY パドック | 約 11.4 | 約 8.7 | 表示 | 変わらない | 届く → 届く（余裕 5.3 → 8 ms） |
| STRESS | 12.8〜13.8 | 8.2〜9.2 | 表示 | 変わらない | 届く（余裕 2.9〜3.9 ms）→ 余裕 7.5〜8.5 ms |

- 11-10 の見込み（STRESS 11.5〜11.8、レース 10〜10.3、WIDE 2 13.6〜13.9 ms、frame-breakdown.md §8）より 2〜3 ms 短い。inline も同じだけ縮んだので、**2 コア化で縮む幅は 7 ms 前後から 3〜6 ms に減った**。
- 16 ms の inline の「届く」は平均の話。ログを書くフレーム、GC、続きのターン（STRESS で 6/60、実測）は別に乗る。11-09 の 16 ms 実測では、仕事 13.4 ms のパドックでも窓の最長は 21.7〜22.4 ms だった（その後 `ESP_LOG` を 1 行 1 回の送信にした。今の最長は未測定）。
- 33 ms のまま 2 コア化して得るものは、UI タスクが 1 フレームに塞がる時間が減ることだけ（入力の取り込みが早くなる、など）。表示が出る時刻は変わらない（§0）。電力は測っていない。

### 8.4 候補の置き場所

| 候補 | 中身 | 置く場所 | inline での見込み | 2 コアでの見込み |
| --- | --- | --- | --- | --- |
| **#1** 模様線のフレームで、捨てる entry ごとの `packet_next` を近道に（`ksn_shiki_frame.c` の ext の歩み） | 描画側 | **#2 の後で要否を決める。** #2 のビット表が有効な帯は、掛かる entry しか訪れない（作業ツリーの `BAND_FOR(start)`）ので、捨てる entry がほぼ無くなる。残るのは、表が無い（確保できない・帯をまたぐ行・`candidate_sig` 以外）ときの道だけ | 描画 −0.1〜0.3 ms、フレーム −0.05〜0.2 ms（推定、lcd-dma-driver.md §9.17）。#2 の後は、そのうち残る道の分だけ | 同じ。描画は表示側なので、効くのは「送信が隠していない描画」の範囲（実測の上限: レース 0.44〜1.5、STRESS 0.56〜0.69 ms） |
| **#2** 帯ごとの「掛かる entry」のビット表（`band_bits`、実装中） | commit（署名の歩みで作る）＋描画（表だけを歩く） | **P2a より前、inline で入れて測る。** `kasane_ab` に腕 `band_bits`（`g_pocket_proc_band_bits`）を足し、`KASANE_PIECES` の「隠れない描画」と、ターン（commit が延びる分）の両方を見る | 描画 −0.2〜0.6 ms、commit ＋0.03〜0.06 ms（どちらも推定、根拠は弱い: memset 約 2.2 KB と entry×帯ごとの OR）。フレームに届くのは隠れない描画まで | **差し引きが変わる。** commit の増分は `js_post`（直列）に丸ごと乗り、描画の得は表示側の隠れない分だけ。表示が律速の場面では得がそのまま届き、JS が律速の窓（WIDE 2）では commit の増分だけが残る（推定 +0.03〜0.06 ms、小さい） |
| **#3** 汚れた帯の中で送信を狭める | 送信 | 採らない（lcd-dma-driver.md §9.15・§9.17: 狭い窓が Shiki の遅い道に落ちる、開き直しの費用、差し引き ±0.3 ms、推定） | — | 2 コア化の後は表示側が律速なので、送る bytes を減らす価値は上がる（host の計数で STRESS 56〜70%）。**P2a の実測で表示が律速と確かまったら見積もり直す。** 遅い道の問題はそのまま |
| **#4** 不透明のパネル | 描画（画素が変わる） | 持ち主の判断。2 コア化と独立。入れるなら M1 の後（基準を動かさない） | 未見積もり | 効くのは隠れない描画の範囲。2 コア化で価値は増えも減りもしない |
| **#5** 2 コアの重ね合わせ | — | M0 → M1 → G1 → P2a-V → P2a | — | §8.3 |
| **#6a** `pocket_derby.c` の soft-double の残り（`cam_q` `:482-495`、`js_aim` `:922-940`、レーンの pan、`PART_PANVIS`/`PART_PANPOLES` `:1041-1055`） | JS のターン（`pocket.derby` は `TRY_NOJOIN` なので `js_pre`） | **2 コア化と独立、いつでも。** 先に `turnperf` で事象ごとのサイクルを取り、0.1 ms 未満なら手を付けない | −0.1〜0.4 ms（推定、根拠は弱い: 1 フレームに double の演算 100〜数百回 ＋ sqrt 数回。S3 の FPU は単精度だけ） | **表示が律速の場面では 0。** JS が律速の窓（WIDE 2）では inline と同じだけ縮む。60 fps を inline で狙うなら、WIDE 2 を縮める手はここと `drawMany` の類しかない |
| **#6b** `derby_prog.js` `prail()` の Newton 3 段（`:390-397`、Shiki の VM で走る） | JS のターン（commit の前 = `js_pre` のはず。`FRAME_P0` で確かめる） | 同上 | −0.05 ms 以下（推定: 柱 1 本あたり掛け算 2〜4 回の差）。精度の検討（遠→近で発散しない条件）が要るので、測ってから | 同上 |
| **#6c** Gouraud の double の辺関数（`ksn_shiki_frame.c:335-372`、`gouraud_band`） | **JS のターンではなく描画**（帯を描くときに画素ごとに評価する） | #1・#2 と同じ扱い: 描画側。どのアプリのどの場面が通るかを先に数える（未確認） | 未見積もり（画素あたり double の積和 3 本 ＋ 補間。面積に比例） | 表示側。隠れない描画の範囲で効く。Gouraud の帯は描画が送信より重くなりやすいので、通る場面があるなら隠れない分は大きいはず（推定） |

**旧 P3（commit の前倒し）と `box_reuse`・#2 の関係。**
- 旧 P3 が前へ出そうとした 2 つのうち、`frame_damage` の歩みは `box_reuse` で消えた（表示中のフレームの箱は `front_box` に持つ。実測: ターン −0.1〜0.27 ms、lcd-dma-driver.md §9.16）。残っているのは `band_signature`（11-05 の実測で STRESS 0.64〜0.68 ms、レースの commit 全体 0.42〜0.51 ms、パドック 0.26 ms、lcd-dma-driver.md §9.11。その後の hot/cold で 1 割ほど短いはず、推定）。
- `band_signature` は今、`merge()` の後の前面を読み、`candidate_sig` と（#2 で）`band_bits` に書く。**どちらも表示タスクが読む**（§1.2 の `candidate_sig` の行。`band_bits` も同じ行に入る: 書くのは join の後の commit だけ、`merge()` が `band_bits_valid=false`、確保は UI の commit。作業ツリーの変更は I1 を守っている）。したがって join の前に書くには **2 面目が要る**。
- **P3' の形**: tail のまま署名する（entry の添字は `start − tail_base`）。書き先は 2 面目の署名（1 面 約 180 B、推定: hash 68 ＋ x0/x1 34 ＋ first/end 68 ＋ 箱 8）と 2 面目のビット表（1 面 = 17 帯 × ⌈entry 数 / 32⌉ 語 × 4 B。STRESS の 1,024 entry で 2,176 B、DERBY の 768 で 1,632 B）。箱からの damage もスカラーなので前に出せる。join の後に残るのは、`any_pending()` の見直し、`merge()` の写し、2 面のポインタの入れ替え、publish。
- **値段**: Shiki のヒープに面あたりもう 1 枚（STRESS で +2.2 KB、合わせて 約 4.4 KB）。#2 を入れずに P3' だけなら +180 B で済むが、#2 を入れた後で表だけ join の後に作ると歩みが 2 回になり、P3' の得を食う。**#2 と P3' はまとめて「2 面にするか」を決める。**
- **見込み（推定）**: 表示が律速の場面で、`js_post` から `js_pre` へ移した分がそのまま縮む（STRESS −0.5〜0.6 ms、レース −0.3〜0.4 ms、パドック −0.1〜0.2 ms）。JS が律速の窓では 0。inline では 0（順番が変わるだけ）。

### 8.5 次の一手と、持ち主が決めること

**次の実装は M0（測定の道具）、続けて M1 の実機 2 本。** 出荷の挙動を変えない小さな変更で、今の計画でいちばん大きい未知（遅延モードが実機でどう動くか、inline が 16 ms でどこまで届いているか、命令キャッシュの共有で JS がどれだけ伸びるか）が 1 回で埋まる。#2 は並行して inline で進めてよい（`kasane_ab` の腕で測る。M1 の基準を動かさないよう、M1 のビルドは #2 の腕を固定する）。

持ち主が決めること:
1. **60 fps（周期 16 ms）を出荷の目標にするか。** しないなら、2 コア化は P2a（既定 n）で凍結するのが筋（33 ms では得が無い、§8.3）。
2. **G1（M1 の後）**: inline の 16 ms の実測を見て、(a) 平均が届いていて足りないのは重い窓だけ → 2 コア化を続ける／#6 と JS 側で詰める、のどちらを先にするか、(b) JS の CPU 時間の伸びが +10% を越えたときに P4（DRAM −3.2 KB）まで払うか。
3. **DRAM**: 表示タスクの静的 約 +6.6 KB（stack 6,144 B）。stack を 4,096 B に縮めるか（空き 約 1.2 KB、推定）、UI の stack を 32,768 → 28,672 B に縮めて相殺するか（UI の空きの最小は実測 22,764 B）。DERBY のレースの逼迫の線との距離は M1 で取り直す。
4. **Shiki のヒープ**: #2 の 1 面（STRESS 2.2 KB、DERBY 1.6 KB）、P3' を入れるなら 2 面。
5. **#4 不透明のパネル**（画素が変わる）。
6. **目視**: M1 と P2a で、遅延したフレームの画面（DERBY のレース、STRESS）と DERBY の効果音。ソフトウェアでは確かめられない（`board_capture` は遅延を止める。MISO は未配線）。

### 8.7 #2 帯ごとの entry のビット表（`band_bits`、2026-11-14、実装・実測）

§8.4 のとおり P2a の前に inline で入れた。`band_signature`（commit、UI タスク、join の後）が、`sig_add` の帯の判定と同じ条件で「帯 b に掛かる entry の先頭 j」のビットを立てる。表は面ごとに 1 枚（17 × ⌈区画/32⌉ 語、1,024 entry で 2,176 B、Shiki の heap から最初の commit で取る。取れなければ従来の歩み）。`render_band` は表のビットだけを昇順にたどる（描く順は同じ）。模様線のあるフレームは各 entry をその先頭から読む（§8.4 の #1 の大半はこれで不要）。同じバイナリの A/B の行 `band_bits`。

- host: STRESS `a93618d1`・DERBY `ecf35bcd` の digest が 0/1 と 8・4 行の帯で一致、`tools/kasane_contract/run.sh` 通過。callgrind（STRESS、4 行）: `render_band` 331 → 225 M（−32%）、commit +1.8 M。
- 実機（a61c5e3、`sdkconfig.variant.kasane_ab`、30 フレームの窓、実測）:

| 場面 | band_bits on | off | 差 |
| --- | ---: | ---: | ---: |
| STRESS: render | 3.73〜3.91 ms | 4.48 ms | **約 −0.6 ms** |
| STRESS: Shiki の画像（1 呼び出し） | 12.8〜13.5 k サイクル | 18.1 k | −27〜30% |
| DERBY レース（CLOSE）: render | 2.36 ms | 3.55 ms | **約 −1.2 ms** |
| DERBY レース: Shiki の画像（30 フレーム、1,020 呼び出し） | 9.54 M サイクル | 18.25 M | **−48%** |
| DERBY レース: 送信が隠していない描画（`KASANE_PIECES`） | 0.19〜0.36 ms | — | §9.17 の 0.44〜1.5 ms から |

- **DERBY のレースの描画は、ほぼ全部が送信の陰に入った**（描画 1.8〜2.9 ms に対し DMA 待ち 3.5〜5.3 ms）。inline でフレームを縮めるのは送信（バイト数）だけになり、2 コア化（§8.2 の M0/M1）の後も表示側の律速は送信。
- turn（commit を含む）の差は窓のばらつきの中（STRESS 5.46〜5.48 ms）。
