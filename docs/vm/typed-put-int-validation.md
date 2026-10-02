# H: TypedArray immediate-int store の検証

2026-10-01。基点 `91801cf`、WIP `a2b06a1`、作業枝 `vm/cloud-h-typed-put`。
WIP の製品コード 18 行は変更していない。`POCKET_VM_TYPED_PUT_INT_FAST` は既定 OFF のまま。
マージ・push・実機アクセスなし。

## 実装の範囲

`JS_SetPropertyValue` の整数添字経路で、値の tag も `JS_TAG_INT` のときだけ、
8/16/32 bit の signed/unsigned TypedArray への `JS_ToInt32Free` を省く。
opcode 専用の新しい store ではない。ほかの値は従来どおり変換し、immutable/bounds の検査は
変換後に行う。clamped、float、BigInt の経路は変更しない。

## 実測(host64)

GCC 14.2.0、x86-64 Linux。`-fno-pie -no-pie`。

- macro OFF / ON のそれぞれで、全 corpus 81/81: O2、ASan+UBSan、O2 forced-yield（計 6 構成）
- 新規 corpus `typed_put_int_fast.js` は各構成で 791 assert
- 値: signed 境界、8/16/32 bit wrap、fraction、NaN、Infinity、-0、boolean/null/undefined/string
- 添字: 整数・範囲外・負・string・-0・fraction・非 canonical、通常 object / proxy の setter
- 変換: valueOf の実行回数、例外 identity、Symbol / BigInt の拒否、変換中の detach / resize
- buffer: 既に detached、resize による bounds 外→内、immutable の不変性
- clamped / BigInt / float の非対象経路も検査
- O2 `quickjs.o` の text は OFF 1,011,576 B / ON 1,011,664 B（+88 B）、data/bss 不変

NaN 添字の valueOf 呼び出し回数は、この基点では 0。テストはこの観測を両 macro 状態で
比較する。これを ECMAScript 全体の適合性の証明とはしない。

再現（既定 OFF と ON は別の object cache）:

```sh
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 bash tools/vmtest/typed_put_int_fast.sh
```

## 未検証・判断

LSan は cloud executor の ptrace 制約により動作しないので無効化した。
Test262、ESP-IDF ビルド、ESP32 の動作・flash/DRAM 差・速度は未実施。
host64 の object サイズを firmware サイズへ換算しない。速さの測定もしていないので、
この結果だけで default ON にせず、候補枝として保持する。

生ログは `.cache/cloud-validation/final-matrix.log` と `typed-put-*.log`（git 管理外）。
