# ペット2アプリのレビュー結果と、残っている修正

確認日: 2026-09-07。`apps/pet/pet.js`、`apps/companion/companion.js`、`main/pet_hub.c`、`main/pet_hub_core.c`、`main/pet_assets.c`、`tools/pet_companion.py` を通しで読んだ結果。

**このファイルはペット担当への引き継ぎです。** 起動しない件だけは実機で切り分けて直しました（`1c2a8eb`）。残りは触っていません。

## 直した1件

**ペットアプリが起動しなかった原因はレイアウトの崖。** `pet.js` はノードを17個作り、ルートを入れて18 taffyノードだった。17〜33の段は単一連続ブロック29,648バイトを要求し、アプリ実行中の最大連続空きは実測11,264。Rust側は確保失敗をエラーで返さずabortするので、症状は再起動になる。`PET_READY` の直後に `frames=0` で落ちていたのはこのため。

バーの溝3本（装飾）を落として15ノードにした。要求は2,048バイトになり、実機で299フレーム描画・キー操作・正常終了を確認済み。

**ゲストヒープを削っても直らない。** QuickJSが手放すのは細かいチャンクで一続きの領域ではなく、アプリ実行中の最大連続は23.5KiB程度で、29,648にはそもそも届かない。詳細は `CLAUDE.md` と `tools/uibudget/README.md`。

## 残っている修正（未着手）

### 1. ノード上限を尊重する仕組みが無い【重要】

`main/pocket_ui.c` は正しい上限を持っている（`UI_SAFE_NODES 15`、`layout_block()`）が、そのガードは `pocket.ui` 自身が作ったノードしか数えない（`live_nodes`）。レガシーの `ui.createNode` を直接叩くアプリは**一度もガードを通らない**。ファームウェアが答えを知っていて、聞かれない。

`pocket.ui` へ移せという話ではない。あれは宣言的で、ペットの絶対配置バーやスプライトを表現できない。**レガシー経路に留まる場合の最低限の義務は、同じ上限（アプリ側15ノード以下）を守ることと、越えたら落ちるテストを持つこと。**

### 2. `pet.companion` が `limits` を何も公開していない

`main/pet_hub.c:187` の初期化子に `.limits` が無い（NULL = `{}`）。一方でコードは7つの値を強制している:

| 強制している値 | 場所 |
| --- | --- |
| 通知 8件 | `pet_hub_core.c:21` |
| タイマー 4件 | `pet_hub_core.c:62` |
| ラベル 24 ASCII | `pet_hub.c:151,159` |
| タイマーID 16文字 | `pet_hub.c:151` |
| ペット番号 0..11 | `pet_hub.c:114,123` |
| アラーム ≤604800秒 | `pet_hub.c:151` |
| 吹き出し 22 ASCII | `pet_assets.c:149` |

アプリはこれらを**投げられて初めて知る**。`docs/common-api.md` §2 が気にしているのはこの向き（強制していない値を公開するのではなく、公開していない値を強制している）。値はすでにソースにあるので、`static const pocket_limit_t pet_limits[]` 1つと `.limits=` 1行で済む。

### 3. `.available=true` が決め打ち

同じ `pet_hub.c:187`。`pet_hub_init()` で `nvs_open` が失敗すると `opened` が false になり、`persist()` は常に失敗し、`pocket.pet.select()` と `wake()` は**常に**投げる（`:116`、`:139`）。それでも capability は available と言い続ける。

§2 は available を「観測値」と定めている。`pocket_capability_probe_fn`（`main/pocket_api.h:76`）がまさにこのために存在する。`opened` を返す probe を足す3行。

### 4. 変化通知を出していないので、JSが毎フレーム聞きに行く

`pet_hub_pump()` は `changed` フラグを持ち（`pet_hub.c:68,77,88,92`）、USBフレーム到着・時計同期・アラート発火を既に知っている。それを再描画（`main.c:327` の `pet_repaint`）にだけ使い、`pocket_api_capability_changed("pet.companion")` を一度も呼ばない。

結果、`pet.js:77` が `pocket.pet.rewards()` を**毎フレーム30回**、`companion.js:35` が毎秒4種類を呼ぶ。変わっていないことを確かめるためのポーリング。**1行足すだけでポーリング周期を落とせる。**

`pet_hub.c:175-185` の `usage()` は呼ばれるたびに `JS_NewObject` 1 + `JS_NewArray` 1 + 窓ごとに `JS_NewObject` の計4オブジェクトを確保する。毎秒4オブジェクトがゲストヒープに落ちる。**その大きさは測っていない。**

### 5. `PocketError` を一度も使っていない

`main/pocket_api.h:24-40` の `PocketError`（code / operation / retryable / outcome）と `pocket_api_throw()` があるのに、生の QuickJS 例外だけを投げている。

- `main/pet_hub.c:114, 116, 135, 137, 139, 149, 154, 158, 160, 164, 174`
- `main/pet_assets.c:128, 132, 146, 153, 180, 184, 193, 200`

対応は機械的。`pet_hub.c:114`（範囲外）→ `POCKET_ERR_INVALID_ARGUMENT`、`:154`（タイマー4本が埋まっている）→ `POCKET_ERR_LIMIT_EXCEEDED`、`pet_assets.c:193`（`malloc(8192)` 失敗）→ `POCKET_ERR_OUT_OF_MEMORY`。

**特に `pet_hub.c:116` と `:139` は `outcome` を持つべき典型。** どちらも保存失敗時に値を実際に巻き戻している（`hub.saved.selected=old` / `hub.saved.wake_minute=old`）のに、JSには素の `InternalError` が届く。呼び出し側が「書けなかった」のか「書けたか分からない」のかを区別できない。`POCKET_OUTCOME_NOT_APPLIED` はこのために `pocket_api.h:42-45` にある。

## PC連携（§13との差分）

入口は `main/main.c:39` の `pet_hub_usb()` 一点で、**USBのキー入力と同じバイト列への相乗り**。48バイト固定、CRC32付き、壊れたフレームはキーに漏らさない（`pet_hub.c:51-57`）。これは健全。

**出口が存在しない。** 端末からPCへ返す経路は `ESP_LOGI` のログ行だけで、`PET_ACK` はACKであってレスポンスではなく payload を運べない。JSアプリがPCへ届ける手段はゼロ。

§13 に届かせるために存在すべきもの:

1. `esp_err_t pocket_bridge_emit(const uint8_t *frame, size_t len);` — **端末→PCの出口。これが無いことが双方向にできない理由そのもの。**
2. `esp_err_t pocket_bridge_install(JSContext *ctx, void *user_data);` — `pocket.bridge.connect` を生やし、`pocket_api_register()` で `bridge.pc` を差し替える。`request()` の Promise は `pocket_api_settled()` / `pocket_api_reject()`、購読は `pocket_api.h` の土台を使う。**土台を書き直さないこと。**
3. `bool pocket_bridge_usb(uint8_t byte);` — `pet_hub_usb()` の一般化。種別+length+sessionId+requestId+CRC を持つ可変長にし、48バイト固定の現行は別種別で互換のまま残す（`tools/pet_companion.py send` を壊さないため）。

PC側は `tools/pet_companion.py` に `serve` サブコマンド（フレームを読み、`agent.*` を dispatch し、応答を書き戻す）が要る。今の `send` は書きっぱなしで、読むのは `PET_ACK` の文字列一致だけ。

なお `bridge.pc` が `main/pocket_api.c:45` で `supported=false` のままなのは**正直な状態**であって見落としではない。`pocket.bridge.connect` が実在しない以上、feature-test したアプリは `TypeError` ではなく `UNSUPPORTED` を受け取るべき。

## 問題ないと確認した点

水増ししないために明記する。

- **`pocket.storage` の使い方**（`pet.js:36,57`）— Promise契約もエラー処理も正しく、`main.c:265` の `local.pet` 名前空間と噛み合っている。「共通APIを使っていない」は事実に反する。
- **`pet_hub` が生NVSを使っていること** — ゲストより長生きするホスト側の状態で、JS向けKVに置く筋のものではない。
- **`pocket.fs` / `pocket.io` を使っていないこと** — 資産はフラッシュ埋め込みを直読み、外部I/Oは触らない。使う理由が無い。
- **動的フォントアトラスを育てていないこと** — `jsfont.c:126` の `JSFONT_WRAP` が包むのは `ui.setText` だけで、両アプリは `ui.replaceText` しか使わない。フォントスロットも設定せず既定のスロット0（固定ASCII）。`tools/test_pet.cjs:11` が `ui.setText` を投げる関数に差し替えてこれを規則として強制している。**意図的で正しい。**
- **非同期完了の再実装は無い** — 12関数すべて同期で、独自の完了経路を発明していない。
- **`pet_hub_usb` が壊れたフレームをキーに漏らさないこと**。
- **`pet_assets_reset()` の順序**（`app_session.c`）— ゲストが死ぬ前にコアポインタを捨てる規約を守っている。
- **ネイティブのDRAM占有** — `tools/memlog.py` の記録で `pet_hub.c.obj` 735バイト、`pet_assets.c.obj` 63バイト、`pet_pixels.c.obj` と `pet_hub_core.c.obj` は0。比較で `solar_sail.c.obj` 30,671、`sound.c.obj` 9,169。**ペットのネイティブが要らないメモリを握っている筋は無い。** 議論ではなく数字。

## 作業するときの注意

- 複数セッションが1つのツリーを共有している。ビルドディレクトリを分け、コミットは自分の変更だけを `git show HEAD:<file>` に当てた blob で staging する（`CLAUDE.md` 末尾）。
- ノード数を変えたら `wsl -e bash -lc "cd tools/uibudget && cargo run --release --bin sweep"` で段差を確認する。
- DRAMを増やしたらビルドが `.cache/memlog/memory.jsonl` に記録する。`python tools/memlog.py --map build_<dir>/cardputer_pocketjs.map` で差分が読める。
