# C: パーサの余剰容量返却（host 検証）

2026-10-01。基点 `91801cf`、WIP `db1e55e`、作業枝 `vm/cloud-c-parser-shrink`。
WIP の製品コード 53 行は変更していない。マージ・push・実機アクセスなし。

## 安全性の境界

関数の解析終了時に bytecode / labels / vars / cpool の余剰容量を返す。
0 は解放、縮小失敗は元の pointer / capacity / ownership を保持する。
任意の縮小を通常の throwing realloc に通すと OOM canary / parser の失敗判定を
汚すため、既存の raw allocator 経路を直接使い、成功時だけ accounting と metadata を更新する。
後続の compile pass は配列を再成長させられる。errored DynBuf の所有 block は維持する。

## 実測(host64)

GCC 14.2.0、x86-64 Linux。`-fno-pie -no-pie`。

- 既存 semantic corpus: O2 80/80、ASan+UBSan 80/80、O2 forced-yield 80/80
- `parser_trim.sh`: O2 / ASan+UBSan とも pass
- helper 単体: 移動する shrink、縮小拒否、0 への解放、sticky DynBuf error、全4配列の再成長、既存の pending exception の保持、OOM generation / canary / accounting
- 実際の compile 5 ソース: closure、class/private fields/super/static initializer、direct eval / arguments、32 locals の hash table、generator/default/rest/finally
- 1,088 allocation 点を1回ずつ失敗させる全点 sweep: 989 点は compile 失敗を処理、302 回の shrink 拒否を観測
- 成功する各点では debug を除いた serialized bytecode が正常時と一致し、実行結果 42。各点の後で同じ runtime / context を再利用して `6*7 == 42`
- すべての runtime 破棄後に独立 allocator の live block / byte が 0
- 別の非 OOM 比較: `91801cf` とアプリ + corpus の **完全な** serialized bytecode（debug を含む）を比較。161 ファイル一致、8 ファイル同じ compile rejection、相違 0

8 rejection は構文エラーの probe 3 本と、アプリ専用 module alias をこの serializer が
解決しない 5 本。実行の成否を判定する corpus は別途 80/80 通している。
任意の OOM では元の debug table 作成が情報を落とす場合があるため、fault sweep の一致は
実行用部分に限定する。非 OOM の完全 bytecode 比較と混同しない。

再現:

```sh
VMTEST_CFLAGS='-fno-pie -no-pie' bash tools/vmtest/build.sh all
bash tools/vmtest/run.sh --variant o2
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 bash tools/vmtest/run.sh
bash tools/vmtest/run.sh --variant o2 --force-yield
bash tools/vmtest/parser_trim.sh o2
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 bash tools/vmtest/parser_trim.sh asan
bash tools/vmtest/parser_trim_compare.sh 91801cf
```

## 未検証・解釈の限界

LSan はこの cloud executor の ptrace 制約で動作せず無効化。ASan/UBSan と独立 allocator の
全解放確認を LSan の pass と呼ばない。Test262、ESP-IDF build、ESP32 動作・ピーク・速度は未実施。
host JSValue 16 B と実機 8 B の差、allocator の配置・in-place shrink の差がある。
この記録はメモリ削減量や実機の採用判定を主張しない。

移動 allocator は `js_realloc2` が公開した usable slack までコピーする必要がある。
初稿のテストは requested length だけをコピーして自己破損したが、修正した最終テストで
上記を再実行した。これは製品コードの不具合として数えない。

生ログは当該 worktree の `.cache/cloud-validation/`（git 管理外）。
