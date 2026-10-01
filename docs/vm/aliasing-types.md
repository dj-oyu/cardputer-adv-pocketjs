# int と int32_t の食い違いを型で直す（strict aliasing の残り）— 2026-09-30

[spread-eval-oom.md](spread-eval-oom.md) §2 の「同じ種類の箇所」の始末。ユーザー決定は **(1) 型を合わせる + (2) 警告の抑止を外す**。`-fno-strict-aliasing` は入れていない（未決定）。

## 1. 何が危ないか

Xtensa の newlib では **`int32_t` は `long`、`uint32_t` は `unsigned long`**。`int` と `long` は同じ32ビットでも別の型なので、`long *` 経由の書き込みは `int` の変数を変えられない、とコンパイラは仮定してよい。`js_string_iterator_next` はこれで `it->idx = idx` が消えた（`f937388`）。host（x86-64・i386）では `int32_t` が `int` なので、この種の誤りは host の試験では**原理的に**出ない。

## 2. 直した箇所

`components/quickjs-ng/CMakeLists.txt` の `-Wno-incompatible-pointer-types` を外して Xtensa 向けにビルドし、警告を一覧にした（GCC 15.2、`esp-15.2.0_20251204`）。

| 箇所 | 関数（到達経路） | 食い違い | 直し方 |
| --- | --- | --- | --- |
| quickjs.c `find_line_num` | `e.stack`（`build_backtrace`）、`Error.prepareStackTrace` の callsite（`js_new_callsite_data`）。3か所にインライン展開 | `int v` を `get_sleb128(int32_t *)` へ | `v` を `int32_t` に |
| `js_parseInt` | `parseInt(s, radix)` | `int radix` を `JS_ToInt32(int32_t *)` へ | `radix` を `int32_t` に |
| `remainingElementsCount_add` | `Promise.all` / `allSettled` / `any` の残数 | `int` を `JS_ToInt32Free(int32_t *)` へ | `int32_t` に |
| `js_promise_all_resolve_element` | 同上の要素ごとの解決（添字） | `int index` を `JS_ToInt32` へ | `index` を `int32_t` に |
| `js_atomics_notify` | `Atomics.notify(ta, i, count)` | 逆向き: `int32_t count` を `JS_ToInt32Clamp(int *)` へ | `count, n` を `int` に |
| libunicode.c `unicode_normalize` / `to_nfd_rec` / `sort_cc` | `String.prototype.normalize`、`localeCompare` | 作業配列を `dbuf_put_u32`・`memcpy`（`uint32_t`）で書き、`int *` で読み書きし、`uint32_t *` で返す。キャストで警告は出ない | 作業配列を最初から最後まで `uint32_t *` に（符号位置は21ビットなので比較は変わらない） |

どれも宣言の型だけを変え、式は変えていない。上流との差分は quickjs.c +12/−5 行（半分はコメント）、libunicode.c +22/−14 行。

**機械語は修正前と同一だった。** 修正前後の `quickjs.c.obj` / `libunicode.c.obj` を関数ごとに逆アセンブルして比べると、違いは `assert` に渡す `__LINE__` の定数だけ（行を足したため。4関数でその定数の命令の形が変わる）。つまり**今のビルドでは6箇所とも誤コンパイルされていなかった**（型の正しいコードと同じ機械語）。直したのは、インライン展開や GCC の版が変わったときに `f937388` と同じ消え方をしないため。

## 3. 警告の抑止

`-Wno-incompatible-pointer-types` は `quickjs.c` `libunicode.c` `libregexp.c` `dtoa.c` から外した。GCC 14 以降はこれがエラーなので、同じ種類の混入はビルドで止まる。

**`quickjs-libc.c` だけ抑止を残した。** 警告は16か所（`js_std_exit` `js_os_open` `js_bjson_read` ほか、すべて `int` を `JS_ToInt32` へ）で、どれも std / os / bjson モジュールの関数。ファームはそのモジュールを登録せず、ELF に入っているのは `js_std_add_helpers` `js_std_dump_error` などの6関数だけ（`nm` で確認）で、16か所のどれも入っていない。`JS_ToInt32` は別の翻訳単位（quickjs.c）にあり LTO も無いので、入っていたとしても aliasing で消える形にはならない。上流の差分を増やさないために残した。

`-Wno-strict-aliasing`（警告だけ）はそのまま。

## 4. 検証

- サイズ: DIRAM 172,540 → 172,540 B（+0）、Flash Code 1,530,112 → 1,530,112 B（`tools/memlog.py`、`idf.py size`。実測）。
- host: `tools/kasane_contract/run.sh`（`GAMES_M32=0`）、`tools/build_app_load_test.sh`: すべて PASS（MEGADEMO_HOST、LCDCATCH / DERBY / BIGWAVE_HOST、ODDS_CHECK、APP_LOAD_OK 32 checks ほか）。host では `int32_t` が `int` なので、これは「挙動を変えていない」ことの確認で、aliasing の検査にはならない。
- 実機（heapprobe、`apps/heapprobe/heapprobe.js` の `alias-*`。修正後の image、2026-09-30）: 9変種すべて評価 ok。出力は host の `vmrun-o2`（`int32_t` が `int` なので aliasing の問題が起きない参照）と一字一句一致。

  | 変種 | 通る箇所 | 実機の出力 |
  | --- | --- | --- |
  | `alias-parseint` / `alias-parseint2` | `js_parseInt` | `255,3,ff` / `35,NaN,31,12,-127,NaN` |
  | `alias-promise-all` / `alias-promise-settled` | `remainingElementsCount_add`、`js_promise_all_resolve_element`（all / allSettled / any / 空配列） | `1,2,3` / `frf 5 0` |
  | `alias-stack-line` / `alias-stack-line2` | `find_line_num`（`e.stack` と `Error.prepareStackTrace` の callsite） | `user.js:3:7` / `f (2:1) g (5:23) <eval> (6:19) \| 2:1,5:23,8:19` |
  | `alias-normalize` / `alias-normalize2` | `unicode_normalize`（NFD、合成、結合文字の並べ替え、ハングル、NFKC、`localeCompare`） | `2 1 233` / `233 61.323.301 3 d55c fi 0 1e69` |
  | `alias-atomics` | `js_atomics_notify` | `0,0,0,0`（待ち手が居ないので `count` は結果に出ない。この箇所は機械語の一致だけが根拠） |

  `alias-stack-line2` の最内フレーム `f` が本体の行（3行目）でなく関数の先頭 `2:1` を指すのは host でも同じで、aliasing とは関係ないエンジンの挙動（未調査）。
- 実機の回帰（通常 image）: `smoke_device.py --cycles 20` SMOKE_OK、`stress_app.py` PASS、`test_app_resume.py` OK、MEGADEMO・LCD CATCH・DERBY WATCH・BIG WAVE を起動して20秒フレームが進みホームへ戻る（致命ログ・評価エラーなし）。音量などの設定は変えていない。
