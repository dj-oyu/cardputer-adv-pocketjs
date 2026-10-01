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
