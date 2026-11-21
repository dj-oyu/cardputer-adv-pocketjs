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
- 実機の速度・ゲストの床・評価の余裕は未測定。
