# R5: 命令キャッシュのミスを実機なしで数える（2026-09-27、`vm/r5-icache`）

R4（[turn-cpi.md](turn-cpi.md)）で、STRESS の JS のターンはサイクルの 70%（QIO にした後も 57%）が flash
キャッシュのミス待ちだとわかった。R5 はミスの**回数**を減らす手を探す。COM3 を使えない間に進めたので、
実機の代わりに QEMU で命令列を取り、キャッシュの模型に通した。この文書の数字は、断りが無いかぎり
**模型の推定**で、実機では測っていない。

## 1. 道具（`tools/r5sim/`）

- **`r5sim` イメージ**: STRESS を画面なしで回す ESP-IDF のプロジェクト。quickjs コンポーネントは実機と共有、
  Kasane・pocket の側は `tools/test_stress_app.c` と同じソースを実機のコンパイラ設定（`-Os` ほか、コンパイル
  コマンドは `-I` 以外一致）で組む。frame() は `app_tick` と同じく `JS_Call` → ジョブの消化 →
  `pocket_kasane_end_turn`。ターンの前後で `r5_turn_begin` / `r5_turn_end` を呼び、ログ上の番地で区切る。
- **QEMU**: Espressif 版（`esp_develop_9.2.2_20250817`、`-machine esp32s3`）。プラグインは無効のビルドなので、
  `-d exec,nochain,in_asm -dfilter 0x42000000..0x43ffffff` で flash 上の基本ブロックの実行順と中身を記録する
  （120 フレームで約 1 GB）。
- **`icache.py`**: `extract` がログから対象ターンの基本ブロック列だけを抜き（2.2 MB）、`model` がそれを任意の
  ファームの ELF の番地に写して 8-way・32 KiB・32 B ラインの LRU に流す。関数の目的コードは両イメージで
  同一（`quickjs.c.obj` のサイズが一致）で、リンク後のサイズがずれる（最大 1.3%）のはリンカの呼び出し緩和
  なので、関数内のオフセットは比例で写す。ファームで IRAM にある関数はキャッシュを通らないものとして扱う。
- **`gen_hot_lf.py`**: 関数の一覧と map から ESP-IDF のリンカ断片を作る（flash 内の並べ替え、または
  `--noflash` で IRAM）。

**模型が見ないもの**: 描画。QEMU は PIE を実装していない（最初の `ee.vldbc.16` で IllegalInstruction）ので、
イメージは描画を 16×8 のスカラーで回して提出を消費するだけにした。実機はターンの間に描画するので、真の値は
「ターンの間でキャッシュが残る（warm）」と「毎ターン空から（cold）」の間にある。置換方式は LRU と仮定した
（TRM に記載が無い）。

### 1.1 実機の測定との照合（推定）

R4 の同一配置の測定では、DIO→QIO で `I_STALL_BUSY` が 1 ターン 1.61M → 0.88M サイクルになった。ミス回数が
同じなら 1 回の値段の比は 1.83 で、SPI のクロック比と整合する。模型の cold 1,932 回を当てはめると、QIO の
ミス 1 回は約 450 サイクル（SPI の読み出し約 250 サイクル＋キャッシュ制御）で、ありえる範囲。**ミス回数を
実機で直接測ってはいない**ので、以下のミリ秒は「1 回 450 サイクル」を仮定した推定。

## 2. 結果（STRESS LV1、ターン 61〜120、`build_cpi` の配置）

| | warm | cold |
| --- | --- | --- |
| ミス / ターン | 1,411 | 1,932 |
| 同じく、完全連想 32 KiB（衝突を除いた場合） | 1,201 | 1,648 |

- **1 ターンに触る命令ラインは 1,457 本 = 45.5 KiB** で、キャッシュ（32 KiB）より大きい。cold のミスの 1,457 回は
  初回の読み込みで、配置では消えない。
- 衝突（8-way と完全連想の差）は **15% 前後**。配置を変えて取れるのはこれが上限。
- ミスは `JS_CallInternal` が 17.6%（25 KB のうち 9.6 KiB を触る）、残りは 1 関数 2% 未満の長い裾。

## 3. 打ち手の比較

### 3.1 ホットな関数を flash 内で連続に置く — 効かない

模型の上で関数を抜き出して連続に置くと cold −12% と出たが、**実際にリンカ断片で並べ替えたファーム**
（ホットな 8 KiB、95 関数）を模型に通すと 1,932 → 1,921（−0.6%）。残りのコードも一緒に動くので衝突の
当たり外れが引き直され、並べ替えた分の利得はその揺れに埋もれる。断片は採らなかった。

補足（ESP-IDF の挙動）: `(default)` だけの割り当ては親と同じとして ldgen が捨てる。フラグ
（`text->flash_text ALIGN(4, pre)`）を付けると個別の規則になり、`.flash.text` の汎用の規則の後ろに置かれる。
ただしオブジェクトに既存の個別規則があると、その隣に置かれる（`libmain.a` の関数は散った）。

### 3.2 IRAM に移す — 効くが DRAM を 1:1 で払う

模型が選んだ「バイトあたりのフェッチが多い順」の関数を `noflash` で IRAM に置いて**実際にビルド**し、その
ELF を模型に通した:

| | 静的 DIRAM（`memlog`） | cold | warm | 1 ターンの短縮（推定） |
| --- | --- | --- | --- | --- |
| 現状 | 159,052 B | 1,932 | 1,411 | — |
| IRAM 4 KiB（68 関数、`candidates/r5_iram4.lf`） | **+4,240 B** | 1,659（−14%） | 1,086（−23%） | 約 0.5 ms |
| IRAM 8 KiB（95 関数、`candidates/r5_iram8.lf`） | **+8,705 B** | 1,436（−26%） | 884（−37%） | 約 0.9〜1.0 ms |

移した関数は小さなもの（`JS_FreeValueRT`、`js_malloc_rt`、`JS_NewObjectFromShape`、`add_property`、
`parse_i16` など）で、`JS_CallInternal`（25 KB）は含まない。

### 3.3 その他の候補（未評価）

- flash 120 MHz（ESP-IDF では実験的機能）。ミスの回数ではなく値段を下げる。
- 描画側のコードを IRAM に置いてターンのコードをキャッシュに残す（warm に近づける）。描画の flash の
  作業集合は QEMU で測れない（PIE）ので、実機の計装が要る。

## 4. 判断と次の手

- **並べ替えは採らない**（§3.1）。
- **IRAM は DRAM の予算の判断**で、実機での確認も要る。4 KiB で 0.5 ms、8 KiB で約 1 ms（どちらも推定）を、
  ゲストとシステムの空きから払う。実機で `CONFIG_POCKET_VM_TURNPERF` の `I_STALL_BUSY` と 1 フレームの JS
  時間を同一条件で比べ、`memlog --check` で空きを確かめてから採否を決める。
- 断片を試すときの注意: 断片ファイルを差し替えただけでは ldgen が読み直さない。`idf.py reconfigure` を挟む
  （この作業で 1 回、古い断片のまま組まれたビルドを比べかけた）。

## 5. 再現

```bash
# r5sim を組んで QEMU で記録（Windows 版 QEMU の例。WSL 版でも同じ）
idf.py -C tools/r5sim -B tools/r5sim/build build
python -m esptool --chip esp32s3 merge-bin --fill-flash-size 8MB -o qemu_flash.bin @flash_args   # build 内で
qemu-system-xtensa -nographic -machine esp32s3 -m 4M -drive file=qemu_flash.bin,if=mtd,format=raw \
    -serial stdio -monitor none -d exec,nochain,in_asm -dfilter 0x42000000..0x43ffffff -D r5.log
python tools/r5sim/icache.py extract r5.log tools/r5sim/build/r5sim.elf turns.pkl.gz --nm <xtensa nm>
python tools/r5sim/icache.py model turns.pkl.gz tools/r5sim/build/r5sim.elf <firmware.elf> --nm <xtensa nm> \
    [--full] [--iram-kib N --emit list.txt] [--top 30]
python tools/r5sim/gen_hot_lf.py [--noflash] list.txt <firmware.map> > fragment.lf
```

QEMU は `R5SIM done` を出した後も止まらないので、見張って終了させる。ログを Python で丸ごと読む処理は、
最初の版が基本ブロックの両端を別々に写して 2 GiB の範囲を作るバグで 2 回メモリ不足で止められた（今は命令
ごとに写す）。
