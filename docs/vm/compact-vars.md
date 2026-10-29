# 局所変数を詰める（compact vars、2026-10-29）

ブランチ `vm/compact-vars`（`main` b290fa5 から）。発端は MEGADEMO の軽量化（[megademo-story.md](../apps/megademo-story.md) の「軽量化と展開の追加」）で、ゲストの heap の中で「名前」の保存が大きいことに気づいた。数値の区分は、実測(host)（m32、ファームと同じ `QJS_DISABLE_DUMPS`、TLSF の長さで課金）、実測(実機)、推定。

## 結論

- **harden したコンテキスト（アプリはすべてそう。[harden.md](harden.md)）でコンパイルした関数は、引数と局所変数ごとに 2 B だけを持つ**: 捕まえられた変数なら `JSVarRef` のスロット番号、そうでなければ `0xffff`。今までの `JSVarDef`（20 B）と、変数名の atom を持たない。
- 効果（実測(host)、`tools/vmtest/compact_cost.py`、コンパイル済みの関数が保つバイト）: **出荷している 16 アプリの合計 −55 KB**。DERBY WATCH −13.8 KB、MEGADEMO −13.0 KB（`main` の版）、BIG WAVE −6.8 KB、news zoom −3.1 KB、stress −3.0 KB ほか。
- **失うもの**: 宣言前の `let`/`const` を読んだときの `ReferenceError` の文言が、変数名ではなく `lexical variable is not initialized` になる（上流が表を落とした関数で使う既存の文言）。harden した関数は `JS_WriteObject` で書けない（エラーを投げる）。バイトコードのダンプに局所変数名が出ない。
- harden していないコンテキスト（test262、`vmrun` の既定）は今までどおり `JSVarDef` を持つ。`eval` の意味は変わらない。

## なぜ名前が要らないか

QuickJS の実行中の命令は、局所変数を番号で読み書きする（`get_loc` / `put_loc` など）。`JSVarDef` を実行中に読むのは次だけだった（コードで確認）:

| 読む所 | 使うもの | harden で |
|---|---|---|
| `get_var_ref`（クロージャを作る、`make_*_ref`、mapped `arguments`） | `is_captured`、`var_ref_idx` | 残す（2 B に詰めた中身） |
| `close_lexical_var`（ブロックを出るときに捕まえた変数を切り離す） | `var_ref_idx` | 残す |
| TDZ の `ReferenceError` の文言 | `var_name` | 表が無い関数の既存の経路（汎用の文言） |
| 直接 `eval` のコンパイル（`add_closure_variables`）: スコープの鎖と名前で外の変数を探す | `var_name`、`scope_level`、`scope_next` ほか | 来ない: harden は `JS_EvalObject` で文字列を解析する前に `EvalError` を投げる |
| `JS_WriteObject` / `JS_ReadObject`、ダンプ | 全部 | 書き出しは拒否、ダンプは名前なし |

クロージャ変数（`JSClosureVar`）の名前も、外の局所変数・引数・親のクロージャ変数を指すもの（`JS_CLOSURE_LOCAL` / `ARG` / `REF`）は `eval` と TDZ の文言でしか読まないので、harden では `JS_ATOM_NULL` にした。グローバルとモジュールのもの（`GLOBAL_REF`・`MODULE_*`）は名前のまま。

子の関数は親より先に作られ（`js_create_function` の最初）、名前の解決はコンパイル中の `JSFunctionDef` の上で終わっているので、親のバイトコードから名前を落としても解決には影響しない。

## 実装（`components/quickjs-ng/quickjs-ng/quickjs.c`）

| 所 | 変更 |
|---|---|
| `fa_get/set_compact_vars`（`header.dummy2` の bit 13）、`fa_compact_vars()`、`js_compact_vars_size()`、`fa_var_ref_slot()` | 詰めた表は cpool の直後、今までの `JSVarDef` の位置に置く（4 B 境界に丸める。後ろが `JSClosureVar` の配列）。詰めた関数では `fa_vardefs()` が NULL を返すので、名前を読む所は「表を落とした関数」の既存の経路を通る |
| `js_create_function` | `ctx->harden` なら詰めた表を作り、変数名の atom を手放す。クロージャ変数の名前（局所・引数・親）も手放す |
| `get_var_ref`、`close_lexical_var` | `fa_var_ref_slot()` でスロットを引く（詰めた表と `JSVarDef` の両方） |
| `JS_WriteFunctionTag` | 詰めた関数は `TypeError`（読み戻すと捕まえた変数を見つけられないため） |
| `compute_bytecode_size` | 詰めた表の大きさを数える |

## 検査（実測(host)）

| 検査 | 結果 |
|---|---|
| `tools/vmtest/run.sh`（asan・o2・`--force-yield`・`--budget-jobs 3`） | すべて 88/88（新しい `compact_vars.js` を含む） |
| 同じコーパスを `--harden` で、変更前と変更後の `vmrun` に通す | 87 ファイルの出力がバイト一致（`#info` のメモリの数字だけ減る）。harden で途中で止まる 13 ファイル（`eval` などを使う）も止まる所まで一致。`closures.js` から `eval` の節を抜いた版も一致 |
| `corpus/compact_vars.js`（`--harden`） | 引数と局所、ループのブロック、3 段の鎖、再帰、generator、await をまたぐ async、mapped `arguments`、クラスのフィールド、TDZ。変更前の出力と比べ、違いは TDZ の文言 2 行だけ |
| `lifecycle.sh asan`（900 ケース）、`tdz_elision.sh`、`stack_probe.sh 2000 o2`、`budget_probe.sh o2` | OK |
| test262（harden なし。共通の経路 `fa_var_ref_slot` を通る、asan、固定の 72faf8ec1445） | 22,021 pass / 247 fail / 1 skip、baseline からの退行 0 |

## アプリの側

MEGADEMO の軽量化で、局所変数の名前を関数をまたいで使い回して atom を減らした（63 個、常駐 −4.5 KB）。この変更の後は局所変数の名前が heap に残らないので、その使い回しは不要になる（読みやすい名前に戻せる）。

## 実機（実測(実機)、2026-10-29）

同じ機体で、この版（c985bf93、ELF 1813e6b0b）と `main`（b290fa5、ELF a366267a）を焼き替えて、同じアプリを動かした。アプリのソースは両方とも `main` のもの。最後は `main` の版を焼いて終えた（機体は通常の image）。

| | `main` | compact vars | 差 |
|---|---|---|---|
| MEGADEMO 評価後の JS（`MEM ... js=`） | 126,024 B | 112,044 B | **−13,980 B** |
| MEGADEMO 評価中の最大（`set` の `used=` の中央） | 120,948 B | 107,340 B | −13,608 B |
| MEGADEMO 90 秒（3 周弱） | DEGRADE なし | DEGRADE なし | |
| DERBY 読み込み後の JS（`MEM ... js=`） | 97,332 B | 90,100 B | **−7,232 B** |
| DERBY `demo` 読み込み後（`used=` の右） | 106,216 B | 98,848 B | −7,368 B |
| DERBY デモ1周（`derby_smoke`） | 完走 | 完走 | |

- **フレームの時間は変わらない**: MEGADEMO の `KASANE_PAINT turn_ms` は場面ごとに両方でほぼ同じ（CORE の最大 25.9 ms と 26.8 ms、ZENITH 8.5 ms）。DERBY も同じ。
- **空いた分が先読みに回った**: MEGADEMO は次の場面の plan を、空き heap の門（`LOAD[2]`）が許す分だけ前の場面のうちに読む。ZENITH に入ったときの読み済み数（`MEGADEMO SCENE ZENITH ... plans=`）は `main` が 1、この版が 16（全部）。
- **MEGADEMO は host の見積もり（−13.0 KB）とほぼ一致。DERBY は見積もり（−13.8 KB）の約半分**。`compact_cost.py` は `apps/derby/derby_prog.js` を書いたまま（20,751 B）コンパイルするが、ファームは `@plan` の関数を IR の文字列に下ろした後（実機で 2,465 B）を読む。host は、実機に無い関数の局所変数まで数えている（差の内訳は未測定）。

焼き込みの経緯: 最初のフラッシュ（`cv-flash-c985bf93`）は書き込みの後、読み戻しの途中（2.4 MB 中 1.25 MB）で止まった（機体の電源が落ちていた）。その後の焼き直しは、依頼に旧版の `lkg_receipt` を付けていたため、「機体の中身が旧版と同じか」の書き込み前の検査で止まり続けた（ブローカーの仕様どおり）。`lkg_receipt` を付けずに焼くと通った。
