# E: 空の fast array の圧迫時容量返却（host 検証）

2026-10-01。基点 `91801cf`、WIP `b0895b7`、作業枝 `vm/cloud-e-array-trim`。
製品コードの WIP 22 行は変更していない。マージ・push・実機アクセスなし。

## 守る範囲

`JS_RunGC` の入口で有限の上限の 7/8 以上なら、cycle collection の後で、生存する
`JS_CLASS_ARRAY` / `fast_array` / `count == 0` の backing allocation を解放する。
通常の GC と無制限の runtime は再利用容量を残す。要素が 0 個でも `.length` は 0
とは限らないので、holes と length は変更しない。割り当てを伴わない返却であり、
返却後の最初の書き込みは確保に失敗することがある。既存の単一添字追加の
OOM 経路は length を先に増やすため、失敗後は有効な hole として残り、再試行できる。

## 実測(host64)

GCC 14.2.0、x86-64 Linux。`-fno-pie -no-pie`。

- 既存 semantic corpus: O2 80/80、ASan+UBSan 80/80、O2 forced-yield 80/80
- `array_trim.sh` の O2 / ASan+UBSan: pass
- 容量 1, 2, 3, 16, 512, 4096 要素で、無制限・閾値直前・閾値一致を検査
- GC 中のすべての allocation を拒否し、attempt 数が増えず、返却量・pointer・capacity・OOM canary が整合することを検査
- cycle 回収だけで圧迫が解消しても入口の判断どおり返却すること、返却後の regrowth OOM と再試行、非空配列の保持、長さ 99 の holes、push/unshift/pop、prototype setter を検査
- テスト用 allocator は全解放時に live block / byte がともに 0 と assert

再現:

```sh
VMTEST_CFLAGS='-fno-pie -no-pie' bash tools/vmtest/build.sh all
bash tools/vmtest/run.sh --variant o2
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 bash tools/vmtest/run.sh
bash tools/vmtest/run.sh --variant o2 --force-yield
bash tools/vmtest/array_trim.sh o2
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 bash tools/vmtest/array_trim.sh asan
```

## 未検証・解釈の限界

LSan はこの cloud executor で ptrace 制約により起動失敗するため無効化した。
ASan/UBSan の pass を LSan の pass と読まない。Test262、ESP-IDF ビルド、ESP32 の実行・
速度・ピーク・断片化は未実施。host の JSValue は 16 B、実機は 8 B。
テストの 12 B 最小 / 4 B 丸めは長さの模型であり TLSF の配置を再現しない。
実機では小ブロック cache が解放された storage を保持する場合があるため、
QuickJS の accounting 減少を物理ヒープの同量増加とは扱わない。

生ログは当該 worktree の `.cache/cloud-validation/` に保存（git 管理外）。
