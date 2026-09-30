# 評価のピーク: 何が食っていて、どう下げるか — 2026-09-30

DERBY WATCH（`apps/derby/derby_watch.js`、ソース 31,980 B、全体が1つの即時実行関数）の評価の余裕は 3.8〜5.6 KB しかない（`83fc908` の記録）。このピークの中身を分け、下げる手段（ソースの書き方、チャンク分割、ES モジュール、パーサの変更、事前コンパイル、遅延 import）を、実機と host の実測で比べた。数値には **実測（実機）／実測（host m32）／計算／推定** を付ける。host m32 は実機と同じ 8 B の JSValue・4 B のポインタで、TLSF の課金（4 B 丸め・最小 12 B）を真似たもの。道具は `tools/vmtest/compile_peak.py`（host）と `POCKET_HEAPPROBE` の再現アプリ（実機、[`spread-eval-oom.md`](spread-eval-oom.md) §3）。

## 1. 結論

| 手段 | 評価できる最小のヒープ上限（実測・実機） | 余裕（160 KiB − それ） | 評価の時間（実測・実機） | 重さ |
| --- | --- | --- | --- | --- |
| 現状（即時実行関数 1 つ） | 158,792 | **5,048** | 約 170 ms | — |
| 即時実行関数を外して 1 スクリプトに平らにする | 156,689 | 7,151（+2.1 KB） | 170 ms | 小 |
| 3 つのグローバルスクリプトに分けて順に評価（B 案） | 140,285 | 23,555（+18.5 KB） | 176 ms | 小（ネイティブ 1 関数） |
| 4 つに分ける（B 案） | 132,923 | 30,917（+25.9 KB） | 180 ms | 同上 |
| host で事前コンパイルしたバイトコードを読む | 132,809 | **31,031**（+26 KB） | **49 ms**（読み 12 ms + 実行 23 ms） | 中（ビルド手順） |
| パーサの余りを関数の終わりで返す（(i) の一部） | —（host のみ） | host のピーク −11.1 KB | 不変 | 小〜中（VM の外） |

- ピークの正体は**コンパイル**（解析の一時領域）で、実行ではない。host のコンパイルだけのピーク +118,700 B のうち、生き残るのは +55,300 B（実測 host）。
- **事前コンパイル**と **4 分割**が同じくらい効く（どちらも約 +26 KB）。事前コンパイルでは、ピークはもうトップレベルの実行（Kasane の場面や表を作る）に移っていて、それ以上はソースの形では下がらない。
- 分割と事前コンパイルは重ねられるが、重ねても効くのは実行側のピークだけ（§4.3）。

## 2. ピークの構成（実測・host m32、`compile_peak.py`）

`JS_Eval(..., JS_EVAL_FLAG_COMPILE_ONLY)` 1 回、確保ごとに呼び出し元のスタックを記録し、ピークの瞬間に生きていたバイトを種別に分けた。ピークは**解析が終わり、最初の子関数を確定し始めた瞬間**（最終の `JSFunctionBytecode` がまだ 1 個・124 B）。

| 種別 | バイト | 割合 | ブロック | 中身 |
| --- | ---: | ---: | ---: | --- |
| pass-1 バイトコード（`fd->byte_code` の DynBuf） | 54,300 | 46% | 51 | 使用 44,972 + 1.5 倍の伸長の余り 9,343（実測 host） |
| JSFunctionDef 本体 | 17,888 | 15% | 52 | 1 関数 344 B（計算: 17,888/52） |
| label slots | 11,240 | 10% | 33 | `LabelSlot` 20 B × ラベル数 + 余り |
| atoms | 10,876 | 9% | 291 | 識別子・プロパティ名・文字列定数。**大半は評価後も残る** |
| 文字列リテラルのトークン | 8,228 | 7% | 147 | 同上（定数として残る） |
| vars / args / vars hash | 10,560 | 9% | 73 | 変数表 |
| scopes / cpool | 4,096 | 3% | 48 | |
| 計（コンパイルが持つ分） | 117,328 | 100% | | 評価前の realm 26,300 B は別 |

pass-1 バイトコードの中身（実測 host、52 関数の合計 44,972 B）: `source_loc`（1 文・1 式ごとの位置、9 B）11,484 B = 26%、`scope_get_var`（未解決の変数参照、7 B）10,703 B = 24%、`push_i32` 4,685、`label` 2,435、`scope_put_var_init` 1,918、`scope_put_var` 1,050、その他。

**仮説の検証**: 「ネストの深さに関係なく、1 スクリプトの JSFunctionDef はすべて同時に載る」は正しい。`js_create_function`（quickjs.c 39879）は解析がスクリプト全体を読み終えてから呼ばれ、子を先にすべて確定して（39932「first create all the child functions」）、確定した子の JSFunctionDef から順に解放する。ネストした関数の一時領域も、トップレベルの関数宣言に平らにした関数の一時領域も、**同じスクリプトなら**解析の終わりまで全部残る。実機で、即時実行関数を外して平らにしても +2.1 KB しか増えず（即時実行関数のクロージャ変数の分）、スクリプトを分けると +18.5〜25.9 KB 増えた。

評価後に残るもの（実測・実機、DERBY WATCH の評価直後 `HEAPPROBE_MU`、評価前との差）: 使用 +85.5 KB。うち atom +19.0 KB（416 個）、関数 +12.6 KB と バイトコード本体 +12.3 KB と 行番号表 +2.4 KB、オブジェクト・形・プロパティ（アプリの状態と Kasane の参照）約 +20 KB。**atom は評価後もずっと残る**ので、長い識別子・プロパティ名・文字列定数は、評価中だけでなく定常の heap も食う。

## 3. 各案

### 3.1 (a) ソースの書き方（今すぐ・ネイティブ変更なし）

ソース 1 バイトあたり、コンパイルのピークは約 3.7 B（計算: 117,328 / 31,980、host）。ピークに効くのは「そのスクリプトの pass-1 バイトコードの総量」なので:

- 1 文ごとに `source_loc` 9 B と、変数参照ごとに 7 B がかかる。**短い文をたくさん書くより、式をまとめる**方が小さい（例: 別々の代入より 1 つの `const a = ..., b = ...`）。
- 関数 1 つで JSFunctionDef 344 B ＋ 各表の最小の割り当て（計 0.3〜0.5 KB、`83fc908` の記録と合う）。**小さなクロージャ（`arr.map(x => ...)` など）を減らしてループにする**のは効く（`83fc908` で 4 KB）。
- 長い関数は pass-1 バイトコードの DynBuf が 1.5 倍で伸びるので、余りが最大 1/3 つく。**大きな関数を 1 つ持つより、いくつかに分ける**方がピークの余りは小さい（`83fc908` の `frame_` 11,623 → 7,749 B）。
- 識別子・プロパティ名・文字列定数は atom になり、評価後も残る（§2）。
- 即時実行関数で包むのをやめても +2 KB しか変わらない。包み方ではなく、**スクリプトの数**が効く（§3.2）。
- 文字列の反復（スプレッド・for-of）は `f937388` 以降の image なら使ってよい（[`spread-eval-oom.md`](spread-eval-oom.md)）。

### 3.2 評価の分割（B 案: 同期のチャンク評価）— 実測

`tools/heapprobe_split.py` で、即時実行関数を外し、トップレベルの文の境目で機械的に N 個のグローバルスクリプトに切った（アプリは触らない。行番号は空行で保つ）。入口のスクリプトが `__hpLoad(k)`（診断のネイティブ関数。`JS_Eval` を埋め込みのチャンクに対して呼ぶだけ）を順に呼ぶ。トップレベルの `const`/`let` は realm のグローバルな字句環境に入り、後のチャンクから見える。

- 実機: 3 分割で余裕 +18.5 KB、4 分割で +25.9 KB（§1 の表）。各チャンクの読み込み（実測・実機）: 7〜11 KB のチャンクが 37〜48 ms。
- host の全シナリオ（`tools/games/test_derby_host.c` の写しで、チャンクを順に評価）: 2/3/4 分割のどれも、完走の行とピクセルのハッシュ（`0e5cb550b20d4001`）が分割なしと同一。
- 見込み（ユーザーとの議論）の「3 分割で約 39 KB」に対し、実測は 18.5 KB（実機）/ 25.3 KB（host）。差は、(1) 最後のチャンクのピークにはその実行（Kasane の場面づくり）の分が乗る、(2) 各チャンクの一時領域がソースの 1/3 より大きい（チャンクの切れ目は文の境目なので均等でない）、から。
- 注意: 関数の巻き上げはチャンクをまたがない。チャンク k のトップレベルの文が、後のチャンクで宣言された関数を**評価の時点で**呼ぶと ReferenceError（DERBY WATCH では起きなかった。関数の中から呼ぶのは実行時なので問題ない）。

### 3.3 ES モジュール（A 案）— host の実測とコード読み

- ピーク: 同じ 33 KB の合成コードで、1 スクリプト 253,006 B、3 スクリプト（B 案）161,913 B、静的 `import` の 3 モジュール 168,017 B、動的 `import()` の連鎖 180,291 B（実測 host m32 の vmrun、確保ヘッダ 48 B 込みなので比だけを見る）。モジュールも 1 つずつ解析・確定されるので、ピークは B 案と同じく下がる。定常はモジュールの記録と名前空間の分だけ B 案より +7 KB。
- **手放せない**: 解決したモジュールは `ctx->loaded_modules` に残り、`m->func_obj`（モジュール本体のバイトコードと、その定数表にある全関数）は realm を壊すまで解放されない（quickjs.c 3114 `js_free_modules` は `JS_FREE_MODULE_ALL` か未解決のものだけ）。B 案のグローバルスクリプトは、トップレベルの関数が評価後に解放され、残るのはクロージャが指す関数だけ（参照を捨てれば GC で外れる）。
- **状態の共有**: import した束縛は読み取り専用。DERBY WATCH のように複数の部分が同じ `let pts` を書き換える作りは、モジュールに**機械的には**分けられない（状態をオブジェクトに移す書き換えが要る）。B 案はそのまま動いた。
- ネイティブの変更: ゲストに `JS_SetModuleLoaderFunc2` と、埋め込みの表から `JS_Eval(COMPILE_ONLY|MODULE)`（または事前コンパイルなら `JS_ReadObject`）で `JSModuleDef` を返すローダを置く。入口をモジュールとして評価するか、スクリプトから動的 `import()` を使う。動的 `import()` の解決はジョブで、同じターンの `vm_sched_drain` で進む（フレームの後）。ローダの中の解析はネイティブ呼び出しの中なので park できない（vm-L2-design §11.2）。トップレベル await のあるモジュールの評価が park を通れるかは**未確認**。

### 3.4 パーサの変更（(i)(ii)(iii)）— host の実験と見積もり

- **(i) バッファの見直し**
  - 関数の解析が終わった時点で、その関数の `byte_code`・`label_slots`・`vars`・`cpool` を実際の長さに縮める（`js_parse_function_decl2` の `done:` に 4 つの realloc）。**実測 host: ピーク +118,700 → +107,600（−11.1 KB、−9.4%）**。バイトコードは不変（`JS_WriteObject` の出力が、アプリ・コーパス 144 ファイルでバイト一致。4 ファイルは両方とも構文で落ちるもの）。実験は scratch のエンジンの写しだけで、リポジトリの quickjs.c は変えていない。
  - `source_loc` を 9 B から差分の可変長（平均 3 B 程度）にする: −7.6 KB（計算: 1,276 個 × 6 B）。pass-1 の内部形式だけの変更で、最終のバイトコードは変わらない見込み（未実験）。`resolve_labels` の読み取り側も直す必要がある。
  - JSFunctionDef 本体（344 B × 52 = 17.9 KB）の縮小は、構造体のフィールドの並べ替え・内蔵配列（`def_scope_array[4]` など）の見直しで数 KB（推定）。
  - 合計で −15〜20 KB（推定）。VM の段（L1〜L3）とは独立で、影響は解析だけ。
- **(ii) 安全な子を早く確定する**: 子関数の自由変数の解決（`resolve_variables`）は、親の変数表が完成していることを前提にしている。親の後ろで宣言される `let`/`var`/関数（巻き上げ）を、先に書かれた子が参照するのは正しい JS なので、「親の解析が終わる前」には、どの子が安全かを決められない。やるなら、楽観的に確定して、後から親のスコープに同名の宣言が来たら**スクリプト全体を早期確定なしで解析し直す**方式（元に戻す仕組み）が要る。`arguments`・`this`・`eval`・`with`・クラスの home object などの例外も多い。効果は、最大で「確定済みの子の一時領域がすべて消える」＝ピークがほぼ「残る量＋最大の関数 1 つの一時領域」まで（推定 −40〜60 KB）。**重い・危険**。
- **(iii) 遅延コンパイル**（本体の解析を最初の呼び出しまで延ばす）: 前処理の解析器（自由変数と宣言の追跡）と、ソースを保持する仕組み（今は `CONFIG_POCKET_VM_STRIP_FN_SOURCE` でソースを捨てている）が要る。V8 の preparser 相当で、quickjs.c の数千行規模（推定）。**最も重い**。遅延 import（§5）で、アプリ側から同じ効果の大部分が得られる。

### 3.5 事前コンパイル（案 b）— 実機で動いた

- host（WSL、`-m32`、同じ quickjs.c）で `JS_WriteObject(JS_WRITE_OBJ_BYTECODE)` した DERBY WATCH（29,397 B、行番号表込み）を埋め込み、実機で `JS_ReadObject` + `JS_EvalFunction` → **正常に起動**（`READY`・`LOADED`・`ODDS` の行がソース版と同じ）。VM の改造（L1〜L3、フラット呼び出し、セグメント、`STRIP_FN_SOURCE`）はバイトコードの形式に影響していない（`BC_VERSION 25`、組み込み atom は同じ `quickjs-atom.h` なので番号が一致）。
- 実測・実機: 読み込み 12.2 ms、トップレベルの実行 22.6 ms、評価の合計 49 ms（ソースは約 170 ms）。読み込みで +60.2 KB（35,376 → 95,572）、実行後 124,492、落ち着いて 119,440（ソース版 119,692 とほぼ同じ定常）。最小のヒープ上限 132,809（余裕 +26 KB）。host でも読み込みのピークは残る量＋1.7 KB（atom の索引）。
- **byte_code_buf を flash に置いたままにはできない（今の形式では）**: `JS_ReadFunctionBytecode`（quickjs.c 42606）が、読み込み時にバイトコードの中の atom の番号を、その実行時の atom に**書き換える**（`put_u32(bc_buf + pos + 1, atom)`）。flash に置くには、(1) atom のオペランドを関数ごとの表を経由して読む（インタプリタの全 atom 命令の変更）、または (2) ユーザーの atom を起動時に決まった順で登録して番号を固定する、のどちらか。インタプリタが実行中にバイトコードを書き換える箇所は、quickjs.c の `pc` への書き込みを探した範囲では見つからなかった（インラインキャッシュ無し）。効果は定常の heap −12.3 KB（バイトコード本体）＋ 行番号表 2.4 KB（実測・実機の `code=12,321 pc2line=2,438`）。F1/F2（ROM atom、[`builtin-floor-plan.md`](builtin-floor-plan.md)）と同じ発想で、**中〜重**。
- 残る課題: (1) ビルドの手順（ESP-IDF のビルドの中で host の m32 の quickjs を作ってバイトコードを出す。今のスパイクは WSL で手で作ったファイルを埋め込む）、(2) quickjs.c を変えたらバイトコードも作り直す（`BC_VERSION` だけでは VM の改造の差を検出できないので、ソースのハッシュを埋め込む）、(3) Playground など実行時にソースを受け取るアプリのためにパーサは残る（両方の経路）、(4) エラーの位置（行番号表を残すか、`JS_WRITE_OBJ_STRIP_DEBUG` で捨てるか）。

## 4. 実機の測り方と注意

### 4.1 再現アプリの変種（`apps/heapprobe/heapprobe.js`）

`derby`（出荷版）、`derby-flat`、`derby-split3`、`derby-split4`、`derby-bc`、`lazy-frame-src`、`lazy-frame-bc`。`derby-bc` は、ビルドの前に WSL で `python3 tools/vmtest/compile_peak.py apps/derby/derby_watch.js --write .cache/compile_peak/derby.bc` を実行したときだけ入る。

### 4.2 最小のヒープ上限の二分探索

`python tools\heapprobe_device.py --port COM3 --bisect 29 --lo 110000 --hi 163840 --step 256`。上限を下げると GC の閾値（上限の半分）も下がるので、「その上限で評価が通るか」を測っている（運用上の余裕そのもの）。実機のピークは TLSF の空きブロックの形にも左右されるので、±数百 B は揺れる。

### 4.3 事前コンパイルと分割を重ねると

事前コンパイルでは、残る量（約 60 KB）を読んだ後のトップレベルの実行が +37 KB（一時 +13 KB を含む）を積んでピークを決める。分割しても実行の総量は変わらないので、重ねて効くのは「後のチャンクの実行を遅らせる」こと、つまり §5 の遅延 import だけ。

## 5. 遅延 import（独立の成果物）

QuickJS には遅延コンパイルが無いので、**使わない部分を読まない**ことが、このエンジンで使える唯一の遅延の仕組み。

### 5.1 実測・実機（`lazy-frame-src` / `lazy-frame-bc`）

| 読み込み | frame() の中の時間 | heap | 失敗時 |
| --- | --- | --- | --- |
| ソースのチャンク 10.7 KB（解析＋実行） | 45 ms（2 回とも 44.8/45.3 ms） | +30.1 KB | — |
| 事前コンパイルした DERBY WATCH 全体（29 KB） | 35 ms（読み 12 + 実行 22） | +88.5 KB | 上限 80,000 で読み込みが OOM → `InternalError: out of memory` が **try/catch で捕まり、アプリは続行**（frame 40 で生存を確認） |
| ソースの DERBY WATCH 全体（参考、評価中の値） | 約 170 ms | — | — |

30 fps（33 ms/フレーム）では、10 KB のソースで 1〜2 フレーム、事前コンパイルした 29 KB で 1 フレームの引っかかり。ソース 30 KB を frame() の中で読むと約 5 フレーム。

### 5.2 期限との関係（コードで確認）

- 評価の 2 秒の期限（`app_start_test` の `deadline=+2 s`）は起動の評価だけ。frame() の中の読み込みには、**そのターンの 250 ms の期限**（`app_session.c` 629 行 `deadline=now+250000`）と `VM_FRAME_RUNAWAY_US`（250 ms、積算）がかかる。
- **解析そのものは割り込みを見ない**（割り込みは実行中のセーフポイントでだけ読まれる）。30 KB の解析（約 145 ms）は期限の内側だが、それより大きなものは、解析が終わって実行に入った瞬間に `interrupted` になりうる。
- 8 ms のターン予算: 読み込みはネイティブ呼び出しの中なので yield も park もできない（vm-L2-design §11.2）。読み込みのターンは予算を超えるが、それは runaway の監視（250 ms）の内側で許される。

### 5.3 A と B の比較

| 観点 | A: 動的 `import()` | B: 同期の `pocket.app.load('name')` |
| --- | --- | --- |
| 書き方 | 標準の JS。`import('./demo.js').then(m => ...)` | 独自 API。`load('demo')` の後はグローバルで見える |
| 読み込みの時点 | ジョブ（frame() の後、同じターンの drain） | 呼んだその場（frame() の中、同期） |
| 読み込み中の frame() | Promise が解決するまで「読み込み中」の表示を自分で出す | 呼んだフレームが止まる（1〜5 フレーム） |
| 失敗 | Promise の reject（OOM・見つからない・構文エラー）。アプリは catch して機能を諦められる（推定。B で OOM が例外に閉じることは実測） | 例外。try/catch で諦められる（実測） |
| 手放し | できない（`loaded_modules` に残る） | 参照を捨てれば GC で外れる（トップレベル関数は評価後に解放） |
| 状態の共有 | import は読み取り専用。可変の状態はオブジェクトに | グローバルの `let` をそのまま共有 |
| 静的 import との統一 | 同じ仕組み（ローダ）で静的 import も書ける | 静的 import は別に要る |
| ネイティブの変更 | ローダの登録、埋め込みの表、入口をモジュールに、ジョブ・park の検査 | `pocket.app` に 1 関数、埋め込みの表 |
| host の検証への影響 | `test_derby_host.c` などにもローダが要る | 同じ関数を host の写しに足すだけ |
| Playground | ソースを受け取るアプリは import できる先が無い（埋め込みだけ） | 同じ |
| 事前コンパイルとの併用 | ローダが `JS_ReadObject` でモジュールを返せばよい | `load` が `JS_ReadObject` + `JS_EvalFunction` |

### 5.4 DERBY WATCH での現実的な単位（推定）

節の大きさ（ソース）: 手続き描画のプログラム 8.0 KB、場面 11.9 KB、カメラ 4.5 KB、デモ 1.9 KB、音 0.8 KB、状態 1.0 KB、先頭のレースの模型 約 3.8 KB。デモ（1.9 KB）を遅らせても、ピークは約 7 KB（計算: 1.9 KB × 3.7）、定常は約 2〜3 KB しか減らない。効くのは、場面ごとに別れる大きな塊（HEAD ON・写真判定・大型画面の演出）を場面の切れ目で読む形だが、今の DERBY WATCH は場面の処理が `frame_` と `paint` に混ざっていて、**機械的には切り出せない**（書き直しが要る）。

### 5.5 読み込みを場面の切れ目で隠す

- 事前コンパイルなら 35 ms（29 KB 全体でも 1 フレーム）なので、ゲートに入る黒い間やディゾルブの 1 コマに収まる。
- ソースのままなら 10 KB で 45 ms（1〜2 フレーム）。デモ移行のディゾルブ（別のエージェントが作業中）の 1 コマ目で読めば、止まったフレームが演出の一部に見える（推定。実機で見て判断するのはユーザー）。

## 6. 推奨の順序（決めるのはユーザー）

1. **いま**: (a) の書き方を続ける（DERBY WATCH の余裕は 5 KB）。`f937388`（文字列の反復の修正）を `vm/main` に入れる。
2. **次に小さく効くもの**: B 案の同期のチャンク評価（`pocket.app.load`）。ネイティブは 1 関数と埋め込みの表で、DERBY WATCH は機械的に分けるだけで +18〜26 KB。手放しもでき、遅延 import の土台にもなる。
3. **その次**: 事前コンパイル（案 b）。起動の時間が 170 → 49 ms、余裕 +26 KB。ビルドの手順と、quickjs.c を変えたときの作り直しの仕組みが要る。B 案の `load` が `JS_ReadObject` を使えば、両方を同じ API で出せる。
4. **パーサの (i)**: 関数の終わりで余りを返す（host −11 KB、バイトコード不変を確認済み）と `source_loc` の圧縮。VM の段と独立で、全アプリに効く。上流との差分が増える。
5. **A 案（ES モジュール）**: 「普通の JS に近い書き方」を最優先するなら。ただし手放せない・状態をオブジェクトに移す必要がある・ジョブ／park との検査が要る。
6. (ii)(iii) と flash 常駐のバイトコードは、上の効果を見てから。

ユーザーが決めること: (1) 静的 `import` の書き方をどこまで優先するか（A を採るか、B で足りるか）、(2) 事前コンパイルをビルドに入れるか（host の m32 ツールチェーンをビルドの前提にするか）、(3) パーサの (i) を上流との差分として持つか、(4) DERBY WATCH を場面ごとの遅延の単位に書き直すか。

## 7. 製品化: `pocket.app.load`（B 案、2026-09-30）

§3.2 の試作（`__hpLoad(k)`、`POCKET_HEAPPROBE` の中）を、通常の機能にした。仕様は [共通 API](../api/common-api.md) §5.1。ここには、作りの理由・ビルドの手順・アプリ作者向けの指針・検証の結果を置く。

### 7.1 作り

| 部品 | 場所 | 役割 |
| --- | --- | --- |
| 面 | `main/pocket/pocket_app_load.c` | `pocket.app.load`。`pocket_app_install()` の中から `pocket_app_load_install()` で入る（`pocket_workspace.c` と同じく `pocket.app` に lazy で足す）。capability `app.load` を `pocket_api_register()` で登録。`pocket_api.c` と `app_session.c` は変えていない |
| 表の型 | `main/pocket/app_chunks.h` | アプリ ID → {名前, ファイル名, 開始, 終了}。ESP-IDF にも QuickJS にも依存しない |
| 表の生成 | `tools/make_app_chunks.py` | `apps/*/chunks.txt` から `build/generated/app_chunks.c` と、埋め込むファイルの一覧を作る（configure のとき） |
| host の表 | `tools/hostshim/app_chunks_host.c` | 同じ `chunks.txt` をファイルから読む（§7.4） |

決めたこと（根拠）:

- **表はセッション開始時のアプリ ID で引く**（`app_registry_current()` を install で1回読む）。眠っているアプリ（常駐中断）の realm が、あとで選ばれた別のアプリのチャンクに届かないように。オーバーレイは自分の ID を選んでから始まる（`ui/overlay.c`）。
- **解析と実行を分ける**（`JS_Eval(COMPILE_ONLY)` → `JS_EvalFunction`）。解析の失敗は何も宣言していないので、もう一度頼める（heap 不足なら `retryable=true`）。実行の失敗は、グローバルの `let` が永久に TDZ のまま残るなど、途中までの宣言が残りうるので、その名前は二度と実行しない（`CORRUPT_DATA`、`outcome=unknown`）。
- **冪等**（読み込み済みは `false`）。再評価はグローバルの `let`/`const` の再宣言の SyntaxError になり、アプリが書いていない行を指すため。
- **読み込み中の自分自身は `CONFLICT`**。循環（A が B を、B が A を読む）を黙って `false` にすると、宣言が半分無いまま先へ進む。
- **期限切れ（捕まえられない例外）は PocketError にしない**。変換すると try/catch で捕まり、アプリを止めるための期限を越えて走り続ける。
- **heap 不足の判定は例外から**（`InternalError: out of memory` か、裸の `null`）。OOM の canary（`JS_TakeOOMCanary`）はターンの終わりに `report_oom_if_any()` が読むもので、ここで読むとその `OOM` 行を奪う。チャンクが自分で `throw null` すると OUT_OF_MEMORY と誤るのが代償。
- 失敗の `message` は `String(e)` と、元の例外のスタックの1行目（`eval_reporting()` の `EVAL_ERROR` と同じ2つ）。捕まえずに `frame()` から出れば `FRAME_WRAP` の `__pjs_error` がそのまま表示し、評価中なら `EVAL_ERROR` の行に出る（書式は変えていない）。
- ログ: 成功すると `APP_LOAD <名前> bytes=… compile_us=… run_us=… used=<前>><解析後>><実行後>`、失敗すると `APP_LOAD <名前> failed …`。契約のマーカーではない（診断ビルドなしで測れるように置いた）。
- 費用（実測・map、`esp_idf_size --diff`、基準 `97553cd`）: DIRAM **+16 B**（`.bss`: 表のポインタと3つのマスク）、flash +2,184 B（コード +1,720、rodata +464）。capability の表は 32 枠のうち、前面アプリで 30（ペットのアプリ、計算）、lazy の表は 28 枠のうち 26（計算）。

### 7.2 ビルドの手順（アプリにチャンクを持たせる）

アプリのディレクトリに `chunks.txt` を置くだけ。`main/CMakeLists.txt` は `apps/*/chunks.txt` を glob するので、**CMake・`main.c`・`shell.c` の編集は要らない**。

```text
# apps/derby/chunks.txt
app local.derby            ← app_registry.c のアプリ ID
scene  derby_scene.js      ← load() に渡す名前（1〜31 バイトの [A-Za-z0-9_.-]）と、このファイルからの相対パス
demo   derby_demo.js
```

- 入口のソースは従来どおり `EMBED_TXTFILES` と `begin_run(...)`。入口はチャンクに数えない。
- 埋め込みのシンボルはファイル名から作られる（`_binary_derby_scene_js_start`）。**ファイル名は全アプリのチャンクと `EMBED_TXTFILES` で重複させない**（アプリ名の接頭辞を付ける）。チャンク同士の重複は `make_app_chunks.py` が configure で断り、`EMBED_TXTFILES` との重複は ESP-IDF の `build/<ファイル名>.S` の衝突で configure が止まる。
- 表はシンボルを持ち、長さを持たない。**チャンクの中身の編集は再 configure 不要**。チャンクの追加・改名・削除は `chunks.txt` の変更で、configure の依存に入っている。
- 断るもの（configure で止まる）: 名前の形式、同じアプリで同じ名前、1 アプリ 33 個以上、ファイルが無い、ファイル名の衝突。
- 診断用の表: `POCKET_HEAPPROBE=ON` のときだけ、`POCKET_APP_CHUNK_LISTS` に一覧を足す（DERBY WATCH の 3/4 分割が `local.derby` の `s3c1`…`s4c4`、`apps/heapprobe/appload_chunks.txt` が `local.hello` の試験用）。通常の image の表は空（map で確認: `appload_`・`hp3_c` の記号 0 件）。

### 7.3 アプリ作者向けの指針

**分け方**

- トップレベルの文の境目で切る（`tools/heapprobe_split.py` がその機械的な例）。各チャンクの先頭に `'use strict';`（スクリプト単位）。
- **関数の巻き上げはチャンクをまたがない**。チャンク k のトップレベルの文が、後のチャンクの関数を**評価の時点で**呼ぶと ReferenceError。関数の中から呼ぶ（実行時）のは問題ない。読む順は、定義の順。
- 効くのは「1 チャンクの大きさ」: ピークは最大のチャンクの解析の一時領域（ソース 1 B あたり約 3.7 B、§3.1）＋それまでに残ったもの。3〜4 個に均等に分けるのが目安（§1: 3 分割 +18.5 KB、4 分割 +25.9 KB、実測・実機）。

**入口**

- 入口は `pocket.app.load('a'); pocket.app.load('b'); …` だけでもよい。ただし **入口のソースに `pocket.kasane` という文字列を入れる**（コメントでよい）。Kasane の arena（約 9.9 KiB）を評価の前に一続きで確保する判定（`app_session.c` の `names_kasane`）は、入口のソースしか見ない。
- `pocket.app.start()` は評価中にしか呼べない（§5）。`globalThis.frame` は評価の終わりに1度だけ読まれる（`bind-frame.js`）。**どちらも、評価中に読むチャンク（か入口）に置く**。`frame()` の中で後から読むチャンクで `globalThis.frame` を置き換えても、ホストは古い関数を呼び続ける。

**共有の書き方**

- 入口と全チャンクで、グローバルの名前空間は1つ。グローバルの `let` をそのまま共有してよい（同じ束縛。DERBY WATCH の `let pts` の形がそのまま動く）。
- 名前の衝突（同じ `const`/`let` を2つのチャンクで宣言）は、後のチャンクの**実行の最初**の SyntaxError（`redeclaration of 'x'`、位置はそのチャンクの 1 行目。`CORRUPT_DATA`、`unknown`）。後のチャンクは1文も実行されず、先のチャンクの束縛はそのまま（host で確認）。短い接頭辞を付ける。
- 評価の余裕を詰めたいアプリで、識別子を長くしない（atom は評価後も残る、§2）。

**いつ・どこで読むか**

- 評価のピークを下げるのが目的なら、**評価中に全部読む**のが簡単で十分（評価の時間は分割しても変わらない: 170 → 176 ms、実測・実機）。2 秒の期限は、入口の評価全体（中の `load` を含む）にかかる。
- 遅らせて読む（使わない場面を読まない）なら、**場面の切れ目・演出の間**に1つずつ。`frame()` の中の読み込みはそのフレームを止める: 10.7 KB で約 45 ms（1〜2 フレーム）、30 KB で約 150 ms（実測・実機、§5.1）。ターンの期限 250 ms の内側だが、**1 チャンク 15 KB 程度まで**を目安にする（推定: 解析は割り込みを見ないので、期限は解析が終わった後の最初のセーフポイントで効き、捕まえられない）。同じフレームで2つ読まない。
- 失敗は try/catch で捕まえて、その機能を諦められる（実機で確認、§7.5）。`OUT_OF_MEMORY` で `retryable=true` なら、何かを手放してから後でもう一度頼める。**トップレベルは宣言だけにして、重い確保（Kasane の場面や大きな表）は関数に入れて後で呼ぶ**。そうすれば、トップレベルの実行中の失敗（再試行できない）が起きにくい。

**手放し方（アンロード）**

- 手放す API は無い。チャンクが作った関数は、**参照を捨てれば GC で解放される**。トップレベルの関数そのもの（スクリプト本体）は評価の直後に解放され、残るのはクロージャが指す関数だけ（§3.3）。
- ただし、トップレベルの関数宣言と `var` はグローバルオブジェクトの設定変更不可のプロパティ、`let` は束縛なので、消せない。値を `null` にする。`const` は捨てられない。**手放したいまとまりは、1つの `var`（か `let`）のオブジェクトの下に置く**（`var demo = { run() {…}, … }` → 使い終わったら `demo = null`）。
- 実測（host 64 bit、`tools/test_app_load.c`）: 48 個のメソッドを1つの `var` に持つ 5,023 B のチャンクが、保持 26,630 B、`alMid = null` と GC で 25,543 B（96%）戻った。残りは atom とプロパティの枠。実機の値は未測定（JSValue が 8 B なので host より小さい、推定）。
- 手放した後も `load(name)` は `false`（冪等）で、**もう一度評価はしない**。同じ場面を何度も読み書きする使い方は、今の API ではできない（§8 の課題）。

### 7.4 host の検証の土台（ゲームのハーネスから使う）

アプリを実物の QuickJS で評価している host のハーネス（`tools/games/test_*_host.c`、`tools/kasane_contract/` の試験）が `pocket.app.load` を使うアプリを動かすための部品。ハーネスそのものはこの作業では変えていない。

1. リンクに足す: `main/pocket/pocket_app_load.c`、`main/pocket/app_registry.c`、`tools/hostshim/app_chunks_host.c`（インクルードに `-Imain/text`、`-Itools/hostshim`）。`pocket_api.c` か `tools/hostshim/pocket_api_stub.c` のどちらかは、ハーネスがすでに持っている。
2. `main()` で、realm を作る前に `app_chunks_host_read("apps/derby/chunks.txt")`（ファームと同じ一覧。パスはリポジトリの根から）、realm ごとに `app_registry_select("local.derby")` と `pocket_app_load_install(ctx)`。終了時に `app_chunks_host_clear()`（ASan の漏れ検査のため）。
3. `pocket_api_stub.c` の `pocket_api_lazy()` は名前空間を即座に `globalThis.app` に作る。ゲームのハーネスは `globalThis.pocket` を JS の前置きで組み立てているので、その前置きに `app: globalThis.app` を足す（`pocket_api.c` をリンクするハーネスなら、`pocket.app` は本物と同じく lazy に作られる）。
4. スタックとエラーの `message` のファイル名は、チャンクのファイル名（`derby_scene.js:12`）。ハーネスの 2 秒の割り込みは、評価中の `load` にもそのままかかる。

試験: `tools/build_app_load_test.sh`（WSL、ASan/UBSan）。本物の `pocket_app_load.c`・`pocket_api.c`・`app_registry.c` で、32 項目（capability と lazy、評価中と `frame()` からの読み込み、冪等、`let`/`const`/`var`/関数の共有、NOT_FOUND・INVALID_ARGUMENT・CORRUPT_DATA（解析／実行）・CONFLICT・OUT_OF_MEMORY からの再試行、期限の例外の素通し、チャンクの無いアプリ、3 セッション、解放の前後の heap）。

### 7.5 実機の結果（2026-09-30、COM3、`POCKET_HEAPPROBE=ON` の image）

`apps/heapprobe/heapprobe.js` の変種を `tools/heapprobe_device.py` で（索引は 41〜46）。

| 変種 | 結果（実測・実機） |
| --- | --- |
| `appload-api`（評価中） | capability `{"maxChunks":32,"maxNameBytes":31}`、`ok`=true・2 回目 false、共有（`alF()+alV+alG+alC.n+alS`）=31、NOT_FOUND・INVALID_ARGUMENT・CORRUPT_DATA（解析: not-applied／実行: unknown、2 回目は not-applied）・自分自身の読み込み CONFLICT、どれも host と同じ |
| `appload-api`（frame 10 の中） | `mid`（5.0 KB）53 ms（解析 50 ms）、`big`（DERBY WATCH の 1/3、10.7 KB）50 ms（解析 40 ms・実行 7 ms）。アプリは frame 40 まで生存 |
| `appload-oom`（上限 80,000 と 90,000、20 KB の詰め物） | 1 回目 `OUT_OF_MEMORY`／retryable=true／not-applied を catch → 詰め物を捨てて 2 回目は読み込み成功、frame 40 まで生存 |
| `appload-uncaught`（frame の中で捕まえない） | `W js: PocketError: chunk 'bad' did not compile: SyntaxError: variable name expected at appload_bad.js:3:1` でアプリが止まり、ホームへ戻る |
| `appload-evalerr`（評価中に捕まえない） | `EVAL_ERROR PocketError: chunk 'thr' threw while it ran: TypeError: … (appload_throw.js:4:1)`、`START_FAILED`。書式は従来どおり |
| `derby-load3` / `derby-load4` | DERBY WATCH の 3/4 分割を `pocket.app.load('s3c1')…` で。`READY`・`LOADED`・`ODDS`・`SAVE` の行が分割なしと同じ |

評価できる最小のヒープ上限（二分探索、256 B 刻み、同じ image）:

| 変種 | 最小の上限 | 余裕（163,840 − それ） |
| --- | ---: | ---: |
| `derby`（分割なし、この branch の DERBY WATCH） | 159,947 | **3,893** |
| `derby-load3`（`pocket.app.load` で 3 分割） | 141,756 | **22,084**（+18.2 KB） |
| `derby-load4`（4 分割） | 135,755 | **28,085**（+24.2 KB） |
| `derby-split3`（試作の `__hpLoad`、参考） | 140,721 | 23,119 |

`load3` と試作 `split3` の差 +1,035 B は、`pocket.app` の名前空間を作る分（DERBY WATCH は `pocket.app` を読まないので、試作では作られなかった）。3 分割の各チャンクの解析は 40〜50 ms（実測・実機）で、評価全体の時間は分割なしと同程度。

満杯まで詰めた heap（`new Uint8Array(512)` を割り当てが断られるまで）で `load` を呼ぶ試しでは、アプリは `load` と無関係な所（詰め物のループの直後）でも裸の `null` を投げて止まった。heap を最後の 1 B まで使う状態は、`load` に限らず JS のどこでも回復できない（エラーのオブジェクトすら作れない）。上の `appload-oom` のように、上限の手前で失敗させるのが意味のある試験。

## 8. 次: 静的 `import`（A 案）への引き継ぎ（設計メモ。実装は §9）

B 案の実装で分かったことと、A 案で決めること。

- **ローダの置き場所**: ゲストのランタイムにはモジュールローダが登録されていない（`quickjs-libc.c` の `JS_SetModuleLoaderFunc2` はワーカーの中だけ。`pocketjs_guest` の `guest.c` は `js_std_init_handlers` だけ）。A 案のローダは、`pocket_app_load.c` と同じ表（`app_chunks_for(アプリID)`）を引けばよく、モジュール名とチャンク名を対応させる（`import './scene.js'` → 名前 `scene`、あるいは表にファイル名で引く道を足す）。登録は `pocket_app_load_install()` の中で `JS_SetModuleLoaderFunc2(JS_GetRuntime(ctx), normalize, loader, NULL, NULL)` とすれば、`app_session.c` を触らずに済む（ランタイムに1つのスロットで、今は誰も使っていない）。ビルドの手順（`chunks.txt`）と host の部品（`app_chunks_host.c`）はそのまま使える。
- **入口をモジュールとして評価するときの `eval_reporting()` の変更点**: (1) `JS_EVAL_TYPE_GLOBAL` 固定を、入口がモジュールのとき（マニフェストのフラグか、`import`/`export` の有無）`JS_EVAL_TYPE_MODULE` にする。(2) quickjs-ng のモジュールの評価は Promise を返す（トップレベル await 対応）。拒否を `JS_PromiseState`/`JS_PromiseResult` で読んで、今と同じ `EVAL_ERROR` の行に写す（書式は契約なので変えない）。未決のまま返ったら、ジョブを回して決着させる（2 秒の期限の内側で）。(3) モジュールのトップレベルの宣言はグローバルに出ない。`bind-frame.js` の `globalThis.frame` の読み直しと `FRAME_WRAP` は、入口が `globalThis.frame = …` と明示的に書くことが前提になる（モジュールの `function frame` は見えない）。(4) `names_kasane`・`uses_legacy_ui` が入口のソースしか見ないのは B 案と同じ（§7.3）。(5) vm-L2-design §11.2: モジュール本体の最初の同期区間は止まらない（yield しない）。トップレベル await の後は普通の async の床として止まる。ローダの中の解析はネイティブ呼び出しの中なので、止まれない。
- **`pocket.app.load` との共存**: 表を共有すると、同じチャンクをグローバルのスクリプト（`load`）とモジュール（`import`）の両方で評価できてしまう（2 回、別物として）。`import`/`export` を含むチャンクを `load` すると解析で SyntaxError（`CORRUPT_DATA`）になるので、そこで区別はつくが、表にチャンクの種別（script / module）を持たせて、合わない読み方を `load` とローダの両方で断るのがよい。モジュールは手放せない（§3.3、`loaded_modules` に残る）ので、**起動時の分割は `import`、遅延の読み込みと手放しは `load`** という分担が自然。
- **状態の共有**: import した束縛は読み取り専用（§3.3）。DERBY WATCH のように複数の部分が同じ `let` を書き換える作りは、B 案ではそのまま動いたが、A 案では状態をオブジェクトに移す書き換えが要る。
- **事前コンパイル（§3.5）との合流**: `load` の `JS_Eval(COMPILE_ONLY)` を `JS_ReadObject` に替えれば、同じ API のまま事前コンパイルを出せる（表に「ソースかバイトコードか」を持たせる）。モジュールのローダも、`JS_ReadObject` で `JSModuleDef` を返せばよい。
- B 案の費用で A 案にも効くもの: `pocket.app` の名前空間を作る分（実測・実機: `pocket.app` を読まない試作の `__hpLoad` との差 +1,035 B、§7.5）。入口の評価中に読む API を、`pocket` の既存の名前空間に置くかぎり、その名前空間の構築の分はかかる。A 案の `import` は名前空間を作らない。

## 9. 静的 `import`（A 案、2026-09-30、実装）

§8 の設計メモを実装した。仕様は [共通 API](../api/common-api.md) §5.2。ここには、決めたことの理由・作者向けの使い分け・測定を置く。

### 9.1 作り

| 部品 | 場所 | 役割 |
| --- | --- | --- |
| ローダ | `main/pocket/pocket_app_load.c` | `module_normalize` と `module_load`。`pocket_app_load_install()` が `JS_SetModuleLoaderFunc2` で毎セッション登録する（ランタイムに 1 つのスロットで、他に使う者はいない） |
| 入口の評価 | 同 `pocket_app_eval_module()` | `JS_Eval(MODULE\|COMPILE_ONLY)`（この中で静的 import がすべて解決・解析される）→ `JS_EvalFunction` → 返る Promise の状態を読む |
| 呼び出し | `main/app_session.c` の `eval_reporting()` | `module` の引数を足しただけ。失敗は従来と同じ `EVAL_ERROR` の行（書式は不変） |
| 種別 | `app_chunks.h` の `module`、`make_app_chunks.py`、`app_chunks_host.c` | ファイル名が `.mjs` ならモジュール。`chunks.txt` の書式は変えていない |

決めたこと（根拠）:

- **入口がモジュールかどうかは、マニフェストの `entry` が `.mjs` で終わるか**（`pocket_app_entry_is_module()`）。ソースから推測しない: quickjs-ng の `JS_DetectModule` は新しいランタイムを作って全体を解析する（この機体では払えない）。字句の目視は、文字列・コメント・正規表現で誤る。`.mjs` は他の JS ホストと同じ約束で、`chunks.txt` のチャンクの種別も同じ規則にした。既存のアプリ（`.js`・Playground・チュートリアル）は一切変わらない。
- **指定子はチャンクの名前だけ**（`import … from 'scene'`）。正規化した名前はチャンクの**ファイル名**（`derby_scene.mjs`）にする。QuickJS は `loaded_modules` を、そのモジュールを解析したときの名前で引くので、ファイル名にしておけば、スタックの `file:line` と引く名前が同じ文字列になる。相対・絶対パス・URL は、名前の文字集合（`/` を含まない）で自動的に断られる。
- **`.js` のチャンクは `load` 専用、`.mjs` は `import` 専用**（§8 の「合わない読み方を両方で断る」）。`load('mjs のチャンク')` は `INVALID_ARGUMENT`（評価しない）、`.js` を import すると `ReferenceError`。
- **解決は入口の解析の間だけ開く**（`resolving` の旗）。QuickJS は静的 import を、そのモジュールの `JS_Eval(COMPILE_ONLY)` の中で解決する（`__JS_EvalInternal` の `js_resolve_module`）。外で呼ばれる正規化は動的 `import()` だけなので、それを `TypeError: import('x') in y: dynamic import() is not supported; use pocket.app.load()` で断る。ジョブの中で解析してモジュールを増やす道（手放せない）を塞ぎ、後から読む道は `load` に一本化する。解決済みのモジュールへの `import()` も、正規化が先に呼ばれるので同じく断られる。
- **トップレベル await は断る**。評価の後に Promise が未決なら `SyntaxError: user.js: top-level await is not supported`。モジュール本体の同期区間は park しない（vm-L2-design §11.2）、起動の評価はターンを持たない、の 2 点から、未決は TLA のときだけ。続きをジョブで走らせると、評価が終わったことにして `frame()` を結んだ後に、そのモジュールが走り、その失敗は評価の失敗として報告できない。セッションは始まらないので、止まったモジュールはゲストと一緒に消える。
- **import attributes（`with {…}`）は断る**。import の入れ子は**深さ 8** まで（`RangeError`）。ローダの各段は `JS_Eval` の C 再帰（UI タスクのスタック）で、表の上限 32 段は積ませない。入口がいくつかの部品を import する形は深さ 1。
- **循環 import は仕様どおり**許す（`cycle` の検査はしない）。B 案（`load`）の循環は `CONFLICT` にしたが、モジュールの循環は ES の正しいプログラムで、初期化前の束縛を読んだときだけ `ReferenceError`（`ta is not initialized`、位置は**読む側のモジュールの import の行**。QuickJS の位置の付け方）。
- **拒否した評価の Promise**: QuickJS は、失敗したモジュールの内側の async 関数の Promise と、グラフの Promise を、ゲストの拒否の追跡に「未処理」として渡す。評価に失敗したセッションは drain に届かず（`START_FAILED`）、`pocketjs_guest_destroy()` が一覧を捨てるので、`Unhandled Promise rejection` の行は出ない。ハンドラを付けて消す処理は置かなかった（内側の Promise には届かない）。
- heap 不足（解析中）は、B 案と違って**再試行の道がアプリに無い**（入口の評価そのものの失敗）。`EVAL_ERROR` と `OOM` の行が出て、アプリは起動せずホームへ戻る。落ちない（実機で確認、§9.4）。部品を小さくするか、`load` で後回しにする。
- ログ: `APP_IMPORT <名前> bytes=… compile_us=… used=<前>><後>`（契約のマーカーではない）。`compile_us` と後の値は、そのモジュールが import する子の分を含む（子の行が先に出る）。

費用（実測・map、`esp_idf_size --diff`、基準 `e15e6eb`、通常の image）: **DIRAM +16 B**（`.bss`: map で `depth` 4 B と `resolving` 1 B。残りは整列の詰め物と推定）、flash +1,448 B（コード +856、rodata +592）。capability は足していない（`pocket.*` の面ではないので。表の 32 枠を使わない）。

### 9.2 測定（実機、2026-09-30、COM3、`POCKET_HEAPPROBE=ON` の image）

同じ内容の 3 つの形（`tools/heapprobe_import_gen.py` が生成、31.6 KB、部品 3 つ × 10.5 KB。アプリ風の関数・クロージャ・メソッド付きのオブジェクト・数表。モジュール版は各部品の末尾に `export { pN_run };` の 1 行が付くだけ）。DERBY WATCH ではない理由: 部品同士がトップレベルの `let` を書き合うので、import の束縛（読み取り専用）では書き直さずに分けられない（§3.3）。変種の索引 47〜49、`tools/heapprobe_device.py --bisect`、256 B 刻み、各 1 回。

| 形 | 評価できる最小のヒープ上限（実測・実機） | 評価直後のゲスト heap（上限 160 KiB） | 評価の時間（実測・実機） |
| --- | ---: | ---: | ---: |
| (a) 1 スクリプト | **評価できない**: 上限 160 KiB・180,000・200,000 で割り当て上限の OOM、240,000 では割り当て器そのものが 6.9 KB を断った（used 112,016） | — | —（失敗まで 92〜192 ms） |
| (b) `pocket.app.load` で 3 分割 | **154,612** | 122,896 | 279 ms（部品の解析 64〜65 ms ×3） |
| (c) 静的 `import` で 3 分割 | **144,139**（(b) より −10.5 KB） | 134,752（(b) より +11.9 KB） | 279 ms（同 64〜66 ms ×3） |

host（64 bit、ASan、`tools/test_app_import.c`、同じ 3 形）: 最小の上限 (a) 258,773 / (b) 191,796 / (c) 181,601、評価後に残る量 (a) 158,476 / (b) 157,328 / (c) 173,162。実機と同じ向き（(c) がピークは最小、定常は最大）。

読み方:

- **分割そのものが効く**のは §3.2 と同じ（この内容では、1 スクリプトは 160 KiB に入らない）。
- **ピークは import の方が低い**（実測・実機 −10.5 KB）。`APP_LOAD`／`APP_IMPORT` の行（実測・実機）では、load は各部品の**解析の直後にその実行**が走り、次の部品の解析はその実行の残り（部品 1 つ約 +3〜6 KB のオブジェクト）と `pocket.app` の名前空間の上で行われる。import は 3 つの解析がすべて先（35,460 → 60,336 → 83,436 → 106,468）で、実行は後。最後の解析の土台が load より低い（83,436 対 94,264）。
- **定常は import の方が高い**（+11.9 KB）。モジュールは関数本体（トップレベルのバイトコード）とモジュールの記録・名前空間を realm の最後まで持つ（§3.3、`loaded_modules`）。load のトップレベル関数は実行の直後に解放される。
- 時間は同じ（279 ms）。解析が支配的で、仕組みの差は見えない。
- 注意: 実行が重い（トップレベルで Kasane の場面や大きな表を作る）アプリでは、import でも実行は解析の後にまとめて来るので、ピークは「全モジュールのバイトコード＋実行の一時領域」になる。この内容の実行は軽い（部品あたり 3 ms・+3〜6 KB）。**重い実行のアプリで import が load より低くなるとは限らない**（未測定）。

### 9.3 作者向け: `import` と `load` の使い分け

| 目的 | 使う | 理由 |
| --- | --- | --- |
| 起動時の評価のピークを下げたい（全部を起動時に読む） | **`import`**（入口を `.mjs`、部品を `.mjs` のチャンク） | 普通の JS の書き方。解析が 1 つずつ。ピークは load より低い（実測、§9.2）。ただし定常は +12 KB 前後（この内容で） |
| 場面ごとに後から読む・使い終わったら手放す | **`load`**（`.js` のチャンク） | 動的 `import()` は断る。モジュールは手放せない |
| 部品が同じ `let` を書き合う（DERBY WATCH の形） | **`load`**、または状態を 1 つのオブジェクトに移してから `import` | import の束縛は読み取り専用（`count = 1` は TypeError、host で確認） |
| 定常の heap が厳しい（評価後に大きな確保をする） | **`load`** | import は評価後もモジュールの本体を持つ |

**import の書き方**

- マニフェスト（`app_registry.c`）の `entry` を `.mjs` にする。入口のファイルも `.mjs` にし、`main/CMakeLists.txt` の `EMBED_TXTFILES` で埋め込む（シンボルは `_binary_<名前>_mjs_start`）。
- `chunks.txt` に `.mjs` のファイルを書く（`scene foo_scene.mjs`）。import の指定子はその名前（`from 'scene'`）。`'./foo_scene.mjs'` は書けない。
- **入口で `globalThis.frame = …` と書く**。モジュールの `function frame` はグローバルに出ないので、ホストは見つけられない（bind-frame.js が `NOT_FOUND` を返し、アプリとして走らない）。`pocket.app.start()` など評価中にしか呼べないものも、入口か、入口が import するモジュールのトップレベルで。
- 入口のソースに `pocket.kasane` の文字列を入れる（§7.3 と同じ。Kasane の arena の先取りは入口しか見ない）。
- モジュールは strict で、`this` は `undefined`。部品の間の状態は export した関数か、export した 1 つのオブジェクトの中身を書き換える。
- トップレベル await・`import()`・`with {…}` は使えない（§9.1）。
- `load` との併用: 入口がモジュールでも `pocket.app.load('x')` は使える（`.js` のチャンクがグローバルスクリプトとして評価される。モジュールの名前はそこから見えない）。

### 9.4 検証

- host（`tools/build_app_import_test.sh`、WSL、ASan/UBSan、実物の `pocket_app_load.c`・`pocket_api.c`・`app_registry.c`）: 102 項目。名前での import、live binding、1 モジュール 1 インスタンス（2 か所から import した `imbase` の評価は 1 回）、無害な循環、import は読み取り専用、モジュールの名前はグローバルに出ない、`load` がモジュールを断る、構文エラー（`import_bad.mjs:3:1`）・トップレベルの例外（`import_throw.mjs:4:1`）・TDZ の循環（`import_tdz_b.mjs:1:1`）・未知の名前・相対／絶対パス・スクリプトのチャンク・attributes・深さ 9・TLA（子と入口の両方）・動的 `import()`（スクリプトから、モジュールのトップレベルから）、上限 120,000 での heap 不足（裸の `null`、落ちない）の後に別のセッションで成功、3 セッション、成功したセッションで未処理の reject が 0。既存の `tools/build_app_load_test.sh`（32 項目）もそのまま通る。
- 実機（変種 50〜56）: `import-api` が `count=41 base=40 runs=1 cyc=ab ok loadModule=INVALID_ARGUMENT`、frame 10 で `dyn=TypeError: import('imbase') in user.js: dynamic import() is not supported; use pocket.app.load()`、frame 40 まで生存。構文エラー・例外・TDZ・未知・スクリプト・TLA は、それぞれ `EVAL_ERROR`（host と同じ文面・同じ `file:line`）と `START_FAILED` の後、次の変種がホームから普通に起動した（アプリは落ちない）。`Unhandled Promise rejection` の行は 0。heap 不足は (a) の `EVAL_ERROR InternalError: out of memory` と `OOM` の行、ホームへ戻る。
- 回帰: `tools/kasane_contract/run.sh`（`GAMES_M32=0`）通過。実機（通常の image）で `smoke_device.py --cycles 20`（`SMOKE_OK 20`）、`stress_app.py`（`STRESS_APP_PASS`）、`test_app_resume.py`（`TEST_APP_RESUME_OK`）が通過。

### 9.5 事前コンパイル（§3.5）への引き継ぎ

- **合流点は 2 か所**: `pocket_app_load.c` の `module_load()` の `JS_Eval(MODULE|COMPILE_ONLY)` と、`js_load()` の `JS_Eval(GLOBAL|COMPILE_ONLY)`。どちらも「解析して値を返す」だけなので、`JS_ReadObject(JS_READ_OBJ_BYTECODE)` に替えれば API は変わらない。表に「ソースかバイトコードか」を持たせる（`app_chunk_t` に 1 バイト足す。`.mjs` の種別と同じく、ファイル名の約束で決めてもよい）。
- **モジュールを `JS_ReadObject` で読むときは、依存の解決を自分で呼ぶ**: ソースの `JS_Eval(COMPILE_ONLY)` は中で `js_resolve_module` まで済ませるが、`JS_ReadObject` は済ませない（quickjs.h の `JS_ResolveModule` の注記）。`module_load()` で読んだ後に `JS_ResolveModule(ctx, value)` を呼ぶ。この呼び出しも `resolving` の窓の中（入口の評価中）なので、動的 `import()` の拒否はそのまま効く。入口を事前コンパイルするなら、`pocket_app_eval_module()` の最初の `JS_Eval` を `JS_ReadObject` + `JS_ResolveModule` に替える。
- **モジュール名**: バイトコードに入るモジュール名は、host でコンパイルしたときの `filename`。ローダが引く名前（チャンクのファイル名 `foo_scene.mjs`）と同じ文字列でコンパイルすること。違うと `loaded_modules` の検索に外れ、同じモジュールが 2 回読まれる。
- **atom の書き換え**（§3.5）: `JS_ReadObject` はバイトコードを RAM に写して atom を書き換えるので、flash 常駐にはならない。import の定常 +12 KB（§9.2）は、事前コンパイルでも残る（モジュールの本体を持つのは同じ）。
- **ピーク**: 事前コンパイルでは解析の一時領域が消えるので、import と load のピークの差（§9.2 の −10.5 KB は解析の順番の差）はほぼ消え、実行の一時領域と、評価後に残る量の差（import が多い）が残る見込み（推定、未測定）。
- 検証の土台: `tools/test_app_import.c` の失敗の表（`file:line` を含む）は、`JS_WRITE_OBJ_STRIP_DEBUG` を使うと行番号が消える。行番号表を残すかどうかは、この試験で決められる。
