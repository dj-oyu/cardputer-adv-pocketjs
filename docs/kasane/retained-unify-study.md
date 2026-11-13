# 保持型の 2 系統（`mount`＋`set` と `replace`/`patch`＋ref）を一本化するかの調査

2026-11-13 起草（SLOW）。基準は main `4840504`。**調査だけで、コードは変えていない。** [api-reorg.md](api-reorg.md) §7.2 の 6 と §7.3 の決定（「調査だけ行う」）を受けた文書。

**数値の区別**（[api-reorg.md](api-reorg.md) と同じ）:

- **コード**: 該当行を読んで確かめた事実。file:line を付ける。
- **map**: `4840504` の map（`.cache/base/fw/cardputer_pocketjs.map`）の関数別・オブジェクト別の集計。
- **host の sizeof**: `gcc -m32 -ffreestanding` で構造体の大きさを出した値。i386 は `uint64_t` を 4 バイトに整列するので、Xtensa（8 バイト）では数 B 大きくなりうる。
- **実測（引用）**: 既存の文書の値。出典を付ける。
- **推定**: 上からの計算、または見込み。**§4 の影響の表はすべて推定で、実機でも host でも測っていない。**

HUD の数（§3）は、アプリのソースを読んで数えた。**実行して `stats()` で確かめてはいない。** 条件で枝分かれする所は、多い側を取った。

## 0. 結論（先に）

1. **推奨: JS の面は一本化しない。** 2 系統は役割が違い、ゲームの HUD は `mount` の schema に載らない。載せるための拡張は、ネイティブの確保とコードを増やし、ゲームの全部を書き換えさせ、速くはならない見込み（§4 の案 A）。
2. **内部の表現は、すでに 1 つ。** `mount` の schema は、`replace`/`patch` と同じ `ksn_view_begin`・`ksn_view_add`・`ksn_view_change`・`ksn_view_submit` を呼ぶ層で、コマンドの置き場・差分・描画は共通（コード、§1.3）。「共通の内部表現だけ統一する」案（案 C）で新しく統一できるものは、アダプタの簿記の小さな重複だけ。
3. **2 系統は、1 つのセッションで同時には使えない**（コード、§1.4）。重複して払っているのは flash のコードと `kasane_state` の中の ref の表 1,024 B で、ターンの時間ではない。
4. **今の整理（api-reorg の P3）で十分。** `replace`/`patch` の毎回の 3 オブジェクトの確保と、`setRect` の配列をやめれば、`replace`/`patch` 側に API の形が原因の確保は残らない。一本化で消える費用は、それより小さい（§5.2）。
5. **残す課題は 2 つ**（どちらも一本化とは別の、小さな仕事。§5.3）: 役割の線引きを API の文書に書くこと、`mount` の `set` の名前の照合を atom にすること（[roadmap.md](roadmap.md) に既出）。

## 1. 2 系統の実装

### 1.1 `mount`＋`set`（宣言型）

入口は `js_presenter_mount`（`main/pocket/pocket_kasane.c:2688`）。引数で 3 通りに分かれる（コード）。

| 形 | 経路 | 定義の置き場 |
| --- | --- | --- |
| `mount('hello')` など登録済みの名前 | `pocket_app_view_lookup`（`main/pocket/app_view_assets.c:281-286`）→ `js_schema_mount`（`pocket_kasane.c:2403`） | flash の `ksn_schema`（`app_view_assets.c`、`.flash.rodata` 8,144 B。map） |
| `mount({version:1, slots, nodes}, initial)` | `runtime_compile`（`:2514`）が 1 回の `calloc` で native の descriptor にする → 同じ `js_schema_mount` | ネイティブ heap。1 node 192 B（`ksn_schema_node` 144 B ＋ 文字のリテラル 48 B）、1 slot 52 B（`ksn_schema_slot` 20 B ＋ 名前 32 B）（`:2525-2529`、host の sizeof） |
| `mount('music')` | `pocket_app_view_provider_lookup`（`main/pocket/app_view_provider.c:8`）→ provider の `mount`。`app_music_view.c` が `ksn_view` を直接呼ぶ（light 3 矩形の PATCH は `:195-216`） | native presenter（schema を通らない、3 つ目の経路） |

schema の語彙と上限（`main/ui/kasane/ksn_schema.h`、コード）:

- slot 24、node 24、文字 47 B（`:8-10`）。slot の型は TEXT・RECT・COLOR・BOOL・U16・RESOURCE・U32（`:15-18`）。
- node は 5 種類: `rect` `roundRect` `text` `image` `plateText`（`:51-54`）。**`strokeRect`・`gradient`・cache の instance は無い。**
- node の属性は bounds・color・text・visible・page・resource・variant・frame・reveal と、U16 の slot を辺に足す `rectAdd`（`:63-71`）。**opacity は 255 固定、clip は viewport 固定**（`ksn_schema.c:250` の `.opacity=255,.clip=viewport`）。`group` も無い。
- slot → node の依存は、slot ごとに 32 ビットのマスク 1 語（`:81-84`）。dirty な slot も 32 ビット（`ksn_schema_session.h:19`）。node が 32 を超える拡張は、この 2 つの型を広げる。

1 回の `view.set({...})`（`js_schema_set`、`pocket_kasane.c:2099-2232`）がすること（コード）:

1. 候補の値の配列を写す（slot 数 × 8 B、`:2111`）。
2. `JS_GetOwnPropertyNames` でキーを列挙（`:2114`）。キーごとに `JS_AtomToCString` と、slot の名前との `strcmp`（最大 24 回、`:2117-2123`）、`JS_GetProperty`、型ごとの検査。文字は `JS_ToCStringLen` で借りる（`:2141`）。
3. 触った slot だけ比べ、変わっていなければ何もしない（`:2181-2195`）。
4. `ksn_schema_preflight_view` で**全 node を解決して**上限を検査（`:2200`、`ksn_schema.c:337-356`）。
5. 変わった slot だけ schema の持つ領域へ写す（`:2205-2218`）。
6. `schema_refresh`（`:601`）→ `ksn_schema_session_step_dirty`（`ksn_schema_session.c:134`）→ `ksn_schema_update_dirty`（`ksn_schema.c:542`）。dirty な node だけを解決し、表示中の ref の値と比べ（`ksn_view_read_ref`）、違う所だけ `ksn_view_change` で PATCH する。**node を 2 回解決する**（比較の周と書き込みの周、`:562-580`・`:586-602`）。

`mount` の性質（コード）:

- **提出が終わっていない間の `set` は、失敗せずに貯まる。** dirty の印が合体し、次のターンで最新の値が出る（`ksn_schema_session.c:72-76`・`:119-122`、`pocket_kasane.c:2229` が `KSN_BUSY` を成功として返す）。
- **見える・見えないの切替、`page` の切替、空の文字は topology の変更で、REPLACE に戻る**（`ksn_schema.c:219-228`・`:565-575`）。REPLACE は世代が変わり、差分は全帯になる（`ksn_core.c:1401-1404`）。
- native の source を slot に結べる（`view.bind`、`:2241`。時計・再生位置）。JS を通さずに値が変わる。

セッションごとの native の確保（`schema_alloc`、`pocket_kasane.c:385-455`）: `ksn_schema_session` 572 B（host の sizeof。両世代の ref 48 個 × 2 と依存表）＋ slot 数 × 8 B ＋ TEXT slot の容量の合計。PET（slot 21、文字の容量の合計 279 B）で約 1.1 KB（推定）。

### 1.2 `replace`/`patch`＋ref（命令型）

入口は `js_replace`・`js_patch`（`pocket_kasane.c:1733`・`:1744`）→ `run_build`（`:1663`）。

1. `apply_outcome`（`:313`）で前の提出の結果を取り込み、候補の ref を有効にする。
2. `ksn_view_begin`（`:1675`）。ticket・tx・modal の 3 オブジェクトを作る（`:1681-1688`。[api-reorg.md](api-reorg.md) の P3 でやめる）。
3. アプリの関数を同期で呼ぶ（`:1690`）。中の `tx.rect` などは `js_tx_primitive`（`:1004`）→ `ksn_view_add` → `expose_ref`（`:991`）で ref のラッパーを返す。`ref.setRect` などは `change_ref`（`:1368`）→ `ksn_view_change` を**直接**呼ぶ（比較も、値の写しも無い）。
4. `ksn_view_submit`（`:1709`）。

ref の簿記: `kasane_state.refs[64]`（`ref_slot` 16 B × 64 ＝ 1,024 B、`:69-74`・`:115`）。1 回の更新で持てる ref は 32（`KASANE_REF_LIMIT`、`:36`・`:286`）。64 は「表示中の世代」と「組み立て中の世代」の 2 つぶん。**ラッパーを捨てた ref は、finalizer が表から外す**（`:248-251`）ので、32 に数えられるのはアプリが変数に持っている ref だけ。

`replace`/`patch` の性質（コード）:

- **提出が終わっていない間の `replace`/`patch` は `BUSY` を投げる**（`ksn_view_begin` が断る）。KEY TEST はこれを避けるために初回を 1 回の `replace` にまとめ（`apps/keytest/keytest.js:28-29`）、LCD CATCH は `try`/`catch` で次のフレームに書き直す（`apps/lcdcatch/lcd_catch.js:160-176`）。
- node は `replace` でしか足せない。`setVisible` は PATCH で、topology を変えない（`ksn_core.c:478`）。
- 語彙は core の全部: 6 種類の描画（`ksn_types.h:14`）、opacity、clip、`group`、cache の instance、modal、animation。文字は 1 つ 128 B まで（`ksn_core.c:318`）。

### 1.3 共通の部分（すでに 1 つ）

| 層 | ファイル | 2 系統の関係 |
| --- | --- | --- |
| コマンドの置き場・世代・差分 | `ksn_core.c`（`.flash.text` 18,716 B、map） | 共通。schema も `ksn_view_add`/`ksn_view_change` でここへ書く（`ksn_schema.c:196`・`:432-463`） |
| 端点・提出・結果 | `ksn_view.c`（2,912 B、map） | 共通 |
| 描画 | `ksn_render.c` | 共通 |
| schema の展開と差分 | `ksn_schema.c` 5,468 B・`ksn_schema_session.c` 1,104 B（map） | `mount` だけ |
| JS のアダプタ | `pocket_kasane.c` | `mount` 側 12,455 B、`replace`/`patch` 側 12,002 B（cache・modal・animation の約 3 KB を含む）、共通と他の面 10,144 B（map、関数名で分けた集計） |

つまり、**schema は「値の表から命令を作り、表示中の値と比べて PATCH を出す」層を、命令型の上に 1 枚かぶせたもの**。一本化の議論は、この 1 枚をゲームにも通すか、という問いになる。

### 1.4 同時には使えない

`run_build` は、`mount` 済みなら `BUSY`（`pocket_kasane.c:1669`）。`js_schema_mount` と provider の `mount` は、`replace`/`patch` を一度でも出していれば `BUSY`（`:2406-2408`・`:2717-2718`）。**1 つのセッションは、どちらか片方だけを使う**（コード）。

## 2. アプリの使い方

### 2.1 `mount` を使うアプリ

| アプリ | 所 | slot / node | 使い方 |
| --- | --- | --- | --- |
| hello | `apps/hello/main.js:2`・`:4`・`:7` | 1 / 5（`app_view_assets.c:10-26`） | キーごとに `set({counter})` |
| imucal | `apps/imucal/imucal.js:3`・`:4`・`:6` | 5 / 7（`:49`） | キーを変数で作って 1 slot ずつ `set` |
| pet | `apps/pet/pet.js:7`・`:9`・`:13` | **21 / 17**（`:104-158`） | 毎回 10 数個の slot をまとめて `set`。画像・`rectAdd` の棒・吹き出しの `visible`・`reveal`・背景色の slot |
| companion | `apps/companion/companion.js:2`・`:22` | 9 / 9（`:101`） | まとめて `set` |
| deskclock | `apps/deskclock/deskclock.js:2` | 2 / 4（`:160-172`） | **`set` を呼ばない。** native の時計の source が slot に結ばれている（`:173-178`） |
| player | `apps/player/player.js:4`・`:11`・`:28`・`:38`・`:95`・`:109`・`:114` | provider | `set({title,message})` と `bind('playback')`・`bind('output')` |
| bridge（埋め込みなし） | `apps/bridge/bridge.js:2-4` | 4 / 7（`:71`） | 1 slot ずつ `set` |
| 検査 | `apps/kasane/*_probe.js`（`text23_probe.js:18`、`dirty24_probe.js:26` ほか） | 最大 24 / 24 | ランタイムの descriptor |

共通点: **配置は決まっていて、変わるのは値だけ。更新はキーや通知の時で、毎フレームではない。** PET は slot 21・node 17 で、今の上限（24・24）の近くにいる。

### 2.2 `replace`/`patch` を使うアプリ

| アプリ | 組み立て | 毎ターンの更新 |
| --- | --- | --- |
| stress | `apps/stress/stress.js:82-93`（`scene`）、`:182`、120 フレームごとに `replace`（`:194`） | `patch(hud)`: `setRect` 1・`setText` 3（`:176-179`） |
| derby | `apps/derby/derby_scene.js:24-51`（`build`。場面で中身が変わる） | `patch(up)`（`apps/derby/derby_play.js:116-133`）: `setVisible`・`setColor`・`setRect`・`setText`。文字は変わった時だけ（`derby_scene.js:80`） |
| bigwave | `apps/bigwave/big_wave.js:326-335`（`ui`）、`:388` | 4 フレームに 1 回まで、変わった文字だけ `patch`（`:338-353`） |
| lcdcatch | `apps/lcdcatch/lcd_catch.js:45-75`（cache の template は `:40-42`） | 変わった所がある時だけ `patch`（`:150-176`）: `setColor`・`place`・`setRect` |
| megademo | `apps/kasane/megademo_set.js:87-162`（`build`）、`megademo_loop.js:60` | `megademo_loop.js:65` → `megademo_set.js:164-270`: `setRect`・`setVisible`・`setText`・`setReveal`・`setImageFrame`・`setColor`・`setClip` |
| keytest | `apps/keytest/keytest.js:11-18`・`:29` | キーの変化の時だけ `patch`（`:45`）、`setText` 5 |
| meshbench・cyclebench・gridlab・videolab | `apps/meshbench/mesh_bench.js:13`、`apps/shikitest/cycle_bench.js:10`・`:20`、`apps/kasane/grid_lab.js:196`、`apps/kasane/video_lab.js:14`・`:28` | 画像 1 枚と文字 2〜4 |

共通点: **Shiki・grid・video の面を画像として 1 枚置き、その上に HUD を重ねる。** 場面で topology が変わり（DERBY・MEGADEMO）、点滅や移動を PATCH で行う。

## 3. ゲームの HUD の数

上限: アプリのコマンド 80（`ksn_core.h:19`）、ref 32（`pocket_kasane.c:36`）、文字の合計 896 B（`ksn_core.h:22`）、cache の instance 8・template 8・コマンド 48（`ksn_cache.h:5-7`）。schema は slot 24・node 24・文字 47 B・node 5 種類。

「コマンド」は core が持つ命令の数（cache の instance は中の矩形の数だけ使う）。「ref」はアプリが変数に持ち続ける数。

| アプリ・場面 | コマンド | ref | schema に無い語彙 | 24 node に入るか |
| --- | --- | --- | --- | --- |
| stress | 9（画像 1・文字 5・`roundRect`・`strokeRect`・`gradient`） | 4 | `strokeRect`、`gradient`、opacity（190・200）、文字の容量 48 | 数は入る。語彙が足りない |
| derby・パドック | **32**（画像 1・矩形 14・文字 17） | 19 | 文字の容量 60・48、`setVisible` の PATCH | **入らない** |
| derby・ゲート／レース／写真 | 11（画像 1・文字 8・矩形 2） | 11 | `setClip`（動かす ref に全画面の clip）、`setVisible` の PATCH（点滅） | 数は入る。語彙が足りない |
| derby・結果 | 8 | 7 | 文字の容量 48 | 入る（容量を除く） |
| bigwave | 5（画像 1・文字 4） | 4 | なし（文字の容量 38 まで、`display` の字） | **入る** |
| lcdcatch | **78**（図形 3・文字 31・矩形 21・instance 7 個で 23） | **31**（＋ instance 7） | cache の instance と `place` の opacity、clip | **入らない**（instance を除いて 55 node） |
| megademo（部屋あり、場面 4 が最大） | 約 44（部屋 24・画像 2・タグ・台詞・題・HUD 4・instance 3 個で 9・幕 2） | 約 20 | `gradient`（dither）、`strokeRect`、opacity、`group`、instance、`setClip` | **入らない** |
| keytest | 6（文字 6） | 5 | 文字の容量 128 | 数は入る。容量が足りない |
| meshbench／cyclebench／videolab／gridlab | 1／5／3／5 | 0〜2 | なし | 入る |

読み取れること:

- **上限の近くにいるのは LCD CATCH（コマンド 78/80、ref 31/32）だけ。** CLAUDE.md の「LCD CATCH と DERBY WATCH は、ここで設計を変えた」と合う。DERBY は今の版で最大 32/80・19/32。
- **schema の 24 node に、数も語彙もそのまま入るのは BIG WAVE と小さな試験アプリだけ。** STRESS・DERBY・MEGADEMO・LCD CATCH・KEY TEST は、node の数、文字の容量、`strokeRect`・`gradient`・opacity・clip・`group`・instance のどれかが足りない。
- DERBY と MEGADEMO は、**場面ごとに topology が違う**。`mount` は 1 セッションに 1 回で（`pocket_kasane.c:2406`）、付け直す入口が無い。載せるなら、場面の数だけ node を並べて `page` で切り替える（node が場面の合計になる。DERBY で 32＋11＋8＝51）か、付け直しの API を足す。
- DERBY の点滅（`R.dm`、16 フレームごと。`derby_play.js:118`）は、schema では topology の変更になり、**切り替わるたびに REPLACE と全帯の差分**になる（§1.1）。ゲームは Shiki の面が毎フレーム全画面を汚すので、帯の数は変わらない見込みだが、REPLACE のたびに全 node を解決し直す（推定。未測定）。

## 4. 一本化の案

### 案 A — schema を広げて `replace`/`patch` を吸収する（JS の面を `mount` に一本化）

要る拡張（コードから導いた一覧）:

| 項目 | 今 | 要る値 | 触る所 |
| --- | --- | --- | --- |
| node の上限 | 24 | 80（LCD CATCH 55＋instance、DERBY を `page` で持つなら 51） | 依存表のマスク（slot ごと 1 語 → 3 語）、`ksn_schema_ref_map`（32 ビット 2 語 → 3 語ずつ）、session の ref 48 × 2 → 160 × 2 |
| slot の上限 | 24 | 約 48（LCD CATCH: 色 26・位置と色 10・opacity 7 で 43） | dirty の 32 ビットを 64 ビットに。`js_schema_set` のスタックの配列 3 本（`:2104-2109`） |
| node の種類 | 5 | ＋ `strokeRect`・`gradient`・instance | `next_draw_until`、`same_topology`、`patch_one`、`runtime_compile` |
| 属性 | — | ＋ opacity、clip、`group`、文字の容量 128 | 同上。`KSN_SCHEMA_TEXT_MAX` は slot の `capacity` が `uint8_t`（`ksn_schema.h:22`）なので型は足りる |
| 見える・見えない | topology の変更（REPLACE） | 命令を保ったまま `KSN_SET_VISIBLE` の PATCH | 「隠れた node は命令も文字の容量も使わない」（`ksn_schema.h:86-88`）という今の契約を変える。PET・COMPANION の画素と上限の検査をやり直す |
| 場面の切替 | `mount` は 1 回 | 付け直し、または `page` | `js_schema_mount`、`app_stop()` の reset の順 |

影響（**すべて推定**）:

| 軸 | 見込み |
| --- | --- |
| ネイティブ DRAM | **増える。** ランタイムの descriptor は 1 node 192 B・1 slot 52 B なので、LCD CATCH（55 node・43 slot）で約 12.8 KB、DERBY のパドック（32 node・約 20 slot）で約 7.2 KB。session は 572 B → 約 2.2 KB（ref 1,280 B、依存表 48 slot × 3 語 ＝ 576 B ほか）。動的な文字は schema の値と core の両方に 1 部ずつ持つ。今の `replace`/`patch` は、descriptor も値の表も持たない（命令は core に直接入る）。`kasane_state` の ref の表 1,024 B は消せる |
| ゲストヒープ | ref のラッパー（1 個 数十 B × 4〜31 個、0.2〜1.5 KB）と、`build` のクロージャが消える。代わりに、descriptor のオブジェクトリテラルが `mount` の間だけゲストに載る（node 1 個にオブジェクト 1 つ・配列 1〜2 つ・プロパティ 5〜8 個。55 node で数 KB〜10 KB の一時のピーク）。**評価のピークに重なると、DERBY の評価余裕（約 28.9 KB、CLAUDE.md）を削る。** slot の名前の文字列がソースと atom に増える |
| flash | schema の拡張で ＋3〜5 KB、`replace`/`patch` のアダプタで −6〜9 KB（12,002 B のうち cache・modal・animation を残すぶんを除く。map の集計からの見積もり）。差し引き −1〜−6 KB |
| ターン時間 | **速くならない見込み。** `set` は呼び出し 1 回だが、キーの列挙・名前の文字列化・`strcmp`・全 node の preflight・node の 2 回の解決が入る。`patch` は項目ごとに呼び出し 1 回（約 1,100〜1,400 サイクル。実測（引用）、[shiki-bundle.md](shiki-bundle.md) §3）で `ksn_view_change` へ直行する。HUD の更新は 1 ターン 1〜5 項目なので、どちらも数十 µs の桁。preflight は node の数に比例するので、80 node では今の PET（17 node）の数倍になる |
| 得るもの | 提出中の更新が `BUSY` にならず貯まる性質（§1.1）がゲームにも来る。ref の 32 個の上限が無くなる。入口が 1 つになる |

移行のコスト: ゲーム 6 本（stress・derby・bigwave・lcdcatch・megademo・keytest）と試験・検査の JS 15 本前後の書き換え、host の契約試験と heapprobe の作り直し、DERBY の評価余裕と画素の一致の取り直し。互換: `replace`/`patch` を消すので、保存済みのユーザーのプログラムを起動前に断る検査が要る。

リスク: (1) PET・COMPANION など、動いている `mount` のアプリの契約（隠れた node の扱い）を変える。(2) DERBY はゲスト最大まで約 5.2 KB、評価余裕 約 28.9 KB（CLAUDE.md）の中で HUD を書き直す。(3) schema の session が大きくなり、`mount` だけを使う小さなアプリ（HELLO・時計の overlay）も同じ大きさを払う（上限を可変にすれば避けられるが、確保が増える）。

**変種 A′ — ゲームの HUD を flash の asset にする。** descriptor を `app_view_assets.c` のように C に置けば、ネイティブ DRAM は増えず（flash ＋144 B/node）、アプリのソースから組み立ての関数が消える（DERBY の `build` と文字の表は約 2.3 KB のソース、`derby_scene.js:19-51`）。**DERBY の評価余裕には効く可能性がある（未測定）。** ただし、アプリの画面の定義が JS と C に分かれ、PLAYGROUND・WORK のユーザーのプログラムには使えず、上の表の拡張（node 32 以上、文字の容量、PATCH の `setVisible`、clip）は同じだけ要る。一本化ではなく「DERBY の評価のヒープを減らす個別の手」で、要るなら [api-reorg.md](api-reorg.md) の P4b の後に、評価余裕の実測を見て別に検討する。

### 案 B — JS の面は残し、`replace`/`patch` を内部で descriptor に下ろす

`replace` の関数の中の `tx.*` を記録して descriptor を作り、`ref.set*` を slot への書き込みに変え、以後は schema の session が動かす案。

| 軸 | 見込み（推定） |
| --- | --- |
| ネイティブ DRAM | 増える。`replace` のたびに descriptor を作る（192 B/node。LCD CATCH で約 10 KB）。文字は 2 部持つ |
| ゲストヒープ | 変わらない（ref のラッパーは残る） |
| flash | 記録の層で ＋1〜2 KB。消えるのは `expose_ref`・`claim_ref`・`apply_outcome` など 1 KB 前後 |
| ターン時間 | **遅くなる。** 今は `ksn_view_change` へ直行する `ref.setRect` が、「slot に書く → dirty を立てる → node を解決 → 表示中の値と比べる → PATCH」になる |
| 互換 | 面は変わらないが、提出中の `BUSY` の挙動が変わる |

向きが逆（§1.3: schema は命令型の**上**の層）で、費用だけが増える。**採らない。**

### 案 C — 内部の表現だけ統一し、JS の面は残す

§1.3 のとおり、表現はすでに共通。残る重複は簿記の 3 か所（コード）:

1. ref の世代の管理が 3 つ: JS 用の `ref_slot[64]`（`pocket_kasane.c:115`）、schema の session の `active_refs`/`candidate_refs`（`ksn_schema_session.h:11-12`）、music の presenter の `refs`（`main/pocket/app_music_view.c:34`）。
2. 提出の結果の取り込みが 2 つ: `apply_outcome`（`:313`）と `ksn_schema_session_settle`（`ksn_schema_session.c:30`）。橋は `schema_note_submitted`（`pocket_kasane.c:456`）。
3. 引数の解析: 矩形は `parse_rect_mode` を共有済み。文字は `text_string` と `JS_ToCStringLen` の 2 通り。

| 軸 | 見込み（推定） |
| --- | --- |
| ネイティブ DRAM | 0（1 と 2 は持ち主が違い、同時には使わないが、型が違うので表は共有しにくい。共有しても、`kasane_state` の 1,024 B か session の 384 B のどちらかが消えるだけ） |
| ゲストヒープ | 0 |
| flash | −0.3〜0.8 KB |
| ターン時間 | 0 |
| 移行・互換 | アプリの変更なし |
| リスク | 低い。ただし「候補を PRESENTED の後で昇格する」規則（`ksn_view.h:38-41`）を 2 か所で同じに保つ検査が要る |

得るものが小さい。[api-reorg.md](api-reorg.md) の P2（`.inc` への分割）で、`mount` 側と `replace`/`patch` 側の境目をファイルで見えるようにすれば、読む側の利益は同じだけ得られる。**独立の段にはしない。**

## 5. 推奨

### 5.1 一本化しない

2 系統を、役割の違う 2 つの入口として残す。

| | `mount`＋`set` | `replace`/`patch`＋ref |
| --- | --- | --- |
| 向く画面 | 配置が決まっていて、値だけが変わる。更新はキーや通知の時 | 場面で配置が変わる。多くの node、点滅や移動、Shiki の面の上の HUD |
| アプリが持つもの | 値（slot の名前） | ref と、組み立ての関数 |
| 上限 | slot 24・node 24・文字 47 B | コマンド 80・ref 32・文字 128 B |
| 提出中の更新 | 貯まる | `BUSY` |
| native の source | 結べる | 結べない |

理由:

1. **ゲームの HUD が載らない**（§3）。載せる拡張は schema の契約を変え、動いている `mount` のアプリを巻き込む。
2. **メモリが増える向き**（§4 の案 A）。この機体で一番きついのはネイティブ heap とゲストの評価のピークで、宣言の表を RAM に持つ形は、どちらにも逆に働く。
3. **速くならない**（§4）。HUD の更新は 1 ターン数項目で、[api-reorg.md](api-reorg.md) §0 の 3 のとおり、API の形の差は実機の揺れ（15%）に埋もれる。
4. **重複の費用が小さい。** 2 系統は同時に動かない（§1.4）。重なっているのは flash の約 12 KB ずつのアダプタ（map）と ref の表 1,024 B。

### 5.2 今の整理で十分な理由

[api-reorg.md](api-reorg.md) の P3 が `replace`/`patch` 側に行うこと: tx と modal を使い回す（1 回の `patch` で確保 4 回前後、約 18 µs が消える。推定、同 §3.6）、`setRect`・`setClip` に整数 4 つ（1 回 約 15 µs。推定）、ticket の戻り値をやめる。**これで、`replace`/`patch` の毎ターンの経路に、API の形が強いる確保は残らない**（文字の `setText` は、文字列を作るアプリの側の仕事）。一本化で追加に消えるのは ref のラッパー（`replace` の時に 1 回作るだけで、毎ターンではない）と flash の数 KB で、P3 の後では取り分が小さい。

`mount` 側は、今のアプリでは毎フレームの経路ではない（§2.1）。

### 5.3 残す小さな課題（一本化とは別。実施は任意）

| | 仕事 | ラベル | 見込み |
| --- | --- | --- | --- |
| S1 | 役割の線引き（§5.1 の表）を `docs/api/common-api.md` と [architecture.md](architecture.md) に書く。新しいアプリが入口を選べるようにする | Sonnet（STEADY がレビュー） | 文書だけ |
| S2 | `view.set` の名前の照合を atom に（`mount` の時に slot の名前の atom を持つ）。[roadmap.md](roadmap.md) の「JS `view.set`」の行に既出 | Opus（atom の寿命を `app_stop()` の reset と合わせる） | 1 キーあたり `JS_AtomToCString` の確保 1 回と `strcmp` 最大 24 回が消える（推定。未測定）。`mount` のアプリは毎フレーム `set` しないので、体感の差は出ない見込み |
| S3 | `ref` の上限 32 を `limits` と文書で「持ち続ける ref の数」と明記する（捨てた ref は数えない。§1.2） | Sonnet | 文書だけ。LCD CATCH が 31 にいる |

### 5.4 それでも実施する場合の段（参考）

利用者が案 A を選ぶ場合の順。**各段の前に、その段の効果を host で測る検査を先に作る**（[api-reorg.md](api-reorg.md) の P0 と同じ考え）。

| 段 | 内容 | ラベル | 門 |
| --- | --- | --- | --- |
| U0 | 測る: DERBY と LCD CATCH の HUD を、今の `mount` のランタイムの descriptor で書いた試作（上限を host だけ広げる）で、ゲストの評価のピーク・ネイティブの確保・1 回の `set` の命令数（r5sim）を `replace`/`patch` と比べる | Opus | **ここで、ネイティブ ＋7 KB 以上、または評価余裕の悪化が出たら止める** |
| U1 | schema の語彙を足す（`strokeRect`・`gradient`・opacity・clip・文字の容量）。上限はそのまま | Opus | PET・COMPANION・時計・music の画素の一致、契約試験 |
| U2 | 見える・見えないを PATCH に（命令を保つ）。上限の検査の契約を変える | Opus | 同上と、隠れた node の文字の容量の検査のやり直し |
| U3 | node 80・slot 48 へ（マスクの型、session の大きさ）。小さな `mount` が同じ大きさを払わない確保の形 | Opus | HELLO・時計の overlay の `idle_free` が減らないこと（実機） |
| U4 | 場面の付け直し（`unmount`、または `page`） | Opus | `app_stop()` の reset の順、ASan |
| U5 | アプリを移す: bigwave → keytest → stress → megademo → lcdcatch → derby | bigwave・keytest は Sonnet、他は Opus | 各アプリの host の画素の一致、DERBY は実機の heapprobe |
| U6 | `replace`/`patch` を消す | Sonnet（STEADY がレビュー） | 保存済みのプログラムを断る検査 |

U0 の結果が良くない限り、U1 以降へ進まない。筆者の見込みでは、U0 で止まる。

## 6. 読んだもの

- 設計: [api-reorg.md](api-reorg.md)（§1・§3.2・§3.6・§5・§7）、[architecture.md](architecture.md)・[decisions.md](decisions.md)・[roadmap.md](roadmap.md)・[verification.md](verification.md) の `mount`・schema の行。
- 実装: `main/ui/kasane/ksn_schema.h`・`ksn_schema.c`・`ksn_schema_session.h`・`ksn_schema_session.c`・`ksn_view.h`・`ksn_types.h`・`ksn_api.h`・`ksn_core.h`、`ksn_core.c` の差分（`:1393-1447`）と文字の上限、`main/pocket/pocket_kasane.c` の状態（`:36-140`）・ref（`:241-330`）・schema の確保と更新（`:385-465`・`:601-680`）・命令型（`:991-1020`・`:1346-1428`・`:1663-1755`）・`mount`（`:2087-2240`・`:2397-2740`）・`limits`（`:2833-2843`）、`main/pocket/app_view_assets.c`、`app_view_provider.c`。
- アプリ: §2 の表の所。
- **読んでいない所:** `ksn_render.c`、`ksn_view.c` の本体、`ksn_cache.c`、`app_music_view.c` と `app_legacy_presenter.c` の本体（music の provider は入口と PATCH の所だけ）、`ksn_source*.c`。`tx.text` の `capacity` の既定値と `tx.image` の拡大の既定は確かめていない（§3 の文字の容量は、アプリが明示した値）。
