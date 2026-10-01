# 通常ビルドに接続した事前コンパイル実験（F候補）

基点 `91801cf0aaf73ae1fd24d2160234af17a4bf874a`。実機を利用しない候補。

## 境界

`POCKET_APP_PRECOMPILE_EXPERIMENT` は既定 OFF。ON にすると通常の
`APP_CHUNK_FILES` を host の同一 vendored QuickJS でコンパイルし、
`experimental-bytecode/<file>.bc` と `.bc.json` を生成する。
**ファームは ON でもソースを埋め込み、従来のソースローダで動く。**
この候補は起動時間・機体heapを改善した製品実装ではなく、再現可能な
ビルド実験の土台である。

入力は `make_app_chunks.py` の選ぶ最終ファイルで、`@plan` なら
`lower_plans.mjs` の出力が CMake dependency になる。元の plan 関数を
再び QuickJS に解析させる誤った比較を避ける。入口 source は含めない。
`.mjs` は configure とツールの双方で拒否する。module graph の解決と
単一instanceを未検証のまま global として扱わない。

## 識別と失敗処理

manifest は source の SHA-256／バイト長／表示ファイル名、BC の SHA-256／
バイト長、全 QuickJS C/header（opcode、atom、ROM生成表を含む）、guest
header、compiler driver、component CMake、build flags の fingerprint、
CC identity、host pointer／JSValue幅／endianness／eval・write flags を残す。
`BC_VERSION` 一致だけでは別の改造engineを識別できない。

compile-only → WriteObject → ReadObject → global function bytecode kind確認
までを行い、app本体は実行しない。debugをstripせずfile/lineを保つ。
失敗したcompileは既存成果を上書きしない。成功時はBCとmanifestを順次
atomic renameで更新し、途中停止で二つが不一致ならverifyが拒否する。
書込み・読込みはコピーを伴い、flash常駐bytecodeではない。

これは不正な入力に対する署名・信頼機構ではない。生成元を信頼する
build artifactの再生成・破損・混線検出であり、runtime loaderには渡さない。

## 実行

Windowsでは既存 UCRT GCC を使える。DLL探索にbinをprocessのPATHへ足す。
PATH変更前に通常Pythonの絶対パスを控えると、UCRT Pythonへ偶然切替わる
ことを防げる。global環境設定や追加installは必要ない。

```powershell
$taskPython = (Get-Command python).Source
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
& $taskPython tools/precompile_apps.py --cc C:/msys64/ucrt64/bin/gcc.exe --outdir .cache/precompile-test apps/hello/main.js
& $taskPython tools/precompile_apps.py --verify --outdir .cache/precompile-test apps/hello/main.js
& $taskPython tools/test_precompile_apps.py --cc C:/msys64/ucrt64/bin/gcc.exe
```

IDF configure は `-DPOCKET_APP_PRECOMPILE_EXPERIMENT=ON`
`-DPOCKET_APP_HOST_CC=C:/msys64/ucrt64/bin/gcc.exe` を明示する。
target toolchainをhost compilerに使わない。

## 製品統合の前に必要なこと

64bit host／16B JSValue と target32bit／8B JSValue が異なること自体を
「必ず互換性がない」とは断定しない。serializerには固定幅encodingもある。
ただし現時点では互換性が証明されていないので、targetが読めるとは扱わない。
target側の実物read-only oracleまたは信頼できるtarget ABI host buildで、
opcode、ROM atom、float、Unicode、closure、eval、OOM cleanup、debug lineを
比較し、正式な互換性keyを固める必要がある。

その後 source／binary の別と正確な長さを表へ追加し、kind／identity検証後
に `JS_ReadObject` へ分岐する。moduleは `JS_ResolveModule`、同じfilename、
静的importだけの解決窓、単一instance、循環、TLA拒否、OOM retryが必須。
entryは app_session の評価・期限・初期化順と一緒に統合する。
source fallbackのflash費用も測る。実機利用禁止中にはruntime統合を採用しない。

既存の旧DERBY実機起動49ms／約26KB削減（eval-peak §3.5）はこの変更の
測定値ではなく、現appの改善保証ではない。今回のhost semantic比較や
heap limit sweepも実機の断片化・ピーク・速度を保証しない。

## cloud host 検証（2026-10-01）

Linux x86_64 / GCC 14.2.0 / Python 3.12.14 / Node 24.19.0。
`tools/test_precompile_apps.py --cc /usr/bin/gcc` と、同じengine本体・driverを
ASan/UBSanで計装する `--sanitize` が入口。後者はLinuxで非PIEにし、
containerのASan起動時mapping失敗を避ける。

```bash
python tools/test_precompile_apps.py --cc /usr/bin/gcc
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
python tools/test_precompile_apps.py --cc /usr/bin/gcc --sanitize
```

28テストが通過。source/BCの14種の意味比較は、両者が同じ誤りを返して
通らないよう期待値とも照合する（closure、直接eval、整数TypedArray、
穴配列、−0/Infinity/NaN、Unicode、TDZ、generator、class/super、new.target、
分割代入/rest、optional chain、BigInt、DataView）。file/line保持、再生成の
バイト一致、compiler cacheの再利用、compile時の副作用不実行も確認。

本番の `make_app_chunks.py` が選ぶ全6チャンクを実際に読み、`@plan` の
ROM loweringを済ませた最終sourceからcompile/read-back/manifest検証を
行う。これはJS本体の実行、IDF CMake graphの実行、Xtensa互換性の証明
ではない。CMake/ESP-IDF/target compilerはcloud環境に無く、未実行。

BC読込み・実行のfail-allocation 0〜120（121点）と6種のheap上限×3反復は、
失敗後に同じruntimeの制限を戻して42の式を再評価し、回復まで検査する。
teardown後の独自allocator生存byte数も0を要求する。2段階rename間の
中断を注入し、BCだけが更新されたpairをmanifest検証が拒否することも確認。

このcloudではLeakSanitizerが最小のmalloc/freeプログラムでもptrace制約で
異常終了するため、上記では `detect_leaks=0`。ASan/UBSanおよび独自allocator
検査の通過をLSan通過とは扱わない。通常のhostではLSanを有効にした検査も
残る。実機のheap・断片化・速度・flash費用は未測定。
