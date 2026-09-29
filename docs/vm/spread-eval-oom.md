# 文字列の反復が止まらない（Xtensa の strict aliasing）— 2026-09-30

DERBY WATCH の1文 `const GK = [...'adesr1,/', 'tab'];` が、実機の評価中にゲストのヒープを使い切った（`OOM first_req=30 used=163,848`）。host の QuickJS では再現しなかった。原因は **QuickJS の文字列イテレータの1行が、実機のコンパイラで消えていた** こと。VM の改造（L1〜L3）とは無関係で、上流の quickjs-ng のコードに、Xtensa の型でだけ効く未定義動作があった。修正はコミット `f937388`（1関数、ソースの意味は不変）。

## 1. 原因（特定済み。逆アセンブルと実機で確認）

`components/quickjs-ng/quickjs-ng/quickjs.c` の `js_string_iterator_next`:

```c
uint32_t idx;
...
c = string_getc(p, (int *)&idx);   /* string_getc は *pidx を int として書く */
it->idx = idx;
```

- ESP-IDF の Xtensa newlib では **`uint32_t` は `unsigned long`**。`int` と `unsigned long` は別の型で、`int *` 経由の書き込みは `unsigned long` の変数を変えられない（C の strict aliasing 規則）。GCC -O2 はこれを使って「`idx` は `string_getc` の前後で変わらない」と判断し、`it->idx = idx` を「読んだばかりの値を書き戻すだけ」として**消した**。
- 修正前の逆アセンブル（`xtensa-esp32s3-elf-objdump -d --disassemble=js_string_iterator_next`）: `call8 string_getc` の後に `it->idx`（`a7+12`）への `s32i` が無い。修正後は `l32i.n a8, a1, 0` → `s32i.n a8, a7, 12` が戻っている。
- 結果: 文字列のイテレータが**進まない**。`'ab'[Symbol.iterator]()` は `'a','a','a',…` を返し続け、`done` にならない。
  - スプレッド `[...s]`、`Array.from(s)`、`Math.max(...s)` → 1文字の文字列を配列に積み続けて、ヒープの上限で OOM（`first_req=30` は配列の伸長か 1 文字の文字列）。
  - `for (const c of s)` → 何も積まないので無限ループになり、ウォッチドッグで `InternalError: interrupted`。
  - `const [a, b] = 'ab'` → **黙って `'a','a'`**（止まらないが値が違う）。
  - 空文字列だけは最初の `next` で終わるので正常。
- host で再現しなかった理由: x86-64 と i386（m32 ビルド）では `uint32_t` は `unsigned int` で、`int` と符号違いの同じ型なので aliasing が許される。**ホストの型がファームの型と違う場所は、テストが嘘をつく**（CLAUDE.md の `test_solar_time.c` と同じ種類）。host の vmtest・m32 ビルド・Node はどれもこの差を再現できない。
- 評価中か `frame()` 中かは無関係（§3 の表）。DERBY WATCH で評価中に見えたのは、その文がトップレベルにあったから。

## 2. 修正（コミット `f937388`）

`int` の一時変数を通す（`int next = (int)idx; c = string_getc(p, &next); it->idx = (uint32_t)next;`）。バイトコードも意味も不変。host のコーパス（vmtest o2）80 件合格。実機では §3 の全変種が正しい値で通る。

### 同じ種類の箇所（未修正。実機では今は正しく動く）

`-Wno-incompatible-pointer-types` を外して Xtensa 向けにコンパイルすると、`int *` を `int32_t *`（Xtensa では `long *`）に渡す箇所がさらに6つ出る。GCC 15 ではこれはエラー（permerror）で、`components/quickjs-ng/CMakeLists.txt` の抑止で隠れている。

| 箇所 | 関数 | 実機の確認（heapprobe） |
| --- | --- | --- |
| quickjs.c 9176, 9187 | `find_line_num`（`get_sleb128(&v)`、スタックの行番号） | `alias-stack-line`: `user.js:3:7` 正しい |
| 50541 | `js_parseInt`（`radix`） | `alias-parseint`: `255,3,ff` 正しい |
| 60034, 60064 | `Promise.all` の残数・添字 | `alias-promise-all`: `1,2,3` 正しい |
| 67060 | `js_atomics_notify`（`count`） | 未確認（Atomics は使っていない） |
| libunicode.c 959〜1056 | 正規化の作業配列（`int *` と `uint32_t *`） | `alias-normalize`: `2 1 233` 正しい |

今は正しいが、インライン展開が変われば同じ消え方をしうる（今回の行も、上流で長く動いていたコード）。**判断はユーザー**: (1) 6 箇所の型を合わせる（各1語）、(2) quickjs.c から `-Wno-incompatible-pointer-types` を外して、以後の混入をビルドで止める、(3) quickjs.c だけ `-fno-strict-aliasing`（網羅的だが、インタプリタの速度への影響は未測定）。

## 3. 再現アプリ（`POCKET_HEAPPROBE`、既定 OFF）

`apps/heapprobe/heapprobe.js` の `//@ 名前` ごとに別のソース。ホームで USB `Q<番号>[,<上限>]\n` を送ると、メニューのアプリと同じ `begin_run` の経路で新しいゲストに評価され、`HEAPPROBE before/eval ... used=` と `HEAPPROBE_MU`（`JS_ComputeMemoryUsage` の種別）がログに出る。`tools/heapprobe_device.py` が全変種を回し（`--only`、`--limit`）、`--bisect N` で「評価できる最小のヒープ上限」（評価のピークを実機の課金で測ったもの）を二分探索する。通常の image には何も入らない（map に `heapprobe` の記号が無いことを確認）。

```powershell
idf.py -B build_hp -DPOCKET_HEAPPROBE=ON build
idf.py -B build_hp -p COM3 flash
python tools\heapprobe_device.py --port COM3
```

実機の結果（2026-09-30、同じ機体。`before` は評価前のゲスト使用量 約34.2 KB）:

| 変種 | 修正前 | 修正後 |
| --- | --- | --- |
| `[...'adesr1,/', 'tab']`（トップレベル・IIFE・アロー IIFE） | OOM（`first_req=30`） | ok（9） |
| `[...'adesr1,/']`、`[...'abc']`、1/30/100 文字 | OOM | ok |
| `[...'']` | ok | ok |
| `Array.from('adesr1,/')` | OOM | ok |
| `Math.max(...'123')` | OOM | ok |
| `for (const c of 'adesr1,/')`（トップレベル・IIFE） | `InternalError: interrupted` | ok |
| `const [p, q] = 'ab'` | **ok だが `aa`** | `ab` |
| `it.next()` 3回 | **`a`,`a`,done=false** | `a`,`b`,done=true |
| `'adesr1,/'.split('')` | ok | ok |
| `[...arr]`、`[...new Set()]`、`[...arr.values()]`、ジェネレータ | ok | ok |
| `Object.keys/entries`、テンプレート、`Math.max(...[1,5,3])` | ok | ok |
| `frame()` の中の文字列スプレッド | OOM（frame 中） | ok |
| `frame()` の中の文字列 for-of | 止まる | ok |
| DERBY WATCH（出荷版） | ok | ok |
| DERBY WATCH（GK をスプレッドに戻した版） | OOM（`used=163,848`） | ok |

評価中と `frame()` 中の違いは無い。違ったのは値（文字列か否か）だけ。

## 4. アプリ作者へ

この修正の入った image では、文字列のスプレッド・`for...of`・分割代入は普通に使ってよい。修正前の image（`vm/main` の `ff7d809` 以前）で動かす必要があるなら、文字列を反復するところは `s.split('')` か添字のループにする（DERBY WATCH は `split(' ')` にしてある）。評価のピークを下げる書き方は [`eval-peak.md`](eval-peak.md) §6。
