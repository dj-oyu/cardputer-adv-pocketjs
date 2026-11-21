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

## 2. pocket の挙動を残したところ

- **InternalError（out of memory を含む）は JS から捕まえられる。** 上流 90aa2c4・db0fe52 はこれを捕まえられなくした。固定のヒープで確保に失敗したアプリが捕まえて続けられることが、OOM まわりの硬化（[oom-parse-safety.md](oom-parse-safety.md)、`tools/vmtest/corpus/memory_device.js` と `oom_*`）の前提なので、fork の既定は捕まえられる方。`-DJS_UNCATCHABLE_INTERNAL_ERRORS=1` で上流の挙動になる。"interrupted" はどちらでも捕まえられない。
- **`JSStackFrame` は 48 B のまま。** 上流の `cur_gc_obj` は入れず、pocket の `coro_kind` で持ち主を求める。上流が `CallSite.isConstructor` のために足した `is_constructor` は、最後の詰め物の 1 バイト（FLATCALLS で offset 39）に置いた。共有の prologue では `flags` が呼び出し側の値のことがあるので、C からの入口と flat call でそれぞれ書く。
- **関数のソースは捨てたまま。** 上流が `using` のために書き換えた関数本体の構文解析でも保存しない。
- **ArrayBuffer の transfer。** v0.18 で外部メモリの API が `free_func` から `realloc_func` になった（`JS_NewArrayBuffer` の引数も変わる）。Shiki の窓は `realloc_func = NULL`（誰も解放しない）で作り、その種類の ArrayBuffer だけを transfer できなくした（[shiki-spec.md](../kasane/shiki-spec.md) §9.3）。`realloc_func` が所有するものは上流どおり transfer できる。

## 3. マージで直した不具合

host の試験で見つけたもの。どれもマージの衝突の外にあった。

- **`hash_atom` の幅。** v0.18 は shape のプロパティの位置を `uintptr_t` の幅の乗算ハッシュで決める（cad311e）。64 bit の host と 32 bit の実機で別のバケットになり、host で作る ROM の shape の表が実機で引けない。fork はどの host でも 32 bit の式にした。
- **pocket の 2 か所が古いハッシュのまま。** 遅延組み込みの並べ直し（`lazy_reorder`）と ROM の生成器（`tools/vmtest/floor/gen_rom_intrinsics.c`）が `atom & mask` で表を作っていた。前者は `Array.prototype` を全部展開した後の引き当てをすべて外し（for-in と `Object.keys` が壊れる）、後者は ROM 版のコンテキストで組み込みのメソッドを全部 `undefined` にした（実機の構成。r5sim の hot/cold の学習で見つかった）。
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
