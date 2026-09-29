# キー状態層（keystate）と `pocket.input.keys`

2026-09-29、ブランチ `vm/key-state`。ゲーム（連続移動＋別キーのアクション）を作れるように、JSアプリへ「いま押されている物理キーの集合」と押下・解放の辺を渡す。API仕様は [common-api.md §6](../api/common-api.md)、ここは経路・設計・却下案・実機で測る項目の記録。

## 結論

- **システム系APIの「横断的に読める作り」は、キー入力についてだけ成り立っていなかった。** `pocket.*` の面（capability登録・遅延名前空間・ポンプ）と、HALの読み取り専用アクセサ（`solar_time`、`motion` など）は横断的に共有されている。キー入力は「1消費者のストリーム」で、共有できる状態が無かった。FIFOを読むのは `keymap_poll()` 1か所で、通常キーの解放はそこで捨てられ、押下は `keystroke_t` 1個に翻訳されて `main.c` の入力キューへ流れ、ゲストへは `nav` を縮約したパッドのビットだけが渡る。
- そこで **HALに `keystate` を新設**した（`main/hal/keystate.c`）。`keymap_poll()` が翻訳の前に全ての辺（押下・解放・修飾キー）を渡し、どのタスクからもロック無しで一貫したスナップショットを読める。**`keymap_poll()` の戻り値・翻訳・修飾キーの扱いは変えていない。**
- JS面は `pocket.input.keys`（capability `input.keys`、supported=true、`limits` 無し）。`pocket_input.c` に載せ、既存の `held()`・`onAction` は変えていない。

## 現状の経路（読んで確かめたこと）

| 段 | 場所 | 内容 |
| --- | --- | --- |
| ハード | `board_key_event()`（`main/hal/board.c`） | TCA8418 の FIFO（`0x03` 件数、`0x04` イベント）から**1回に1イベント**。読むたびに `0x02`（INT_STAT）へ `0x1f` を書いて全フラグを消す。電気的 7行×8列を論理 4行×14列へ写す（論理 `(r,c)` = 電気 `R=c/2`, `C=r+4*(c%2)`）。 |
| 読み手 | `input_task`（`main/main.c`） | ループ1周で `keymap_poll()` 1回、`motion_poll()`、USB 1バイト、`vTaskDelay(5ms)`。**FIFOの排出は最大1イベント/5ms（計算で200イベント/秒）。** |
| 翻訳 | `keymap_poll()`（`main/hal/keymap.c`） | 修飾キー5個の押下状態だけを `held_*` に持ち、**通常キーの解放は `if(!e.pressed) return false;` で捨てる**。押下を `keystroke_t`（`nav`/`text`/`len`、`toggle_ime`、`force_stop`）へ。 |
| キュー | `keys`（16件、`main.c`） | `xQueueSend(keys,&k,0)`。満杯なら黙って捨てる（[backlog.md](backlog.md)「入力キュー」）。Ctrl+Alt+Del だけはキューを飛ばす。 |
| 消費 | `ui_task` → `tick_run()` | **アプリ実行中は1フレームに1打鍵だけ**受け取る。`nav` を `0x4000/0x10/0x20/0x40/0x80` へ縮約して `app_tick(buttons)`、続けて離したフレーム `app_tick(0)`。Back は保存ターン `app_tick(0x2000)`。 |
| JS | `pocket_input_pump(buttons)`（`app_session.c` の `run_pumps()`） | `held_mask`、`onAction`（press/repeat/release、400ms/120ms）、`held()`。6アクションだけ。継続ターン（L1/L2c）では呼ばれない。 |

依頼時の読みとの食い違いは3点で、どれも結論を変えない。(1) `pocket_input_pump()` はターンの**ポンプ段**（`run_pumps()` の最後、`frame()` の直前）で呼ばれ、ターンの厳密な先頭ではない。(2) `keymap_shift()` / `keymap_fn()` は `keymap.c` の外に呼び手が無い。(3) オーバーレイのセッションは `pocket.input` を注入しない（`app_session.c` の `surfaces_done`）ので、`input.keys` もフォアグラウンドのアプリだけ。

### キー入力の消費者と依存の向き

`board_key_event` を呼ぶのは `keymap.c` だけ、`keymap_poll` を呼ぶのは `main.c` の `input_task` だけ。それより下流は全て `ui_task` が受け取った `keystroke_t` を引数で渡される側で、HALを直接読むモジュールは無い。

| 消費者 | 受け取るもの | 呼ぶ場所 |
| --- | --- | --- |
| `volume_key` / `home_key` / `shell_key` | `keystroke_t` / `nav` | `main.c`（ホーム画面） |
| `editor_key` / `code_key` / `tutorial_key` / `wifi_ui_key` | `keystroke_t`（`takes_text` の画面は `text` だけ読む） | `SCREENS[].key` |
| `pocket_text_key` | `keystroke_t` | `tick_run()`（テキスト欄が開いている間） |
| `pocket_workspace_modal_key` / `sd_picker_modal_key` / `file_picker_modal_key`（→ `pickmodal_key`） | `keystroke_t` | `tick_run()` とオーバーレイの経路 |
| `overlay_key` → `pocket_overlay_key` | `keystroke_t` | オーバーレイの残余ループ |
| `pet_hub_key` | `nav` のみ | `ui_task`（通知が出ている間） |
| ゲスト | `buttons`（パッドのビット） | `app_tick()` |
| **ゲスト（新）** | `keystate` のスナップショット | `pocket_input_pump()` |

向きは HAL（`board` → `keymap` → `keystate`）→ `main.c` → `ui/`・`pocket/`。`keystate` は `keymap.c` が書き、`pocket_input.c` が読む。`keystate.c` 自身は他の何も参照しない。

## 設計

### keystate（`main/hal/keystate.{c,h}`）

- 状態: キーごと（`row*14+col`、56個）の押下ビット、キーごとの押下・解放の辺カウンタ（8bit、剰余）、全辺数、FIFOあふれ回数。32ビット語32個（128 B）＋シーケンス語。
- **書き手は1つ**（`keymap_poll()`、つまり入力タスク）。**読み手は任意**。シーケンスロック: 書き手が奇数にして語を書き、偶数に戻す。読み手は前後で同じ偶数を読めたコピーだけを採る。語は全て relaxed の atomic、順序はフェンスで付ける（C11で正しい seqlock。S3では relaxed の32bit atomic はただのロード/ストア）。書き手の窓はデバイスでは `portENTER_CRITICAL` で囲む — 同じコアの高優先度の読み手が、奇数のまま横取りされた書き手を待って回り続けるのを防ぐため。読み手はロックを取らない。
- **辺は交互にしか数えない。** 押されているキーの押下、離れているキーの解放は何も変えない。重複イベントに強く、あふれ後の遅れて来た本物の解放を二重に数えない。
- **FIFOあふれ**: `board_key_event()` は FIFO が満杯（件数10）のときだけ INT_STAT を読み、OVR_FLOW_INT（bit3）を `overflow` として返す（普段は I2C 転送が増えない）。`keymap_poll()` は押されている全キーを解放扱いにして（辺を数える）から今のイベントを適用し、`KEY_OVERFLOW` をログに出す。TCA8418 には行列キーの現在状態を読むレジスタが無いので、失われた辺は分からない。**押しっぱなしで止まるより、離れたまま止まる側へ倒す。** 修飾キー（`keymap.c` の `held_*`）はあふれでも今までどおりで、変えていない。
- 名前表は `keymap.c`（`keymap_key_name()` / `keymap_key_index()`）。`PLAIN[][]` から導き、印字文字はその文字、制御文字と修飾キーのセルは語（`del` `tab` `enter` `space` `fn` `shift` `ctrl` `opt` `alt`）。別名 `up/left/down/right/esc/back` は `keymap_poll()` が Fn で与える名前のセル。

### `pocket.input.keys`（`main/pocket/pocket_input.c`）

- `pocket_input_pump()` の頭で `keystate_snapshot()` を1回取り、前回のカウンタとの差から、そのターンの `down` / `pressed` / `released` の3つのビット集合を作る。JS の関数はこの集合を引くだけで、ターンの途中でキーボードを読まない。
- **隠す集合（hidden）**: 「アプリがキーボードを見失った」時点（再基準）で押されていたキーは、離されるまで `held`/`down` に出ない。その最初の解放も `released` に出ない（アプリが押下を見ていないので）。再基準のときは辺も数えない。
- `keys` は `pocket.input` の中でさらに遅延構築する（最初に読んだときに getter が本体を作って値に置き換える。`pocket_api.c` の遅延名前空間と同じ手）。`onAction` だけを使うアプリは getter 1個ぶんしか払わない。

### ライフサイクル

| 事象 | 何が起きるか | どこで |
| --- | --- | --- |
| アプリ起動 | 再基準（起動したEnterは離すまで見えない） | `pocket_input_install()` → `pocket_input_reset()` |
| アプリ終了 | 再基準、集合を空に | `app_stop()` → `pocket_input_reset()` |
| 中断（Backで眠る） | 再基準、集合を空に。眠っている間にホームへ送られた辺は届かない | `app_suspend()` → `pocket_input_suspend()` |
| 再開 | 最初のポンプが再基準（再開を選んだEnterは離すまで見えない） | 同上のフラグ |
| テキスト欄が開いている | そのターンごとに再基準 | ポンプが `pocket_text_active()` を見る |
| 入力を止めるSYSTEM通知 | 同上 | ポンプが `pocket_kasane_input_scope(false)==KSN_INPUT_BLOCKED` を見る（`app_tick()` がパッドを0にするのと同じ判定） |
| ピッカー画面（works / SD / file） | ゲストが回らないので、`main.c` の `tick_run()` が `pocket_input_keys_withhold()` で知らせる | `main.c` |
| FIFOあふれ | 押されていた全キーを解放扱い（辺あり） | `keymap_poll()` |

`main.c` の変更は `tick_run()` 冒頭の2行（ピッカー）と診断ビルドのUSB経路だけで、`app_session.c` は診断アプリのソース選択（`POCKET_KEYTEST` の `case 'r'`）だけ。Back の保存ターン（`vm/leave-and-flash` が直している所）には触れていない。

### USB からの注入（診断ビルドのみ）

CMake オプション `POCKET_KEYTEST`（既定OFF）で、USB に `US(0x1f) 'K' op hex hex LF`（op `+`/`-`、hex はキー番号）を送るとキーの押下・解放を注入できる。注入したイベントは FIFO が空のときに `keymap_poll()` が取り出すので、実キーボードと同じく `keystate` と翻訳の両方を通る（注入した Ctrl+Alt+Del はアプリを止める）。

**通常ビルドに入れなかった理由**: USB の1バイト目で分岐する表（`usb_stroke()`）は `tools/*.py` の契約で、`0x1d`（bridge）と `0x1e`（pet）が既に枠の開始に使われている。`0x1f` を通常ビルドで取ると、ホストのスクリプトが送った `0x1f` が黙って飲まれる経路が増える。ゲームのホスト試験に必要になったら、その時点で通常ビルドへ上げる判断をする。同じオプションで診断アプリ `apps/keytest`（USB `r` で起動、メニュー行は増やさない）も入る。

## 却下した案

| 案 | 却下の理由 |
| --- | --- |
| `keymap_poll()` が解放も返す／`keystroke_t` に物理キーを足す | 翻訳の消費者（シェル・IME・エディタ・Wi-Fi画面・ピッカー）全部の前提が変わる。状態は別の層に置けば翻訳を1行も変えずに済む。 |
| 辺をキュー経由でゲストへ流す（`onKey` のイベント列） | 実行中は1フレーム1打鍵、キューは16件で満杯なら黙って捨てる。解放を失えば押しっぱなしになる。§6 の `onKey` は文字の確定も含む別物で、UNSUPPORTED のまま残した。 |
| 読み手が「押された」フラグを消す方式 | 書き手と読み手が同じ語を書く。読み手が2つ以上あると取り合う。カウンタ差なら読み手は何も書かない。 |
| キーごとのイベント時刻 | 今の API に使い手が無く、56×4 B。 |
| 64bitの押下ビットマップを atomic で | Xtensa では 64bit atomic が libatomic のロックを通る。32bit語2つで足りる。 |
| mutex で守る | 読み手が書き手（入力タスク）を待たせ得る。seqlock なら読み手はどのタスク・コアからでも書き手を止めない。 |
| ハードウェアから押下状態を読み直して再同期 | TCA8418 に行列キーの現在状態のレジスタは無い（GPIO モードのピンだけ）。 |
| CFG の OVR_FLOW_M（古い方を上書き）や OVR_FLOW_IEN を変える | 実機で確かめられない今、キーボード全体の挙動を変える。あふれは検出して倒す側で扱った。 |
| 入力タスクが1周で FIFO を空になるまで読む | 排出速度は上がるが、`input_task` の既存の動作（1周1打鍵）が変わる。先に実機であふれが起きるかを測る（下）。 |
| `pocket_input_pump()` に「ホストがキーボードを持っている」引数を足す | `app_session.c` を触る（`vm/leave-and-flash` と衝突しやすい）。テキスト欄と通知はポンプ自身が問い合わせられ、ポンプが回らないピッカーだけ `main.c` から知らせれば足りた。 |

## 検証（ホスト）

| 試験 | 内容 | 結果 |
| --- | --- | --- |
| `tools/build_keystate_test.sh` | keystate 単体: 同時押し、重複、1フレーム内のタップ、あふれ、8bitの桁あふれ。1書き手・2読み手で約20万回の読み取り、ASan+UBSan と TSan の2回 | PASS、torn 0。**検査が効いている確認**: 読み手の再試行を外した版では 200,002回中 40,415回を不整合として検出 |
| `tools/build_input_test.sh` → `/tmp/test-pocket-input` | 実物の QuickJS・`keymap_poll()`・`keystate` で `input.keys` の全メソッド: 遅延構築、capability、同時押し、ターン内の一貫性、タップ、別名、不正な名前、あふれ、テキスト欄・通知・ピッカーのターン、中断、終了。既存の `onAction`/`held` の試験もそのまま | PASS。変異試験（再基準の除去、隠しの解除の除去、隠しキーの解放の扱い、BLOCKED 判定の除去）はどれも FAIL になる |
| `tools/build_keytest_app_test.sh` → `/tmp/test-keytest-app` | KEY TEST アプリを実物の QuickJS・`pocket.kasane`・`input.keys` で、起動キー・4キー同時押し・タップ・6キー・FIFOあふれ・遅れた解放の台本 | PASS（7行、max 6、押下9/解放9、`KEYTEST_SKEW` 0）。この試験が最初に `V.replace` 直後の `V.patch` が `BUSY` で断られるアプリの不具合を見つけた（直した） |
| `tools/kasane_contract/run.sh` ほか既存 | 下の「ビルドと試験の結果」 | |

## ビルドと試験の結果

| 項目 | 値 | 種別 |
| --- | --- | --- |
| `idf.py -B build_keystate build`（通常ビルド） | 成功、`cardputer_pocketjs.bin` 2,175,184 B | 実測 |
| 静的 DIRAM（`tools/memlog.py`、基準 a574ad4 を別ツリーで同条件ビルド） | 171,900 → 172,188 B、**+288 B**（`pocket_input.c.obj` +145、`keystate.c.obj` +140） | 実測（mapから） |
| `idf.py -B build_keytest -DPOCKET_KEYTEST=ON build` | 成功、2,177,552 B。`keymap_inject_usb` と `_binary_keytest_js_start` は診断ビルドの map にだけあり、通常ビルドの map には無い | 実測（map） |
| ホーム画面の空きヒープ | 未測定（実機を使っていない）。静的 +288 B がそのまま減ると見込む | 推定 |
| ゲストのヒープ | `pocket.input` を読んだアプリは getter 1個ぶん増える。`input.keys` を読んだアプリはさらにオブジェクト1個と関数4個 | 未測定 |

`keystate.c.obj` の 140 B は語 32 個（128 B）＋シーケンス語＋ portMUX。`pocket_input.c.obj` の +145 B は前回カウンタ 112 B と5つのビット集合、再基準フラグ。capability 表は 29 → 30 件（`POCKET_MAX_REGISTERED` は 32、数えたのはコードの登録呼び出し）。

既存のホスト試験: `tools/test_board_present_sync.py`（board.c を読む）PASS、`build_pocket_text_test.sh`（`keymap.h` を含む）all passed、`build_lazy_test.sh` LAZY_NAMESPACE_OK。**`tools/build_kasane_test.sh` を使う既存の試験（`build_stress_app_test.sh` など）はリンクできない**（`pocket_kasane.c` が参照する `pocket_grid_*` / `pocket_video_*` / `pocket_pixel_*` が入力に無い）。この変更が触れていないファイル間の参照で、`test_keytest_app.c` はそれらを stub にして通している。

## 実機で測る項目（未測定）

**すべて未測定。** `limits` に同時押しの上限を出していないのはこのため。道具は診断ビルドの KEY TEST（[apps/keytest/README.md](../../apps/keytest/README.md)）で、画面の `NOW`/`MAX` と、ログ `KEYTEST ...`・`KEY_OVERFLOW`・`KEYTEST_SKEW` を見る。

1. **ゴースト（計算による予測、ハードは未確認）。** 行列にキーごとのダイオードが無ければ、電気的な長方形の3隅を押すと4隅目が押されたと報告される。`board_key_event()` の写像から計算すると:
   - `A`+`S`+`D` → `F` が出る（A=電気(R1,C2)、S=(R1,C6)、D=(R2,C2)、F=(R2,C6)）
   - `A`+`D`+`E` → `T` が出る（E=(R1,C5)、T=(R2,C5)）
   - `E`+`S`+`D` と `E`+`A`+`S` は長方形にならない
   - 矢印: `;`+`,`+`/` → `enter`、`,`+`.`+`/` → `space`
   手順: KEY TEST で上の組を押し、1行目に予測のキーが出るか、`MAX` がいくつになるかを見る。出たら、ゲームは「ゴーストになる組」を避けるか、ゴーストのキーを無視する必要がある（ドキュメントに組を書く）。
2. **同時押しの上限。** 押したまま1つずつ足していき、増えなくなる数（`NOW`）と、取りこぼし（押したのに出ない）が起きる数。E/A/S/D＋アクションキー（例 `k` `l` `space`）の組で測る。
3. **FIFOあふれ。** 6キー以上を同時に押して同時に離す（12辺以上が 5ms 間隔の排出より速く積まれる）。`KEY_OVERFLOW` が出るか。出ないのに離した後も `NOW` が0に戻らない場合は、**INT_STAT の OVR_FLOW_INT が OVR_FLOW_IEN=0 では立たない**ことになる（`board.c` のコメント参照）。その場合はあふれ検出を別の方法（例: 件数10を見たら再基準）に替える。
4. **キー位置。** E/A/S/D と `;` `,` `.` `/` が、物理的に逆T字に並んでいるか（行ごとの横ずれが無ければ、E は S の真上、`;` は `.` の真上になる — 論理の列番号からの推測）。写真か目視で確認する。
5. **遅延。** 押してから `KEYTEST` 行が出るまで（入力タスク 5ms ＋ 次のフレームのポンプ、計算で最大約40ms）。必要なら。

測った値は、この節と common-api.md §6 に実測として書き、コードで強制する値ができたときだけ `limits` へ出す。
