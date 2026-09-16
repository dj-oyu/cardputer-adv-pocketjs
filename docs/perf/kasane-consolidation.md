# Kasane 系統の一本化 — 12 分岐・14 コミットの行き先

対象は `perf/kasane-opt`（基点 `origin/vm/design-contracts`）。2026-09-16、`/workspace/pjs-kasane`。
**実機には触れていない**（`/dev/ttyACM0` 不在）。この文書に新しいミリ秒は 1 つも無い。引用した
時間の話はすべて「どの分岐のコミットが測ったか」「どの文書にあるか」を添えてある。

---

## 0. 結論

`perf/kasane-*` の topic 枝に「取り残されている」と見えていたユニーク 14 コミットは、
**14 本すべて既に `perf/kasane-opt` の中に内容として入っている**。

| 問い | 答え |
| --- | --- |
| 今回 cherry-pick した本数 | **0 本**。14 本とも内容が既に在り、適用すると空コミットになるだけだった（`git cherry-pick -n -X ours` で試して 11 本は差分ゼロ、残り 3 本は同内容のブロックが別位置にあるだけ。§1 手順 6） |
| 衝突の解決 | **0 件**（解決すべき取り込みが無い） |
| この作業で足した最適化コミット | **0 本**（足したのはこの文書 1 本だけ） |
| `git cherry perf/kasane-opt origin/perf/kasane-<b>` の `+` | 12 枝で合計 **18 行**（重複を除くと 14 本）。**これは patch-id の産物で、「内容が無い」意味ではない**（§1） |
| `+` 行を 0 にする方法 | topic 枝を消すことだけ（§6）。履歴の書き換えでは 0 にできない |

前の 3 次にわたる統合（`docs/perf/kasane-opt-integration.md` §1 / §5 / §6）は、衝突を
**「両側を残して」**解いている。そのため統合コミットの patch は元コミットの patch と
（内容が同じでも）一致せず、`git cherry` は恒久的に `+` を出す。`+` は「未取り込み」ではなく
「cherry-pick の形跡が patch-id として一致しない」だけである。

---

## 1. 「入っている」の判定手順（この文書の主張の作り方）

1. **列挙**: 12 枝それぞれについて `git cherry perf/kasane-opt origin/perf/kasane-<b>` を実行し、
   `+` を集める。合計 18 行、重複（前提コミット `8e57dcb` が anchor / imgopt / pet / stretch /
   tile の 5 枝に載っている）を除いて **14 本**。§2 の表はこの 14 本である。
2. **対応する統合コミットの特定**: `docs/perf/kasane-opt-integration.md` の「元 → 統合後」表
   （§1 の段 1〜5、§5.1 の段 1〜5、§6.1 の段 1〜5）と subject の一致で対応を決め、
   **その統合コミットが `perf/kasane-opt`（= `HEAD`）の祖先であること**を
   `git merge-base --is-ancestor <統合コミット> HEAD` で確かめた（14 本すべて yes）。
3. **ファイル単位の裏取り**: 元コミットが触ったファイルについて `git rev-parse <元>:<file>` と
   `git rev-parse HEAD:<file>` を比べる。**byte 同一**なら内容は入っている（§2 の「byte 同一」欄）。
   同一でないファイル（`ksn_render.c` / `ksn_render.h` / `run.sh` / `docs/README.md`）は
   60 コミット分の後続作業を含むため差が出るので、シンボル・スイッチ・ハーネスで裏を取る。
4. **シンボルとスイッチ**: 元コミットが導入したスイッチ／関数／マクロ（`g_ksn_*` / `KSN_*` /
   `ksn_*`）が `HEAD` に存在し、既定値が元の意図どおりであること（§3）。
5. **振る舞い**: `HEAD` の `tools/kasane_contract/run.sh` に各分岐の専用ハーネスが結線されており、
   それが**両腕比較**（切替の on/off を同一バイナリで比較）で PASS すること（§4）。
   ハーネスは切替のシンボルを参照してコンパイルされるので、切替が消えていればビルドが落ちる。
6. **取り込み結果のシミュレーション**: 14 本それぞれに
   `git cherry-pick -n -X ours <元コミット>` を試す（衝突はこちら側＝現在の `perf/kasane-opt` を採る）。
   **11 本は差分ゼロ**（＝そのコミットの内容は既にこの木にある）。
   残り 3 本（`bbcb69c` / `ce56f69` / `e7282ed`）だけが挿入を出すが、それは
   `group_pixel` / `blend_lut_*` / `g_ksn_tile_*` のブロックが**同じファイル内の別の位置に
   既にあるために二重定義として現れる**だけで、定義数はどれも `HEAD` に 1 つである
   （実測 `grep -c`: `static void group_pixel` 1、`KSN_TILE_MEASURED static void group_pixel` 1、
   `blend_lut_build_row` の定義 1、`int g_ksn_blend_lut=` 1、`int g_ksn_tile_pixels=` 1、
   `static bool tile_block_reached` 1）。**`-X ours` の結果に新しい内容は 1 行も無い。**
   この 3 本が二重になるのは統合記録 §5.2.5 / §6.2.4 の `group_pixel` の名前衝突と、ブロックの
   挿入位置が動いたことの帰結である。

再検証したいときのコマンド:

```bash
# 12 枝の + 一覧（18 行・14 ユニーク）
for b in affine alpha256 anchor coverage decode imgopt lut pet rowtable stretch tile visible; do
  echo "== $b"; git cherry perf/kasane-opt origin/perf/kasane-$b; done
# 統合コミットが HEAD の祖先か
for i in 7420e4d 79fc53d 9ad51f2 930e0bc 1af4a90 b6a8211 f7e46dc 7795bb9 \
         d9a17d1 b9e65e4 a84b118 dc0bb19 d0bf4a1 7391fab; do
  git merge-base --is-ancestor $i HEAD && echo "$i yes"; done
# 切替の既定（この木）
git grep -nE '^(int|bool) g_ksn_' -- main/ui/kasane main/pet
```

---

## 2. 14 本それぞれの行き先

「統合」欄は `perf/kasane-opt` の祖先である統合コミット。「byte 同一」は元コミットのファイルが
`HEAD` と 1 バイトも違わないもの。「この木の出力」は §4 で実際に走らせたハーネスが出した値。

| # | 元コミット | 出所の枝 | 統合（祖先） | byte 同一のファイル | この木の出力（実測） |
| ---: | --- | --- | --- | --- | --- |
| 1 | `bbcb69c` 被覆判定を行ごとの x 区間へ | `perf/kasane-coverage` | `7420e4d` | — （`test_coverage_runs.c` は `ksn_frame_view` への retype で 3 行差） | `predicate and runs agree on 157111512 pixels over 10668504 rows` / `120 frames identical both ways` (hash `a73ff47f`) |
| 2 | `da13f79` フレームコマンドを1フレームに1回だけ展開 | `perf/kasane-decode` | `79fc53d` | — | `120 frames, 135 renders, 3 arms (on/off/alternating) byte-identical, 13 commands` / `read calls: switched on=1770 reference=32912 ratio=18.6x` |
| 3 | `286573d` 可視しきい値スキップの当たり率を測る | `perf/kasane-visible` | `9ad51f2` | — （`test_visible_skip.c` に LUT を落とす 14 行の適応） | `CORPUS 5 scenes x 120 frames, 7490216 blend pixels ... step=1.8% bound=8.9% both=10.0% exact=0.8%` / `visible skip PASS: the counters change no pixel` |
| 4 | `ce56f69` 直接ブレンド連鎖を量子化キー表に | `perf/kasane-lut` | `930e0bc` | — | `solid arm ... 214499328 per-channel comparisons ... 0 moved, worst channel step 0` / `alpha arm 16 levels ... 2047578 moved (24.505%), worst channel step 2` |
| 5 | `58b717a` 565 ブレンド/パックの /255 を 255→256 へ | `perf/kasane-alpha256` | `1af4a90` | `docs/perf/kasane-alpha256.md`、`tools/pie/run_models.py` | `default ... 140838 of 3888000 panel pixels (3.622%) move ... worst 565 step 1` / `mismatches=0` |
| 6 | `1b643a2` 不透明群の層の連なりを 1 枚のアフィン写像へ | `perf/kasane-affine` | `b6a8211` | `tools/kasane_contract/test_group_affine.c` | `folded arm and isolated tile chain agree on 6966000 pixels` / `120 frames identical both ways (arms 0/1)` (hash `3db0d222`) / `folded rows: switch off=0, on=10575` |
| 7 | `0896f47` x 依存の画素値を行ごとの表へ | `perf/kasane-rowtable` | `f7e46dc` | `tools/kasane_contract/test_row_table.c` | `table values match sample on 47939774 pixels over 3313229 rows` / `sample calls in one full REPLACE frame: (1,0) reference=19004 (1,1) table=102` |
| 8 | `eb22dfb` 群の畳みを不透明でない連なりへ（近似） | `perf/kasane-affine` | `7795bb9` | `docs/perf/kasane-group-affine.md`、`test_group_affine.c` | `step 2 moved 86650 of 3888000 pixels ... worst 8-bit channel step r/g/b=9/5/9` |
| 9 | `8e57dcb` 回転画像のソース添字を商と剰余の加算ステップへ | `perf/kasane-imgopt`（anchor / pet / stretch / tile の前提） | `d9a17d1` | `docs/perf/kasane-image-transform-step.md` | `image rotate arms PASS: 11833 configs, 23665 panel hashes, worst pixel step 0, fetches identical` |
| 10 | `d025d9d` 拡大縮小画像スパンのソース添字を同型に | `perf/kasane-stretch` | `b9e65e4` | `docs/perf/kasane-image-transform-stretch-step.md`、`test_image_render.c`、`test_image_stretch_arms.c` | `image stretch arms PASS: 131 configs, 259 panel hashes, worst pixel step 0, fetches identical` |
| 11 | `e7282ed` 群のタイルを子の被覆区間と到達判定に合わせる | `perf/kasane-tile` | `a84b118` | — | `exact arms: 4050000 pixels identical (reach on/off, tile 64/16, smooth=1)` / `tile counters: ... waste 27.25%, blocks 214564 skipped 39773` |
| 12 | `b93dcf1` PET 画像 provider の行の重複復号を 64 行キャッシュで消す | `perf/kasane-pet` | `dc0bb19` | `main/pet/ksn_pet.c`、`main/pet/ksn_pet.h`、`docs/perf/kasane-pet-row-cache.md`、`test_pet_row_cache_arms.c` | `48495 panel comparisons, worst pixel step 0, hash d6eb4edb, cache decodes 0.019% of the fetches, 64 of 330420` |
| 13 | `ca2e3fd` 滑らかな層をブロック定数＋画素増分で書く | `perf/kasane-tile` | `d0bf4a1` | `tools/kasane_contract/test_group_tile.c` | `approximate arm: worst step 1 5/6/5 level(s) ... exact arm: 0` / `parameter space: 1555200 pixels compared, 9942 moved (0.64%)` |
| 14 | `a3e6426` 群のタイル面の測定を記録する（doc のみ） | `perf/kasane-tile` | `7391fab` | `docs/perf/kasane-tile.md` | —（文書コミット） |

補足:

- 前提コミット `8e57dcb` は 5 枝に重複して載っているので、ユニーク 14 本のうち 1 本（#9）である。
- 12 枝の `-` 行（patch 同値＝既取り込み）は `b17262a`（imgopt の前提）/ `590f5aa`・`55b3c74`
  （anchor）/ `b4b9674`（decode のテスト）/ `12ecdd5`（lut のテスト）で、いずれも
  `perf/kasane-opt` に内容がある。
- 元コミットの指示命令数（例: 回転スパン 18 命令/画素・除算 2 本 → 15-16・0 本、
  PET の重複復号 −70.4%）は**各分岐の文書の値**である。この作業では再測定していない
  （この作業が測ったのは §4 のとおり「この木でハーネスが何を出したか」だけ）。
- **実機のミリ秒は 14 本のどれにも無い。** 14 本すべてがホスト計測（命令数・回数・画素・
  オブジェクトサイズ）で、各分岐の文書がそう明記している。

---

## 3. 実行時スイッチと既定（この木、`git grep -nE '^(int|bool) g_ksn_'`）

| スイッチ | 型 / 既定 | 由来 | 意味 |
| --- | --- | --- | --- |
| `g_ksn_decode_once` | `int` = 1 | #2 `da13f79` | フレームコマンドを 1 回だけ展開（1 = 新経路） |
| `g_ksn_row_coverage` | `int` = 1 | #1 `bbcb69c` | 行ごとの x 区間で被覆を解く（1 = runs 腕、0 = 画素ごとの述語） |
| `g_ksn_blend_lut` | `int` = 1 | #4 `ce56f69` | 定色コマンドの量子化キー表（厳密） |
| `g_ksn_blend_lut_alpha` | `int` = 0 | #4 `ce56f69` | TEXT の 16 段近似（既定 OFF。動く画素は §4 の harness 値） |
| `g_ksn_scale256` | `int` = 0 | #5 `58b717a` | 255→256 粗スケール（既定 OFF = 厳密。ON は 3.622% の画素が動く） |
| `g_ksn_row_table` | `int` = 1 | #7 `0896f47` | x 依存の画素値を行ごとの表へ |
| `g_ksn_group_affine` | `int` = 1 | #6 / #8 `1b643a2` `eb22dfb` | 1 = 不透明群の畳み（ビット一致）、2 = 非不透明への近似（既定 OFF） |
| `g_ksn_image_rotate_step` | `bool` = true | #9 `8e57dcb` | 回転スパンの商と剰余の加算ステップ |
| `g_ksn_image_stretch_step` | `bool` = true | #10 `d025d9d` | 拡大縮小スパンの同型 |
| `g_ksn_tile_pixels` | `int` = 64 | #11 `e7282ed` | タイルのブロック幅（64 / 16） |
| `g_ksn_tile_reach` | `int` = 1 | #11 `e7282ed` | 子の到達区間でループを絞る（厳密） |
| `g_ksn_tile_smooth` | `int` = 1 | #13 `ca2e3fd` | 滑らかな層のブロック定数＋増分（1 = 厳密、2 = 近似） |
| `g_ksn_pet_row_cache` | `bool` = true | #12 `b93dcf1` | PET provider の 64 行キャッシュ |
| `g_ksn_visible[]` / `KSN_BLEND` | 計数のみ | #3 `286573d` | 可視しきい値の当たり率。`KSN_COUNT_VISIBLE` のビルドでだけ計器を通る |

**実機 A/B にそのまま載るのは `int` のものだけ**である。`main/app_session.c` の `KASANE_AB` は
`{const char *name; int *flag;}` の表へ 0/1 を書くので、`bool` の 3 本
（`g_ksn_image_rotate_step` / `g_ksn_image_stretch_step` / `g_ksn_pet_row_cache`）は
**そのままでは表に載らない**（4 バイト書き込みを 1 バイトのオブジェクトへ行う）。載せるなら
`int` へ変えるか、別の代入口を用意する。実機の計測は今回も未実施である。

---

## 4. この作業で走らせた検証（すべてホスト、実機なし）

| 何を | どう | 結果 |
| --- | --- | --- |
| 契約スイート | `tools/kasane_contract/run.sh`（リポジトリのものは書き換えず、`/tmp` の写しから sanitizer の旗の文字列だけ外した `-O2 -fstrict-aliasing` 腕） | **exit 0**、PASS 行 **71**（重複を除いた実ケース **34**、2 腕 × ケース＋`probe` など） |
| 分岐が持つ専用ハーネス | `run.sh` に結線済み（row table / visible skip / blend lut / coverage runs / pet row cache arms / image rotate arms / image stretch arms / decode reuse / render prof / span count） | すべて PASS。#2〜#14 の両腕比較はここで回っている |
| group affine | `bash tools/kasane_contract/run_group_affine.sh` | rc=0、`group affine PASS` |
| group tile | `bash tools/kasane_contract/run_group_tile.sh` | rc=0、`group tile: PASS (arithmetic swept, exact arms identical, approximation measured and named, 120 frames compared)` |
| PIE モデル | `python3 tools/pie/run_models.py` | rc=0、`mismatches=0` / `all models agree` |
| PIE カーネル | `python3 tools/pie/test_kernels.py`（unittest） | `Ran 10 tests ... OK`。`test_piesim.py` 4 tests OK、`test_frost.py` PASS |
| JS スイート | `node tools/test_pet.cjs` / `node tools/test_companion.cjs` | どちらも PASS（`PASS: selection, care, sleep, naming, ...` / `PASS: companion placement, providers, ...`） |
| VM の JS コーパス | `bash tools/vmtest/build.sh o2` → `bash tools/vmtest/run.sh --variant o2` | `corpus [o2]: 63 passed, 0 failed`、rc=0（VM 主線のコーパス。`.cache/vmtest` を汚しただけで追跡ファイルは変えない） |
| ファームウェア | `. /opt/esp-idf/export.sh && idf.py -B build_kasane build`（`.cache` は `/workspace/pjs-render` への既存シンボリックリンク。lock の md5 は `2af139a85ae590d68d5a772efd3b1039` で一致） | **rc=0**、`Project build complete` |
| サイズ | `stat -c %s build_kasane/cardputer_pocketjs.bin` | **2,210,544 B** |
| メモリ | `python3 tools/memlog.py --map build_kasane/cardputer_pocketjs.map` | `commit=d3fc8ba diram=144556 flash=1611976` |
| ASan | **使っていない**。この容器の ASan は `AddressSanitizer:DEADLYSIGNAL` の無限ループで、統合記録 §0 のとおり pristine でも再現する。sanitizer の腕が緑になった段は前 3 次統合でも 1 つも無い | — |

ビルドは doc 追加前（= `d3fc8ba` の木）で、**この作業はソースを 1 バイトも変えていない**ので
バイナリとサイズは枝の現在値そのものである。ASan を外した写しを使った関係で、
リポジトリの `tools/kasane_contract/run.sh` は 1 文字も書き換えていない。

---

## 5. 取り込まなかったもの / 対象外

| 対象 | 判断 |
| --- | --- |
| `perf/kasane-kernel` / `perf/kasane-measure` | 統合記録 §1 の段 1・段 4 で既取り込み。**ローカルの枝はこの作業の途中（2026-09-16）に既に削除されていた**（`git branch --list 'perf/kasane*'` に現れない）。判定対象の 12 枝には含めていない |
| 12 枝の `-` 行（patch 同値） | `b17262a` / `590f5aa` / `55b3c74` / `b4b9674` / `12ecdd5`。内容は `perf/kasane-opt` にある |
| 第 3 次統合で「統合木側の算術を残した」2 箇所（`eb22dfb` の合成ループ、`ce56f69` の LUT と行テーブルの順序） | 分岐側の実装をそのまま採らず、統合木側（行テーブル・タイル・ブロック幅を持っている側）の算術で同じ最適化を実現している。判断の理由は統合記録 §6.2.2 / §6.2.8。**同じ画素集合・同じ切替**なので、最適化としては入っている |
| 黙って落としたもの | **無し**。今回「内容が無い」と判定したコミットは 1 本も無い |

---

## 6. 削除してよい topic 枝

内容が `perf/kasane-opt` にあることを §2 で確認した枝。削除は**こちらの指示で後に行う**
（この作業ではローカルもリモートも消していない）。

```
perf/kasane-affine     perf/kasane-alpha256   perf/kasane-anchor     perf/kasane-coverage
perf/kasane-decode     perf/kasane-imgopt     perf/kasane-lut        perf/kasane-pet
perf/kasane-rowtable   perf/kasane-stretch    perf/kasane-tile       perf/kasane-visible
```

（`origin/` 側も同じ 12 本が残っている。削除するまでは `git cherry` の `+` 行 18 本は消えない。）

---

## 7. まだ負っているもの

- **実機の時間は 1 つも無い。** §3 のスイッチを実機で 0/1 に振る A/B（`KASANE_AB` の表）と
  `KASANE_PAINT` の `render_ms` が、この系統の採否を決める最後の判定である。`bool` の 3 本は
  そのままでは表に載らない（§3 の補足）。
- **`memlog.py` は今回ようやく取れた**（`d3fc8ba`）。ただし取得できたのは
  「HOST（コンテナ）のビルドの値」で、実機の実測ではない。タイルの `.bss` や PET の 8,208 B を
  実機の DRAM 予算と突き合わせる作業は残っている。
- 基点 `origin/vm/design-contracts` は 1 コミット（`36e4e04` `chore(esp-idf): refresh component
  dependency lock`）だけ先に進んでいる。**統合していない**。理由は、それが
  `dependencies.lock` を書き換えるため、いま共有している `.cache`（`pjs-render` と同一 lock
  `2af139a8`）の系列キーが変わるから。基点を進めるときは lock の更新と `.cache` の作り直しを
  セットで行うこと。
- 12 枝の削除（§6）と、そのあとの `git cherry` の `+` 0 の確認。
