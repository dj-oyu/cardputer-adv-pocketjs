# SHIKI TEST — Shiki エンジンの実機での突き合わせ

[Shiki 仕様書](../../docs/kasane/shiki-spec.md) §10 の実機側。C のエンジン（`main/ui/kasane/ksn_shiki.c`）が、実機（Xtensa、double はソフトウェア）でも host と同じビットを出すかを確かめる。**メニューには出さない**。USB broker の `app_session` で `local.shikitest` を起動する。

## 流れ

1. アプリが決まったシード（`SEED`）でランダムな式を `N` 本作る（`shikiGen`）。式の 2 割はバイト列を 1 ビット壊して、登録の拒否も比べる。
2. 1 本ごとに試験専用の面 `pocket.shikiDebug.run(code, args, inputs)` で登録と draw を 1 回行い、ログへ `SK <最初の番号> <ケースの行のハッシュ> <結果の行のハッシュ> <終わり方> ...` を 4 本ずつ 1 行で出す（broker のログは 1 回 256 行まで）。1 本に実機の JS で 30〜60 ms かかり、ターンが長すぎると中断される（`InternalError: interrupted`）ので、1 フレームは 60 ms で切る。（33 ms を超えるフレームが続くと、以前は CPU 1 のアイドルタスクが走らず、タスクのウォッチドッグが 5 秒ごとに警告した。描画のループが周期を超えたフレームの後に 1 tick 譲るようにして直した。`main/main.c`）最後に `SK END`。
3. host で `node tools/kasane_ir/shiki_device.mjs ログ.txt`。このファイルをそのまま Node で読んで同じケースを作り、C の実行器と参照実装（`tools/kasane_ir/shiki_ref.mjs`）の結果の行のハッシュと比べる。

## なぜハッシュか

結果の行は、線分 1,024 本で約 30 KB になる。ゲストの文字列にすると評価のヒープを食うので、C が行を作りながら FNV-1a 32 を取り、8 桁だけを返す（`main/pocket/pocket_shiki.c`）。ケースの行のハッシュは JS で取る。ケースのハッシュが違えば、実機の QuickJS と Node が生成器の計算か `String(number)` で食い違った（結果は比べない）。ケースが同じで結果が違えば、エンジンか、JS の数を固定小数にする変換（§2.4、実機ではソフトウェアの double）が食い違った。

## 試験専用の面

`pocket.shikiDebug` は仕様 §9 の API ではない。manifest で `kasane.shiki.debug` を要求したセッションにだけ入る（`pocket.derby` と同じ形）。1 回の `run` ごとに式と線分（約 11 KB）を確保して返す前に解放するので、通常のファームの `.bss` は増えない。

## SHIKI DATA（`local.shikidata`、仕様 v1.3 のデータ領域）

同じ流れで、データ領域を持つ式を試す（`shiki_data_test.js`、`shikiDataGen`）。式は 1〜4 個の領域を宣言し、ケースは型・Q 形式・`k_rec`・中身（または前の領域と同じメモリ）を持つ。`run(code, args, inputs, data)` の `data` は `[{t, q, k, v} | 番号, ...]` で、結果の行には実行後の全領域の中身が付く（` D ...`）。ケースの行・結果の行の書式は `tools/kasane_contract/shiki_run.c` の先頭。ログは `SD ...`、最後に `SD END`。host は `node tools/kasane_ir/shiki_device.mjs ログ.txt --data`。4 分の 1 の式は、登録で拒否される誤り（`PUT` の書く旗、型の不一致、レコード 0 件、範囲外の `FETCH`）を混ぜ、1 割はバイト列を 1 ビット壊す。

## SHIKI API（`local.shikiapi`、仕様 §9 の `pocket.kasane.shiki`）

manifest に `kasane.shiki` を挙げたアプリとして起動し（セッションの開始時に Shiki のヒープ 48 KiB を取る）、`shiki_api_test.js` の自己検査を 1 フレームで回す: capability、`heap()`、`data()` の検査と予算、`options.data` の結び付けと `PUT` の書き込み、`release` の `BUSY`・`CLOSED`・別の view、`transfer` の門（長さに関わらず `TypeError`、窓は残る）、手続き型の呼び出しから Shiki の handle が見えないこと、16 個の上限、`INVALID`・`LIMIT`・有限でない入力の誤り、共有の `beginFrame`・`grid`。失敗した項目ごとに `SA FAIL ...`、最後に `SA END pass=N fail=M`。同じファイルを host で `python tools/kasane_contract/run_pocket_shiki_api_qjs.py`（本物の QuickJS と C、ASan・UBSan・リーク検出）が走らせる。

## CYCLE BENCH（`local.cyclebench`）

同じ試験専用の面の `pocket.shikiDebug.bench(i)`（ケースの数は `benchCount()`）で、整数の命令（`mull`・`mulsh`・`quos` など、値の大きさ別）と、GCC が呼ぶ 64 ビットのライブラリ関数（`__divdi3` など）と、Shiki エンジンの命令 1 つ（`ksn_shiki_draw` の中）のサイクル数を測る。画面には蓋絵（題、進み具合の棒、終わったケースの数）を出す。LCD の転送は非同期なので、ケースは 2 フレームに 1 回だけ測り、間のフレームで前の絵の転送を終わらせる（転送と測定を重ねない）。ログへ `CB ...` を 1 行ずつ出し、最後に `CB END`。方法は `main/hal/int_latency.c`、結果は [docs/perf/int-latency.md](../../docs/perf/int-latency.md)。これもメニューには出さず、USB broker の `app_session` で起動する。
