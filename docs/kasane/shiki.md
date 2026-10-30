# Shiki — Kasane の高速化用の命令セットと API（名前の決定と棚卸し）

2026-10-30。Kasane の上で、JS が登録したプログラムを native が回す仕組み（今の `pocket.kasane.procedural` の plan・IR・VM と、`pocket.kasane.grid` の grid IR）の**総称を Shiki（式）に決めた**。この文書は (1) 名前と旧名の対応、(2) 今の API の棚卸し（事実だけ）、(3) 棚卸しで見えた問題、(4) ランタイムのメモリ（Shiki 専用ヒープの計画）、(5) 数の型（固定小数だけ）を置く。**コードの改名はまだしていない**。API の形を固めてから一度に行う（§6）。新しい命令（表引き・除算など）は、この整理の後に設計する。

**規範は [shiki-spec.md](shiki-spec.md)（v1 草案）。** この文書は経緯・理由・計測の記録。

数値の区別: **コード** は該当行の `#define`・検査で確かめた値。**実測** は引用した文書の実機の値（その文書の image・workload に限る）。**推定** は計算。

## 1. 名前

### 1.1 なぜ改名するか

| 今の語 | 出現（コード／docs、2026-10-30 の grep） | 問題 |
| --- | ---: | --- |
| VM | 1,021 ／ 919 | `docs/vm/` は QuickJS の VM。「VM の高速化」がどちらか分からない |
| IR | 1,330 ／ 529 | 手続き面の IR と grid IR の 2 系統がある |
| plan | 2,864 ／ 937 | music の presenter の計画（`music_plan`、全計画 PATCH）と衝突 |
| procedural／手続き型 | 322 ／ 126 | 何をするかが伝わらない。データの読み込みを足すと「線描き」でもなくなる |
| `ksn_proc_` | 2,263 ／ 141 | proc は process・procedure とも読める |

### 1.2 Shiki を選んだ理由

数式（計算）と儀式（決まった手順を検証済みの形のまま繰り返す）の両方の意味を持ち、線描き・格子（grid）・データの読み込みをまとめて呼べる。リポジトリ内に `shiki` は 0 件（衝突なし）。比較した案は Kata（型。SKK の `SKK_MODE_KATA` と 1 件衝突、線描き寄り）、Hata（機）＋紋紙、Ori（織）。grid も同じ名前の下に入れる（ユーザー決定）。

**表記の規則: この仕組みを指すときは、日本語の文書でもラテン文字の `Shiki` と書く。** docs には一般語の「式」が 573 回ある（「大きさの式」など）ので、漢字では grep で区別できない。

### 1.3 対応表

| 対象 | 旧名 | Shiki での名前 |
| --- | --- | --- |
| 仕組み全体・JS の名前空間 | `pocket.kasane.procedural`、`pocket.kasane.grid`、グローバル `gridFold` | **`pocket.kasane.shiki`**（grid も同じ名前空間の下へ） |
| 命令セット | 手続き型 IR、plan code、grid IR | **Shiki 命令**（Shiki ISA） |
| 1 本のプログラム | plan | **式（a shiki）**。「式を書く・登録する・描く」 |
| 登録した実体 | plan handle | **handle**（引数を当てた式） |
| 実行器 | VM | **Shiki エンジン**（「VM」は QuickJS 専用の語にする） |
| ファームの組み込み表 | ROM plan、built-in plan | **組み込みの式**（`register('derby.crowd', args)` の形は保つ） |
| JS で書く原本の記法 | `@plan`、`@planDecoder` | **`@shiki`**（復号器の印は §3 の整理で要否から決める） |
| C の接頭辞 | `ksn_proc_`、`ksn_proc_grid_`、`ksn_grid_` | **`ksn_shiki_`**、`ksn_shiki_grid_` |
| 出力の種類 | 手続き面（線分）、grid（int16 の格子） | 式の**種類**として区別する（線・格子） |
| データ領域（新規） | なし | 命令の設計と一緒に決める |

## 2. 今の API の棚卸し（2026-10-30 の main、`7ee19cc`）

### 2.1 入口と capability

| 名前空間 | 入口 | capability と `limits` | 実装 |
| --- | --- | --- | --- |
| `pocket.kasane.procedural` | `register`・`unregister`・`beginFrame`・`draw`・`commit`・`resource`・`createSurface` | **専用の capability なし**（`display.kasane` の名前空間に同居）。**上限は `limits` に出ていない** | `main/pocket/pocket_proc.c` |
| `pocket.kasane.grid` | `register`・`registration`・`registerResize`・`registerResizeSource`・`run`・`profile`・`measure`・`resource`・`explain` | 同上（capability なし、`limits` なし） | `main/pocket/pocket_grid.c` |
| グローバル `gridFold` | `fold`・`index`・`view`（初回使用時に JS の前段を読む） | — | 同上 |
| `pocket.shapes` | `project`・`fill`・`outline` | `kasane.shapes`（manifest で宣言したアプリだけ）。`maxPoints` 1024・`maxPolygonPoints` 256・`maxHoles` 6 | `main/pocket/pocket_shape.c`、[shapes.md](../api/shapes.md) |
| `pocket.derby` | `course`・`view`・`ser`（`project` は実験フラグ `POCKET_DERBY_PROJECT_BATCH`、既定 OFF） | `derby.series`（DERBY だけ）。`maxEnds` 32・`maxSections` 4 | `main/pocket/pocket_derby.c`、[derby-ser-native.md](../apps/derby-ser-native.md) |

### 2.2 `procedural`（線の式）の呼び出し

| 呼び出し | 引数 | 主な誤り（`PocketError` の code: message） |
| --- | --- | --- |
| `register(rows, points?)` | 命令の行の配列 1〜64 個。行は `[op, dst, a, b, value, color]` | `INVALID_ARGUMENT`: expected 1..64 instructions／invalid instruction entry／invalid procedural program、`LIMIT_EXCEEDED`: procedural plan limit、`OUT_OF_MEMORY`: plan allocation failed |
| `register(name, args?, points?)` | 組み込みの式の名前。`args` は宣言した数ちょうど（無ければ `undefined`／`null`／`[]`） | `INVALID_ARGUMENT`: unknown built-in plan／plan argument count mismatch／invalid plan argument |
| `points`（両形の最後の引数） | `{kind: 'affineQ14Points', x, y, color, coeff: [6 個]}`。最大 128 点 | `INVALID_ARGUMENT`: invalid typed point batch、`OUT_OF_MEMORY`: point allocation failed |
| `unregister(handle)` | | `CLOSED`: stale procedural handle |
| `beginFrame(color, surface?)` | RGB565、面の ID（省略で面 0） | `BUSY`: previous frame awaits presentation or repair、`OUT_OF_MEMORY`: frame allocation failed |
| `draw(handle, ...inputs)` または `draw(handle, [inputs])` | 0〜8 個の有限な数（足りない分は 0） | `BUSY`: beginFrame required、`CLOSED`、`INVALID_ARGUMENT`: non-finite input／typed point run failed、`LIMIT_EXCEEDED`: frame drawing limit |
| `commit()` | | `BUSY`: resource(surface ID) required（面 1 で `resource` 前） |
| `resource({maxSegments}?)`／`createSurface({maxSegments}?)` | 1〜1,024 | `LIMIT_EXCEEDED`: two surfaces maximum |

`pocket.derby.ser()` は JS を通らずに `pocket_proc_draw_numbers()`（`draw` の後半）を呼ぶ。`pocket.shapes` は組み立て中のフレームへ直接書く（`pocket_proc_native_line()` など）。

### 2.3 上限（コードで強制している値）

| 項目 | 値 | 場所 |
| --- | ---: | --- |
| 線の式: 命令 | 64 | `KSN_PROC_CODE` |
| レジスタ（float32） | 16 | `KSN_PROC_REGS` |
| draw 1 回の入力 | 8 | `KSN_PROC_INPUTS` |
| 組み込みの式の引数 | 8 | `KSN_PROC_ROM_PARAMS` |
| ループの入れ子 | 8 | `KSN_PROC_LOOP_DEPTH` |
| draw 1 回の実行ステップ | 10,000 | `KSN_PROC_STEPS` |
| フレームの線分 | 1,024（面ごとに 1〜1,024 へ縮められる） | `KSN_PROC_SEGMENTS`、`maxSegments` |
| draw 1 回のラスタ | 8,192 | `KSN_PROC_RASTER_STEPS` |
| 同時に登録できる式 | 32 | `PROC_HANDLES` |
| 型付きの点 | 128/式 | `PROC_POINT_MAX` |
| 座標の範囲 | −480〜720 | `PROC_POINT_LOW/HIGH`、VM の `coordinate()` |
| 面 | 2 | `PROC_SURFACES` |
| 格子の式: 同時の登録 | 6 | `POCKET_GRID_MAX_SLOTS` |
| 格子の出力 | 4,096 画素 | `GRID_APP_MAX_PIXELS` |
| 格子の入力の合計 | 8,192 個の int16 | `GRID_APP_MAX_INPUTS` |

### 2.4 命令セット

**線の式**（`ksn_proc_op`、16 種）: `SET`・`INPUT`・`ADD`・`MUL`・`SIN`（算術、float32）、`REPEAT`（定数 1〜255）・`REPEAT_REG`（0〜255）・`END`・`BREAK_IF_GT`（制御）、`MOVE`・`PLOT`・`LINE`・`PLOT_COLOR_REG`・`LINE_COLOR_REG`・`CUBIC`・`LINE_PATTERN`（出力）。

**格子の式**（`ksn_grid_opcode`、5 種）: `CONST`・`LOAD`・`ADD`・`MUL`・`MIN`（int16、有限の矩形反復。添字は `base + x·X + y·Y + tapX·TX + tapY·TY`、係数は登録時の引数で決まる）。

書けないもの（線の式）: `if`/`else`（`break` のみ）、`>` 以外の比較、変数での除算、`min`/`max`/`abs`/`floor`/`%`・ビット演算、**配列・表の読み込み（メモリアクセス）**、draw をまたぐ状態（C の `ksn_proc_begin_state` は JS に出ていない）、塗り・画素の出力。

### 2.5 式の渡し方（4 通り）

| 方式 | ゲストのメモリ | 使う所 |
| --- | --- | --- |
| A. 命令の行の配列を `register()` | 最も重い。1 命令 約 119 B を登録が返るまで保つ（host 実測、[js-to-ir.md](js-to-ir.md)） | プローブ類、`proc_news_zoom`（JS の関数で行を作る） |
| B. 詰めた文字列＋ゲスト内の復号器 → 配列 → `register()` | 重い。復号 12〜20 ms（`apps/bigwave/big_wave.js` のコメント） | BIG WAVE、凍結した `apps/heapprobe/derby_v0` |
| C. 組み込みの式を名前で登録 | 引数の配列だけ（native 1 本 40 + 4 × 引数 B） | DERBY（`derby.*`）、MEGADEMO（`kasane.*`） |
| D. C の原本を `@plan` の JS 関数で書く（ビルド時に Node で変換） | C と同じ | DERBY だけ。MEGADEMO の原本は文字列のテンプレート（`apps/kasane/megademo_plans.js`） |

### 2.6 アプリの使い方の実態

- 登録はどのアプリも**1 フレームに 1 本、空きヒープを見てから**行う: DERBY の `load()`（空き 18 KB 未満なら待つ、`apps/derby/derby_view.js`）、MEGADEMO の `prefetch()`（`LOAD[2]` の門、`apps/kasane/megademo_set.js`）、BIG WAVE の `pump()`（`apps/bigwave/big_wave.js`）。同じ仕組みを各アプリが自前で書いている。
- 毎フレーム `beginFrame` → `draw` を何度も → `commit`。draw の回数は DERBY 15〜26・BIG WAVE 16・MEGADEMO 7 回/フレーム（[derby-native-survey.md](../apps/derby-native-survey.md)、[derby-pan-memory.md](../apps/derby-pan-memory.md)、[derby-finish-fx.md](../apps/derby-finish-fx.md)）。
- 引数は登録時に固定。変えるには `unregister` → `register`。

## 3. 棚卸しで見えた問題

1. **JS とネイティブの往復が費用の大半。** draw 1 回の固定費は約 100〜115 µs（実測、[derby-background-cost.md](derby-background-cost.md)）で、VM の 1 ステップ 0.45 µs の約 230 倍。式がデータの列を読めないので、項目ごとに JS で計算して draw を呼ぶ形になる（DERBY でこの固定費だけで 1.5〜3 ms/フレーム、推定）。JS で 1 点を投影すると 42 µs、入力の配列が 23 µs（実測、[derby-pan-camera-cost.md](../apps/derby-pan-camera-cost.md)）。`pocket.derby.ser()` の native 化が効いたのは、「列を 1 回の呼び出しで回す」をアプリ専用に作ったから。**メモリアクセス（データ領域を式から読む）は、これを汎用にする入口になる。**
2. **表で済む値を毎回計算している。** `SIN` を引数約 201 超で呼ぶと 7〜9 倍遅かった（実測、[derby-background-cost.md](derby-background-cost.md)。現在は 2π で折り返す修正済み、`bd1d829`）。BIG WAVE は除算が無いので `1/d` を級数で進めている（誤差 最大 3.1 px、[big-wave.md](../apps/big-wave.md)）。表を命令に焼き込むと 64 命令に当たって式を分け、draw がさらに増える（[derby-finish-fx.md](../apps/derby-finish-fx.md)）。
3. **式の渡し方が 4 通り、原本の書き方が 3 通り**（`@plan` の JS、MEGADEMO の文字列テンプレート、手書きの文字列・配列）。
4. **自作アプリは重い方式（A・B）しか使えない。** C・D はファームのビルド時に表を作る。実機のエディタで書くアプリには、ゲストに命令を持たせない経路が無い。
5. **登録の待ち行列とヒープの門を各アプリが書いている**（§2.6）。
6. **上限が `limits` に出ていない。** `procedural` と `grid` は専用の capability を持たず、§2.3 の値はアプリが feature-test で知れない（CLAUDE.md の「`limits` に出す値はコードで実際に強制している値だけ」の対象になっていない）。`shapes`・`derby.series` は出している。
7. **アプリ名がフレームワークの中にある。** `pocket_proc.c` の `draw_run()` が `"derby.runner"` を名前で比べる特別経路（`POCKET_DERBY_RUNNER_AFFINE`、既定 OFF）。`pocket.derby` も DERBY 専用の面。
8. **入力 8 個・引数 8 個の壁**で、値を詰めるか式を分けている。
9. **JS と Shiki エンジンで意味が少しずれる**（ループ回数が整数でないとき、JS は切り上げて回り、エンジンは draw ごと失敗する。`/` は定数でだけ、2 の冪以外は厳密でない。[js-to-ir.md](js-to-ir.md) §5.2）。
10. **線の式と格子の式で、登録・実行・資源の API の形が違う**（`draw`＋`commit` と `run(handle, buffers, params)`＋`resource(handle)`、`explain`・`profile` は格子だけ）。

## 4. ランタイムのメモリ: Shiki 専用ヒープ（計画、2026-10-30 ユーザー合意）

### 4.1 今の確保（コードで確認）

ゲストの 160 KiB は QuickJS の確保量を数えて止める**上限値**（`JS_SetMemoryLimit`、`main/app_session.c` の `gc.heap_limit`）で、専用の区画ではない。Shiki の native 側も同じシステムヒープから `malloc`/`calloc` で取る（`pocket_proc.c`、`pocket_grid.c`）。ゲストの GC が作る穴と Shiki の大きなブロックが、同じヒープの中で混ざる。

| もの | 大きさ | 取る時 |
| --- | --- | --- |
| 面のフレーム（組み立て中・表示中の 2 枚） | 各 6 + 10 × 線分 B（1,024 本で 10,246 B） | 最初の `beginFrame`／`resource`／`createSurface` |
| 作業用のフレーム（`workspace`） | 10,246 B | 同上 |
| エンジンの状態（`vm`） | 約 1 KB（推定。命令の写し 768 B が大半） | 同上 |
| 線の式 1 本 | 40 + 12n B（配列）／40 + 4 × 引数 B（組み込み） | `register` |
| 型付きの点 | 点の数に比例（`points_bytes()`） | `register` |
| 格子の式・入力・出力 | 式ごと | `register`／`run` |

### 4.2 専用ヒープで直接効く既知の問題

1. **連続した大きいブロックが取れない。** DERBY の最大の連続ブロックが 10,240 B で、10,246 B のフレームが入らず第 2 面を作れなかった（[surface-segment-cap.md](surface-segment-cap.md)）。DERBY と MEGADEMO は「穴がまだあるうちにフレームを取る」回避を書いている（`apps/derby/derby_view.js` の冒頭）。
2. **アプリごとのヒープの門**（§2.6）。Shiki が自分の予算を知らないので、アプリがシステム全体の空きから推し量っている。
3. **上限を `limits` に出せない**（§3 の 6）。予算の決まった区画なら残量をそのまま出せる。
4. **データ領域（メモリアクセス）の置き場所。** これから足す表・型付き配列を置く場所として自然で、PIE が要る 16 B 境界もそこで保証できる。

### 4.3 形

- **起動時に 1 区画を取る。** アプリが予算を宣言し（manifest か最初の呼び出し。例 `shiki: {bytes: 40960}`。形は §6 の 1 で決める）、ヒープがまだきれいなセッション開始時に連続で確保する。`.bss` に固定で持たないので、Shiki を使わないアプリの空きは減らない（CLAUDE.md の「capability を足すたびにゲストの部屋が減る」を避ける）。
- **区画の中は寿命で 2 つに分ける。** 前側にセッションの間ずっと使うもの（フレーム・作業用フレーム・エンジンの状態）を詰めて置き、残りに登録・解除で変わるもの（式・点・データ領域）を置いて区画の中だけの TLSF で管理する。ESP-IDF の `multi_heap` を区画の上に載せる想定（**v6.0.1 で使えるかは未確認**）。
- **登録時に予算で判定する。** 足りなければ `LIMIT_EXCEEDED` を返し、レースの途中の `OUT_OF_MEMORY` ではなく登録の時点で失敗を分からせる。
- **`app_stop()` で区画ごと捨てる。** 解放漏れが構造的に起きない。

### 4.4 欠点

- 予算を多く取れば、その分ゲストとシステムが使えない。PSRAM が無いので、予算の決め方が肝心。
- 区画の中でも、毎フレームの登録・解除で穴はできる。ゲストの GC と混ざらない分ましになる見込み（推定）。
- 一度取った区画は伸ばせない。

### 4.5 決める前に測ること（実機）

**DERBY・MEGADEMO・BIG WAVE で Shiki の native 側が使う量の最大は、まだ誰も記録していない。** まず確保を 1 か所に集めて数える（現在量・最大・最大の 1 個・確保の回数を、上の表の種類ごとに）。その最大で、予算の既定値と、区画にする利益（最大の連続ブロック `lg` とターン内の最小 `mn` がどれだけ戻るか）を判断する。計数は製品 image に残さない（既定 OFF のビルドの選択肢にする）。

### 4.6 ゼロコピーの寿命の規則（計画、2026-10-30 ユーザー合意）

専用ヒープの寿命を制御すれば、**JS アプリ ↔ Shiki の受け渡しと Shiki の内側はゼロコピーにできる**。LCD までの全経路をゼロにするものではない。

**今のコピー（コードで確認）**

| 区間 | 今 | ゼロコピーにする方法 |
| --- | --- | --- |
| ① draw の入力（数 8 個） | 呼ぶたびに JS の数を float へ（`draw_impl`） | 入力を Shiki 所有の `Float32Array` に置き、draw がその場で読む |
| ② 登録の行・点 | JS の配列を 1 要素ずつ読んで native へ（登録時 1 回、`register_impl`・`read_points`） | JS が Shiki のメモリへ直接書き、登録は領域を指すだけにする |
| ③ 格子の入力 | `run` のたびに 16 B 境界の native 領域へコピー（`profile` の `copyUs`、[grid-js-notation.md](grid-js-notation.md)） | Shiki が 16 B 境界で確保した領域へ JS が直接書く |
| ④ 式の命令 → エンジン | draw のたびにコピー（`begin_core`） | 描画ごとの無駄の削減（別作業、実装中）で消す |
| ⑤ エンジンの出力 → フレーム | 作業用フレームに書いてから組み立て中のフレームへ `memcpy`（`draw_run`） | 組み立て中のフレームへ直接書き、失敗したら線分の数を戻す（`pocket_proc_native_rollback` がすでにある）。作業用フレームの 10,246 B も不要になりうるが、今は画像モードの画素キャッシュと共用（`proc_workspace`）なので、その置き場所を先に決める |
| ⑥ フレーム → LCD | 帯へラスタ化 → バイトを入れ替えて転送用バッファへ → DMA | 消さない。ラスタ化と形式の変換で、コピーではない（[README](README.md) の「全経路 1 copy 以下は必須目標にしない」） |

**規則**

1. **呼び出しの中だけなら、ゲストのメモリを借りてよい。** `pocket.derby` と `pocket_pixel.c` は今も JS の型付き配列をその場で読む。同期の呼び出しの中で読み終えるなら安全。**Shiki は戻った後にゲストのポインタを持たない。**
2. **呼び出しをまたぐメモリは Shiki が持ち、JS には窓として見せる。** QuickJS-ng の `JS_NewArrayBuffer(ctx, buf, len, free_func, opaque, …)` で、Shiki のヒープ上のメモリを ArrayBuffer として渡す。外部メモリは JS の確保量の上限（`JS_SetMemoryLimit`）に数えられない見込みで、Shiki の予算で管理する（**数えられないことは未確認**）。
3. **解放は持ち主が 2 人とも手放してから。** `free_func` は「JS が手放した」という合図にとどめ、その時点では解放しない。Shiki の登録も外れて初めて解放する。~~JS の `buf.transfer()` は元の ArrayBuffer を detach し、`free_func` をすぐ呼ぶ~~ **（2026-10-30 訂正、SLOW のレビューで判明しコードで確認）** `transfer()` が `free_func` を呼ぶのは長さ 0 のときだけ。同じ長さなら外部メモリと `free_func` を新しい ArrayBuffer へ渡し、**長さが違えば外部メモリを `js_realloc` する**（`components/quickjs-ng/quickjs-ng/quickjs.c` の `js_array_buffer_transfer`）。Shiki の区画のポインタを realloc されるとヒープが壊れ、Shiki はこの呼び出しを止められない。仕様では、Shiki を使うセッションの `ArrayBuffer.prototype.transfer`／`transferToFixedLength` を消す（[shiki-spec.md](shiki-spec.md) §9.3）。逆に `unregister` では Shiki から `JS_DetachArrayBuffer` を呼び、古い窓を長さ 0 にする（解放済みのメモリへの読み書きにならない）。
4. **Shiki がデータを読むのは draw／run の中だけ。** 描画タスクは 1 つで draw は同期なので、JS の書き込みと Shiki の読み込みは重ならない。後の段階（commit 後の帯の描画など）でデータを読むなら、その時点で値を固める（コピーか 2 面）必要があり、ゼロコピーではなくなる。結果はフレームが持つ形を保つ。

**セッションの終了の順番**: ゲストを先に壊し（ここで `free_func` が呼ばれ切る）、Shiki の区画をその後に丸ごと捨てる。CLAUDE.md の「ゲストのコールバックを保持するモジュールはゲストより先に reset」とは逆向きの順序になるので、実装時に `app_stop()` へ明記する。

**効き目（見込み）**: `memcpy` 自体は数百 B で小さい。大きいのは JS 側で配列を作る費用（入力の配列 23 µs、配列の読み 2.75 µs／要素、実測、[derby-pan-camera-cost.md](../apps/derby-pan-camera-cost.md)）が消えることと、データ領域を式から読めれば 1 回の draw で列を回せて draw の回数が減ること（§3 の 1）。

**確かめること**: 外部 ArrayBuffer が `JS_SetMemoryLimit` の計数に入らないこと、`transfer()`・`structuredClone` 相当で外部 ArrayBuffer がどう扱われるか（コピーか、detach だけか）、ガベージコレクタが `free_func` を呼ぶ時期。いずれも host の試験で先に見る。

## 5. 数の型: 固定小数だけ（2026-10-30 ユーザー決定）

**Shiki が受け付ける実数は固定小数だけにする。** レジスタ・データ領域・入力・引数のすべてで float をやめる。

### 5.1 理由

- **命令の取り出しが重い（1 ステップ約 108 サイクル）のは、次に解く壁が見えただけ**（ユーザーの判断）。データ領域で列を 1 回の draw で回し、本体を PIE に下ろせば、残るのは演算になる。その時点で整数の方が速い。
- **PIE は整数しか持たない。** float の式を PIE へ暗黙に下ろす案は、丸めと途中の非有限値の意味論が違うので棄却していた（[d3-path-float-decision.md](d3-path-float-decision.md)。`0.4999 + 0.0002` が float では x=1、Q14 では x=0）。全部が固定小数なら、すべての式が最初から明示的に型付けされ、この棄却理由が消える。**この決定で D3 の「PIE は明示的に型付けした Q14 点列だけ」は、Shiki の全体へ広がる。**
- **host と実機が整数で一致する。** 今は実機の `sinf`（newlib）と積和の縮約（`MADD.S`）が host の float32 と 1 ulp も違わないかを見ていない（[js-to-ir.md](js-to-ir.md) §5.5）。しかも実機の画素は読み戻せない（`board_capture` は転送前）。整数なら host の全画素ハッシュがそのまま実機の答えになる。
- 1 ステップごとの `isfinite` の検査が要らない（代わりに桁あふれの規則を決める）。データ領域が int16 なら float32 の半分。

### 5.2 PIE の事実（TRM v1.8）

| 演算 | 8 ビット | 16 ビット | 32 ビット |
| --- | --- | --- | --- |
| 掛け算 | `EE.VMUL.S8/U8`（p.201、207） | `EE.VMUL.S16/U16`（p.198、204）: 16×16 の積を `SAR` だけ**算術右シフト**して下位 16 ビット | **無い** |
| 加減算 | あり | あり | `EE.VADDS.S32`／`EE.VSUBS.S32`（p.149、284）: **飽和する** |
| 比較・最小／最大 | あり | あり | `EE.VCMP.*.S32`（p.156〜162）、`EE.VMAX/VMIN.S32`（p.183、192） |

このプロジェクトの実測の注意（[docs/perf/pie-simd.md](../perf/pie-simd.md)）: 16 ビットのレーンをシフトする命令は無く、定数との掛け算で表す。シフト量は `SAR` 1 つを全レーンで共有し、途中で切り替えると利得が消えやすい。

### 5.3 ここから決まる意味論（案）

PIE の経路とスカラーの経路が**ビット単位で同じ結果**を出すこと（D3 の教訓）を、最初の制約にする。

| 項目 | 案 | 理由 |
| --- | --- | --- |
| 幅 | **2 つ**: レジスタは 32 ビット、PIE に下ろす本体とデータ領域の掛け算は 16 ビット | PIE の掛け算は 16 ビットまで。スカラーのレジスタまで 16 ビットにすると、座標と縮尺の幅が足りない |
| 32 ビットの既定の形式 | Q16.16（±32,768、1/65,536 刻み） | 座標の範囲 −480〜720 と遠近の縮尺の両方が入る |
| 16 ビットの形式 | 式が宣言する（座標なら Q12.4 で ±2,048 px・1/16 px、係数なら Q2.14 など）。今の型付きの点の Q14 は、この特別な場合 | `SAR` が全レーン共通なので、1 つの演算の中で形式を揃える必要がある |
| 掛け算の丸め | **切り捨て（算術右シフト）** | `EE.VMUL.S16` がそう動く。最も近い値への丸めを足すと、PIE に命令が 1 つ増える |
| 桁あふれ | **飽和** | `EE.VADDS.S32` がそう動く。巡回（wrap）にすると、遠近の計算で画面の反対側に線が飛ぶ |
| 座標への変換 | 切り捨て（`floor`）に統一 | 今の float の `lroundf` と Q14 の floor の食い違い（D3）を、片方に揃えて消す |
| 0 での割り算 | 失敗させず、飽和した値にする（案） | 今の「非有限は draw ごと失敗」を引き継ぐかは要決定 |
| 色 | 固定小数とは別の整数の型（u16） | 0xFFFF などが Q16.16 の ±32,768 に入らない（§5.5） |

### 5.4 未決と、先に確かめること

1. **値の幅（いちばん大きい）。** 遠近の `1/z` は z が 0.5〜500 m に広がる。固定小数で扱うため、幅を自分で正規化する命令を持つ。候補は `RECIP`（逆数。PIE の逆数は表＋Newton 1 段で 0.36 px、2 段で 0.01 px の数値実験がある、[oval-pie-design.md](oval-pie-design.md) §5.2）と `PROJECT`（投影）。DERBY の観客・首振りの式を Q16.16 で書き直し、float の参照との px の誤差の最大を host で測ってから決める。
2. **`SIN`** は表（データ領域の `LOOKUP`）に置き換える。表の大きさと補間の有無。
3. **`CUBIC`・`LINE_PATTERN` の中身**（今は `fmodf`・`lroundf` を使う float の C）を整数に書き直す。
4. **JS との境目。** JS の数は double。入力・引数を固定小数に変える丸めの規則（API で 1 つに決める）。データ領域は `Int16Array`／`Int32Array` を窓にする（§4.6）。JS 側も整数で計算すると速いかもしれない（QuickJS の小さい整数の計算はソフトウェアの double を避ける。未計測の仮説）。
5. **移行。** DERBY・MEGADEMO・BIG WAVE の式はすべて float で書かれている。`@shiki` の変換器が float の JS を固定小数の式へ下ろす（倍率の決め方を変換器が持つか、書き手が宣言するか）。旧 API の float の式を残すかは、§6 の 1 の互換の順番で決める。
6. **格子の式**は今も int16（`ksn_grid_opcode`）で、この決定と既に揃っている。線の式と番地・形式の規則を共通にする。

### 5.5 Q16.16 で DERBY の式を走らせた誤差（host、2026-10-30）

`python3 tools/kasane_ir/q16_error.py`（`tools/kasane_ir/q16_error.mjs`）。DERBY の `@plan` の式を同じ AST から double（正解）・float32（今の VM）・固定小数の 3 通りで走らせ、式が描画に渡す座標（画素に丸める前）を double と比べた。入力は、check_equivalence.py の捕獲（7,763 個の実入力と 13 本の摂動）と、首振り（`pocket.derby.ser()`）の JS の写し `derby_ser_oracle.js` が出す draw（実ゲーム 3 段階と、ランダムなカメラ 1,500 件、式ごと 4,000 本まで）。固定小数の規則は §5.3 の案（掛け算は切り捨て、加減算は飽和、定数・入力は最も近い値、`sin` は正確な値を刻みに丸めた理想の表）。**host の計算で、実機ではない。**

| 式（入力の出どころ） | float32 の最大誤差 | Q16.16 の最大（99.9%） | Q11.20 の最大（99.9%） |
| --- | ---: | ---: | ---: |
| `prail`（首振り・実ゲーム） | 0.0007 px | 0.78（0.44） px | 0.044（0.029） px |
| `prail`（首振り・ランダム） | 0.022 px | **9.8**（1.1） px | 0.72（0.074） px |
| `pt`（首振り・実ゲーム） | 0.0005 px | 0.64（0.30） px | 0.045（0.020） px |
| `pt`（首振り・ランダム） | 0.006 px | 1.7（0.66） px | 0.045（0.028） px |
| `hl`（首振り） | 0.00003 px | 0.001 px | 0.00008 px |
| `conf`（紙吹雪・実ゲーム） | 0.001 px | 1.8 px | 0.07 px |
| `dust`・`bokeh` | 0.03・0.21 px | 0.32・0.19 px | 0.04・0.016 px |
| その他 15 本（`crowd`・`runner`・`stands`・`rail` など） | 0.0002 px 以下 | 0.03 px 以下 | 0.00001 px 以下 |

**読み方（2026-10-30 のユーザーの指摘「飽和はデータ型ではなく手続きのせいでは」を確かめた結果）:**

1. **演算の途中の飽和はほぼ無い。** Q16.16 で数えた飽和は、ほとんどが「値を固定小数に入れた時点」のもの（色の定数・入力 0xFFFF などが ±32,768 を超える）で、演算の途中は `dust`・`pt` のランダムな入力で 25・53 回だけ。**色は実数ではなく整数（u16）として、固定小数とは別の型にする**（§5.3 に追加する未決）。
2. **大きい誤差は値の幅ではなく、式の書き方から来る。** 小数部を 16 → 20 ビットにすると誤差が約 1/16 になる（分解能に比例）。原因の式は 2 つ:
   - `prail`・`pt` は、1/Z' を Newton 法で前の値から更新し、奥行き `pz += dz` を**足し込んで**進む。`dz` が小さい（最悪の入力で 0.0033）と、刻みに丸めた誤差が支柱の数（最悪で 115 本）だけ積もる。1/Z' 自体も 0.003〜2.5 と小さく、相対の分解能が落ちる。
   - `conf` は `sin(i * 2.1 + (i * .00041 + .037) * t)` の小さな定数 0.00041 が丸めで約 0.5% ずれ、時間 `t` が掛かって sin の位相が動く（軌道はなめらかなまま別の乱数になる。見た目の破綻ではない）。
3. **したがって、型の問題ではなく手続きの問題として直せる見込み**（推定）: 足し込みを `z + j * dz` の形にする、1/Z' を 2^8 倍などの倍率で持つ（その変数だけ Q11.20 相当の分解能になる）、小さな定数は掛ける順序を変える。直した式で同じ計測を通すのが次の確認。`RECIP`／`PROJECT` のような幅を扱う命令は、この結果を見る限り必須ではない。

### 5.6 `prail`・`pt` を固定小数向けに書き直した結果（host、2026-10-30）

書き直しは [`tools/kasane_ir/plans_q16/derby_pan.js`](../../tools/kasane_ir/plans_q16/derby_pan.js)（出荷しない）。§5.5 の範囲の調べで、奥行き `z` は −0.8〜−0.05、その増分 `dz` は 0.0005〜0.08（実ゲーム）と小さく、1/65,536 刻みでは `dz` の丸めが相対 1/70 になっていた。1/Z' にあたる `q` は 1.2〜18 で精度は足りていた。そこで**奥行きだけを 256 倍の単位で持つ**: 呼び出し側が `z`・`dz`・`farZ` を 256 倍して渡し、Newton の更新 `q (2 + pz q)` を `q (2 + PZ q / 256)` にする。256 の割り算は固定小数でも double でも正確なので、**double で走らせると元の式と全座標で差 0**（下の「alt f64」）。式の命令の数は変わらない。

`node tools/kasane_ir/q16_error.mjs --alt tools/kasane_ir/plans_q16/derby_pan.js apps/derby/derby_prog.js .cache/kasane_ir/q16_pan_cases.json`（正解は元の式の double）:

| 式（入力） | float32（今の VM） | Q16.16 元の式 | **Q16.16 書き直し**（99.9%） | 画素がずれた端点 |
| --- | ---: | ---: | ---: | ---: |
| `prail`（実ゲーム、4,000 本・135,548 座標） | 0.0007 px | 0.78 px | **0.008**（0.005） px | 79（0.06%） |
| `prail`（ランダムなカメラ、4,000 本） | 0.022 px | 9.8 px | **0.049**（0.010） px | 102 |
| `pt`（実ゲーム、8,000 本） | 0.0005 px | 0.64 px | **0.009**（0.005） px | 85 |
| `pt`（ランダムなカメラ、3,949 本） | 0.006 px | 1.7 px | **0.016**（0.010） px | 59 |

- 誤差は 1/16〜1/200 になり、すべて 0.05 px 未満。float32 より 1 桁大きいが、画素に丸めて変わる端点は 0.1% 未満（境目のちょうど上にある点だけ）。
- 演算の途中の飽和は `pt` のランダムな入力で 68 回（状態と線分の数は double と同じ）。実ゲームの入力では 0。
- **§5.4 の 1 の結論: 値の幅を扱う命令（`RECIP`／`PROJECT`）は、DERBY の遠近には要らない。** 要るのは、変数ごとに倍率（Q 形式）を選べること。（2026-10-30 ユーザー決定: 書き手が `@q` で宣言する。[shiki-spec.md](shiki-spec.md) §8.3。§5.6.1 で、この宣言だけで同じ結果になることを確かめた。）今回の倍率は、捕獲した入力の範囲から人が選んだ。
- 実機の C（`pocket.derby.ser()`、`main/pocket/pocket_derby.c`）は `z`・`dz`・`farZ` を 256 倍して渡す必要がある。**出荷している式と C は変えていない。**

### 5.6.1 仕様の規則で測り直した結果（host、2026-10-30、草案 2）

SLOW のレビュー（M2）で、§5.5・§5.6 の計測スクリプトの規則が仕様と違う（`/256` の飽和の位置、入力の丸め、回数の読み方）と指摘されたので、`q16_error.mjs` を [shiki-spec.md](shiki-spec.md) 草案 2 の規則（§2.2・§2.4・§4.4 の表・§4.5 の +0.5・§8.3 の Q 形式の規則）そのものに書き直した。`prail`・`pt` は、手で 256 倍した書き直しをやめ、**出荷している関数に `@q z=24, dz=24, pz=24`（`pt` は `farZ`・`fz0`・`fz` も）を足しただけ**（[`plans_q16/derby_pan.js`](../../tools/kasane_ir/plans_q16/derby_pan.js)）。

`node tools/kasane_ir/q16_error.mjs --alt tools/kasane_ir/plans_q16/derby_pan.js apps/derby/derby_prog.js .cache/kasane_ir/q16_pan_cases.json .cache/kasane_ir/cases.json`:

| 式（入力） | float32 | §5.5 の素朴な Q16.16 | **仕様の規則**（99.9%） | 画素がずれた端点 |
| --- | ---: | ---: | ---: | ---: |
| `prail`（実ゲーム／ランダム） | 0.0007／0.022 px | 0.78／9.8 px | **0.008（0.005）／0.049（0.010） px** | 79／102 |
| `pt`（実ゲーム／ランダム） | 0.0005／0.006 px | 0.64／1.7 px | **0.009（0.005）／0.016（0.010） px** | 85／59 |
| `conf`（実ゲーム／ランダム） | 0.001／0.002 px | 1.8／11.5 px | **0.12（0.11）／0.73（0.50） px** | 458／1,164 |
| `dust`（実ゲーム／ランダム） | 0.019／0.028 px | 0.24／0.32 px | **0.59（0.58）／2.0（0.52） px** | 636／576 |
| `bokeh`（実ゲーム／ランダム） | 0.001／0.21 px | 0.054／0.19 px | **0.19（0.19）／0.96（0.57） px** | 849／1,276 |
| その他 15 本 | 0.0002 px 以下 | 0.03 px 以下 | 0.025 px 以下 | `vis` 128 本（下） |

- **`prail`・`pt` は §5.6 と同じ数値**（`MUL` のシフト量への畳み込みで飽和の位置が変わったが、実ゲームの入力では飽和は元々 0）。**§5.6 の書き直しは仕様の `@q` だけで表せる**ことが確かめられた。
- **入る時点での飽和が 0 になった**（色・回数を Q0 と推論したため。§5.5 の素朴な規則では色が ±32,768 を超えて飽和していた）。
- `conf` は、掛け算の定数を最大の Q 形式で持つ規則（§8.3 の 4）で 1.8 → 0.12 px。
- **`dust`・`bokeh` は sin の表で悪くなった。** `sin(u * 137 + t)` のように sin の結果に大きな数を掛けて乱数代わりにする式で、表の位相の刻み（1/65,536 周）が増幅される。`dust` のランダムな入力の 2.0 px は、`GR > sin(u * 53)` の分岐が反転して粒の長さ（2 px）が変わったもの。粒子の位置は元々乱数なので見た目の破綻ではないが、この種の式は「float と同じ絵」にはならない（仕様 §4.4 に記録）。
- `vis` の 128 本は、誤差が 1e-5 px 以下なのに画素が違う端点。ちょうど半分の負の座標で、仕様の丸め（+0.5 して切り捨て、正の方向）と旧 API の `lroundf`（0 から遠い方）が違う（仕様 §4.5）。

### 5.7 `sin` の表（2026-10-30 ユーザー決定: 表引きを採る）

`sin` は表引きにする。表と多項式の関係:

- **Weierstrass の近似定理**: 閉区間の連続関数は、多項式でいくらでも一様に近似できる。**Chebyshev の等振動定理**（最良近似の誤差は区間で等しい高さで符号を交互に変える）と、それを解く **Remez 法**で、決めた次数の最良（minimax）多項式が求まる。
- **区間の縮小**: 周期と対称性で、引数を 0〜π/2（4 分の 1 周）へ落とせる。整数の位相（例: 1 周を 2^16）なら、上位ビットで象限、下位ビットで区間内の位置がそのまま取れる。固定小数と相性が良い。
- **形の候補**: (a) 4 分の 1 周の表＋線形補間（表 256 個の int16 で 512 B、誤差は概ね 2^-16 前後。推定）、(b) 区間ごとの低次多項式（表は係数だけ。掛け算が増えるが、16 ビットの掛け算は PIE で 8 レーン並ぶ）、(c) 4 分の 1 周の minimax 多項式 1 本（5 次で概ね 1e-4〜1e-5。推定）。データ領域の `LOOKUP` で JS が表を持つか、組み込みの表にするかも併せて決める。
- 注意: [oval-pie-design.md](oval-pie-design.md) で棄却したのは、円弧を歩く**漸化式**としての Chebyshev（誤差が歩数だけ積もる）で、1 回ずつ評価する多項式とは別の話。

## 6. 次の作業

0. **Shiki の native 側のメモリを実機で数える**（§4.5）。専用ヒープの予算と利益の判断材料。
0. **固定小数の値の幅を host で確かめる**（§5.4 の 1）: 済み（§5.5）。誤差は型ではなく式の書き方（足し込み・小さな値の Newton）から来る。`prail`・`pt` を書き直して 0.05 px 未満に下がった（§5.6）。残りは `conf` の書き直し、色を整数の型に分けること、変数ごとの倍率（Q 形式）を記法でどう決めるか。
1. **API の形の設計**（この文書の続き、または別文書）: 専用ヒープの予算の宣言（§4.3）、式の渡し方を 1 本にする（登録したら native が持ち、JS は捨てる。自作アプリも同じ経路）、データ領域（型付き配列を登録して式から読む）、列を 1 回の draw で回す呼び方、登録の待ち行列を土台へ、`shiki` の capability と `limits`、線と格子の API の揃え方、旧 API の互換と移行の順番。
2. **新しい命令**（表引き・除算・比較など）は 1 の後。命令の解釈部分と DRAM に効くので、PIE・命令キャッシュに触れる判断として STEADY 以上で扱う。
3. 設計と仕様は SLOW の独立レビューにかける（CLAUDE.md のルーティング）。
4. **改名の実施**は 1 が固まってから一度に行う。CLAUDE.md の規則どおり、コードが参照するファイル名・節番号・決定番号は保ち、移したら参照を直して `python tools/check_docs.py` を通す。旧名（procedural・plan・IR・VM）で書かれた既存の文書は書き換えず、この文書の §1.3 を対応表とする。

関連: [js-to-ir.md](js-to-ir.md)（JS → IR のコンパイラと `@plan`）、[flash-plan.md](flash-plan.md)（組み込みの式）、[grid-js-notation.md](grid-js-notation.md)（格子の式の記法）、[plan-sized-alloc.md](plan-sized-alloc.md)、[derby-background-cost.md](derby-background-cost.md)（費用モデル）、[oval-pie-design.md](oval-pie-design.md)（列のウォーカー P5）。
