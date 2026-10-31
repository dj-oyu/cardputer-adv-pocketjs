# SHIKI TEST — Shiki エンジンの実機での突き合わせ

[Shiki 仕様書](../../docs/kasane/shiki-spec.md) §10 の実機側。C のエンジン（`main/ui/kasane/ksn_shiki.c`）が、実機（Xtensa、double はソフトウェア）でも host と同じビットを出すかを確かめる。**メニューには出さない**。USB broker の `app_session` で `local.shikitest` を起動する。

## 流れ

1. アプリが決まったシード（`SEED`）でランダムな式を `N` 本作る（`shikiGen`）。式の 2 割はバイト列を 1 ビット壊して、登録の拒否も比べる。
2. 1 本ごとに試験専用の面 `pocket.shikiDebug.run(code, args, inputs)` で登録と draw を 1 回行い、ログへ `SK <最初の番号> <ケースの行のハッシュ> <結果の行のハッシュ> <終わり方> ...` を 4 本ずつ 1 行で出す（broker のログは 1 回 256 行まで）。1 本に実機の JS で 30〜60 ms かかり、ターンが長すぎると中断される（`InternalError: interrupted`）ので、1 フレームは 60 ms で切る。最後に `SK END`。
3. host で `node tools/kasane_ir/shiki_device.mjs ログ.txt`。このファイルをそのまま Node で読んで同じケースを作り、C の実行器と参照実装（`tools/kasane_ir/shiki_ref.mjs`）の結果の行のハッシュと比べる。

## なぜハッシュか

結果の行は、線分 1,024 本で約 30 KB になる。ゲストの文字列にすると評価のヒープを食うので、C が行を作りながら FNV-1a 32 を取り、8 桁だけを返す（`main/pocket/pocket_shiki.c`）。ケースの行のハッシュは JS で取る。ケースのハッシュが違えば、実機の QuickJS と Node が生成器の計算か `String(number)` で食い違った（結果は比べない）。ケースが同じで結果が違えば、エンジンか、JS の数を固定小数にする変換（§2.4、実機ではソフトウェアの double）が食い違った。

## 試験専用の面

`pocket.shikiDebug` は仕様 §9 の API ではない。manifest で `kasane.shiki.debug` を要求したセッションにだけ入る（`pocket.derby` と同じ形）。1 回の `run` ごとに式と線分（約 11 KB）を確保して返す前に解放するので、通常のファームの `.bss` は増えない。
