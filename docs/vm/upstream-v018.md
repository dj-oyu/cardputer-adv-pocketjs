# 上流 quickjs-ng v0.18.0 の取り込み

[VMの文書](README.md)

2026-11-21。fork [dj-oyu/quickjs](https://github.com/dj-oyu/quickjs) の `claude/hopeful-heisenberg-7wu34t` に、上流 v0.18.0（v0.14.0 から 202 コミット）をマージした。fork の範囲は `cd51ea7..55b82c7`。

## 1. arena アロケータは取り込まない

v0.18 の 9de2921（Add arena allocator）は、512 B 以下の確保を 4 KB の size class 別ページから切り出し、`ref_count`・`mark`・`gc_obj_type` をすべての確保の前の 8 B のヘッダに移す。160 KiB のゲストには載らない。

**実測（host、32 bit・`-malign-double`、2026-11-21）。** 裏のアロケータが渡したバイトを TLSF と同じ数え方（4 B 単位、最小 12 B、ヘッダ 4 B）で数えるハーネスで、スクリプトが完走できる最小のヒープを二分探索した。上流の `--memory-limit` は v0.18 では arena の中のブロックしか数えず、ページの空きを数えないので使わなかった。

| 作業負荷 | v0.14 | v0.18 | v0.18、`JS_ARENA_LARGE_BLOCKS_ONLY` | v0.18 から 9de2921 を戻したもの |
| --- | ---: | ---: | ---: | ---: |
| 空のスクリプト | 64,767 | 156,669 | 74,238 | 69,630 |
| 3,000 個のオブジェクト | 429,305 | 523,768 | 474,872 | 434,425 |
| 2,000 個のクロージャ | 332,538 | 412,409 | 358,138 | 337,402 |
| Float64Array 300 本 | 171,005 | 269,051 | 192,509 | 176,125 |
| vmtest `generators.js` | 90,110 | 186,109 | 104,446 | 99,070 |

arena は空のスクリプトで約 +90 KB（約 24 の size class が 1 ページずつ持つ）、小さい作業負荷で +20〜140%。大きいブロックだけにしてもヘッダで +8〜19%。戻したものは v0.14 の +1〜10% で、その分は v0.18 が足した組み込み（`DisposableStack`、`SuppressedError`、`atob`/`btoa`、`Iterator.prototype` の新メソッドなど）。

fork では 9de2921 を v0.18.0 に対して戻したもの（`89df4c3`、整形して `e08d22d`）をマージした。後から arena のマクロを使っていた 7955cfd と 849220c は残し、マクロの代わりにヘッダの欄を直接使う。上流の tests.conf（0/122）と test262（40/82,220、同じ 40 件）は arena の有無で変わらない。

## 2. kobako の挙動を残したところ

- **InternalError（out of memory を含む）は JS から捕まえられる。** 上流 90aa2c4・db0fe52 はこれを捕まえられなくした。固定のヒープで確保に失敗したアプリが捕まえて続けられることが、OOM まわりの硬化（[oom-parse-safety.md](oom-parse-safety.md)、`tools/vmtest/corpus/memory_device.js` と `oom_*`）の前提なので、fork の既定は捕まえられる方。`-DJS_UNCATCHABLE_INTERNAL_ERRORS=1` で上流の挙動になる。"interrupted" はどちらでも捕まえられない。
- **`JSStackFrame` は 48 B のまま。** 上流の `cur_gc_obj` は入れず、kobako の `coro_kind` で持ち主を求める。上流が `CallSite.isConstructor` のために足した `is_constructor` は、最後の詰め物の 1 バイト（FLATCALLS で offset 39）に置いた。共有の prologue では `flags` が呼び出し側の値のことがあるので、C からの入口と flat call でそれぞれ書く。
- **関数のソースは捨てたまま。** 上流が `using` のために書き換えた関数本体の構文解析でも保存しない。
- **ArrayBuffer の transfer。** v0.18 で外部メモリの API が `free_func` から `realloc_func` になった（`JS_NewArrayBuffer` の引数も変わる）。Shiki の窓は `realloc_func = NULL`（誰も解放しない）で作り、その種類の ArrayBuffer だけを transfer できなくした（[shiki-spec.md](../kasane/shiki-spec.md) §9.3）。`realloc_func` が所有するものは上流どおり transfer できる。

## 3. マージで直した不具合

host の試験で見つけたもの。どれもマージの衝突の外にあった。

- **`hash_atom` の幅。** v0.18 は shape のプロパティの位置を `uintptr_t` の幅の乗算ハッシュで決める（cad311e）。64 bit の host と 32 bit の実機で別のバケットになり、host で作る ROM の shape の表が実機で引けない。fork はどの host でも 32 bit の式にした。
- **kobako の 2 か所が古いハッシュのまま。** 遅延組み込みの並べ直し（`lazy_reorder`）と ROM の生成器（`tools/vmtest/floor/gen_rom_intrinsics.c`）が `atom & mask` で表を作っていた。前者は `Array.prototype` を全部展開した後の引き当てをすべて外し（for-in と `Object.keys` が壊れる）、後者は ROM 版のコンテキストで組み込みのメソッドを全部 `undefined` にした（実機の構成。r5sim の hot/cold の学習で見つかった）。
- 遅延の Uint8Array に base64/hex のメソッドが付かない、DOMException の 2 度目の登録で abort する、遅延の DOMException の `length` が 2 のまま、ROM 版のコンテキストが消えた `js_random_init` を呼ぶ（実機のビルドだけが通る経路）。
- `to_propkey_obj`（最後の命令）を外した。上流 0db3949 が代入の ToPropertyKey を右辺の後にしたので、使う場所がなくなった。

## 4. 結果

- host: tests.conf 0/122、api-test 合格、test262（v0.18 の pin）60 件失敗のうち、マージ前の fork が通るものは 0 件（844 件が新たに通る）。vmtest コーパス 88/88（ASan）。ROM 版（`asan-rom`、`--harden`）は 16 件落ちるが、マージ前と同じ 16 件で中身も同じ（列番号の 2 件を除く）。
- vmtest の期待出力を更新したもの: `generators`・`yield_async_from_sync`（上流 d5a59c3・f2da139: for-await の break で `return()` を待つ）、`lazy_builtins`・`lazy_intrinsics`（新しい組み込み、DOMException の `name`/`message`/`code` が列挙可能なアクセサに）、`regexp_oom`（`v` フラグが使える）。`--fail-alloc` の番号は 4 件とも振り直し、`oom_targets.sh` と修正を戻したコピーで ASan に当たることを確かめた（約 +39〜45 回、増えた atom と組み込みの分）。
- ファーム（ESP-IDF 6.0.1）: イメージ 2,635,184 B（v0.14 版より約 +33 KB）、DIRAM 172,856 B（−24 B）。hot/cold は学習し直して quickjs.c 191 関数・不一致 0。

## 5. 実機（2026-11-21、ブローカー、同じ機体で v0.18 → v0.14 の順）

v0.18 は 50b5fe4e（fork 55b82c7）、v0.14 は f5fe594a（fork cd51ea7）。各 30 秒、`KASANE_PAINT turn_ms` の平均（最初の 1 本を除く）。

| | v0.14 | v0.18 | 差 |
| --- | ---: | ---: | ---: |
| STRESS turn_ms | 5.361 | 5.436 | +1.4% |
| DERBY turn_ms（パドック） | 2.99 | 3.03 | +1.3% |
| DERBY 読み込み後の js | 91,232 | 92,604 | +1,372 B |
| MEGADEMO | 起動（`MEM free=32892` まで） | **起動失敗**: `chunk 'wake' did not compile: out of memory` | |
| `GUEST_ARENA`（MEGADEMO 起動時） | 65,536 | 57,344 | −8 KiB |
| ホームの空き（`MEM free`、前のアプリ停止時） | 164,512 | 163,760 | −752 B |

- **MEGADEMO が起動しない（退行）。** arena は起動時の空きで 8 KiB 刻みに決まる（`guest.c`、空き ≥ 希望 + 56 KiB）。v0.18 はゲストが約 1.1〜1.4 KB 多く（host の床 24,792 → 25,948 B、`floor32.sh`）、起動時の空きが約 750 B 少なく、段が 64 → 56 KiB に下がった。v0.14 でも MEGADEMO は読み込み直後に空き 0 の MEM_PRESSURE を出しており、余裕はもともと無かった。
- **速度の +1.3〜1.4% は原因未特定。** 命令キャッシュの配置だけでビルド間 15% 動く機体なので、この差はまだエンジンのせいと言えない。候補は shape のハッシュ（`hash32(atom) >> shift`、v0.14 は `atom & mask`）、呼び出しごとの `is_constructor` のストア、コードの増加（+33 KB）による flash キャッシュのヒット率の低下。XTPERF で命令数とキャッシュ待ちを分けて測る。
- 機器は計測後に元の LKG（a806b21f）へ戻した。

## 6. デバッグオフとハッシュ（2026-11-21、fork `0d281e1`・`cbf7803`）

- **デバッグの切り替えを 1 つにした。** ファームはアサーションを切る（`sdkconfig.defaults` の `CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_DISABLE`、`NDEBUG`）。fork は `NDEBUG` のとき行番号表を作らず、`OP_source_loc` も出さない（`JS_NO_LINE_TABLES`）。それまでの strip は最後の表を省くだけで、1 段目には `OP_source_loc`（9 B）を全部書いて捨てていた。v0.18 の d90ab51 は識別子を読むたびにそれを出すので、MEGADEMO のコンパイルのピークが 134,064 → 149,256 B に上がっていた（host m32、TLSF）。出さないと 132,432 B。アプリの `option strip-debug`（MEGADEMO・DERBY）は外した（仕組みは残る）。host のビルドは `NDEBUG` を付けないので、表を作る。
- **アサーションなしのファーム**（ローカルのビルド）: イメージ 2,635,184 → 2,500,951 B（−134 KB）、DIRAM 172,856 → 161,819 B（−11.0 KB）。`assert()` の中に副作用のある式は無い（全部、値を調べるだけ）が、`main.c` が入力タスクと UI タスクを `configASSERT(...)` の中で作っていて、アサーションを切ると生成ごと消えた（起動はするが `HOME_READY` が出ない。実機で見つけて `5ac61d36` で直した。生成してから `abort()` で確かめる）。r5sim（hot/cold の学習）も同じ設定にした。
- **shape のハッシュは AND に戻した。** v0.18 の cad311e は `hash32(atom) >> shift`。r5sim（QEMU icount、同じ木・同じプロファイル、デバッグオフ）で DERBY 100,139 → 99,562 サイクル/ターン（−0.58%）、HUD 10,941 → 10,853（−0.80%）、Shiki 144,614 → 144,340（−0.19%）。atom は連番の索引なので AND で偏らない。
- hot/cold は最終の構成で学習し直した（quickjs.c 190 関数・不一致 0）。
- **デバッグは必要なときだけオンにする**: `sdkconfig.debug.defaults`（アサーションあり、行番号表あり）を重ねた別ディレクトリでビルドする。コマンドはファイルの先頭。

### 実機（2026-11-21、5ac61d36、同じ機体。v0.14・v0.18 は §5 の値）

| | v0.14 | v0.18 | デバッグオフ＋AND |
| --- | ---: | ---: | ---: |
| STRESS turn_ms | 5.361 | 5.436 | **5.317**（v0.14 比 −0.8%） |
| DERBY turn_ms（パドック） | 2.99 | 3.03 | **3.00**（+0.2%） |
| DERBY 読み込み後の js | 91,232 | 92,604 | **91,264** |
| DERBY 読み込み後の空き（`MEM free`） | 53,508 | 52,392 | **63,152**（+9.6 KB） |
| MEGADEMO | 起動 | 起動失敗 | **起動**、`GUEST_ARENA` 65,536 |
| MEGADEMO 読み込み後の空き | 32,892 | — | **41,732**（+8.8 KB） |
| MEGADEMO act2 のコンパイル | 80.0 ms | 85.8 ms | **71.1 ms** |
| ホームの空き（起動直後、player の後） | — | 168,960 | **179,328** |

速度は v0.14 と同等〜わずかに速い（15% の配置の揺れの内側なので「同等」と読む）。メモリは v0.14 より約 9 KB 多く空く（DIRAM −11 KB が効いている）。

## 7. A/B の検査の基準を fork に移した（2026-11-21）

`tools/vmtest/mixed_numeric/compare.py` と `tools/vmtest/function_allocation/compare_main.py` は、基準のエンジンをこのリポジトリの dd36a29（v0.14、submodule 化の前）から `git show` で取り出していた。v0.18 の `quickjs.h`（`JS_NewArrayBuffer`、割り込みのハンドラ）では、その `quickjs.c` がコンパイルできない（PR #137 の CI）。基準は fork の commit にして、submodule の履歴から読む（`tools/vmtest/engine_pin.py`）。

- **基準は fork `0d281e1`。** v0.18 のマージ（`55b82c7`、実機で測ったもの）に、strip のビルドで `OP_source_loc` を出さない変更を足したもの。`55b82c7` を基準にすると、function_allocation の `o2 --strip` で `oom_callsite_double_free` が別の出力になる（`--fail-alloc 1455` が狙う確保の前で、確保の回数が減るため。strip の変種は期待出力を強制しない）。意図した変更なので、dd36a29 の三角関数と同じく基準を進めた。後の `cbf7803`（AND のハッシュ）は確保の順を変えない。
- **v0.14 → v0.18 は一度だけ別に確かめた。** 古い木（pocketjs 86bbcf55、fork cd51ea7）と新しい木（fork cbf7803）で mixed_numeric（x64）を走らせ、`numeric.log`・BIG WAVE の `app-behaviour.log`・`panels.bin`（7,279 枚、471,679,200 B）がバイト単位で一致した。
- 同じ実行のゲストのヒープ（host 64 bit、TLSF のモデル）: 評価後 124,624 → 128,148 B、評価中のピーク 153,944 → 178,968 B。host は `NDEBUG` が無いので行番号表を作り、v0.18 の d90ab51 は識別子を読むたびに `OP_source_loc` を出す（§6）。デバッグオンの数字で、ファームの数字ではない。

## 8. fork の `main` に上げた（2026-11-21、fork `ff809dd`）

fork の作業ブランチを `main`（`v0.14.0` から切った kobako の系統。`master` は上流の写しのまま）へマージした（fork PR #1、マージコミット。`cbf7803` は `main` から辿れる）。上流の CI（130 件）を初めて通すために直したもの:

- **`JS_UPSTREAM_COMPAT`。** fork 自身のビルド（CMakeLists.txt・meson.build、amalgam の CI）だけが定義し、上流の挙動に戻す: どのビルドでも行番号表を作る（`NDEBUG` で落とさない）、関数のソースを保つ、`Math.sin/cos/tan` は libm。pocketjs は `quickjs.c` を自分でコンパイルし、これを定義しない。
- **`gen/*.c` を作り直した。** 上流の qjsc が上流の atom 番号で書いたバイトコードのままで、standalone と REPL が atom を別の atom として読んでいた。
- **`libunicode-table.h` の綴り。** ESP-IDF 形式への整形の綴り直しが、script の別名 `Beng`・`Maka` を `Being`・`Make` にしていた。`\p{Script=Bengali}`・`\p{Script=Makasar}` が実機でも「unknown unicode script」だった。このファームで直る。
- 使われない変数（`-Werror`）、`quickjs-vm.c` と kobako の見出しを amalgam と meson に、構造体の大きさの assert を実機の配置（32 bit・NaN boxing・int64 が 8 境界・GCC/Clang・Windows 以外）に限る、`ATOMIC_CHAR_LOCK_FREE` は tcc と MSVC で検査しない、`vmtest_seg_alloc_hint` の weak 参照は ELF だけ。
- CI の Windows（clang-cl + ASan の debug）は、上流 v0.18.0 でも同じく落ちていた（runner の Visual Studio 同梱の ASan DLL が先に読まれる）。リンクした DLL を実行ファイルの隣に置いた。

**実機のコードは変わらない。** このファームの `quickjs.c` を cbf7803 と ff809dd で同じコマンド（`build/compile_commands.json`）でコンパイルし、コードとデータのバイトが一致した（text 509,248 B）。hot/cold は quickjs.c 190 関数・不一致 0 のまま。変わるのは Unicode の表の 2 つの文字列だけ。

## 9. main のベースライン（2026-11-21、pocketjs `d3300996`）

PR #139（アプリ専用のネイティブを `main/apps/` へ）までを main に入れた後の、性能の作業の基準。§6 と同じ機体・同じブローカーの手順で、fetch → build → flash → 各アプリ 30 秒（入力なし、`start=fresh`）→ LKG へ戻す。すべて実機の実測。ログの抜粋は [`logs/baseline-main-20261121-d3300996.txt`](logs/baseline-main-20261121-d3300996.txt)。

| | §6（`5ac61d36`） | main `d3300996` |
| --- | ---: | ---: |
| STRESS turn_ms | 5.317 | **5.311**（28 窓、σ 0.066） |
| DERBY turn_ms（パドック） | 3.00 | **3.005**（14 窓、σ 0.018） |
| DERBY turn_ms（レース） | — | **5.39**（10 窓、σ 0.22） |
| DERBY 読み込み後の js | 91,264 | **91,276** |
| DERBY 読み込み後の空き（`MEM free`） | 63,152 | **65,340** |
| MEGADEMO | 起動、`GUEST_ARENA` 65,536 | 起動、`GUEST_ARENA` 65,536 |
| MEGADEMO 読み込み後の空き | 41,732 | **40,980** |
| MEGADEMO act2 のコンパイル | 71.1 ms | **71.5 ms** |
| ホームの空き（起動前） | 179,328 | **181,596〜182,244** |

- **速度は §6 と同じ。** fork の `main` への bump（§8、`quickjs.c` のコードは同一）とファイルの移動（セクション単位で同一）は実機のコードを変えないので、差は測定の揺れの範囲。
- **読み込み後の空きは DERBY +2.2 KB、MEGADEMO −0.75 KB と逆向きに動いた。** js（ゲストの使用量）はほぼ同じで、原因は確かめていない（ホームの状態・断片化の違いの可能性）。この差を変更の効果として読まない。
- MEGADEMO は読み込み直後に `MEM_PRESSURE mask=0x1`（`guest_free=25560`）を出す。§6 でも同じで、起動と描画は通る。
- MEGADEMO の turn_ms は場面で 3〜12 ms と大きく違うので、一つの平均にしない。場面ごとの値はログの抜粋。
- 同じ木の TURNPERF（JS のターンの性能カウンタの内訳）は [turn-cpi.md](turn-cpi.md) §6。

## 10. fork の dtoa/atod を 32 bit で動かした（2026-11-22、fork `b716bd1`）

fork の `main` に入った PR #2（NEON と 128 bit の dtoa/atod）は、`__int128` が要り、`ESP_PLATFORM` を除いていたので、この機体では従来の多倍長の dtoa が動き続けた。PR #5 が両方の制限を外し、64×64→128 の積を 32×32→64 の 4 本で組んだ（`dtoa.c` だけの変更）。gitlink を `b716bd1`（#5 のマージコミット）に上げた。

- **host（実機ではない）。** 移植前の `dtoa.c` と約 600 万件を比べた。`js_atod` はビット単位で同じ。`js_dtoa`（`FORMAT_FREE`、10 進）は 2 の冪 54 個（と負数）だけ違い、移植前が最短でない長い文字列を出していた（例 2^-1017）。全出力が元の値に戻る。`__int128` の経路と `JS_DTOA_NO_INT128` の経路は同じ結果。
- **コードの形（コンパイルの確認で、実行ではない）。** Espressif の `xtensa-esp32s3-elf-gcc`（`-Os -mlongcalls`）で `dtoa.o` の text は 6,310 → 18,965 B（+12.6 KB、うち 10.7 KB が 10 の冪の表で flash）。`js_dtoa`/`js_atod` に `mull`/`muluh` がインラインで入り、新しい経路からの `__muldi3`/`__udivdi3` は無い。
- **実機（各 1 回）。** 同じベンチを fork main `5cc5724`（A2）と `b85cc94`（B2）で走らせた。生ログは [logs/dtoa-schubfach-20261122.txt](logs/dtoa-schubfach-20261122.txt)。

| µs/回（空ループ 5.1 を含む） | A2 | B2 | |
| --- | ---: | ---: | ---: |
| `String(float)` | 74.9 | 62.8 | −16% |
| `parseFloat(str)` | 26.8 | 25.4 | −5% |
| `Number(str)` | 27.6 | 25.4 | −8% |
| `toFixed(3)`（変更の外） | 16.3 | 16.8 | +3% |
| `toUpperCase` 4 KB（変更の外） | 3353.1 | 3343.6 | −0.3% |
| `indexOf` 4 KB（変更の外） | 185.6 | 184.8 | −0.4% |

- **読み方。** 変更の外の項目は ±3% 以内。ビルドのアラインメントだけで同じコードが 15% 動くことがあるので、1 回ずつの 16% は幅の中に入る。しかしチェックサムは同じで、外の項目が動いていない。各値には JS 呼び出しの費用が入るので、dtoa の取り分はこれより小さい。PR #2 の 12 倍は Cortex-A53 の値で、この機体では `String(float)` が約 16%。`toFixed`/`toExponential`/`toString(radix)` は別の形式で、変わらない。
- **測っていないもの。** (1) gitlink の `b716bd1` は PR #3（配列の遅延コピー、`quickjs.c` +568 行）を含み、A2/B2 の木にはない。S3 のヒープと速度への影響は未測定。(2) `quickjs.c` が変わったので、hot/cold を学習し直す必要がある（`tools/qjs_hotcold/train.sh`）。未実施（2026-11-22 に確かめた: PR #4 で `JS_CallInternal`・`build_arg_list`・`js_function_apply` が外れ、今のファームでは `JS_CallInternal` が並べ替えられていない。fork main が落ち着いてから学習し直す。[pr8-cold-outline.md](pr8-cold-outline.md) §4）。(3) ファーム全体の flash の増分（+12.6 KB は `dtoa.o` 単体）。

## 11. fork の `main` `4492bd1` に上げ、改名し、hot/cold を学習し直した（2026-11-23、fork `c7d48fd`）

gitlink を `b716bd1` から fork の PR #6〜#11 の後（`4492bd1`）へ上げ、同時にプロジェクトを `pocket` から `kobako` へ、fork のオプションを `POCKET_VM_*` から `QJS_LEAN_*` へ改名した（`tools/rename/kobako_rename.py`、fork 側は `c7d48fd`）。そのあと `quickjs.c` と `main/` の hot/cold を学習し直した。

- **F2 は S3 では 1 のまま。** fork の PR #11 が `POCKET_VM_LAZY_BUILTINS`（今の `QJS_LEAN_LAZY_BUILTINS`）の既定を 0 にしたので、`components/quickjs-ng/CMakeLists.txt` と、`quickjs.c` をコンパイルする `tools/` の道具 37 本が 1 を渡す（ROM の atom の表の生成器だけは除く）。STRESS の起動直後のゲストは A・B とも `js=42800` で同じ（実測（実機））。
- **プロファイル。** `quickjs.c` は 192 関数がすべて当たる（外れ 0）。`main/` の側は `kobako_derby.c` 5・`ksn_shiki.c` 3 が外れ、改名前と同じ数（[qjs-hotcold.md](qjs-hotcold.md) §13.7）。今の main（A）の `JS_CallInternal` は、fork の PR #4 から並べ替えが外れていた（§10）。
- **改名だけの差は 0。** hot/cold を切って改名の前後を組むと、`.bin`・Flash Code・Flash Data・DIRAM がバイト単位で同じ（実測（静的））。

実機の A/B（TURNPERF、固定の種、焼き順 B → A → B）。A は今の main（`e0ecbd9`、fork `b716bd1`）、B は上の木（`e245a78`）。B の 2 回は DERBY・STRESS とも 0.03% 以内で一致し、3 回とも DERBY の着順・タイムが同じ。生ログは [logs/fork-c7d48fd-ab-20261123.txt](logs/fork-c7d48fd-ab-20261123.txt)。

| 実測（実機）、1 ターンあたり | DERBY A | DERBY B | 差 | STRESS A | STRESS B | 差 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| サイクル | 1,314,802 | 1,226,910 | **−6.7%** | 1,265,106 | 1,240,796 | **−1.9%** |
| 時間 | 5.58 ms | 5.20 ms | −6.7% | 5.49 ms | 5.38 ms | −1.9% |
| 退役命令 | 487,257 | 468,827 | −3.8% | 599,396 | 602,760 | +0.6% |
| `istall` | 519,084 | 469,493 | −9.6% | 418,815 | 385,651 | −7.9% |
| 振り分け（`insn_jx`） | 6,988 | 6,833 | −2.2% | 2,190 | 2,128 | −2.8% |
| `dstall` | 38,058 | 30,799 | −19.1% | 36,070 | 31,785 | −11.9% |

- **読み方。** 振り分けの減り（−2〜−3%）は fork の PR #6・#7 の比較と分岐の融合（2 命令を 1 回の振り分けで済ませる）、`istall` の減り（−8〜−10%）は `JS_CallInternal` の並べ替えが戻ったことと読む（推定。2 つを分けて測ってはいない）。どちらも配置の偶然では説明しにくい（命令と振り分けの数そのものが減っている）が、CLAUDE.md のビルド間の揺れ（15%）より小さいので、熱い領域をずらした像（[qjs-hotcold.md](qjs-hotcold.md) §7 の B''）では確かめていない。
- **測っていないもの。** DERBY の評価の余裕とゲスト最大（heapprobe）、smoke と設定の試験（`smoke_device.py`・`test_settings.py` はブローカーからは走らない）、STRESS 以外の出荷アプリ。
