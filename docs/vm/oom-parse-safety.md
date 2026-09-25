# 確保失敗時のコンパイル経路の安全性

ブランチ `vm/oom-truncated-bytecode`（`vm/main` の `953c627` から）。**VM の段（L0〜L5）とは独立**の
不具合修正で、L3a の作業中に見つかった（[vm-L3-results.md](vm-L3-results.md)、backlog の L3 #6）。
すべてホストの実測（ASan/UBSan、`tools/vmtest/`）。

## 1. 何が起きていたか

JS ソースのコンパイル中に確保が 1 回失敗すると、QuickJS はパースを**止めずに続ける**。失敗は
bytecode を積む `DynBuf` のエラー旗に残るだけで、例外は投げられているが誰も見ない。そのまま
後段（`resolve_variables` → `resolve_labels` → 解放）が**壊れたバッファを命令列として読む**。
結果は4種類に分かれた:

| 症状 | 例 |
| --- | --- |
| **範囲外の読み書き**（ASan） | `resolve_variables` / `resolve_labels` の読み越し、`js_parse_class` の書き越し |
| **assert で abort**（実機では再起動） | `js_create_function` の `assert(cpool_idx >= 0)` |
| **atom の二重解放・ゴミ atom の解放** | 解放時に `rt->atom_array` の範囲外や自由リストの値を atom として読む |
| **黙った誤コンパイル** | 捕捉すべき変数がグローバル読みとして出力され、プログラムが走って `ReferenceError` |

最後のものが最も悪い。`closures.js` を `--fail-alloc 1764` で走らせると、正しい出力を 2 行出したあと
`ReferenceError: shared is not defined` で止まる。**同名のグローバルが在れば、例外すら出ずに別の
変数を読む。** コンパイルが失敗を報告していないので、アプリ側からは検出できない。

## 2. 見つけ方 — 全点掃引

`tools/vmtest/oom_sweep.sh`: 確保の試行を**1回ずつ全部**落として、1点ごとに実行する
（`vmrun --fail-alloc N`）。closures.js は約 4,690 回確保する。

最初の発見は L3a の 15 点の粗い掃引で、当たったのは 1 点（N=1500）だけだった。**同じファイルの全点
掃引では 71 点、generators.js では 133 点**。粗い掃引の 1 点は氷山の一角で、しかも5つの別々の場所に
散っていた。

修正ごとの推移（各 4,800 点、ASan/UBSan の報告数）:

| 段階 | closures | generators |
| --- | --- | --- |
| 修正前 | 71 | 133 |
| + `resolve_variables` の入口、+ cpool の assert | 14 | 15 |
| + `resolve_labels` の短縮パスを飛ばす | 8 | 7 |
| + `DynBuf` のエラーを固定（§3.1） | **12** | **10** |
| + リロケーションの書き戻しを飛ばす | 3 | 1 |
| + `JS_NewObjectFromShape`、for-of の `memset` | 1 | 0 |
| + `resolve_labels` の失敗経路 | **0** | **0** |

4 行目で**増えている**のは、正しい修正が別の欠陥を露出させたからである（§3.3）。

## 3. 根本原因と修正

### 3.1 `DynBuf` のエラーは「成長する書き込み」にしか効いていなかった（`cutils.h`）

`dbuf_putc` / `dbuf_put_u16/u32/u64` / `dbuf_put` / `dbuf_printf` の速い経路は「余りに収まるか」
だけを見て、エラー旗は `dbuf_claim` の中でしか見ていなかった。なので成長に失敗したあとでも、
**余りに収まる小さな書き込みは成功する**。

`tools/vmtest/test_dbuf_sticky.c` で単体に切り出して確かめた:

```text
未修正:  put_u32=-1 error=1 | putc after error=0  | size 12 -> 13   NOT STICKY
修正後:  put_u32=-1 error=1 | putc after error=-1 | size 12 -> 12   sticky
```

命令長で読むバイト列にとってこれは**ずれたストリーム**を意味する — 失敗したオペランドが抜け、
次のオペコードがその位置に座る。

**修正**: 失敗時（`dbuf_claim` の確保失敗と `dbuf_set_error`）に `allocated_size = size` へ畳む。
以後の書き込みは長さに関わらず遅い経路へ回り、エラーで -1 になる。**速い経路の費用はゼロ。**
`DynBuf.allocated_size` を読むのは `cutils.h` だけ（`quickjs.c` の `arg_allocated_size` は別物）で、
エラーを戻すコードも無いことを確かめた。

### 3.2 入力が切れている — `resolve_variables`

エラー旗が立った bytecode を、命令長で進みながら `dbuf_put` でコピーする。最後の命令のオペランドが
無いので読み越す。**修正**: 入口で `dbuf_error` を見て OOM を投げる。`emit_atom` は確保に成功して
から atom を複製するので、切れた命令は atom の参照を持っておらず、参照数の辻褄は合う。
解放側の `free_bytecode_atoms` には上流自身が
`/* may happen if there is not enough memory when emitting bytecode */` と書いた防御が既に在る。

### 3.3 記録位置への書き戻し — `resolve_labels` と `js_parse_class` ほか

ジャンプのリロケーション位置は「プレースホルダを書いた**後の** `size - n`」で記録される。書き込みが
失敗すると `size` は進まないので、その位置は**1つ前の命令の中**を指し、ラベルに達したときの
`put_u32` がその命令のオペランド（atom など）をジャンプ差分で上書きする。解放時にそれを atom として
読んでいた。

§3.1 の修正で件数が**増えた**のはこれが原因である。修正前は、失敗の後に余りへ書かれた別の命令が
たまたまその位置を埋めることがあったが、修正後は `size` が絶対に進まなくなり、上書きが**必ず**
前の命令に当たるようになった。§3.1 は正しく、この欠陥を露出させた。

**修正**: 出力にエラーが立っていたら書き戻しを飛ばす（項目は解放する）。同じ形の直接書き戻しを
パーサ全体で洗い、`get_prev_opcode()`（失敗時に `OP_invalid` を返す）で守られていない3箇所 —
クラスのコンストラクタの cpool 番号（`ctor_cpool_offset`、記録が書き込みの**前**なので失敗時は有効
データの直後を指す）、フィールド初期化関数の brand、`switch` の `default` ラベル（上流のコメントが
「shameful and risky」と書いている） — にも同じ条件を付けた。

### 3.4 `resolve_labels` の失敗経路での atom 二重解放

上流の `fail:` は `/* XXX: not safe */` と書かれていて、実際に安全でなかった。`add_reloc` の確保失敗で
そこへ来ると、`bc_out` を捨てて**入力**を `byte_code` のまま残す。ところが第1パスは入力の命令を
書き換えるときに消費した atom を既に `JS_FreeAtom` しており、呼び出し側の失敗経路が入力の全 atom を
解放する — 二重解放。`QJS_WATCH_ATOM` の一時計測で、同じ atom 595 が `resolve_labels` と
`free_bytecode_atoms` の2回解放されるのを確かめた。

**修正**: `bc_out` にエラーを立てて共通の末尾へ合流させ、`bc_out`（短縮オペコード形式、
`use_short_opcodes` 付き）を据える。処理済みの atom は `bc_out` に1回ずつ、消費済みの atom は
消えているので、各 atom はちょうど1回解放される。未処理の入力側の atom は解放されずに残るが、
`JS_FreeRuntime` が atom 表ごと回収する（下記のとおり LSan もリークを報告しない）。末尾で、
**解決されずに残ったリロケーション項目**も解放する（`js_free_function_def` は配列は解放するが
鎖は解放しない。これは上流の経路でも漏れていた）。短縮パスはエラー時に飛ばす。

### 3.5 「確保失敗」と「見つからない」の区別 — 黙った誤コンパイル

`get_closure_var()` は無ければ作る関数なので、**-1 は常にエラー**（確保失敗か 16bit 上限）である。
`resolve_scope_var` の呼び出し元はそれを3通りに誤って扱っていた:

- `if (idx >= 0)` で素通り → 親スコープの探索 → **グローバル変数としての出力**（§1 の誤コンパイル）
- 検査せず -1 を u16 のオペランドに書く → var_ref 番号 0xFFFF
- 検査せず `goto has_idx` → **`s->closure_var[-1]` を読む**

同じ関数の `add_arguments_var` / `add_func_var` の -1 も同じ穴へ落ちていた（`closures.js` の
`--fail-alloc 1903` で `mapped()` が `ReferenceError: arguments is not defined`）。
`add_eval_variables` は5箇所とも戻り値を捨てており、直接 `eval` から見えるはずの変数が消える。

**修正**: すべて `closure_fail` に集め、出力にエラーを立てる。`resolve_variables` が末尾で OOM を
投げ、関数はコンパイルに失敗する。`add_eval_variables` は `void` なので、関数の bytecode に
エラーを立て、§3.2 の入口検査がそれを拾う。

### 3.6 その他

- `js_create_function`: `assert(cpool_idx >= 0)`。`cpool_add` の3つの呼び出し元が失敗を確かめずに
  `parent_cpool_idx` に入れる。実機は assert 有効なので **パース中の OOM で再起動**していた。
  子を作る前に弾いて OOM を投げる（子はリストに残り、失敗経路がまとめて解放する）。
- `JS_NewObjectFromShape`: 上流のコメントは `/* cannot fail */` だが、`add_property` は OOM で NULL を
  返し、次の行が参照する（実行時、`new Array` に既定でないプロトタイプ）。
- `js_parse_for_in_of`: bytecode が一度も確保されていないと `memset(NULL, x, 0)`（UB）。

### 3.7 OOM のエラー経路での参照の漏れ — `JS_FreeRuntime` の assert（§5.1 の残り 4 件）

`JS_DUMP_LEAKS`（`JS_SetDumpFlags`、内部参照を `gc_decref` で除いて外部参照だけを出す）で見ると、
4 点とも**漏れているのは1個**で、それが prototype 経由で組み込み群を生かしていた。特定の
オブジェクトの `js_dup` / `JS_FreeValueRT` を `__sanitizer_print_stack_trace()` で追って場所を出した:

| 点 | 漏れたもの | 場所 |
| --- | --- | --- |
| `special_calls.js` 1979 / 2047 | タグ付きテンプレートの strings 配列（空の Array、ref 1） | `cpool_add`。`emit_push_const` は `js_dup()` した値を渡すが、`js_resize_array` の失敗時に捨てずに `-1` を返す。呼び出し元 `js_parse_template` は自分の参照を既に解放している |
| `yield_job_tails.js` 2643 / 2659 | `Promise.all` の値配列の要素（非同期ジェネレータの iterator result） | `js_json_to_str` の配列分岐。要素 `v` を読んだ直後の `JS_ToStringFree(js_int64(i))`（16 バイトの文字列）が失敗すると `goto exception` するが、共通の末尾は `v` を解放しない |

**修正**: `cpool_add` の失敗経路で `val` を解放（他の呼び出し元は `JS_NULL` を渡すので無害）、
`js_json_to_str` の当該経路で `v` を解放。どちらも quickjs-ng master（2026-09-23）に同じコードが残っている。

## 4. 上流の状況

**quickjs-ng master と bellard/quickjs master（どちらも 2026-09-23 取得）で、上のどれも修正されて
いない。** 確かめた箇所: `cutils.h` の失敗時の畳み込み無し、`resolve_labels` の `XXX: not safe` 残存、
`ctor_cpool_offset` の書き戻しに検査無し、`assert(cpool_idx >= 0)` 残存、`add_property` の
`cannot fail` 残存、`_with_` 経路の `if (idx >= 0)` 素通り残存。

## 5. 検証

| 検査 | 結果 |
| --- | --- |
| `oom_sweep.sh`（closures + generators、各 4,800 点） | **0 件**（修正前 71 / 133） |
| `oom_sweep.sh`（コーパス 64 ファイル、各 3,000 点） | **ASan/UBSan 0 件**（192,000 点）。サニタイザ以外の 18 件は修正前から在るもの（§5.1） |
| 以前落ちていた 8 点を LSan 有効で | 8 点とも `InternalError: out of memory` で終了、**リーク 0** |
| `test_dbuf_sticky.c` | 修正後 pass、未修正ヘッダで fail |
| `run.sh`（o2、通常 / `--force-yield`） | 75/75、75/75 |
| Test262 標準集合（o2、通常 / `--force-yield`） | 7,501 / 194 / 0、**基準と同一**、退行 0 |
| 実機ビルド（書き込みはしていない） | 通る。DIRAM ±0、Flash **+244 B**（1,402,876 → 1,403,120、`vm/main` の既定ビルド比） |

### 5.1 コーパス全体の掃引

`oom_sweep.sh -n 3000` をコーパス 64 ファイルに（長い `bench_*` と `runaway_jobs` / `budget_starve` /
`tco_guards` は除外）。**192,000 点で、ASan/UBSan の報告は 0 件。**

途中の版（古い実行ファイル、31/64 ファイル）で `js_parse_class` の書き越しを 19 件見つけ、§3.3 を
クラス解析まで広げた。その3ファイル（`l2b_flat_calls` / `lazy_call_inputs` / `l2b_async_flat`）は
最終版で各 3,000 点とも 0 件。

サニタイザ以外の報告が 18 件残った。**18 件とも修正前のビルドで同じ点・同じ症状が再現する**
（修正とは無関係に以前から在る）:

| 件数 | ファイル | 症状 | 何か |
| --- | --- | --- | --- |
| 14 | `yield_then_handler.js` | 30 秒でタイムアウト | **テストの作り**。末尾の `afterAll` が `order.length < 5` の間、自分を `Promise.resolve().then` で再登録し続ける。OOM で `.then` の1つが落ちると 5 に届かず永久にポーリングする。VM は Promise ジョブを正しく回している（SIGABRT で取ったスタックは `JS_VMCallJob` の中）。実機では同じ形（無限 Promise 連鎖）をドレインの暴走ガードが 250 ms で止める（L3a の実機計測で診断 `'6'` が `RUNAWAY one drain spent 250845 us`） |
| 4 | `special_calls.js`、`yield_job_tails.js` | **abort**: `JS_FreeRuntime` の `assert(list_empty(&rt->gc_obj_list))` | **OOM のエラー経路での参照の漏れ**。破棄の時点で約 160 個の GC オブジェクトが残り、大半が素の Object と C 関数、参照数 11〜13 のものも在る — 組み込みのプロトタイプ群がまるごと生き残る形で、C 側の参照が1本解放されずにコンテキストの全体を生かしている。**実機では `app_stop()` がこの assert を踏むので、実行中に OOM を踏んだアプリを閉じると再起動する**（推論。実機では未確認）。→ §3.7 で修正済み（4 点とも assert なしで終了、LSan 0、2 ファイル各 3,000 点で 0 件） |

## 6. メモリ安全性の後に残っていたもの — すべて修正済み

§3〜§5 の修正後、エラーの**種類**を全点で分類した（`--fail-alloc N` ごとの最初の例外）。
メモリ安全性の問題ではない（ASan/UBSan 無反応）が、**OOM が別の例外や黙った誤りとして出る**点が
残っていた。並行して調査・修正し、すべて `vm/oom-truncated-bytecode` に統合した:

| 症状 | 最初の点数 | 修正 |
| --- | --- | --- |
| `SyntaxError`（存在しない構文エラー） | closures 112 / generators 69 | §7 |
| `TypeError: not a function`（初期化時に組み込みが黙って欠ける） | 12 / 9 | §8.1 |
| `TypeError: cannot read property 'constructor' of null` | 0 / 6 | §8.2 |
| `JS_FreeRuntime` の assert（参照の漏れ） | 4 点（§5.1） | §3.7 |
| `yield_then_handler.js` のタイムアウト | 14 点（§5.1） | §8.5 |
| 正規表現のコンパイラ（未掃引だった） | 新コーパスで 4 件 | §8.3 |
| 実行時の `not a function`、`this` のグローバル読み、名前付きグループの欠落 | 正規表現の掃引と §8.1 の後に判明 | §8.4 |

**実機では1度も確かめていない。** 実機の assert（§3.6、§3.7）や `exit(1)`（§8.1）が再起動を
起こしていたかどうかは、ホストでの推論である。

## 7. パース中の OOM を `SyntaxError` と報告していた件（ブランチ `vm/oom-fix-syntaxerr`）

§6 の 1 行目。`--fail-alloc` で `SyntaxError` になる 181 点（closures 112 / generators 69）の各点で、
`js_parse_error` に届いた時点の状態を一時的な計測で調べた。**181 点すべてで、そのパースの中で
`JS_ThrowOutOfMemory` が既に呼ばれていた**（確保失敗の OOM は既に投げられていて、それを
`SyntaxError` が上書きしていた）。入口は 2 つ:

| 入口 | 点数 | 仕組み |
| --- | --- | --- |
| 関数の bytecode `DynBuf` の失敗 | 170 | パーサは止まらず、`get_prev_opcode()` が `OP_invalid` を返し、左辺値の判定が「invalid assignment left-hand side」「invalid increment/decrement operand」を出す（§6 の推測どおり） |
| `js_parse_skip_parens_token` の先読み | 11 | 先読みの字句解析で atom が作れず失敗するが、上流の設計どおり（`XXX: should clear the exception`）黙って推測を返す。パーサが別の分岐に入り「Unexpected token '=>'」「variable name expected」「expected 'of' or 'in'」を出す |

**修正は 2 点で、どちらも中央に置いた。** `JSRuntime` に `oom_count`（`JS_ThrowOutOfMemory` が毎回
増やす）を足し、`js_parse_init` がパース開始時の値を控える。

1. `js_parse_error`: 開始時から値が動いていれば `SyntaxError` ではなく OOM を投げる。OOM を1度も
   踏まないパースは値が動かないので、**本物の構文エラーは変わらない**。
2. `__JS_EvalInternal`: `js_parse_program` が成功しても値が動いていればコンパイルを OOM で失敗させる。
   1 だけの版で掃引すると、**確保に失敗したのにパースが成功し、プログラムがそのまま走る点**が
   69 点あった（closures 10 / generators 59）。内訳は先読みの失敗、`push_scope` の失敗（全呼び出し元が
   戻り値を見ないので、ブロックの字句変数が外側のスコープに入り、対応する `pop_scope` が違うスコープを
   閉じる）、`js_new_function_def` のファイル名 atom の失敗。69 点とも出力は正常時と同じだった
   （誤コンパイルは観測していない）が、先読みと `push_scope` はパーサがしていない判断の上にコンパイル
   させる。個々の呼び出し元を直すより、パース中の確保失敗を一律にコンパイル失敗にする方が漏れがない。

| 分類（各 4,800 点） | closures 前 → 後 | generators 前 → 後 |
| --- | --- | --- |
| `InternalError: out of memory` | 1,200 → 1,322 | 2,633 → 2,761 |
| `SyntaxError` | 112 → **0** | 69 → **0** |
| 例外なし | 3,476 → 3,466 | 2,059 → 2,000 |
| それ以外（§6 の `TypeError` など） | 12 → 12 | 39 → 39 |

`oom_sweep.sh` 0 / 0、`run.sh` o2 75/75（通常 / `--force-yield`）。本物の構文エラーの番人として
Test262 の `language/expressions` `language/statements` `language/arguments-object`（20,712 ファイル、
38,566 pass / 1,191 fail / 2 skip）を修正前後の o2 で走らせ、**結果ファイルが1行も違わない**
（負のテストは `SyntaxError` の型を検査する）。quickjs-ng master（2026-09-23）も
同じ振る舞い（`js_parse_error` は無条件に `SyntaxError`、先読みの `XXX` と `push_scope` の無検査が残る）。

## 8. 並行して直したもの（2026-09-23）

§6 の各項目を、難易度ごとに別々のエージェント（Sonnet / Opus / Fable）へ割り当て、それぞれ別の
ワークツリー・別ブランチで調査・修正させ、`vm/oom-truncated-bytecode` へ 1 本ずつ統合した。
各ブランチは修正前後の分類と `oom_sweep.sh`・`run.sh` を自分で取り、統合側で差分を読んでから
取り込んだ。**上流 quickjs-ng master（2026-09-23 取得）は、以下のどれも同じコードのまま。**

### 8.1 初期化時の OOM で組み込みが黙って欠ける（`vm/oom-fix-initoom`）

`JS_NewContext` は `JS_AddIntrinsic*` の戻り値しか見ず、その中の大量のプロパティ定義
（`JS_SetPropertyFunctionList`、各 `JS_DefineAutoInitProperty`、エラー型のプロトタイプ…）は結果を
捨てていた。**初期化区間 1,238 点のうち約 400 点で、組み込みがどれか欠けたままプログラムが最後まで
走っていた**（例外として表に出たのは 2 点だけ）。

修正: すべての確保拒否が通る OOM カナリアの件数を、コンテキスト構築の前後で比べ、動いていたら
コンテキストを解放して NULL を返す（数百箇所の個別検査ではなく1箇所）。件数は読むだけで消さない
ので、ホストのターンごとの `JS_TakeOOMCanary` には影響しない。同じ症状の関連3件も直した:

- `JS_AutoInitProperty`: 遅延生成の組み込みを、結果を見る前に `undefined` で上書きしていた
  （generators.js の 2877 で `Generator.prototype.throw` が消える）。失敗時はスロットをそのまま残し、
  次のアクセスで作り直す。
- `JS_NewContextRaw`: 早い失敗で、GC リストに繋がったままのコンテキストを解放していた。
- `js_std_init_handlers`: 確保失敗で **`exit(1)`（実機では再起動）**、終了処理の登録失敗で 160 B の漏れ。

`pocketjs_guest_create` と vmrun は、`js_std_add_helpers` の後にもカナリアを見て、失敗した
ゲストを `ESP_ERR_NO_MEM` で断る。

### 8.2 Error が作れないと `null` を投げていた（`vm/oom-fix-ctornull`）

`JS_ThrowError2` は、Error オブジェクト自体の確保に失敗すると素の `null` を投げていた。ゲストの
`catch (e) { e.constructor... }` が OOM ではなく `cannot read property 'constructor' of null` に
なる（generators.js の 6 点、すべて実行中）。`JS_ThrowOutOfMemory` 経由にし、その
`in_out_of_memory` 旗で再帰を 1 段に抑える。本当に枯渇していればこれまでどおり `null`。

### 8.3 正規表現のコンパイラ（`vm/oom-fix-regexp`）

同じ `DynBuf` で bytecode を組むのに一度も掃引されていなかった。新しいコーパス `regexp_oom.js`
（6,240 確保）を全点掃引して 4 件 → 0: `|` のジャンプを記録位置へ失敗後に書き戻す書き越し、
名前付きグループ表の追記失敗でも旗を立てる読み越し、失敗後も再帰を続けて NULL を `memmove`、
古い文字列を残す 2 つの失敗経路。再帰パーサの共通入口（`lre_check_size`）で `dbuf_error` を見る。
`lre_compile` は OOM を `*plen = -1` で知らせ、`js_compile_regexp` はそれを `InternalError` にする
（上流は正規表現のコンパイル失敗をすべて `SyntaxError` にする）。

### 8.4 OOM が別の例外・黙った誤りとして出る残り（`vm/oom-fix-silentundef`）

正規表現の掃引で見つかった「例外のプロトタイプが無い」「`rx.flags` が例外なしで `undefined`」は、
失敗した確保の C スタックを取ると**どちらもコンテキスト初期化の中**で、§8.1 で閉じていた
（プロトタイプが欠けたまま作られていた）。実行時のプロパティ読み出しに不具合は無かった。
残りは別の原因で、次を直した:

- `JS_EvalFunctionInternal`: `js_closure` の失敗を `JS_CallFree` に渡し、保留中の OOM を
  `not a function` で上書きしていた（すべてのスクリプトと直接 `eval` が通る）。
- `resolve_pseudo_var`: 「`this` の束縛が無い」と「`add_var` の確保失敗」を同じ -1 で返し、OOM で
  `this` を**グローバル読み**としてコンパイルしていた（§3.5 と同じ黙った誤コンパイル）。確保失敗は
  -2 で区別する。`add_eval_variables` が捨てていた `add_var` 系の結果 14 箇所も検査。
- 正規表現の名前付きグループ表は 2 本目の `DynBuf` で書き込みが無検査だった
  （`groups === undefined` のままコンパイルされる）。メッセージ無しで失敗する 2 つの補助関数も直した。

### 8.5 `yield_then_handler.js` のタイムアウト（`vm/oom-fix-pollhang`）

テスト側の問題。末尾の関数が 5 件そろうまで自分を再登録し続けるので、OOM で 1 件欠けると永久に
待つ。1,000 回で諦めて例外を投げるようにした（通常は 9 ジョブで終わるので出力は変わらない）。
vmrun の暴走ガードは既定で無効（使うファイルがヘッダで要求する）なので、既定を変えずテストを直した。

## 9. 統合後の最終検証（2026-09-24、`09d3973` + 本文書、実測(host)）

§3〜§8 のすべてを統合した `vm/oom-truncated-bytecode` で取り直した。

| 検査 | 結果 |
| --- | --- |
| `oom_sweep.sh -n 3000`、コーパス 64 ファイル（192,000 点） | **0 件**（§5.1 で残っていたタイムアウト 14・abort 4 も含めて 0） |
| `oom_sweep.sh -n 6240 regexp_oom.js` | **0 件** |
| 全点の例外の種類、closures（4,800）/ generators（4,800）/ regexp_oom（6,240） | `InternalError: out of memory` と「例外なし」だけ（regexp_oom はほかに実行時の `out of memory in regexp execution` 58 点）。**`SyntaxError` / `TypeError` / `ReferenceError` は 0** |
| `run.sh`（o2、o2 `--force-yield`、asan） | 76/76、76/76、76/76 |
| Test262 標準集合（o2、通常 / `--force-yield`） | 7,501 / 194 / 0、**基準と同一**、退行 0 |
| `test_dbuf_sticky.c` | pass |
| 実機ビルド（書き込みはしていない） | 通る。DIRAM ±0、Flash **+652 B**（`vm/main` の既定ビルド 1,402,876 → 1,403,528） |

「例外なし」は、落とした確保がプログラムの確保より後だった点、プログラムが自分で扱った点、そして
§8.1 でコンテキストの構築が OOM で断られた点（vmrun は `guest setup failed: out of memory` を出し、
`XxxError:` の行を出さない）を含む。

**実機では1度も走らせていない。** `vm/main` へ戻す前に smoke と `memlog --check` を実機で通す。
