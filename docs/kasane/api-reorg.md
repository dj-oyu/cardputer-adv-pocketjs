# Kasane ファームの実装整理と JS API の再設計（設計書）

2026-11-13 起草（SLOW）。基準は main `4840504`。**設計だけで、コードは変えていない。** 実装は §6 の段ごとに、難易度のラベルに従って STEADY（Opus）と FAST（Sonnet）が行う。

依頼: 「kasane fw の実装を整理する。quickjs-ng と Shiki IR の最適化の過程を踏まえて API を整理する。既存のアプリを書き換えることも射程に入れる。QEMU で性能の変化をモニターし続け、実機で実装前後の性能向上を確かめる」。

**数値の区別**（この文書の全体に適用）:

- **コード**: 該当行を読んで確かめた事実。file:line を付ける。
- **map**: `4840504` のファームの map（`.cache/base/fw/cardputer_pocketjs.map`、親セッションが 2026-11-13 に組んだもの）を、筆者がオブジェクト別・セクション別に集計した値。「バイナリに入っているか」はこれで確かめた。
- **実測（引用）**: 既存の文書の実機・QEMU の値。出典を付ける。その文書の image と workload に限る。
- **推定**: 上の値からの計算、または見込み。**§3 の「期待効果」はすべて推定で、P0 のベースラインの後に各段で測り直す。**

## 0. 結論（先に）

1. **API の表面は、使われ方に対して広すぎる。** `pocket.kasane` を最初に読むと、アプリが使わない `cache`・`grid`・`video`・`pixel` とグローバルの `gridFold` まで一度に作る（コード、§1.1）。出荷アプリが 1 つも使わない入口が 12 個ある（§1.3）。
2. **毎ターンの経路に、API の形が原因の確保が残っている。** `replace`/`patch` は呼ぶたびに JS オブジェクトを 3 つ作る。`drawMany` は件数を受けないので、DERBY は `subarray` を毎回 2 つ作る。`setRect` は配列を受ける（コード、§1.4）。
3. **ただし、API の形だけを直しても、実機の `turn_ms` に見える差（ビルド間の揺れ 15% を超える差）は出ない見込み。** STRESS で数十 µs / 6.2 ms（推定、§3.6）。見えるのは r5sim のカウント、ゲストヒープ、静的 DRAM、flash。**速さの大きな余地は「アプリの毎ターンの JS の仕事を、状態の領域と `drawList` で native へ移す」側**（v2.0 で API は実装済み、DERBY は未移行。効果は未測定、§5）。だから P0 で DERBY を r5sim に載せ、JS 側の内訳を先に見る。
4. **静的 DRAM は `pocket.kasane.grid` の表 7,200 B が、grid を使わない全セッションで常駐している**（map）。使うセッションだけが払う形にすると、ホームの空きが同じだけ増える（推定）。
5. **flash は、診断用の `pocket.shikiDebug` と試験アプリ 6 本で約 47 KB**（map の合計）。製品 image から外すかは利用者の判断（§7）。
6. **ファイルの移動・改名は最後に回す。** `tools/` の 126 ファイルが `main/` のパスを直書きしている（コード）。`pocket_kasane.c` の分割は、同じ翻訳単位のまま `.inc` に分ける（機械語を変えない。§4.2）。

## 1. 現状の棚卸し

### 1.1 入口と、作られる時

`pocket_kasane_install`（`main/pocket/pocket_kasane.c:2845`）が capability `display.kasane` を登録し、`pocket.kasane` を遅延名前空間にする。アプリが `pocket.kasane` を**最初に読んだ時**に `build_kasane`（同 `:2786`）が走り、次を一度に作る（コード）。

| 作るもの | 所 | 中身 |
| --- | --- | --- |
| クラス 10 個の登録 | `pocket_kasane.c:2789-2798` | tx・modal・ref・instance・animation・template・image・ticket・scene・schema |
| 直下の関数 10 個 | `:2730-2738` | `resource` `mount` `createScene` `replace` `patch` `poll` `cancel` `features` `stats` `inputScope` |
| `cache` | `:2812-2822` | `create` `release` |
| `grid` とグローバル `gridFold` | `:2823`、`pocket_grid.c:1045-1053`・`:1063-1072` | `register` `registration` `registerResize` `registerResizeSource` `run` `profile` `measure` `resource` `explain`、`gridFold.{fold,index,view}` |
| `video` | `:2824`、`pocket_video.c:290-299` | `open` `push` `select` `selectAudio` `resetTimeline` `streamStart` `streamPoll` `streamPause` `streamState` `streamStop` |
| `pixel` | `:2825`、`pocket_pixel.c:135` | `open` `stage` |
| `shiki`（Shiki heap のあるセッションだけ） | `:2829-2830`、`pocket_shiki_api.c:563-593`、`pocket_shiki_frame.c:1320-1327` | 下の §1.2 |

`capabilities` と `apiVersion` 以外は遅延（CLAUDE.md）だが、**`pocket.kasane` の中は遅延でない**。`mount('hello')` しか呼ばない HELLO も、`grid`・`video`・`pixel`・`cache` のオブジェクトと `gridFold` を持つ。ゲストヒープの費用は未測定（§3.6 に推定、P0 で測る）。

`gridFold` は [shiki-v2.md](shiki-v2.md) §2.2 が「消す」とした項目だが、今も `pocket_grid.c:1072` でグローバルに置かれている（コード）。同じ節が「`share()` をやめる」とした写しも、`grid` について残っている（`pocket_shiki_api.c:557-561`・`:591`）。

### 1.2 面の一覧

「使うアプリ」は `apps/` の検索（2026-11-13）。別名（`const V = pocket.kasane` など）を追って数えた。**太字は `main/pocket/app_registry.c` に行がある、メニューから起動できるアプリ。** `apps/kasane/*_probe.js` などの検査用は「検査」とまとめた。

**保持型の画面（capability `display.kasane`）**

| API | 実装 | 使うアプリ |
| --- | --- | --- |
| `mount(name)`・`mount({version:1,slots,nodes}, initial)`、`view.set/bind/unbind` | `pocket_kasane.c:2688`・`:2403`・`:2398-2400`、`ksn_schema*.c`、`app_view_assets.c`、`app_view_provider.c` | **hello・deskclock・imucal・pet・companion・player**、bridge（埋め込みなし）、検査。`unbind` はどこも呼ばない |
| `resource(name)` | `:1056` | **pet・companion** |
| `replace(fn)`・`patch(fn)`、tx の `background` `rect` `roundRect` `strokeRect` `gradient` `text` `image` `group` `instantiate` | `:1733`・`:1744`・`:2012-2020`、`ksn_core.c`・`ksn_view.c`・`ksn_render.c` | **stress・derby・bigwave・lcdcatch・megademo・meshbench・shikitest 系**、keytest。`group` は megademo と demo.js だけ |
| ref の `setRect` `setClip` `setColor` `setVisible` `setText` `setReveal` `setImageFrame` `setRotation` `animate` | `:2026-2031` | `setText`・`setRect` は上の全部。`setClip`・`setVisible`・`setColor` は derby・lcdcatch。`setReveal`・`setImageFrame` は megademo と demo.js。`animate` は demo.js だけ。**`setRotation` はどこも呼ばない** |
| `cache.create/release`、`tx.instantiate`、instance の `place` `setVisible` | `:1845`・`:1877`・`:2038-2039`、`ksn_cache.c` | **lcdcatch**、demo.js |
| `modal.open/close`、`inputScope()` | `:2023`・`:2003`、`ksn_modal.c` | demo.js だけ |
| `createScene`（`invalidate` `flush`） | `:2047`・`:2075-2080` | `create_scene.js`、vmprobe の `condition.js` だけ |
| `poll()`・`cancel(ticket)` | `:1908`・`:1925` | `poll` は demo.js と検査 1 本。**`cancel` はどこも呼ばない** |
| `features()` | `:1937-1969` | **どこも呼ばない**（`apps/` の `.features()` は `pocket.fs` の 1 件だけ） |
| `stats()` | `:1971` | **stress**（60 フレームごと）、検査 |

**Shiki（capability `kasane.shiki`、manifest が名指ししたセッションだけ）**

| API | 実装 | 使うアプリ |
| --- | --- | --- |
| `beginFrame` `commit` `resource({maxSegments,spare})` | `pocket_shiki_frame.c:1320-1326` | **stress・derby・bigwave・megademo**、shikitest 系 |
| `createSurface` | 同 `:1326` | shikitest の API 試験だけ |
| `beginMeshFrame` | 同 `:1323` | **meshbench** |
| `register` `unregister` `draw` | `pocket_shiki_api.c:566-568` | 上の 4 本（stress は `draw` を使わず `drawMany` だけ） |
| `drawMany(handles, inputs, stride)` | `:573` | **stress**（`Float64Array`）、**derby**（`Int32Array`） |
| `data` `release` | `:570-571` | 上の 4 本 |
| `fields` | `:572` | API 試験だけ |
| `sin` `cos` `sinMany` | `:574-576` | **derby** |
| `list` `setList` `drawList` `releaseList` | `:577-580` | **API 試験だけ**（どのアプリも未使用） |
| `heap()` | `:569` | stress（60 フレームごと） |
| `grid`（`pocket.kasane.grid` の写し） | `:591` | megademo（`megademo_view.js:4`） |

**その他の面**

| 名前空間 | capability | 実装 | 使うアプリ |
| --- | --- | --- | --- |
| `pocket.kasane.grid`、`gridFold` | なし（`display.kasane` の中） | `pocket_grid.c`、`ksn_proc_grid*.c` | **gridlab・megademo**、検査 |
| `pocket.kasane.video` | なし | `pocket_video*.c`、`ksn_video_frames.c` | **videolab**、検査 |
| `pocket.kasane.pixel` | なし | `pocket_pixel.c`、`ksn_pixel_function.c`・`ksn_pixel_span.c` | `pixel_lab_probe.js` だけ |
| `pocket.shapes`、`pocket.ayatsuri` | `kasane.shapes` | `pocket_shape.c:244-247`・`:672-680`、`pocket_ayatsuri_model.inc`、`main/ayatsuri/` | **megademo・meshbench** |
| `pocket.derby` | `derby.series` | `pocket_derby.c:713-725` | **derby** |
| `pocket.shikiDebug` | `kasane.shiki.debug` | `pocket_shiki.c:488-500`、`ksn_shiki_bench.c` | **shikitest・drawcost・cyclebench・meshbench** |
| `pocket.overlay` | `ui.overlay` | `pocket_overlay.c:322-325` | **player** は `onKey` だけ。**`begin`・`rect`・`text` はどこも呼ばない** |

`grid`・`video`・`pixel` は専用の capability を持たず、上限が `limits` に出ていない（[shiki.md](shiki.md) §3 の 6 が指摘した点。今も同じ。コード）。

### 1.3 出荷アプリが使わない入口

`apps/` に呼び出しが無い、または検査用の JS だけが呼ぶもの（コード、検索）: `features`、`cancel`、`poll`、`inputScope`、`createScene`、`modal.*`、`ref.setRotation`、`ref.animate`、`view.unbind`、`shiki.fields`、`shiki.createSurface`、`shiki.list`・`setList`・`drawList`・`releaseList`、`overlay.begin`・`rect`・`text`、`pixel.*`。

**事実と推測の区別:** 「使われていない」は `apps/` の検索の結果で、実機の PLAYGROUND・WORK に保存されたユーザーのプログラムは見ていない（§7 の 8）。

### 1.4 毎ターンの経路に残る確保と変換（コード）

| 所 | 何が起きるか |
| --- | --- |
| `run_build`（`pocket_kasane.c:1663`） | `replace`/`patch` のたびに ticket（`:1681`）・tx（`:1684`）・modal（`:1686`）の 3 オブジェクトを作り、tx に `modal` を自前のプロパティで足す（`:1688`）。出荷アプリは戻り値の ticket を使わない |
| `drawMany` の引数 | 件数を受けない（`handles.length` が件数）。DERBY は `BH.subarray(0, bn)`・`BI.subarray(0, bn * bs)` を flush のたびに作る（`apps/derby/derby_view.js:222`、flush の呼び出しは 4 か所） |
| `ref.setRect(tx, [x0,y0,x1,y1])` | 配列を受ける。STRESS は毎フレーム配列リテラルを 1 つ作る（`apps/stress/stress.js:178`） |
| `poll()`・`stats()`・`heap()` | 呼ぶたびにオブジェクト（`poll` は文字列 3 つも）を作る（`:1908-1923`・`:1971-2001`、`pocket_shiki_api.c:549-551`）。毎ターンは呼ばれていない |
| STRESS の `drawMany` の入力 | `Float64Array`（`stress.js:76`）。入力 1 つ約 156 サイクルの変換が残る（実測（引用）、[shiki-bundle.md](shiki-bundle.md) §6） |

### 1.5 重複・旧経路・ビルドに入らないファイル

| 項目 | 事実 | 区別 |
| --- | --- | --- |
| `ksn_shiki_surface.c`・`ksn_shiki_layers.c`・`ksn_shiki_present.c` | `main/CMakeLists.txt:89-95` の `KASANE_PROC_DEVICE_PROBE` の中だけで加わるが、その option は `:69-73` で `FATAL_ERROR`。ファームには入らない。`tools/kasane_contract/` の host 検査 5 本が使う | コード |
| `ksn_pixel_span_device_probe.c`（378 行）・`ksn_grid_scan_cost_device_probe.c`（139 行） | 同じ到達できない塊の中。`tools/` からの参照は 0 | コード |
| `main/CMakeLists.txt:118-124`・`:566-571` | D2 の probe の option（`FATAL_ERROR`）と、到達できない `-O2` の指定 | コード |
| `ksn_device_probe.c`・`ksn_stress_probe.c`・`ksn_frost.c` | `KASANE_SOURCES`（`:35`）に無条件で入る。`ksn_device_probe.c` は全体が `#ifdef CONFIG_KSN_DEVICE_PROBE`。3 つとも `4840504` の map にオブジェクトが無い（`--gc-sections` で落ちている）。`features().frosted` は常に `false`（`pocket_kasane.c:1955`） | コード・map |
| `ksn_shiki_bench.c`・`pocket_shiki.c`（`pocket.shikiDebug`） | 無条件でリンクされる。`.flash.text` 6,043 B と 5,880 B、`.flash.rodata` 46 B と 552 B。`.bss` は無い | map |
| 試験アプリの JS | `shiki_test.js` 5,431・`shiki_data_test.js` 5,147・`shiki_api_test.js` 12,738・`cycle_bench.js` 1,817・`draw_cost.js` 3,024・`mesh_bench.js` 6,681 B（`.flash.rodata`、計 34,838 B） | map |
| `pocket_grid.c` の `slots` | `.bss` 7,200 B が常駐（`pocket_grid.c:115`）。grid のコードは `pocket_grid.c` 12,057 B ＋ `ksn_proc_grid*.c` 13,048 B（`.flash.text`） | map |
| `pocket_overlay.c` の `items` | `.bss` 1,440 B（`:35`、24 項目）。`begin`・`rect`・`text` の描画経路は生きている（`main/ui/overlay.c:495` が `pocket_overlay_paint` を呼ぶ）が、呼ぶアプリが無い | コード・map |
| handle の表が 2 つ | `slots[PROC_HANDLES]`（`pocket_shiki_frame.c:109`）と `binds[BINDS]`（`pocket_shiki_api.c:50`、「one per slot」）。同じ handle の簿記を 2 か所で持つ | コード |
| `features()` | 返す真偽は `modal`（overlay かどうか）以外すべて定数。`capacity`・`cache` は `display.kasane` の `limits`（`:2833-2839`）と同じ定数の写し | コード |
| 名前の食い違い | `ksn_shiki_frame.c` は Shiki のエンジンではなく、線分のフレームの帯ラスタライザで、公開名は `ksn_proc_*`。`pocket_shiki.c` は診断（`shikiDebug`）、API は `pocket_shiki_api.c`、フレームは `pocket_shiki_frame.c`（公開名は `pocket_proc_*`）。`main/` の中の出現は `ksn_proc_` 532・`pocket_proc_` 238・`KSN_PROC_` 265 | コード（grep） |
| 診断の `#ifdef` | `main/CMakeLists.txt:68-180` に Kasane の probe の option が約 40 個。`pocket_shiki_frame.c` の中に `KASANE_PROC_JS_DIAGNOSTIC`（7 か所）・`KASANE_MEGADEMO_TRACE`（4）・`KSN_MESH_PREPARED_PROTOTYPE`（7）・`POCKET_PROC_SIG_CHECK`（8）ほか | コード |
| 文書の古い参照 | [architecture.md](architecture.md) の末尾が挙げる `ksn_presenter.c` は存在しない | コード |

**推測（未確認）:** 上の probe の option のうち、今も組めるものがどれかは確かめていない（`FATAL_ERROR` と明記された 3 つ以外）。P1 で 1 つずつ configure して確かめる。

### 1.6 大きさ（map、`4840504`）

Kasane・Shiki・関連の面（`ksn_*`、`pocket_kasane`・`pocket_shiki*`・`pocket_grid`・`pocket_shape`・`pocket_video*`・`pocket_pixel`・`pocket_derby`・`pocket_overlay`・`shiki_*`・`app_*view*`・`ayatsuri*`）の合計: `.flash.text` 316,391 B、`.flash.rodata` 70,152 B（うち Shiki の ROM の表 29,176 B、試験の JS 23,316 B を含む）、`.dram0.bss` 32,030 B、`.iram0.text` 4,277 B。

`.bss` の上位: `ksn_render.c` 11,412（`decoded` 5,572、ブレンドの LUT 2,048 × 2）、`ksn_pet.c` 8,208、**`pocket_grid.c` 7,206**、`pocket_overlay.c` 1,630、`pocket_shiki_frame.c` 1,247、`pocket_shiki_api.c` 1,000、`shiki_heap.c` 533。

`.flash.text` の上位: `pocket_kasane.c` 35,418、`ayatsuri_model.c` 33,364、`pocket_shape.c` 29,906、`pocket_derby.c` 22,769、`ksn_render.c` 20,061、`ksn_core.c` 18,248、`ksn_shiki_frame.c` 18,200、`ksn_shiki.c` 14,976（＋ IRAM 2,589）、`ksn_shiki_jit.c` 14,809。

## 2. 整理の原則

QuickJS-ng と Shiki の最適化の記録から、API の形に効くものだけを取り出す。各原則に根拠の文書と数値を付ける。

| | 原則 | 根拠 |
| --- | --- | --- |
| G1 | **毎ターンの呼び出しは、項目数に比例させない。** O(項目数) の JS→native の呼び出しを O(1) にする。O(1) の呼び出しを 3 回から 1 回にすることは追わない | 呼び出し 1 回は約 1,100〜1,400 サイクル（4.7〜5.8 µs）。入力 8 の draw は 1 回ずつ 19.5 µs、`drawMany` の `Int32Array` で 2.5 µs（実測（引用）、[shiki-bundle.md](shiki-bundle.md) §3・§6）。3 回を 1 回にしても約 10 µs（計算）で、ターン 6 ms の 0.2% |
| G2 | **毎ターンの経路で、API がゲストの確保を強いない。** 戻り値のオブジェクト、引数の配列・オブジェクトリテラル、`subarray`、コールバックのためのクロージャを要らなくする | アロケータは STRESS のターンの模型サイクルの 35.7〜37.9% で、QuickJS 本体より大きい（QEMU の命令列と模型の推定、[qjs-observe.md](../vm/qjs-observe.md) §0・§7）。確保 248 回で 26.5 万サイクル＝1 回（解放込み）約 1,070 サイクル（計算）。窓例外の 7 割は、ブロックキャッシュに外れた確保の鎖の奥（同 §15） |
| G3 | **数は整数で、まとめて渡すなら型付き配列で。** double を境界に通さない | S3 に double の演算器は無い。`__divdf3` は 1 回 392 命令（[qjs-observe.md](../vm/qjs-observe.md) §3.1）。入力の変換は 1 つ 147〜390 サイクル、Q 形式の整数なら 0（[shiki-bundle.md](shiki-bundle.md) §4・§6）。JS の配列の要素の読み取りは 1 つ約 400 サイクル、引数は約 47（同 §3・§4）。`parse_i16` の整数の早道で r5sim −2.2%（[qjs-observe.md](../vm/qjs-observe.md) §11） |
| G4 | **重い仕事は登録の時に 1 回、できればビルドの時に。** ゲストに持たせないものは flash に置き、名前で引く | 式を flash の表にして、native 9.9 → 1.2 KB（推定）、ゲスト −3.1〜5.7 KB（host 実測）、登録 1,252 → 570 µs（実機）（[flash-plan.md](flash-plan.md)）。リストは handle の検査を作る時の 1 回にする（[shiki-v2.md](shiki-v2.md) §5）。JIT は登録の時（[shiki-jit.md](shiki-jit.md)） |
| G5 | **アプリのソースを小さく、クロージャを少なく書ける API にする。** 評価のピークは定常の約 2 倍 | ソースのバイト数がヒープを食う（6.5 KB のアプリでゲスト 107 KiB）。クロージャ 1 つで評価のピーク約 0.3〜0.5 KB。1 関数のバイトコードが大きいと連続 1 ブロック（26 KB）で落ちる（CLAUDE.md、[eval-peak.md](../vm/eval-peak.md)、[shiki-v2.md](shiki-v2.md) §11.1） |
| G6 | **状態はデータ領域に、順番はリストに。** JS は窓に整数を書き、native が回す | [shiki-v2.md](shiki-v2.md) §4・§5（実装済み、アプリは未移行、効果は未測定）。`pocket.derby.ser()` を native にして評価余裕 22.7 → 30.6 KB、ゲスト最大 138.7 → 133.3 KB（実機、CLAUDE.md） |
| G7 | **使わない面を、作らない・載せない。** 名前空間の中も遅延にし、検査用・実験用の面は manifest が名指ししたセッションだけに載せる | 「capability を足すたびにゲストの部屋が減る」（CLAUDE.md）。`kasane.shapes`・`derby.series`・`kasane.shiki.debug` は既に manifest の門がある（`main/app_session.c:1913-1927`） |
| G8 | **`limits` はコードで強制している値だけ。同じ事実の入口は 1 つ。** | CLAUDE.md。`features()` は `limits` の写し（§1.5） |
| G9 | **毎ターンの native は、途中で確保しない。** 大きさは最初に宣言する | 追加領域をやめて予備を宣言に（[qjs-observe.md](../vm/qjs-observe.md) §25.6）。途中の確保は Shiki heap の空きを分ける |
| G10 | **機械語の配置を動かす変更と、動かさない変更を分ける。** 動かさない変更は「カウントが同じ」で合格にできる | 同じカーネルがビルド間で 15% 動く（CLAUDE.md）。r5sim のカウントは同じ像で同じ値（[qjs-observe.md](../vm/qjs-observe.md) §9） |

## 3. 新しい API の形

方針: **足す → アプリを移す → 消す** の 3 段。足す段では古い呼び方をそのまま残す（native の関数表の行が残るだけで、ゲストの費用は無い）。**JS の互換層（shim）は作らない**（ゲストのソースとヒープを使うので、G5 に反する）。

### 3.1 名前空間

| 今 | 後 | 理由 |
| --- | --- | --- |
| `pocket.kasane` 直下 10 個 | `mount` `resource` `replace` `patch` `stats` の 5 個を残す。`features` は消す。`poll`・`cancel`・`inputScope`・`createScene` は §7 の判断 | G8、§1.3 |
| `pocket.kasane.cache` | 残す。**最初に読んだ時に作る** | G7。lcdcatch が使う |
| `pocket.kasane.grid` とグローバル `gridFold` | capability `kasane.grid` を新設し、manifest が名指ししたセッションだけに載せる。`gridFold` は `pocket.kasane.grid.fold/index/view` へ移し、グローバルをやめる。`slots` の 7,200 B は、最初の `register` で 1 回確保する | G7、G8、[shiki-v2.md](shiki-v2.md) §2.2 |
| `pocket.kasane.shiki.grid`（写し） | やめる。`pocket.kasane.grid` を使う | 入口は 1 つ（G8） |
| `pocket.kasane.video` | capability `kasane.video`、manifest の門 | G7 |
| `pocket.kasane.pixel` | capability `kasane.pixel`、manifest の門 | G7 |
| `pocket.kasane.shiki` | 残す（§3.3） | |
| `pocket.shikiDebug`、試験アプリ 6 本 | Kconfig `CONFIG_POCKET_SHIKI_DEBUG`（既定は §7 の判断）で、面と `ksn_shiki_bench.c` と JS の埋め込みを一緒に外せるようにする | G7 |
| `pocket.overlay.begin/rect/text` | §7 の判断（消すと `.bss` 1,440 B と描画経路が消える） | §1.5 |
| `pocket.shapes`・`pocket.ayatsuri`・`pocket.derby` | 変えない | 既に manifest の門がある。ayatsuri は別の作業の範囲 |

名前空間の中の遅延は、土台に入れる: `pocket_api.c` に「遅延名前空間の中の、遅延の子」を 1 つ足す（今の `pocket_api_lazy` は `pocket.<name>` だけ。`main/pocket/pocket_api.h:157`）。「新しい面は `pocket_api.c` を編集しない」規則の例外ではなく、土台の機能の追加として 1 回だけ行う。**STEADY の仕事**（アクセサの差し替えと OOM の再試行の規則を、子でも保つ）。

### 3.2 保持型の画面（`display.kasane`）

| 変更 | 形 | 互換 |
| --- | --- | --- |
| `replace`・`patch` が確保しない | tx と modal のオブジェクトはセッションに 1 組だけ作って使い回す（呼び出しの間だけ opaque を入れる。今も終わりに `NULL` へ戻している、`:1694`）。`modal` は tx の prototype の getter にする。戻り値は `undefined` | ticket を返さなくなる（§7 の 2 で `poll`・`cancel` を残すなら、`poll()` が今の提出を指す） |
| `ref.setRect`・`setClip` が整数 4 つを受ける | `setRect(tx, x0, y0, x1, y1)`。配列の形も受け続ける | 追加だけ |
| `features()` を消し、`limits` に足す | `display.kasane.limits` に `textBytes`（896）・`animations`（6）・`cacheCommands`（48）を足す（どれも強制している値: `ksn_core.h:22`・`:7`、`ksn_cache.h:7`） | `features()` は P5 で消す |
| `stats()` | そのまま（診断。毎ターンは呼ばない、と文書に書く） | |

保持型の入口が `mount`＋`set` と `replace`/`patch`＋ref の 2 系統あることは、今回は変えない。一本化するかの調査は §7 の 6（schema は slot 24・node 24・文字 47 B、node は 5 種類で、`strokeRect`・`gradient`・`group`・cache の instance が無い。`ksn_schema.h:8-10`・`:51-53`。ゲームの HUD をそのまま載せられない）。

### 3.3 Shiki（`kasane.shiki`）

| 変更 | 形 | 互換 |
| --- | --- | --- |
| `drawMany` が件数を受ける | `drawMany(handles, inputs, stride, count?)`。`count` を渡すと先頭の `count` 個だけ描く（`inputs` は `count × stride` 個以上あればよい）。省くと今と同じ | 追加だけ。仕様 §9.4 の「長さはちょうど」の規則に、`count` のある時の規則を足す |
| handle の表を 1 つに | `slots` と `binds` を 1 つの表にまとめる（内部） | API は同じ |
| `grid` の写しをやめる | §3.1 | megademo を先に直す |
| `createSurface`・`fields`・`sin`/`cos`（1 値） | 残す（仕様にある。使うアプリが無いのは API 試験が覆う） | |
| `list`・`drawList` ほか | 残す。仕様（[shiki-spec.md](shiki-spec.md) §9.4）へ v2.0 の凍結の時に入れる項目のまま | |

**足すかどうか判断が要るもの（§7 の 7）: 定数のデータ領域を flash に置く。** STRESS は起動時に JS でパレットと点列を計算して窓へ書く（`apps/stress/stress.js:24-60`、約 3.5 KB のソースとクロージャ約 6 個）。式の表（flash-plan）と同じ考え方で、ビルドが値を計算して flash の表に置き、`register` の `options.data` に**名前**で渡せるようにすると、その JS・クロージャ・一時配列がゲストから消え、読み取り専用の領域は Shiki heap も使わない（G4・G5）。命令の追加は要らないが、「flash の領域への `PUT`・`STORE` は登録時に `INVALID_ARGUMENT`」の規則と、変換器（`emit_shiki_rom.mjs`）の拡張が要る。効果は未測定。

### 3.4 capability と `apiVersion`

- 新設: `kasane.grid`・`kasane.video`・`kasane.pixel`（`supported: true`、`available` は manifest が名指ししたセッションで true。`limits` は各面が強制している値だけを P3 で洗い出して載せる）。
- `kasane.shiki.debug`: `CONFIG_POCKET_SHIKI_DEBUG=n` の image では登録しない（`supported` を問えば false）。
- `kasane.shiki.limits`: 変えない。
- `apiVersion`: 今は `"0.1.0"`（`main/pocket/pocket_api.h:21`）。**足す段（P3）では上げない。消す段（P5）で `"0.2.0"` に上げる。** feature-test は capability で行う規則（common-api）なので、アプリは版の文字列を見ない。

### 3.5 互換

- P3〜P4 の間、古い呼び方（`gridFold`、`shiki.grid`、`features()`、ticket の戻り値、`drawMany` の 3 引数、配列の `setRect`）はすべて動く。
- P5 で消すのは、**その時点で `apps/` に呼び出しが無いもの**だけ。消した名前を呼ぶと `TypeError`（存在しない関数）になる。旧 `ui.*` の `APP_LEGACY_UI`（`main/app_session.c:1736`）のような、起動前に断る検査は足さない（対象が検査用の入口で、保存済みのプログラムが使っている見込みが低いため。§7 の 8 で確認）。

### 3.6 期待効果（すべて推定。P0 の後に測る）

| 変更 | ゲストヒープ | ネイティブ DRAM | flash | ターン時間 |
| --- | --- | --- | --- | --- |
| `pocket.kasane` の中を遅延に、`grid`・`video`・`pixel`・`gridFold` を manifest の門に | Kasane を使う全セッションで −1〜1.5 KB（オブジェクト 5 個・プロパティ 26 個・名前の atom の見積もり。未測定） | 0 | +0.2 KB 程度（土台の子の遅延） | 0 |
| grid の `slots` を使う時に確保 | 0 | **静的 −7,200 B**（map の大きさ）。grid を使わないセッションの空き +7.2 KB、使うセッションは同じ | ±0 | 0 |
| `replace`/`patch` が確保しない | 定常は 0（ごみが減る） | 0 | ±0 | 1 回の `patch` で確保 4 回前後が消える＝約 4,300 サイクル（18 µs）。STRESS の 6.2 ms の 0.3% |
| `drawMany` の `count` | 0 | 0 | ±0 | flush 1 回で `subarray` 2 個（確保 4 回前後）＝約 18 µs。DERBY で 1 ターン 3〜6 回なら 54〜108 µs（7.2 ms の 0.7〜1.5%）。flush の回数は P0 で数える |
| `setRect` の整数 4 つ | 0 | 0 | ±0 | 1 回約 3,500 サイクル（15 µs）: 配列の確保 2 回と要素の読み取り 4 つ |
| STRESS の入力を `Int32Array` に | 0 | 0 | 0 | 入力 90 個 × 約 156 サイクル＝約 1.4 万サイクル（58 µs、0.9%）。式の入力の Q 形式の宣言を見直す必要があり、画素が変わりうる |
| `features`・`poll`・`cancel`・`createScene`・`inputScope` を消す | 0 | 0 | −2〜3 KB（関数の大きさの見積もり。未測定） | 0 |
| `shikiDebug` と試験アプリを製品 image から外す | 0 | 0 | **−47.3 KB**（map の合計） | 0 |
| `overlay.begin/rect/text` を消す | 0 | 静的 −1,440 B | −1〜2 KB（未測定） | 0 |
| 定数のデータ領域を flash に（STRESS） | −数 KB（ソース約 3.5 KB と評価のピーク。未測定） | Shiki heap −約 1 KB（定数の領域の合計の見積もり） | +約 1 KB | 0 |
| DERBY の走者を状態の領域と `drawList` へ（§5） | 未測定（`BH`・`BI` の配列が消え、順番の表が増える） | Shiki heap +数百 B | 0 | **未測定。この作業で一番大きい可能性があるが、JS 側の内訳を P0 で見てから見積もる** |

**読み方:** 上の表の時間の行は、1 つずつでは実機のビルド間の揺れ（15%）よりはるかに小さい。合格は r5sim のカウント（同じ像で同じ値、[qjs-observe.md](../vm/qjs-observe.md) §9）と確保の回数で判定し、実機は「悪くなっていない」ことと、メモリ（ゲスト最大・評価余裕・`idle_free`）の差を確かめる（§6）。

## 4. ファーム内部の整理

### 4.1 責務の境界（変えない）

| 層 | ファイル | 責務 |
| --- | --- | --- |
| core | `ksn_core.c`・`ksn_view.c`・`ksn_runtime.c`・`ksn_cache.c`・`ksn_modal.c` | layer・bank・ticket・damage・repair。JS を知らない |
| 描画 | `ksn_render.c`・`ksn_blend_pie.c`・`text/ksn_font.c`、`ksn_shiki_frame.c`（線分の帯ラスタライザ） | 帯の合成。PIE はここに閉じる |
| schema・source | `ksn_schema*.c`・`ksn_source*.c` | descriptor の展開、slot、source |
| Shiki のエンジン | `ksn_shiki.c`・`ksn_shiki_jit.c` | 式の検査・実行・JIT。JS を知らない |
| JS の束ね | `pocket_kasane.c`、`pocket_shiki_api.c`・`pocket_shiki_frame.c`、`pocket_grid.c`・`pocket_video.c`・`pocket_pixel.c` | 引数の検査、handle、ゲストの寿命 |
| アプリ別 | `pocket_derby.c`・`pocket_shape.c`・`app_music_view.c` ほか | core に入れないアプリの意味 |

[architecture.md](architecture.md) の境界のとおりで、今回はここを動かさない。直すのは、境界の内側の散らかり（§1.5）。

### 4.2 `pocket_kasane.c`（3,209 行）の分割 — 同じ翻訳単位のまま `.inc` に

`pocket_kasane.c` は `-O2`（`main/CMakeLists.txt:551-552`）で、毎ターンの経路（`mutate`・`parse_i16`・`parse_rect_mode`）が IRAM の一覧に**オブジェクト名つき**で載っている（`main/vm/helper_iram8.lf:100-103`）。別の `.c` に分けると、(a) `static` の関数の間のインライン展開が変わる、(b) `.lf` の行がオブジェクト名で外れる、(c) host の検査の組み立て（`tools/` の 10 ファイルが `pocket_kasane.c` を名指しし、そこから組む検査はさらに多い）に全部ファイルを足す必要がある。命令キャッシュの配置が動けば、15% の揺れの中で前後を比べられなくなる（G10）。

そこで、**`pocket_kasane.c` が `#include` する `.inc` に分ける**（先例: `pocket_shape.c:171` の `pocket_ayatsuri_model.inc`）。翻訳単位は 1 つのままなので、機械語は変わらない見込み（合格基準で確かめる）。

| ファイル | 中身（今の行のおよその範囲） |
| --- | --- |
| `pocket_kasane.c` | 状態・クラス・`ensure_state`・`build_kasane`・install・reset・capability（1〜270、2740〜2850、2905〜2980） |
| `pocket_kasane_parse.inc` | 引数の検査と読み取り（270〜970） |
| `pocket_kasane_tx.inc` | tx の命令、ref、animation、instance、modal（972〜1630） |
| `pocket_kasane_scene.inc` | `run_build`、scene、cache、`poll`・`cancel`・`features`・`stats`（1630〜2046） |
| `pocket_kasane_schema.inc` | `mount`、schema、source capability（2047〜2730、2851〜2904） |
| `pocket_kasane_present.inc` | present・ターンの終わり・invalidate（2980〜3209） |

`mount`・schema の部分（登録の時だけ走る約 700 行）を別の `-Os` の翻訳単位に出すと flash が数 KB 減る見込み（推定、未測定）だが、配置が動くので今回は**やらない**。やるなら単独の段として、同じバイナリの A/B で測る。

### 4.3 probe・bench を本番 image から外す

| 対象 | 方法 |
| --- | --- |
| 到達できない CMake の塊（`:68-73`・`:89-95`・`:118-124`・`:566-571`）と、そこでしか使わない 2 ファイル | 消す。`ksn_shiki_surface.c`・`_layers.c`・`_present.c` は host の検査が使うので、**場所を動かさず**残し、先頭のコメントに「ファームには入らない、host の検査用」と書く |
| `ksn_device_probe.c`・`ksn_stress_probe.c`・`ksn_frost.c` | `KASANE_SOURCES` から外し、`if(CONFIG_KSN_DEVICE_PROBE)` の中で足す。今も map に無いので、バイナリは変わらない見込み |
| `pocket.shikiDebug`（`pocket_shiki.c`・`ksn_shiki_bench.c`）と試験アプリ 6 本 | `main/Kconfig.projbuild` に `CONFIG_POCKET_SHIKI_DEBUG` を足し、ソース・`EMBED_TXTFILES`・`app_registry.c` の行・`shell.c` のメニューの行を一緒に切る。**メニューの行が変わるので、`tools/test_settings.py`・`tools/capture_home.py`（押下回数を数える）を同じ変更で直す**（CLAUDE.md）。既定を `y` のままにすれば、この 2 つは動かない |
| Kasane の probe の option 約 40 個 | P1 で 1 つずつ configure し、組めないものは option ごと消す。組めるものは残す（消すかどうかは持ち主の判断。§7 の 1） |
| `pocket_shiki_frame.c` の中の診断の `#ifdef` | 触らない（host の検査が使うものが混ざる。`POCKET_PROC_SIG_CHECK` など）。P2 で一覧を先頭のコメントにまとめるだけ |

オプションを重ねるビルドは `-DSDKCONFIG=<dir>/sdkconfig` で分ける（CLAUDE.md。ルートの `sdkconfig` に値が残る事故の再発を防ぐ）。

### 4.4 ファイルの移動と改名

**今回の P1〜P5 では、既存のファイルを動かさず、改名もしない。** `tools/` の 126 ファイルが `main/` のパスを直書きしていて（`grep -rln "main/" tools`）、動かすと host の検査が黙って壊れる（CLAUDE.md の `e770950` の事故）。新しいファイル（`.inc`）を足すことと、ビルドに入らないファイルを消すことだけを行う。消す時も、先に `grep -rn "main/" tools/` で参照元を洗う。

名前の食い違い（§1.5）は、P6（任意、§7 の 5）でまとめて直す。直すなら次の 2 つのファイルだけにし、シンボルの一括の改名（`ksn_proc_*` 532 か所ほか）は行わない案を推す:

| 今 | 後 | 理由 |
| --- | --- | --- |
| `pocket_shiki.c`・`.h` | `pocket_shiki_debug.c`・`.h` | 中身は `shikiDebug` |
| `ksn_shiki_frame.c`・`.h` | `ksn_seg_frame.c`・`.h` | Shiki のエンジンではなく、線分のフレームの帯ラスタライザ |

シンボルは、各ヘッダの先頭に「`proc` は Shiki の旧名（procedural）。[shiki.md](shiki.md) §1.3」と 1 行書くに留める。

## 5. アプリの書き換え計画

順番は「検査 → 計測の負荷 → 小さいもの → 一番厳しいもの」。各アプリで、書き換えの前後に host の全画素の一致（そのアプリの host の検査がある場合）、ゲストヒープ、評価のピークを比べる。**JS のコメントは増やさない**（ヒープ。理由は隣の `README.md` へ）。

| 順 | アプリ | 変えること | ラベル |
| --- | --- | --- | --- |
| 1 | shikitest（`shiki_api_test.js`） | 新しい形の検査を足す: `drawMany` の `count`、整数 4 つの `setRect`、`pocket.kasane.grid.fold`、`replace`/`patch` の戻り値、`features` が無いこと（P5 の後）。古い形の検査は P5 まで残す | Sonnet |
| 2 | stress | `setRect` を整数 4 つに。`P.grid` は使っていないので変更なし。**r5sim の負荷なので、書き換える前の `stress.js` を固定の写しとして残す**（§6 P0）。`Float64Array` → `Int32Array` は別の小さな段（画素が変わりうるので、host の digest を取り直す） | Sonnet（`Int32Array` は Opus） |
| 3 | bigwave、lcdcatch、keytest | `setRect` の形、`patch` の戻り値に依存していないことの確認 | Sonnet |
| 4 | megademo、gridlab | `gridFold` → `pocket.kasane.grid.fold`、`H.grid` → `V.grid`、manifest に `kasane.grid` | Sonnet |
| 5 | videolab、pixel の検査 | manifest に `kasane.video`・`kasane.pixel` | Sonnet |
| 6 | derby（その 1） | `bflush` を `drawMany(BH, BI, bs, bn)` に（`subarray` をやめる）。`setRect`・`setClip` を整数に | Sonnet（差分は小さいが、評価余裕と host の 19 本の検査を通す） |
| 7 | derby（その 2、§7 の 10） | 走者（`legs`・`run` の 16 draw）を、状態の領域と `drawList` へ（[shiki-v2.md](shiki-v2.md) §4.3・§5.1 の例のとおり）。式 `run`・`legs`・`frl`・`frb` を `DRAW_INDEX` と `LOAD` で書き直す | **Opus**（評価余裕 約 28.9 KB・ゲスト最大 134.1〜134.8 KB・Shiki heap の最も厳しい窓の空き 2,492 B の中で行う。式の書き直しは画素の一致が要る） |
| 8 | hello・deskclock・imucal・pet・companion・player | **変えない**（`mount`＋`set` は今の形のまま）。遅延化でゲストヒープが減ることだけを測る | — |

heapprobe の DERBY（評価の余裕の二分探索）は、6・7 の後に今の DERBY から作り直す（[shiki-v2.md](shiki-v2.md) §2.1 の表のとおり）。

## 6. 段階的な実施計画

共通の規則:

- 各段は worktree（`.claude/worktrees/<名前>`、ブランチ `vm/<題目>`）で行い、`git merge --no-ff` で入れる（CLAUDE.md）。
- **各段の前後で §6.1 の指標を取り、[`docs/kasane/logs/api-reorg-perf.md`](logs/api-reorg-perf.md)（P0 で作った。ベースラインと読み方）に 1 行ずつ足す。** コミットの sha、何を測ったか（実測か、QEMU のカウントか）を必ず書く。
- 実機が要る検証を host だけで済ませない。実機を使い終えたら通常の image に戻す。
- FAST（Sonnet）の段でも、合格基準の数字の解釈は STEADY が行う（CLAUDE.md のルーティング）。

### 6.1 指標

| 指標 | 道具 | 何を見るか |
| --- | --- | --- |
| Q1 命令/ターン（カウント） | `tools/r5sim/fast.sh <dir> -DR5SIM_DEVICE=1 -DR5SIM_APP=<負荷>`（約 23 秒） | `R5SIM ccount per_turn=`。1 カウント ≒ 6.54 命令。同じ像で同じ値なので、小さな差も読める。**ベースライン: Shiki 版 STRESS で 154,595**（`4840504`、親セッションの 2026-11-13 の値、`.cache/reorg/perf.log`。筆者は再測定していない） |
| Q2 確保/ターン、ブロックキャッシュの当たり | r5sim の `R5SIM block_cache hits= misses=`（`r5sim.c:499`）と、P0 で足す 1 ターンあたりの確保の回数 | G2 の直接の指標 |
| Q3 模型サイクル、部品別（アロケータ比・quickjs.c・窓例外・libgcc） | `tools/r5sim/observe.py`（全命令のトレース、1 回約 30 分・10 GB） | **段の区切りだけ**で取る（P0、P3 の後、P4 の後） |
| Q4 像の大きさ | `fast.sh` の末尾（r5sim の像）と、ファームの `idf.py -B <dir> size`、`tools/memlog.py --map` | flash code・IRAM・DIRAM。ベースライン: ファームの DIRAM used 181,332・flash code 1,722,132（同じ出典） |
| Q5 ゲストヒープ（host） | `tools/build_stress_app_test.sh`、`tools/games/run_derby.py --m32`、`tools/vmtest/compile_peak.py` | 評価後の `malloc_size`、評価のピーク、最大の 1 ブロック |
| D1 実機のターン | `KASANE_PAINT turn_ms/render_ms/send_ms`（30 フレームの平均、`main/app_session.c:3082`） | DERBY は待機画面・CAM WIDE・CAM FIELD・CAM VISION を、Shiki の steps が同じ窓で比べる（種が起動ごとに違う。[qjs-observe.md](../vm/qjs-observe.md) §23.2 の方法）。STRESS は LV1 の 24 窓の平均 |
| D2 実機の Shiki | `SHIKI_TURN draws= steps= engine_ms= total_ms= jit= shk=`（同 `:3151`） | `engine_ms`、Shiki heap の使用量と最大の空き塊 |
| D3 実機のメモリ | `MEM free= largest= js=`（同 `:1222`）、`tools/memlog.py --port --check`、heapprobe の二分探索 | 読み込み後の空きとゲスト、`idle_free`、DERBY の評価余裕とゲスト最大 |

**実機の差の読み方:** ビルド間で 15% は動く（CLAUDE.md）。15% 未満の速さの差を主張する段は、同じバイナリの中で切り替える A/B（Kconfig の実行時の切り替え、または同じ像での新旧の JS）で測る。それ以外の段の実機の合格は「悪くなっていない（15% を超える悪化が無い）」と、メモリの差（メモリは配置で揺れないので、そのまま比べられる）。

**r5sim の限界**（[qjs-observe.md](../vm/qjs-observe.md) §5・§12・§23.1）: QEMU に PIE が無く、Shiki のエンジンは C のモデルで走る（描画の命令数は実機のものではない。JS・アロケータ・libgcc・窓例外は実機と同じコード）。flash の待ちは見えない。heap の配置が実機と違う（アリーナが実機では取れなかった例がある）ので、メモリの主張は実機で確かめる。

### P0 — 計測基盤とベースライン（Opus: r5sim の拡張の設計と DERBY の負荷／Sonnet: スクリプトと記録）

コードの振る舞いは変えない。

1. **r5sim に負荷を 2 つ足す。**
   - `-DR5SIM_APP=derby`（[qjs-observe.md](../vm/qjs-observe.md) §16 の宿題）: DERBY のデモのレースを固定の種で回す。部品は host の `tools/games/test_derby_host.c` と同じ（チャンクの読み込み、`pocket.derby`、入力の台本、時間の進め方）。Shiki の表は `derby_prog.js` から `emit_shiki_rom.mjs` で作る（STRESS の `-DR5SIM_APP=shiki` と同じ道）。**場面ごとに区切って出す**: `R5SIM ccount scene=<wait|wide|field|vision|headon> per_turn=`。場面の境目は JS のログ（`DERBY ...`）から取る。評価中の `pocket.memory.info().internalFreeBytes` は実機と同じく `null` を返す（CLAUDE.md）。
   - `-DR5SIM_APP=hud`: 保持型だけの負荷。LCD CATCH を固定の入力の台本で回す（`tools/games/test_lcd_catch_host.c` の部品）。`replace`/`patch`・ref・cache の経路（`pocket_kasane.c`）を、Shiki の描画に埋もれさせずに見る。
2. **STRESS の固定の写し。** `tools/r5sim/main/` に、`4840504` の `apps/stress/stress.js` の写しを `stress_shiki_v1.js` として置き、`-DR5SIM_APP=shiki_v1` で回せるようにする。アプリを書き換えた後も、「古い呼び方のままの負荷」でファームの退行を見るため（P5 で古い呼び方を消す時に、この負荷も終わる）。
3. **確保の回数を速い計測で出す。** `-DR5SIM_DEVICE=1` でも 1 ターンあたりの確保・解放の回数を数えて `R5SIM alloc per_turn=` で出す（今は `-DR5SIM_GUEST_ALLOC=1` の版だけが大きさ別に数える）。
4. **名前空間を作る費用の host の検査。** `pocket.kasane` を読む前後の `malloc_size` の差を出す小さな検査を `tools/` に足す（§3.6 の 1 行目の見積もりを実測に置き換える）。
5. **1 回で回すスクリプト** `tools/r5sim/reorg.sh`: 負荷 4 つ（`shiki`・`shiki_v1`・`derby`・`hud`）の `fast.sh` と、ファームの `size` を回し、`docs/kasane/logs/api-reorg-perf.md` の形の 1 行を出す。
6. **ベースラインを取る。** Q1〜Q5 を `4840504` で。Q3（トレース）は `shiki` と `derby` の 2 本。**DERBY のトレースで、JS 側の内訳（quickjs.c・アロケータ・`pocket_derby.c`・libgcc）と、1 ターンの確保と native の呼び出しの回数を出す**（§5 の 7 の見積もりの材料）。
7. **実機のベースライン**（D1〜D3）: DERBY（待機画面とデモのレース 1 本、90 秒）、STRESS LV1（60 秒）、BIG WAVE（60 秒）、ホームの `idle_free`、DERBY の評価余裕。起動を変えて 2 回ずつ。

受け入れ基準: (a) 4 つの負荷が r5sim で `frames=120 oom=0 fail=0 bad_present=0` 相当で終わり、2 回走らせて同じカウント。(b) `shiki` のカウントが 154,595 と一致する（計測の道具を足しただけで、負荷が変わっていないこと）。(c) ベースラインの表が sha つきで記録されている。(d) 実機の値が [qjs-observe.md](../vm/qjs-observe.md) §25.5・§25.6 の値（DERBY 待機画面 3.91〜3.94 ms、STRESS 6.2 ms 前後）と 15% 以内で合う。合わなければ、先に理由を調べる。

### P1 — ビルドに入らないものを消し、診断を門の中へ（Sonnet、STEADY が合格を判定）

**実施済み（§7.3 の決定により `CONFIG_POCKET_SHIKI_DEBUG` の既定は `n`。下の「既定 `y` で足すだけ」「メニューは変わらない」は、メニューが元々変わらない（試験アプリはホームの行ではなく `native_apps`／`app_registry` の行）ことを除いて置き換わった）。** 実測（map、`.flash.text`／`.flash.rodata`／`.dram0.bss`／`.iram0.text`）: `CONFIG_POCKET_SHIKI_DEBUG=y` の image は P0 の `4840504` とバイト単位で同一（1,722,132／822,644／76,640／81,771）。`=n` の image は `.flash.text` −27,324 B、`.flash.rodata` −39,200 B、`.dram0.bss` −16 B、`.iram0.text` ±0（合計 −66,540 B。§3.6 の見積もり −47.3 KB より大きいのは、診断だけが呼んでいた関数も `--gc-sections` で落ちたため）。

範囲: §4.3 の表の上 4 行。`CONFIG_POCKET_SHIKI_DEBUG` は**既定 `y` で足すだけ**（メニューは変わらない）。[architecture.md](architecture.md) の `ksn_presenter.c` の参照を直す。

受け入れ基準: (a) ファームの `.flash.text`・`.flash.rodata`・`.dram0.bss`・`.iram0.text` の大きさが P0 と**同一**（Q4。消したのはリンクされていなかったものだけ、という主張の確認）。(b) r5sim の 4 負荷のカウントが同一。(c) `tools/kasane_contract/run.sh` と host の検査一式が通る。(d) `CONFIG_POCKET_SHIKI_DEBUG=n` で組めて、flash の減りが §3.6 の 47.3 KB と合う（別のビルドディレクトリと `-DSDKCONFIG=`）。(e) `python tools/check_docs.py`。実機: (d) の image は焼かない（既定は変えていない）。

### P2 — `pocket_kasane.c` を `.inc` に分ける（Sonnet、STEADY がレビュー）

範囲: §4.2。関数の順番を変えない（順番が変わると配置が動く）。

受け入れ基準: (a) `pocket_kasane.c.obj` の各セクションの大きさが同一、ファームの大きさが同一。(b) r5sim の 4 負荷のカウントが同一。(c) `helper_iram8.lf` の `pocket_kasane:` の 3 行が今も当たっている（map で `mutate`・`parse_i16`・`parse_rect_mode` が `.iram0.text` にある）。(d) host の検査一式。実機: 起動と HELLO・STRESS の動作だけ（数値は取らなくてよい。大きさとカウントが同一なら機械語は同じ）。

### P3 — API を足す（非破壊）（Opus）

範囲: §3.1 の遅延と manifest の門（土台の「子の遅延」を含む）、grid の `slots` を使う時に確保、§3.2 の `replace`/`patch` の確保の除去と整数の `setRect`・`setClip`、`limits` の追加、§3.3 の `drawMany` の `count` と handle の表の統合、`pocket.kasane.grid.fold/index/view`（`gridFold` は残す）。仕様の文書（[shiki-spec.md](shiki-spec.md) §9、common-api）を同じ変更で直す。

Opus とする理由: 毎ターンの経路の確保（`run_build`）、draw の `lookup`（1 draw 約 107〜138 サイクル、実測（引用））、静的 DRAM、ゲストの寿命（使い回す tx のオブジェクトと `app_stop()` の reset の順）に触る。

受け入れ基準:

- host: 契約試験一式、API の自己試験（新しい形の検査を足したもの）、`replace` が OOM で途中で失敗した後の再構築（STRESS の LV3 の経路）、`app_stop()` の後に tx のオブジェクトが残らないこと（ASan）。
- Q1・Q2: `hud` で 1 ターンの確保が 3 回以上減る。`shiki_v1`（古い呼び方）のカウントが増えない（+0.5% 以内）。`derby` は変わらないか減る。
- Q4: 静的 DRAM が約 7.2 KB 減る（`memlog.py --map`）。flash の増加は 1 KB 以内。
- Q5: HELLO・DERBY・STRESS の評価後のゲストヒープが増えない。P0 の検査（名前空間を作る費用）で、`pocket.kasane` の最初の読み取りの費用が減る。
- 実機: ホームの `idle_free` が約 7 KB 増える（D3）。DERBY・STRESS の `turn_ms` が 15% を超えて悪化しない（D1）。DERBY の評価余裕とゲスト最大が悪化しない（D3）。GRID LAB・MEGADEMO・VIDEO LAB が起動して動く（grid の確保が使う時に通ること。**物理の表示の確認を依頼する**）。

### P4 — アプリを移す（§5 の 1〜6: Sonnet／STRESS の `Int32Array`: Opus）

範囲: §5 の表の 1〜6。1 アプリ 1 コミット。

受け入れ基準: 各アプリで、(a) host の画素の一致（DERBY は 19 本の検査、STRESS は digest、BIG WAVE・LCD CATCH・MEGADEMO は各 host の検査）、(b) Q5 のゲストヒープと評価のピークが増えない、(c) r5sim: `shiki`（書き換えた STRESS）のカウントが `shiki_v1` より小さい、`derby` のカウントと確保の回数が P3 より小さい、(d) 実機: そのアプリの `turn_ms` が悪化しない、DERBY は評価余裕 28.9 KB・ゲスト最大 134.8 KB を悪化させない。段の終わりに Q3（トレース）を `shiki` と `derby` で取り、アロケータ比を P0 と比べる。

### P4b — DERBY の走者を状態の領域と `drawList` へ（Opus。§7 の 10 で行うと決めた場合）

**実施（2026-11-13、[derby-runners-native.md](../apps/derby-runners-native.md)）: 状態の領域と `drawList` ではなく、`pocket.derby.runners()`（C）1 回に下ろした（同文書 §4 で比較）。r5sim の `derby` のレースの場面 −8.8〜−18.3%、確保 78 → 66 回/ターン、host の全フレームの画素一致。トレースの代わりに、走者の経路を落とした 2 つの変種の r5sim の差で経路の重さを測った（同 §1）。実機の A/B は未実施（同 §7）。DERBY は `drawMany` を使わなくなったので、§5 の 6 の `bflush` の項は不要になった。**

範囲: §5 の 7。先に P0 の DERBY のトレースで、走者の経路（`rin`・`bput`・`bflush`・`pocket.derby.lanes`）が JS 側の何 % かを出し、**見込みが小さければ行わない**（見積もりを文書に残して終える）。

受け入れ基準: (a) host の 19 本の検査で画素が一致、(b) r5sim の `derby` のカウントが場面ごとに P4 より小さい、(c) 実機: 同じバイナリの中で新旧の JS を切り替えた A/B で `turn_ms` を比べる（15% 未満の差を主張するため）。`SHIKI_TURN` の `shk=` の最大の空き塊が、最も厳しい窓（HEAD ON・FINISH）で 2,492 B を下回らない。評価余裕とゲスト最大が悪化しない。レースを FINISH・写真判定まで走り、予備不足・`FRAMEFAIL`・OOM が 0。

### P5 — 古い呼び方を消す（Sonnet、STEADY がレビュー）

範囲: §7 で消すと決まったもの（`features`、`poll`・`cancel`・ticket、`createScene`、`inputScope`、`gridFold` のグローバル、`shiki.grid` の写し、`overlay.begin/rect/text`）。`apiVersion` を `"0.2.0"` に。`stress_shiki_v1.js` の負荷を r5sim から外す。検査用の JS（`apps/kasane/*_probe.js`、`demo.js`、`create_scene.js`）のうち、消した入口を使うものを直すか消す。仕様と common-api を直す。

受け入れ基準: (a) `apps/` と `main/` に、消した名前の呼び出しが無い（grep）。(b) Q4: flash の減りを測って記録する。`overlay` の項目を消した場合は静的 DRAM −1,440 B。(c) r5sim の `shiki`・`derby`・`hud` のカウントが増えない。(d) host の検査一式、`tools/test_settings.py`・`capture_home.py`（メニューの行を変えた場合）。(e) 実機: smoke（`smoke_device.py`）、player の overlay（`onKey` が動く）、HELLO、DERBY、STRESS。

### P6 — ファイルの改名（任意。Sonnet、機械的）

範囲: §4.4 の 2 ファイル。先に `grep -rn "main/" tools/` で参照元の一覧を作り、同じコミットで全部直す。

受け入れ基準: (a) ファームの大きさが同一、r5sim のカウントが同一。(b) host の検査一式と `tools/pie/` の 3 層が**実際に走って**通る（「走らせていない検査は通っているように見える」、CLAUDE.md）。(c) `python tools/check_docs.py`。

### 段の依存

P0 → P1 → P2 → P3 → P4 →（P4b）→ P5 →（P6）。P1 と P2 は順不同。P4b は P4 の後ならいつでもよい。P5 は P4 の全アプリの後。

## 7. リスクと、決めること

### 7.1 リスク

| | リスク | 備え |
| --- | --- | --- |
| R1 | API の整理の速さの効果は小さく（§3.6）、実機では揺れに埋もれる。「速くなった」と言えない段が続く | 合格を r5sim のカウント・確保の回数・メモリで判定する。速さを主張する段（P4b）だけ、同じバイナリの A/B |
| R2 | r5sim は PIE と JIT が無く、heap の配置も違う | Shiki のエンジンの速さとメモリの主張は実機で。r5sim は JS 側の向きを見る |
| R3 | `.inc` への分割で機械語が変わる（関数の順番、`static` の可視性） | 順番を変えない。大きさとカウントの同一を合格にする。違えば戻す |
| R4 | tx のオブジェクトを使い回すと、アプリが tx を `patch` の外へ持ち出した時の扱いが変わる | 今も呼び出しの終わりに opaque を `NULL` にしている（`:1694`）。持ち出した tx は次の `patch` の間だけ再び有効になるので、「コールバックの外で使った tx は `CLOSED`」を世代の番号で保つ。P3 の検査に入れる |
| R5 | manifest の門で、PLAYGROUND・WORK のプログラムが `grid`・`video`・`pixel` を使えなくなる | §7.2 の 4 |
| R6 | DERBY は評価余裕 約 28.9 KB、ゲスト最大まで約 5.2 KB、Shiki heap の最も厳しい窓の空き 2,492 B（CLAUDE.md、[qjs-observe.md](../vm/qjs-observe.md) §25.6） | DERBY は最後に移す。1 変更ごとに実機で heapprobe |
| R7 | メニューの行が変わると `test_settings.py`・`capture_home.py` が黙って別の行を押す | 行を変える変更（`CONFIG_POCKET_SHIKI_DEBUG=n` を既定にする場合）は、同じ変更で 2 つを直す |
| R8 | host の台本が、実機で `null` になる値を固定値で返す | r5sim の DERBY の負荷も `null` を返す（P0） |
| R9 | 複数セッションが作業ツリーを共有する | worktree と、blob を直接 staging するコミットの手順（CLAUDE.md） |
| R10 | 実機の操作が broker 経由に限られる（DERBY・app_session・capture） | §7.2 の 9 |

### 7.2 利用者に決めてもらうこと

| | 決めること | 推す案 |
| --- | --- | --- |
| 1 | 診断（`pocket.shikiDebug`、試験アプリ 6 本、probe の option）を製品 image から外すか。`CONFIG_POCKET_SHIKI_DEBUG` の既定 | 既定は `y` のまま（実機の照合に使う）。flash が要る時に `n` にできる状態にする。組めない probe の option は消す |
| 2 | `poll()`・`cancel()`・ticket の戻り値を消すか | 消す。出荷アプリは使っていない。提出の成否が要るアプリが出たら、整数を返す `status()` を 1 つ足す |
| 3 | `createScene`・`inputScope`・`modal` を消すか | `createScene` は消す（[architecture.md](architecture.md) が escape hatch と呼び、検査の JS しか使わない）。`modal`・`inputScope` は残す（製品の機能として検証済みで、使う予定があるなら）。**使う予定の有無を教えてほしい** |
| 4 | `grid`・`video`・`pixel` を manifest の門に入れるか。PLAYGROUND・WORK のプログラムにどの capability を与えるか | 門に入れる。PLAYGROUND・WORK には今と同じ面を与える（`kasane.grid` などを名指しする）かどうかは、エディタで grid を書く使い方があるかで決める |
| 5 | ファイルの改名（P6）を行うか。シンボルの一括の改名は行わない、でよいか | 2 ファイルだけ改名。シンボルはそのまま |
| 6 | 保持型の 2 系統（`mount`＋`set` と `replace`/`patch`＋ref）を、将来 `mount` に一本化する調査をするか | 今回は調査だけを別の文書に（schema の上限と node の種類の拡張が要る。ゲームの HUD の node と ref の数を P0 で数える） |
| 7 | 定数のデータ領域を flash に置く機能（§3.3）を足すか | P4 の後に、STRESS で効果を見積もってから決める。Shiki の仕様の変更なので、v2.0 の凍結と一緒に扱う |
| 8 | 保存済みのユーザーのプログラムの互換。P5 で消す名前を使うプログラムが実機にあるか | 無ければ、起動前に断る検査は足さない。あれば `APP_LEGACY_UI` と同じ形の検査を 1 つ足す |
| 9 | 実機の計測の経路。broker で STRESS・BIG WAVE の 60 秒のログと `memlog.py --port` 相当が取れるか。取れなければ、直接のシリアルを使ってよいか | P0 の前に確認したい。取れない指標は、段の区切り（P0・P3・P4・P5）だけ直接のシリアルで取る |
| 10 | DERBY の走者を状態の領域と `drawList` へ移す（P4b）を、この作業に含めるか | P0 の DERBY のトレースの結果を見てから決める。含めない場合も、P4 までで API の整理は完結する |
| 11 | `pocket.overlay.begin/rect/text` を消すか | 消す（呼ぶアプリが無く、`.bss` 1,440 B が全セッションで常駐）。overlay のアプリを Kasane で描く方針（player の先頭のコメント）と合う |

### 7.3 利用者の決定（2026-11-13）

- **未使用の入口（§7.2 の 2・3・11）:** `modal` 以外は P5 で消す。`poll`・`cancel`・ticket の戻り値、`createScene`、`inputScope`、`overlay.begin/rect/text`、`gridFold` のグローバル、`shiki.grid` の写しが対象。`modal` は残す。
- **診断（§7.2 の 1）:** 製品 image から**既定で外す**（`CONFIG_POCKET_SHIKI_DEBUG` の既定は `n`）。メニューの行が変わるので、`tools/test_settings.py` と `tools/capture_home.py` を同じ変更で直す。
- **DERBY の P4b（§7.2 の 10）:** この作業に**含める**。P0 のトレースは見込みの記録として残すが、実施の可否の門にはしない。
- **保持型の一本化（§7.2 の 6）:** **調査だけ**行う（別の文書。実装はしない）。
- 9（実機の経路）は broker の `derby`・`app_session` で取れる範囲を使い、取れない指標は段の区切りで別に相談する。

## 8. 読んだもの

- Kasane: [README.md](README.md)、[architecture.md](architecture.md)、[decisions.md](decisions.md)、[shiki.md](shiki.md) §2・§3、[shiki-spec.md](shiki-spec.md) §9、[shiki-v2.md](shiki-v2.md)、[shiki-bundle.md](shiki-bundle.md)、[flash-plan.md](flash-plan.md)・[plan-sized-alloc.md](plan-sized-alloc.md)・[surface-segment-cap.md](surface-segment-cap.md)・[image-rows.md](image-rows.md)（索引の要約と該当の節）。
- VM: [qjs-observe.md](../vm/qjs-observe.md)（§0〜§2、§9〜§12、§15〜§20、§23、§25）、[turn-cpi.md](../vm/turn-cpi.md)・[gc-cap-backoff.md](../vm/gc-cap-backoff.md)・[math-call-fast.md](../vm/math-call-fast.md)・[memory-pressure-api.md](../vm/memory-pressure-api.md)・[function-allocation.md](../vm/function-allocation.md)（冒頭）。[eval-peak.md](../vm/eval-peak.md)・[guest-memory-design.md](../vm/guest-memory-design.md) は CLAUDE.md の要約に拠り、本文は読んでいない。
- 実装: `main/pocket/pocket_kasane.c`・`pocket_shiki_api.c`・`pocket_shiki_frame.c`・`pocket_shiki.c`・`pocket_grid.c`・`pocket_video.c`・`pocket_pixel.c`・`pocket_shape.c`・`pocket_derby.c`・`pocket_overlay.c` の入口と表、`main/CMakeLists.txt`、`main/app_session.c` の install の順、`main/vm/helper_iram8.lf`、`apps/` の全 JS の API の呼び出し。**読んでいない所:** `ksn_core.c`・`ksn_render.c`・`ksn_shiki.c`・`ksn_shiki_jit.c` の本体、`pocket_kasane.c` の引数の検査（270〜970 行）の詳細、`main/ayatsuri/`。§4.2 の行の範囲はおよその値で、分割の時に関数の境目で決める。
