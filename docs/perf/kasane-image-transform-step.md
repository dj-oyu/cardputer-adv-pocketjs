# 回転画像スパンのソース添字 — 商と剰余の加算ステップ（切替つき）

対象: `perf/kasane-imgopt`、基点 `origin/vm/design-contracts` = `e323e56`。
実機には触れていない（`/dev/ttyACM0` は別セッション）。**ミリ秒は 1 つも無い**。ここにある数は
target ツールチェーンの `objdump`/`size`/`nm` の実測と、ホストで走らせたテストの出力である。
調査と候補の判断は `docs/perf/kasane-image-transform-recon.md`。

---

## 1. 変えたもの

`main/ui/kasane/ksn_render.c` の `image_read` の rotate 分岐に、**厳密に同じソース添字を出す
加算ステップ経路**を足し、`g_ksn_image_rotate_step`（`ksn_render.h`、既定 `true`）で切り替える。

- 旧経路（`g_ksn_image_rotate_step == false`）は**そのまま残した**。1 画素ごとに
  `u*sw/umax` と `v*sh/vmax` を 64bit 有理除算で求める（`ksn_render.c:83-101`）。
- 新経路（既定）は、同じ `floor(U/B)` と `U mod B` を **1 画素あたり加算・比較・条件付き減算
  だけ**で進める（`ksn_render.c:102-146`）。除算は **16 画素の窓に 2 本**（アンカー）だけ。
- 受け入れ判定もソース添字側へ移した（`0 <= U < B*sw` ⟺ `0 <= sx < sw`）。同値である理由は
  調査文書 §7・§11。
- `!sw || !sh || !w || !h`（退化した窓）と `rotation == 0` は従来どおりの経路に入る。
  スイッチは**変更の全部を戻す**（切ればアンカーの除算も含めて旧経路に戻る）。

増分の定義は調査文書 §7 のとおり: `D = du*sw` を `D = q*B + r`（`q = floor(D/B)`, `0 <= r < B`）に
分け、`rem += r` が `B` を跨いだら `q+1` を足して `rem -= B`。`rem, r ∈ [0,B)` なので跨ぎは
高々 1 回で、条件は `rem >= B - r` と書き換えて**加算のオーバーフローを避けている**。

## 2. 厳密性

1. **算術の総当たり**（ホスト、使い捨ての `/tmp` プログラム）: 旧式と新式のソース添字・
   受け入れを 852,295,680 点（12 通りの dest 寸法 × 9 通りの source 窓 × 回転 0..2047 ×
   画素位置）で比較し、**不一致 0**。
2. **既存の独立参照テスト**: `tools/kasane_contract/test_image_render.c` は倍精度の
   oracle（`acos`/`cos`/`sin`/`floor`）と全 32,400 画素を比べる。rotation 付きの
   360 ケース（bounds も source 窓も毎 step 変わる）を**新経路が既定**で通る →
   `image render PASS: crop/scale/alpha/group/retry, 240 moving and 360 stretch PATCH/full comparisons`。
3. **アーム比較テスト（新規）**: `tools/kasane_contract/test_image_rotate_arms.c`。
   1 バイナリの中に両アームを持ち、場面を**アームごとに作り直して**（`test_render_prof.c` と
   同じ作法）全画素を比べる。出力:

   ```
   animated track: 120 frames both arms, alternating arm identical, 330420 fetches
   image rotate arms PASS: 2104 configs, 4207 panel hashes, worst pixel step 0, fetches identical
   ```

   - 2,104 通りの場面（回転 0..1023 の全 1024 + サイズ・source 窓 18 組 × 回転 28 +
     source 原点 16 組 × 回転 11 + clip/不透明度/群 392 + 動く場面 6）。
   - 各場面で「壊れたフレームのアーム既定」と「全面再描画のもう一方のアーム」を
     32,400 画素で比較。**最悪の画素差 0**。
   - provider の取得回数も両アームで**一致**することを課している（同じブロックを要求すること）。
   - 120 フレームの animated transform track（位置 + 拡大縮小 + 2 回転ぶんの回転、30 fps、
     loop）を、既定・旧・**フレームごとに交互** の 3 通りで回して同一のハッシュ。
     交互にすることで「直前にどちらのアームが描いたか」への依存も見ている。

## 3. target `-Os` の実測（命令数・オブジェクト）

| 項目 | 基点 | 変更後 | 出所 |
| --- | ---: | ---: | --- |
| rotate の 1 画素（ループ本体。アンカーの除算は別） | **121 + 2 × 223**（`__divdi3` 2 本）= 567 | **100**（うちステップ 24、スクラッチ消去 19、域判定 10、ブロック取得ブロック 22、合成 11、キー比較 8） | `objdump -d` の逆アセンブルと `-g` の行対応 |
| アンカーの 64bit 除算 | 画素ごと 2 本 | **窓（16 画素）ごと 2 本** + 定数 58 命令 | 同上（`__divdi3` の reloc 2 箇所） |
| `image_read` 全体 | 346 | 604（両アーム同居） | `objdump` の命令数 |
| `ksn_render.o` `.text` | 5,544 B | **6,184 B** | `size -A` |
| `.rodata`（sine 表 514 B + bayer 16 B） | 566 B | **566 B**（不変） | `size -A` / `nm -S` |
| `.data` | 0 B | **1 B**（スイッチ `g_ksn_image_rotate_step`） | `nm -S` |
| `.bss` | 0 B | **0 B** | `size -A` |
| libgcc の呼び出し | `__divdi3` | `__divdi3` のみ（`__moddi3` を出さない書き方を選んだ） | `nm -u` |

つまり **1 変換画素あたりの命令は 567 → 100（約 5.7 分の 1）**、画素ごとの 64bit 除算は
**2 本 → 0 本**（16 画素ごとに 2 本）。DRAM の増分はスイッチの 1 バイトだけで、表は増減していない。

## 4. 残っているコスト（次の一手）

窓ごとのアンカーは 2 本の `__divdi3`（223 命令 × 2）と定数 58 命令で、16 画素に割ると
**約 31 命令/画素** — いまの画素ループ本体（100 命令。うち取得ブロック 22）と同程度である。
したがって次に効くのは「アンカーを行ごとの表にする／コマンドの先頭から行単位で状態を
引き継ぐ」ことで、調査文書 §6 の候補 A の残りである。今回は**1 コミット 1 関心事**のため
入れていない（ステップの厳密性だけを確定させた）。

## 5. 負っているもの

- **実機の時間は未測定**。この容器では測れない（ホストで測れるのは命令数と回数だけ）。
- `_cy` 相当のサイクル計器は kasane の描画経路に無い（`kasane-opt-survey.md` §8）。
- アンカー表の次段は未実装（§4）。
