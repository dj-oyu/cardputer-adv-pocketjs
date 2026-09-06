# PIE カーネルのチューニングツール

`main/shell.c` と `main/render_accel.c` にある ESP32-S3 PIE（SIMD）カーネルを、**実機に焼く前にホストで検証・解析する**ためのツール群です。背景と設計判断は [docs/pie-simd.md](../../docs/pie-simd.md) を参照してください。ここではツールの使い方と、どの局面で何を走らせるかだけを書きます。

すべて Python 3 標準ライブラリだけで動きます（C モデルのみホストの C コンパイラが要ります）。実行はリポジトリのルートから。

## 一覧

| ファイル | 役割 | 所要時間 |
| --- | --- | --- |
| `stalls.py` | **静的パイプライン解析。** インラインアセンブリを読み、TRM 表 1.7-2 に基づいて「ロード / `EE.VMUL` / `EE.VRELU` の結果を直後に使っている」ストール箇所を列挙する。メモリ命令の比率、`loopgtz` の 256B 制限に対する本体サイズも出す | 即時 |
| `test_kernels.py` | **命令レベルの実行検証。** C ソースからアセンブリと定数配列 `k[]` を抜き出し、`piesim.py` で実行してスカラー参照（`ocean_row_scalar` / `wave_row_scalar` / Rust の合成式）と全画素比較する | 約 1 秒 |
| `piesim.py` | 上記 2 つが使うライブラリ。TRM 1.8 の擬似コードどおりに q0〜q7・QACC・SAR・メモリを模倣する解釈実行器と、C ソースからアセンブリ文字列・`k[]` 初期化子を取り出す関数 | — |
| `run_models.py` + `models/*.c` | **算術の総当たり証明。** カーネルが使う式（表の畳み込み、逆数乗算、/255 の恒等式、565↔888 の展開）が参照実装とビット一致することを入力の全域で確認する C プログラム | 数秒 |
| `models/accel_host_test.c` | `render_accel.c` の入口関数（`accel_fill` / `accel_blend`）を `-DRENDER_ACCEL_HOST_MODEL` でホストビルドし、行頭・行末のスカラー処理とポインタ計算をランダム矩形で検証する | 約 1 秒 |

3 つの層で守っているものが違います。

```
run_models.py   … 「その式は正しいか」      算術の全域証明。式・表・定数を変えたら再実行
test_kernels.py … 「アセンブリはその式か」  レジスタ割当・定数順・レーン・ポインタの検証
stalls.py       … 「その並びは速いか」      データ依存ストールの静的検出
```

## いつ何を走らせるか

| 局面 | 走らせるもの |
| --- | --- |
| カーネルの**命令を並べ替えた**（スケジューリング、レジスタの付け替え） | `test_kernels.py` → `stalls.py`。ビット一致のまま 0 ストールになるまで往復する |
| **定数 `k[]` の順序や値**を変えた | `test_kernels.py`。`k[]` 初期化子は C ソースから評価されるので、アセンブリ側の読み順とずれれば落ちる。blend は `blend_constants()` を `test_kernels.py` の `blend_k()` に手で写す |
| **式・表・スケーリング**を変えた（例: 表に定数を畳み込む、シフト量を変える） | `run_models.py` で該当モデルを更新して総当たり → `test_kernels.py` |
| `render_accel.c` の**ラッパー**（矩形の頭・尾、整列判定）を触った | `run_models.py accel` |
| **新しい命令**を使った | `piesim.py` にその命令を TRM の擬似コードから追加してから `test_kernels.py`。未対応命令は `NotImplementedError` で止まる（黙って素通りはしない） |
| 実機で「命令数のわりにサイクルが多い」 | まず `stalls.py`。その `estimated cycles per block` と実測（`PERF` の `kernel=` を 30×行数で割る）を突き合わせる。一致すればカーネルは下限で走っており、次に効くのは命令数削減だけ。大きく外れるなら計測にカーネル以外が混ざっている |
| 実機に焼く直前 | 3 つ全部 |

## 使い方

```sh
# ストール解析（関数名は C ソース内の名前）
python tools/pie/stalls.py main/shell.c ocean_row_pie
python tools/pie/stalls.py main/shell.c wave_row_pie
python tools/pie/stalls.py main/render_accel.c blend_blocks_pie

# 実行検証（4 カーネル）
python tools/pie/test_kernels.py

# 算術の総当たり（ocean / wave / blend / accel を個別指定も可）
python tools/pie/run_models.py
python tools/pie/run_models.py blend
```

`stalls.py` の出力例（海面 v1 → v2 の差）:

```
ocean_row_pie: 66 instructions per block, 37 memory (56%), 16 ldxq, 17 vldbc
loop body ~213 bytes
21 stall(s): producer -> consumer (index: op) on register
    3: ee.vldbc.16.ip       ->   4: ee.vadds.s16         q2
    ...
estimated cycles per block: 87.6 (66 issue + 1 store x0.6 + 21 stalls)
```
```
ocean_row_pie: 62 instructions per block, 35 memory (56%), 16 ldxq, 15 vldbc
loop body ~201 bytes
no stage-2 producer followed immediately by its consumer: 0 data stalls
estimated cycles per block: 62.6 (62 issue + 1 store x0.6 + 0 stalls)
```

`estimated cycles per block` は実機で測った機械の下限（[docs/pie-simd.md §3.5](../../docs/pie-simd.md)）から出しています: **PIE は命令の種類を問わず 1 命令 1 サイクルで発行します**。融合ロード（`.LD.INCP`）も索引ロード（`EE.LDXQ.32`）も、本数を 0 から 32 まで振っても追加コストは出ませんでした。払うのは 128bit ストア 1 本あたり 0.6 サイクルと、ストール 1 つあたり 1 サイクルだけです。

```
cycles/block = 命令数 + 0.6 x ストア数 + ストール数
```

実動作中の実測は、行あたりの前処理とタスク切り替え（PIE はコプロセッサ 3 なので状態の退避・復元が起きる）で**この下限より 2〜3 割高く出ます**。それが正常です。

実測がこの見積もりを大きく上回るときは、まずカーネル以外が計測に混ざっていないかを疑ってください。海面で一度「25〜35 サイクル/ブロックの残差」を追いかけて `ldxq` を疑いましたが、正体は `loop` の計測値に空の塗りつぶし 37 行とタイマ呼び出しが含まれていたことでした（`PERF` の `kernel=` はその後分けたもの）。

## 新しいカーネルを書くときの手順

1. スカラー参照を C で書き、`__attribute__((unused))` でソースに残す（`test_kernels.py` の参照はこれを写す）。
2. 式を 16bit レーンに収まる形に書き換え、`models/` に総当たりモデルを追加して `run_models.py` で証明する。
3. アセンブリを書き、`test_kernels.py` にテストを追加して通す。
4. `stalls.py` で 0 ストールになるまで並べ替える（ビット一致は 3 が守る）。
5. `xtensa-esp-elf-gcc` でコンパイルし、`objdump -d` で `loopgtz` の本体が 256B 以内か、`kp` と `k` が別レジスタかを見る。
6. 実機で計測し、キャプチャで絵を確認する。

## 前提と限界

- `piesim.py` は**機能モデル**です。タイミングは持たず、対応命令は各カーネルが使うものだけ（ファイル冒頭の一覧）。QACC は 8 レーン × 40bit、`EE.SRCMB.S16.QACC` はシフト結果を QACC に書き戻す仕様（TRM 1.8.54）まで模倣しています。
- `stalls.py` は TRM 表 1.7-2 の QR オペランドの def/use 段だけを見ます。QACC と SAR の依存は表に無いため扱いません。本体サイズは通常のエンコード（EE 命令 3B、`EE.LDXQ.32` 4B、`mov`/`addi` 2B）からの見積もりで、境界付近は `objdump` で確認してください。
- テーブル（正弦、softness）は Python の倍精度で生成しており、`sinf`/`expf` と 1 ずれる要素があり得ます。比較の両側が同じ表を使うので判定には影響しません。
- `test_kernels.py` の海面テストは、`shell.c` に `ocean_sine16`（表引き版）があればビット一致を要求し、無ければ放物線近似版とみなして、そのカーネルのコメントが約束する範囲（r/b は 1 段、g は 2 段まで、動く画素は 2 割未満）で判定します。近似の許容を変えたらテスト側の閾値も更新すること。
- C の `/` は `extract_constants()` で `//` に置き換えます。現在の `k[]` に負の被除数はありませんが、増やすときは注意してください。
- 資料: [ESP32-S3 TRM](https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf) 第 1 章（1.7 パイプライン、表 1.7-2、1.8 各命令）。表 1.7-2 はテキスト抽出で列が崩れるので `pdftotext -raw` で読むこと。
