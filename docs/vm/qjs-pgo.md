# QuickJS のプロファイル誘導最適化（PGO、2026-11-05）

[VM の文書](README.md) / 前提: [turn-line-use.md](turn-line-use.md)（行の使用率 57.5%）、[r5-icache.md](r5-icache.md)（模型）

QEMU で STRESS を流して取った実行回数で QuickJS をコンパイルし直す（`-fprofile-use`）。ねらいは
[turn-line-use.md](turn-line-use.md) で見つけた「取り込んだ行の 4 割が走らないバイト」を減らすこと。

**状態: 試行（既定 OFF、`CONFIG_POCKET_VM_QJS_PGO`）。** 実機の 4 アプリで、効果は測れる大きさに無かった（§5、実測）。

## 0. 結論

> 60 fps への道のり全体の要約は [perf/60fps-roadmap.md](../perf/60fps-roadmap.md) にある。

- **実機（2026-11-05、表示の周期 16 ms の測定版で、4 アプリの fps と JS の turn）では差が出なかった。**
  BIG WAVE 58.5 / 58.5 fps、STRESS 50.5 / 50.7 fps（turn 6.72 / 6.73 ms）、MEGADEMO 52.2 / 52.1 fps、
  DERBY 53.1 / 53.8 fps（窓のレースが違う）。模型の見込み（1〜2%）は窓ごとの揺れに埋もれた（§5）。既定は OFF のまま。
- **最初の Windows のビルドは失敗した。** GCC は static 関数の数え先を「ソースの絶対パス」の hash で探すので、
  このコンテナで学習したプロファイルは別の木では一つも当たらない。`tools/pgo/rekey.py` が組む木のパスで
  鍵を作り直す（§1.1）。値のプロファイル（間接呼び出しの先）は同じ問題を内側に持つので切った。

- **本命の hot/cold 分割は、この GCC では使えない。** `-freorder-blocks-and-partition` は Xtensa の GCC
  （esp-15.2.0）が指定を無視して切る（`-Q --help=optimizers` が `[disabled]`。同じ玩具は x86 の GCC なら分割する）。
  [p1-iram-placement.md](p1-iram-placement.md) の「関数内の hot/cold 分割をしない」と同じ壁。
- **使えるのは関数内のブロックの並べ替え。** `-Os` のまま、プロファイルと `-freorder-blocks-algorithm=stc`
  で、走った経路をつなげて置き、走らなかったブロックを関数の後ろへ寄せる。
- **模型の効果は小さい（推定）。** 1 ターンに触る行 1,281 → 1,245（−2.8%）、warm ミス 869 → 826（−4.9%）、
  cold ミス 1,441 → 1,400（−2.8%）、実行ブロック 74,584 → 70,809（−5.1%、分岐が減る）、実行命令 +1.0%。
  helper 群が一番効いた（11.95 → 11.10 KiB）。サイクルの模型（命令 ＋ 成立分岐 × 2 ＋ flash 待ち）で見ると、
  JS のターンで **1〜2% 程度**と見込む。**命令キャッシュのアラインメントで同じカーネルが 15% 動く**（CLAUDE.md）
  ので、実機で 2 つのビルドを比べても、この差は揺れに埋もれうる。
- **代償はほぼ無い。** ファームで flash +1.5 KB（+0.1%）、IRAM +40 B（S3 では DRAM と同じプール）、
  DRAM の .data/.bss は 0。IRAM に置く 93 個の helper はそのまま IRAM（§3.3）。
- `-O2` ＋ プロファイルは捨てた: `JS_CallInternal` が 22 KB → 31.8 KB、r5sim の flash +116 KB。触る行が増える方向。
- 学習と評価が同じ STRESS の 60 ターン。他のアプリ（DERBY）の経路は、学習で走らなければ「冷たい」側に
  寄せられる。

## 1. 仕組み

| 段 | 何をするか | 場所 |
| --- | --- | --- |
| 計装 | r5sim の QuickJS を `-fprofile-generate -fprofile-update=single -fprofile-info-section` で組む。数え先（64 bit、約 220 KB）は DRAM に入らないので QEMU の PSRAM（`-m 4M`）へ | `tools/pgo/qjs_pgo.cmake`、`tools/r5sim/main/gcov_info.lf`、`tools/r5sim/sdkconfig.pgo_gen.defaults` |
| 学習 | QEMU で STRESS 120 フレーム。終わりに libgcov の `__gcov_info_to_gcda` で gcda の流れを 16 進でシリアルへ | `tools/r5sim/main/r5sim.c`（`R5SIM_PGO_GEN`） |
| 復元 | 16 進をバイトに戻し、`gcov-tool merge-stream` で `.gcda` に。`tools/pgo/qjs/` へ写す | `tools/pgo/train.sh` |
| 利用 | `.gcda` を各オブジェクトの隣に写し、素の `-fprofile-use` で組む | `tools/pgo/qjs_pgo.cmake`、`CONFIG_POCKET_VM_QJS_PGO` |

### 1.1 決めたことと理由

- **r5sim の QuickJS をファームと同じ目的コードにした。** r5sim は `main/Kconfig.projbuild` を持たないので、
  `CONFIG_POCKET_VM_ROM_INTRINSICS`（ファームの既定 y）が無く、コンテキストの組み立ての関数がファームと違っていた。
  プロファイルがその関数を欠き、ファームのビルドが `Missing counts for called function`（`-Werror` で止まる）に
  なった。r5sim は今、この定義を QuickJS に付ける。`quickjs.c.obj` の逆アセンブルはファームと一致
  （確認済み）。**この変更で r5sim の記録は以前と比べられなくなった**（触る行は 1,305 → 1,281 前後）。
  [turn-line-use.md](turn-line-use.md) の数字は変更前の r5sim。
- **組む木ごとに鍵を作り直す（`tools/pgo/rekey.py`）。** GCC は関数の数え先を ident で探し、static 関数の ident は
  `crc(crc(crc(0, ソースの絶対パス), 名前), -o の相対パスから .obj を除いたもの)`、ソース位置の照合値は
  `crc(crc(行, パス), 名前)`（crc は GCC の `crc32_string`。esp-15.2.0 の `.gcno` と突き合わせて確かめた）。
  学習した木とパスの違う木（Windows のビルド、別の場所の checkout）では static 関数が一つも当たらず、
  「profile for function ... not found」の後に `Missing counts`（`-Werror` で止まる）になった（broker の Windows の
  ビルドで実際に起きた）。`train.sh` が学習時の `.gcno` から関数ごとの名前・ファイル・行（`.gcda.ids`）を書き、
  configure の時に `rekey.py` がその木のパスで ident と照合値を計算し直して `.gcda` を書く。`-o` の区切り
  （`/` か `\`）は分からないので static 関数は両方の綴りで書く。学習した木のパスで作り直すと 1,766 関数すべてが
  元の値に一致し、別の場所に写した木から組んだファームは、元の木から組んだものと `quickjs.c` の全関数の命令が一致した
  （確認済み）。
- **値のプロファイルは切った（`-fno-profile-values`）。** 間接呼び出しの先（`rt->mf.js_malloc` など）を数える
  カウンタは、呼び先を同じ ident で持つ。これは `-o` の綴りが分からないと書き換えられず、残すと別の木からの
  ビルドで 12 関数（`js_malloc_rt` の一族の、昇格した直接呼び出し）が違った。切ると `.gcda` は 233 → 122 KB。
- **`.gcda` は各オブジェクトの隣に置く。** `-fprofile-use=DIR` はオブジェクトの絶対パスを名前に埋め込むので、
  Linux と Windows のビルドで名前が変わる。素の `-fprofile-use` は「オブジェクト名から `.obj` を除いて `.gcda`」を
  探し、ビルドディレクトリからの相対で同じ。
- **`-Wno-coverage-mismatch`。** GCC の関数ごとの照合はソースのパスを含むので、別の木（Windows）で組むと全関数が
  「source locations ... have changed」で止まる。この場合も数は使われる（同じ木から組んだものと目的コードが一致する
  のを x86 の GCC 13 で確認）。制御の形が本当に変わった関数は別の照合で落ち、プロファイル無しで組まれる。
  **ここは黙って起きる**ので、QuickJS を直したら学習し直す（§4）。rekey の後は照合値も一致するので、この指定は
  学習の後に QuickJS を直したときの保険。
- **`-Wmissing-profile` は警告で残す。** `.gcda` が見つからない・読めないときにファイルごとに出る。ファームの
  ビルドでは 0 件（r5sim に無い関数が 0 になったため）。
- **計装と利用で同じコード生成の指定。** 早い段のインライン展開の後でプロファイルを読むので、`-Os` で学習して
  `-O2` で使うと数が合わない（実際に `Missing counts` で止まった）。`POCKET_QJS_PGO_OPTS` を両方に渡す。
- **`-fno-reorder-functions`。** プロファイルがあると GCC は関数を `.text.hot.<名前>` / `.text.unlikely.<名前>` に
  入れる。`main/vm/helper_iram8.lf` は `.text.<名前>` で 93 個の helper を IRAM へ写すので、そのままだと黙って
  flash に残る（JS が約 13% 遅くなる側。docs/vm/p1-iram-placement.md §9）。
- **PSRAM の学習イメージでは原子操作を戻す。** PSRAM を有効にすると IDF が全ファイルに
  `-mdisable-hardware-atomics` を付け、QuickJS の目的コードが変わる（`_Static_assert` も落ちる）。QuickJS の
  原子変数はすべて DRAM、数え先は原子操作を使わないので、QuickJS にだけ `-mno-disable-hardware-atomics` を付ける。
  `.bss` は QuickJS の分をまるごと PSRAM へ（ldgen は節の名前の中のワイルドカードを受けない）。

## 2. 試したもの

| 指定 | 結果 |
| --- | --- |
| `-Os` ＋ プロファイル（ブロックは "simple"） | 関数が `.text.hot` / `.text.unlikely` に分かれるだけ。並べ方は変わらない |
| `-O2` ＋ プロファイル ＋ `-freorder-blocks-and-partition` | 分割は起きない（Xtensa で無効）。`JS_CallInternal` 22 KB → 31.8 KB、flash +116 KB。捨てた |
| **`-Os` ＋ プロファイル ＋ `stc` ＋ `-fno-reorder-functions`** | 採用（§3） |

## 3. 結果（すべて模型の推定。ログ: [logs/qjs-pgo-20261105.txt](logs/qjs-pgo-20261105.txt)）

STRESS LV1 のターン 61〜120、r5sim 自身の配置、8-way 32 KiB LRU（`turnset.py` と `lineuse.py`）。
A はプロファイルなし、B はあり。どちらも ROM intrinsics 入りの r5sim。

### 3.1 ターンあたり

| | A | B | 差 |
| --- | --- | --- | --- |
| 触る行 | 1,281（40.0 KiB） | 1,245（38.9 KiB） | −2.8% |
| warm ミス（8-way / 完全連想） | 869 / 897 | 826 / 824 | −4.9% / −8.1% |
| cold ミス（8-way / 完全連想） | 1,441 / 1,383 | 1,400 / 1,337 | −2.8% / −3.3% |
| 行の使用率 | 58.1% | 59.3% | +1.2 点 |
| 実行ブロック | 74,584 | 70,809 | −5.1% |
| 実行命令（flash のコード） | 170,232 | 171,964 | +1.0% |

ブロックは QEMU の翻訳単位で、分岐で切れる。ブロックが減って命令が少し増えたのは、走る経路を
まっすぐ並べるために、合流の手前で命令を写した（`stc` の複製）分と読む。

### 3.2 部品ごと（触る KiB/ターン）

| 部品 | A | B |
| --- | --- | --- |
| interp-core | 7.80 | 7.96 |
| interp-helpers | 11.95 | **11.10** |
| object/alloc/GC | 5.60 | 5.43 |
| builtins | 6.17 | 5.89 |
| native boundary | 8.32 | 8.32 |

`JS_CallInternal` はわずかに増えた（オペコードの処理は短い塊が `jx` で散らばるので、並べ替えの効く余地が小さい）。
効いたのは、多くの場合分けを持つ helper（`JS_SetPropertyInternal2` など）。

### 3.3 ファーム（このコンテナで組んだ、`sdkconfig.defaults` 対 ＋`sdkconfig.qjspgo.defaults`、`-fno-profile-values` の後）

| | 既定 | PGO | 差 |
| --- | --- | --- | --- |
| `.flash.text` | 1,629,820 | 1,631,284 | +1,464 B |
| `.iram0.text` | 81,687 | 81,727 | +40 B |
| `.dram0.data` / `.dram0.bss` | 21,893 / 76,448 | 同じ | 0 |

`-Wmissing-profile` は 0 件。IRAM の helper（`JS_FreeAtom` など）は IRAM の番地のまま。

## 4. 使い方

ファームを組む（Windows、EIM の PowerShell。CLAUDE.md の手順で IDF を有効にしてから）:

```powershell
idf.py -B build_qjspgo -DSDKCONFIG=build_qjspgo/sdkconfig "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.qjspgo.defaults" build
```

- プロファイルは `tools/pgo/qjs/`（git 管理）。**学習した GCC（esp-15.2.0_20251204）でしか読めない。**
  IDF の版を変えたら学習し直す。
- 学習し直し（Linux / WSL、Espressif 版 QEMU が要る）:

```bash
tools/pgo/train.sh
```

- 学習し直すべきとき: QuickJS（`components/quickjs-ng`）を直したとき、IDF/GCC を変えたとき、
  r5sim の作業（`stress_r5.js`）を変えたとき。古いプロファイルは、形の変わった関数だけ黙って外れる（§1.1）。
- 模型で確かめる（r5sim を同じ指定で組んで記録する）: `idf.py -C tools/r5sim -B <dir> -DSDKCONFIG=<dir>/sdkconfig -DR5SIM_PGO=use build`、
  その後は [turn-working-set.md](turn-working-set.md) §5 の手順（`-icount shift=2` を付ける）。

## 5. 実機（2026-11-05、実測）

表示の周期 16 ms の測定版の上で、PGO なし（A）とあり（B、rekey 後の `f67662b`）を、同じ 4 アプリで比べた。fps は
`KASANE_PAINT`（30 フレームごと）の時刻から。記録は [../kasane/logs/apps-60fps-recheck-20261105.txt](../kasane/logs/apps-60fps-recheck-20261105.txt)。

| アプリ | A fps | B fps | A turn | B turn |
| --- | --- | --- | --- | --- |
| BIG WAVE | 58.5 | 58.5 | 7.2 | 7.1 |
| STRESS | 50.5 | 50.7 | 6.72 | 6.73 |
| MEGADEMO | 52.2 | 52.1 | 7.02 | 7.04 |
| DERBY | 53.1 | 53.8 | 7.59 | 7.26 |

- 差は窓ごとの揺れの中。STRESS は学習に使った作業そのものだが、turn は 6.72 対 6.73 ms で変わらない。
  DERBY は窓に入ったレースの区間が両者で違うので、平均の差は比べられない（重い WIDE 2 は A 41〜49、B 41〜54）。
- 模型（§3）の「触る行 −2.8%、warm ミス −4.9%」は、実機の turn には現れなかった。読み: 実機ではターンの間に
  描画がキャッシュを使い、IRAM に置いた 93 の helper は模型の外、ミス 1 回の値段も模型より小さい可能性がある
  （未検証。`TURNPERF` の `I_STALL_BUSY` で確かめられる）。
- 結論: 既定 OFF のまま。仕組み（学習・rekey・ビルドの配線）は残す。次に効かせるなら、学習に DERBY を足すより、
  模型と実機の差（I_STALL_BUSY）を先に測る。
