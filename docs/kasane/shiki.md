# Shiki — Kasane の高速化用の命令セットと API（名前の決定と棚卸し）

2026-10-30。Kasane の上で、JS が登録したプログラムを native が回す仕組み（今の `pocket.kasane.procedural` の plan・IR・VM と、`pocket.kasane.grid` の grid IR）の**総称を Shiki（式）に決めた**。この文書は (1) 名前と旧名の対応、(2) 今の API の棚卸し（事実だけ）、(3) 棚卸しで見えた問題、(4) ランタイムのメモリ（Shiki 専用ヒープの計画）を置く。**コードの改名はまだしていない**。API の形を固めてから一度に行う（§5）。新しい命令（表引き・除算など）は、この整理の後に設計する。

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

- **起動時に 1 区画を取る。** アプリが予算を宣言し（manifest か最初の呼び出し。例 `shiki: {bytes: 40960}`。形は §5 の 1 で決める）、ヒープがまだきれいなセッション開始時に連続で確保する。`.bss` に固定で持たないので、Shiki を使わないアプリの空きは減らない（CLAUDE.md の「capability を足すたびにゲストの部屋が減る」を避ける）。
- **区画の中は寿命で 2 つに分ける。** 前側にセッションの間ずっと使うもの（フレーム・作業用フレーム・エンジンの状態）を詰めて置き、残りに登録・解除で変わるもの（式・点・データ領域）を置いて区画の中だけの TLSF で管理する。ESP-IDF の `multi_heap` を区画の上に載せる想定（**v6.0.1 で使えるかは未確認**）。
- **登録時に予算で判定する。** 足りなければ `LIMIT_EXCEEDED` を返し、レースの途中の `OUT_OF_MEMORY` ではなく登録の時点で失敗を分からせる。
- **`app_stop()` で区画ごと捨てる。** 解放漏れが構造的に起きない。

### 4.4 欠点

- 予算を多く取れば、その分ゲストとシステムが使えない。PSRAM が無いので、予算の決め方が肝心。
- 区画の中でも、毎フレームの登録・解除で穴はできる。ゲストの GC と混ざらない分ましになる見込み（推定）。
- 一度取った区画は伸ばせない。

### 4.5 決める前に測ること（実機）

**DERBY・MEGADEMO・BIG WAVE で Shiki の native 側が使う量の最大は、まだ誰も記録していない。** まず確保を 1 か所に集めて数える（現在量・最大・最大の 1 個・確保の回数を、上の表の種類ごとに）。その最大で、予算の既定値と、区画にする利益（最大の連続ブロック `lg` とターン内の最小 `mn` がどれだけ戻るか）を判断する。計数は製品 image に残さない（既定 OFF のビルドの選択肢にする）。

## 5. 次の作業

0. **Shiki の native 側のメモリを実機で数える**（§4.5）。専用ヒープの予算と利益の判断材料。
1. **API の形の設計**（この文書の続き、または別文書）: 専用ヒープの予算の宣言（§4.3）、式の渡し方を 1 本にする（登録したら native が持ち、JS は捨てる。自作アプリも同じ経路）、データ領域（型付き配列を登録して式から読む）、列を 1 回の draw で回す呼び方、登録の待ち行列を土台へ、`shiki` の capability と `limits`、線と格子の API の揃え方、旧 API の互換と移行の順番。
2. **新しい命令**（表引き・除算・比較など）は 1 の後。命令の解釈部分と DRAM に効くので、PIE・命令キャッシュに触れる判断として STEADY 以上で扱う。
3. 設計と仕様は SLOW の独立レビューにかける（CLAUDE.md のルーティング）。
4. **改名の実施**は 1 が固まってから一度に行う。CLAUDE.md の規則どおり、コードが参照するファイル名・節番号・決定番号は保ち、移したら参照を直して `python tools/check_docs.py` を通す。旧名（procedural・plan・IR・VM）で書かれた既存の文書は書き換えず、この文書の §1.3 を対応表とする。

関連: [js-to-ir.md](js-to-ir.md)（JS → IR のコンパイラと `@plan`）、[flash-plan.md](flash-plan.md)（組み込みの式）、[grid-js-notation.md](grid-js-notation.md)（格子の式の記法）、[plan-sized-alloc.md](plan-sized-alloc.md)、[derby-background-cost.md](derby-background-cost.md)（費用モデル）、[oval-pie-design.md](oval-pie-design.md)（列のウォーカー P5）。
