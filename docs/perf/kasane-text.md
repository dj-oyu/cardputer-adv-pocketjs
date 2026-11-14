# Kasane のテキスト描画の最適化（damage の絞り込みと span）

> 吸収した旧文書: `kasane-text-damage.md`、`kasane-text-span.md`。各節の中の節番号（§N）は旧文書のものを保つ。

## テキストの過剰再描画: 帯を名指しする invalidate と、帯の列範囲（旧 kasane-text-damage.md）

2026-09-19。文字の再描画が広すぎる件を実機の計器で切り分け、3つの原因を順に直した記録。
§1〜4 が段階1（帯を名指しする invalidate）、§5 が段階2（damage が帯ごとの列範囲を持つ）、
§6 が段階3（TEXT の damage が変わった字だけになる）。
**段階2 だけではアプリのテキストは速くならず、3つ揃って hello の1桁更新が
11,520 B → 768 B、3.22 ms → 1.92 ms になる。** 残る場合は §7。

### 1. 何を測ったか

計器は既存の `KASANE_PAINT`（`main/app_session.c`、30フレーム窓）。読むのは
`band_mask` / `bytes` / `bands` で、`band_mask=0x1ffff` は「1フレームで17帯すべてを
再合成して 64,800 B 送った」という意味になる。ビルドは `build_text`、実機は COM3。

**計測A — hello のカウンタ更新**（`tools/benchmark_app.py --port COM3 --samples 4`、
`KEY PRESSES: n` の1桁が変わるだけの PATCH）:

```
turn_ms=0.43 render_ms=1.55 send_ms=1.67 bytes=11520 bands=3 band_mask=0x00e00
```

4本とも同じ帯（9,10,11）。**変わった画素は caption 1文字ぶん 6×12 px、
再描画したのは 240×24 px。** 面積比で 40 倍を塗り直している。

**計測B — `pocket.input.text` に打鍵**（USB診断 `K` のテキスト欄へ70打鍵、
`band_mask` を全部拾う）。修正前の16本の窓に `band_mask=0x1ffff`（17帯・64,800 B）が
2本現れた。**1打鍵が全画面再描画になる経路があった。**

### 2. なぜ全画面だったか

編集欄は Kasane のコマンドではない。`pocket_text_overlay()` が
`app_session.c` の present 経路から、Kasane が描いた各帯の上へ直接合成している
（`pocket_text.h` の冒頭が書いているとおり、欄はホストのもので、ゲストは確定文字しか見ない）。
したがって欄の変化は **Kasane が自分のコマンド差分からは導けない damage** で、外から教える必要がある。

教える口は `app_force_redraw()` しかなく、その先は
`pocket_kasane_invalidate()` → `ksn_runtime_invalidate()` → `ksn_core_invalidate()` で、
`ksn_core_damage()` は `invalidated` を見て 17 帯を返していた。
`pocket_text_key()` は先頭で無条件に `dirty=true` を立てるので、
**受理されなかったキーでも全画面再描画**になる。

### 3. 置いたもの

`ksn_core` の `bool invalidated` を 2つの帯集合にした。

- `invalidated`: owner が要求し、まだ `prepare_frame` が取っていない帯。
- `repair_bands`: 飛行中のフレームが負っている帯。present 成功で 0 に戻る。

`ksn_core_invalidate()` は両方に全17ビットを立てる従来どおりの入口で、
新しい `ksn_core_invalidate_bands(core, mask)` が帯を名指しする。
`ksn_core_damage()` は早期全面返却の条件から `invalidated` と `repairing` を外し、
代わりに `*bands` を `repair_bands` で**種として初期化**してから命令差分を OR する。
修復フレームは新旧が同じ bank なので差分は空になり、この種だけが damage になる。
`frame.full_redraw` は「全面」の意味を保つため、無指定 invalidate のときだけ真にする。

上位は素通しの `_bands` 版を足しただけ（`ksn_view_host_invalidate_bands` →
`ksn_runtime_invalidate_bands` → `pocket_kasane_invalidate_bands` →
`app_force_redraw_bands`）。`pocket_text` 側は `dirty` と一緒に
**欄の占める帯を累積**する（`field_bands()`）。累積なのは、閉じたセッションは
自分の座標を持って行くのに、空けた行の repaint は残るから。
`pocket_text_take_dirty()` は bool ではなく帯集合を返すようになった。

### 4. 結果

**ホスト（`tools/kasane_contract/test_repair.c`、ASan/UBSan と O2 の両方で PASS）**:
パネルを 0x5a5a で塗り潰してから帯 3,4 だけを invalidate すると、
`transfers==2`、`stats.bands==0x18`、`transferred_bytes==7,680`、
帯 3,4 は確定画像と一致し、**帯 0〜2 は 0x5a5a のまま**。
パネル外のビットは無視、空集合は no-op であることも検査する。
これが恒久的なガードで、「17 回ではなく 2 回しか転送しない」を機械が守る。

**実機**: 修正後に計測Bを同じ手順で取り直すと `0x1ffff` は消えた（16本の窓に0本）。
ただし **K診断は自前の回転アニメーションで毎フレーム14帯 dirty なので、
この画面では改善幅を測れない**。帯数の上限が下がったことしか言えない。
`kasane_input_device_test.py`（hello/pet/K/text、2周）と
`smoke_device.py --cycles 20` はいずれも PASS。

**幅の見積もり（実測ではない）**: ホスト試験のログに出る実際の欄は
`TEXT_OPEN 224x20 at 8,56`、つまり行 56..75 で帯 7,8,9 の 3 帯。
静止した画面でこの欄に打鍵した場合、17 帯 64,800 B → 3 帯 11,520 B。
**静止画面＋編集欄という組み合わせの実機計測はまだ無い**（hello に欄がなく、
pet の NAME 編集は NVS を書くので今回の試験では触っていない）。

**静的 DIRAM**: `memlog` で +16 B（`pocket_text.c.obj` +4、残りは core の帯集合と整列）。

### 5. 段階2: damage が帯の列範囲を持つ（2026-09-19）

計測Aが示していたのは、この invalidate では消えない構造のほうだった。
`command_bands()` は y しか見ず、`ksn_render.c` の帯ループは毎帯 240 px 全幅を塗り、
`transferred_bytes` も全幅で数えていた。

#### 置いたもの

`ksn_core_damage()` の出力を `uint32_t` から `ksn_damage`（帯集合＋帯ごとの `x0/x1`、
17×2×int16 = 72 B、呼出側のスタック）へ変えた。変化した命令の**旧と新の切り取り済み矩形**を
帯ごとに union する。修復・全面再描画の帯だけは列を語る者がいないので [0,240) を入れる。

display port に `present_rect` を**追加**した（`present` は不変なので既存の実装は無改造）。
これを持つ port にだけ renderer は狭い帯を渡す。**狭めるかどうかは画素を書く前に決める**
—— 帯を部分幅で合成すると共用ストリップの残りは1つ前の帯の画素を持ったままなので、
全行しか送れない port には全行を渡さねばならない。
`board_present_rect()` は CASET/RASET/RAMWR で窓を張り、行を詰めながらバイトスワップする。
`board_capture` 実行中は PIX が全240列を印字するので `present_rect` を引っ込める
（`board_capture_active()`）。

#### 実機で分かった2つの落とし穴（どちらも実測）

1. **端数の窓は転送経路を落とす。** 最初の実装（窓 184 px）は
   `bytes 11,520 → 8,832` と減ったのに **render 1.55 → 1.65 ms、send 1.67 → 1.88 ms と悪化**した。
   詰め直した行が 32 B 境界にも 32 B 倍数にもならず、board のバイトスワップが PIE から
   スカラーへ落ち、背景塗りも `fill_blocks` から外れていた。
   そこで**窓を 16 画素へ外側丸め**する（16 画素 = 32 B = カーネル1ブロック）。
2. **ほぼ全幅の窓は割に合わない。** 全幅経路は RAMWR を開いたまま流すので再アドレス指定が
   0 コマンド（`board.c` の `next_row`）。狭い窓は帯ごとに 3 コマンド払う。
   閾値 `KSN_NARROW_MAX = 192`（80%）を超える帯は全幅のまま送る。

#### 結果

**ホスト**: `tools/kasane_contract/test_narrow.c` を追加。同じ台本を2つの core へ流し、
片方に `present_rect` を与え、120 フレームで**パネルを全画素比較**する。
62 フレームが実際に狭まり（234 回の窓転送）、**全画素一致**。
狭まった回数も検査するので「何も狭めずに通る」ことはない。
パネルとストリップは毎フレーム 0xa55a で汚してから描くので、書き忘れた列は毒として現れる。
契約スイート全体（ASan/UBSan・O2）、pocket_text、Kasane QuickJS、session dispatch も PASS。

**実機**: `kasane_input_device_test`・`smoke --cycles 20`・`test_settings`・`capture_home` すべて PASS。
静的 DIRAM 増分 0。

**そして hello は速くならなかった。** `bytes=11520 bands=3` のまま、render 1.60 / send 1.68 ms。
理由ははっきりしている —— counter の `bounds` は `[28,77,212,89]` で **184 px 幅**、
16 画素丸めで 208 px、閾値 192 を超えるので全幅のまま送られる。
**宣言された矩形が広いので、damage を矩形にしても狭くならない。**

### 6. 段階3: TEXT の damage が変わった字だけになる（2026-09-19）

`ksn_core_damage()` は `memcmp` で文字列の違いの**有無**しか見ず、違えば bounds 全体を
damage にしていた。`KEY PRESSES: 5` → `6` は caption 1 文字 6 px である。

#### 置いたもの

`ksn_text_port` に **1 スカラーの送り幅**を返す問い合わせを足した:

```c
unsigned (*advance)(void *,ksn_font font,uint32_t codepoint);
```

字形の有無に依存しない**論理メトリック**なので、`main/text/ksn_font.c` の実装は
`(cp<128?6:font==KSN_BODY?12:8)*scale` の 1 式で、span の 2 本のアームが
ペンを進めるのに使っているのと同じ値である。これで `ksn_core` はフォントに依存しないまま、
「どのスカラーが違うか」を「どの列か」へ変換できる。

`ksn_core_damage()` は port を受け取り、TEXT 同士の差分で
**箱・clip・font・色・bank offset が同じ**ときだけ、新旧を**スカラー単位で同時に歩く**。
歩きながらペンを積み、最初に違ったスカラーの左端から最後に違ったスカラーの右端までを damage にする。

- 送り幅が途中で食い違う、またはスカラー数が違えば、そこから右は全部ずれるので**箱全体**に戻す。
- **両方の reveal より後ろ**のスカラーは描かれないので、そこの違いは違いではない。
  したがって「隠れている部分だけを書き換えた setText」は damage を 1 画素も生まない。
- reveal の変化は「見えるか見えないか」の違いとして同じ歩きで扱われる。

#### 結果

**実機（hello、`benchmark_app.py`、1桁更新の PATCH）**:

| | bytes | bands | render | send | 合計 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 着手前 | 11,520 | 3 | 1.55 ms | 1.67 ms | 3.22 ms |
| 段階1+2 の後 | 11,520 | 3 | 1.60 ms | 1.68 ms | 3.28 ms |
| **段階3 の後** | **768** | 3 | **1.07 ms** | **0.85 ms** | **1.92 ms** |

**転送 15 分の 1、フレーム 40% 減。** 768 B は 3 帯 × 16 列 × 2 B × 8 行で、
予測値と一致する（窓は 106..112 px が 16 画素丸めで 96..112）。

**実機（`pocket.input.text` に打鍵、USB 診断 `K`）**: 着手前は 38,400〜64,800 B /
render 12.8〜36.2 ms だったものが **13,312〜29,952 B / render 8.0〜29.5 ms**。
この画面は自前の回転アニメーションで毎フレーム 14 帯 dirty なので、
ここに出ているのは打鍵ぶんだけの改善ではない。

**ホスト**: `test_narrow.c` に**符号点で字形が変わる固定送り幅のフォント**を足し、
テキスト命令と3種の更新（同じ長さの1桁・長さの変わる書き換え・reveal）を台本へ入れた。
広いアームには `advance` を渡さない（＝箱全体が damage）ので、両アームの damage は**違う**。
それでも 120 フレームのパネルが全画素一致し、同じ長さの1桁更新 9 回で
**狭いアームのバイト数が広いアームの 1/4 以下**であることを検査する。
契約スイート（ASan/UBSan・O2）、pocket_text、Kasane QuickJS、session dispatch、lessons も PASS。

**実機回帰**: `kasane_input_device_test` / `smoke --cycles 20` / `test_settings` / `capture_home` PASS。
静的 DIRAM 増分 0。

### 7. 追補: 長さが変わる書き換えも「箱」ではなく「中身」まで（2026-09-19）

§6 の歩きは、スカラー数か送り幅が食い違った時点で**宣言された箱全体**へ落ちていた。
`9` → `10` のような更新はそこに入る。

落ち先を箱から**描かれている幅**へ変えた。歩きが崩れた位置から右は確かに全部ずれるが、
ずれる範囲は「両方の run のうち広いほうの実描画幅」で止まる —— reveal が通した
スカラーの advance 合計であり、命令が確保した箱ではない。テキスト命令の箱は普通
伸びしろを持って宣言されるので、これは**「書いた分」と「予約した分」の差**になる。

ホスト（`test_narrow.c`）では狭まるフレームが 75 → 81、同じ長さの1桁更新が 9 → 12 本、
**長さの変わる更新 3 本が全幅の半分以下**になった。120 フレームの全画素一致は維持。

**実機の勝ちは観測できていない。** witness に選んだ IMU CALIBRATION（6 つの数値を毎フレーム
書き換え、値の長さが符号と桁で変わる）は
`bytes 5,120〜9,216 / render 4.0〜4.3 / send 1.95` → `5,120〜9,216 / 3.9〜4.1 / 1.90` で、
差は測定のばらつきの中にある。このアプリでは同じ長さの更新が多く、§6 の厳密な経路に
既に乗っていたためと考えられる（確かめてはいない）。
**したがってこれは「悪い最悪ケースを消した」変更であって、実測の改善ではない。**
なお imucal で一番大きい項は `turn_ms=4.4`（ゲストの JS）で、描画ではない。

### 8. まだ箱全体になる場合

- **ネイティブ画面**（home / overlay / picker / editor）は Kasane が所有していないので
  この経路に乗らない。[Kasane残タスク](../kasane/roadmap.md)のFramework全体を参照。
- **`advance` を持たない text port**（ホスト試験の広いアーム）。実機は `ksn_font_port` が
  持っているので該当しない。

## テキスト span のインクループ（境界 4 の残り、実機の計器が指した 1 本）（旧 kasane-text-span.md）

2026-09-15、ブランチ `perf/kasane-opt`（親 `e9ff9a1`）。対象は
`main/text/ksn_font.c` の `span`、つまりテキスト 1 命令の 1 行 × 64 画素チャンクを
被覆マスクへ落とすループである。**このファイルは今まで一度も計器の対象になっていない**
——[kasane-opt-survey.md](kasane-opt-survey.md) §10-2 が「フォント側の内部ループ
（`ksn_font.c:32-49` のグリフごとの 64 画素窓走査）の命令量は本調査で数えていない」と
書いているのがそれで、本稿はそこを埋める。

### 1. なぜここか（実機の推測ではなく、実機の数字から）

`tools/host_kasane_opt_ab.py` の A/B は `KASANE_PAINT` に
境界 7a のカウンタ（`fill/span/blend/read` の回数と `rsr.ccount` のサイクル）を
出している。2026-09-15 の 3 回目の走行 `opt-ab-20260915T203701Z/raw.log` の
`prof=1` の窓（30 フレーム）がこれ:

```
KASANE_PAINT turn_ms=1.25 render_ms=2.18 send_ms=1.59 bytes=11520 bands=3 band_runs=1
  band_mask=0x00e00 prof=1 fill_n=90 fill_cy=174410 span_n=1200 span_cy=7490983
  tile_n=0 tile_cy=0 blend_n=1170 blend_cy=4600276 read_n=600 read_cy=515575 frames=30
```

1 フレームあたりに直すと（240 MHz、`render_ms` 2.18 ms = 523,200 サイクル）:

| 括り | 回数/フレーム | サイクル/フレーム | render_ms 比 |
| --- | ---: | ---: | ---: |
| **span**（テキスト被覆マスク） | 40 | **249,699** | **48%** |
| blend（タイル合成・群の段・直接経路） | 39 | 153,342 | 29% |
| read（コマンド展開） | 20 | 17,186 | 3% |
| fill（連続 fill） | 3 | 5,814 | 1% |
| 括りの外（帯の取り込み・damage・再描画判定） | — | 97,159 | 19% |

つまり**パッチ 1 フレームで最大の残りはテキストの span**で、1 呼び出し 6.2 k サイクル。
これはアプリ（`apps/hello/main.js`）が毎回書き換える `KEY PRESSES: n` の再描画である。

参考までに、同じ数字で `stalls.py` が指す `ksn_blend_pie` のストール 5 本を評価すると
`blend_n` 相当の 39 ブロック × 5 = 195 サイクル/フレームで、**render_ms の 0.04%**。
`HANDOFF.md` が「核の追い込み」として挙げていたストール除去は、この計器の前では
レバーではない（算術で落ちる）。span の側が 1,000 倍以上大きい。

### 2. 何が無駄だったか

`span` は「チャンク `[x, x+count)`、行 `y`」に対して被覆マスク（0 か 255）を書く。旧ループは
**グリフ 1 個ごとに `count`（最大 64）列を歩き**、各列で

- `gx = x + i - pen` を出して `gx < 0 || gx >= width` を判定し、
- `row = gy / scale`、`col = gx / scale` を**除算**で出し（`scale` は
  `font == KSN_DISPLAY ? 2 : 1` の実行時値なので、コンパイラは `quou` を出す）、
- 5×7 か字形ビットを引く。

セルは `advance` 幅（caption 6、display 12、本文 12、全角 8×scale）しかなく、
インクが出るのはさらに先頭 `width` 列だけなので、**64 列のうち 6〜12 列以外は
「何も書かないと分かっている列」**を歩いていた。しかもチャンクが文字列の後ろにあるほど
前置きのグリフも全部歩き直すので、1 行の再描画は
`Σ_k (k 番目のチャンクまでのグリフ数) × 64` 列になる。

`objdump`（`-Os`、対象ツールチェーン）で旧 `span` は **255 命令**、インクループ本体が
**1 列あたり 70 命令・除算 4 本**。

### 3. 置いたもの

`main/text/ksn_font.c` の `span` だけ（＋`ksn_font.h` の宣言、`app_session.c` の
切替一覧に 1 行）。

- セルが担当する列を先に解く: `left = pen - x`、`right = left + width`、
  `first = max(0, left)`、`end = min(count, right)`。`i - left` が `gx` になるので、
  ループ内の `gx < 0 || gx >= width` の判定は**範囲そのものに吸収される**。
- `scale` は 1 か 2 なので `gy >> shift` / `gx >> shift`（`shift = font == KSN_DISPLAY`）。
  この範囲では `gx` も `gy` も非負なので、シフトは除算と同じ値。
- `g_ksn_span_narrow`（既定 1 = 上の歩き方）と **0 = 変更前の歩き方**を同じバイナリに
  置き、`app_session.c` の `switches[]` に 1 行足した。この規模はビルド間の比較では
  判定できない（配置で 15% 動く、[pie-simd.md](pie-simd.md) §6.3）ので、
  `tools/host_kasane_opt_ab.py` が 13 番目の腕として 0 を回し、同じフレーム列の中で
  新旧を比べる。制御腕の本文は変更前の逐語コピーで、コメントでその旨を明示してある。
- `#ifdef KSN_SPAN_COUNT` のカウンタ（`KSN_TILE_COUNT` と同じ流儀）で
  「歩いた列数」と「復号したセル数」を数えられるようにした。契約テスト
  `tools/kasane_contract/test_span_count.c` は**両腕**の列数を固定する
  （制御腕が変更前の歩き方でなくなったら、A/B が 2 つの変更を同時に測ることになる）。

**捨てなかったもの**: チャンクの外にあるセルも `reveal` を 1 つ消費する（`reveal` は
「何文字目まで表示するか」なので、列を 1 つも書かないセルも数えなければならない）。
だからセルを飛ばす最適化はしていない —— 列だけを 0 回にする。

### 4. 検証（すべてホスト）

**算術の総当たり**（`/tmp/spancheck/harness.c`、リポジトリ外）: 変更前の `span` を
逐語コピーして同じ 2 カウンタを持たせ、フォント 3 種 × テキスト 11 種（空・ASCII・
全角・4 バイト・DEL・空白のみ・顔あり/なし）× 原点 3 種（−3 を含む）× 行 18 種
（負・範囲内・範囲外）× チャンク開始 169 種 × `count` 10 種（1..64）× `reveal` 8 種
（0..255）を総当たりし、64 バイトのマスクを比較。**新旧の両腕 × 顔あり/なし**で:

```
span PASS: 71850240 calls identical (both arms, both face states)
```

狭い腕だけを見たときの歩く列数は 1,309,712,352 → 99,297,408（**7.6%**）。
制御腕の列数は逐語コピーと**完全に一致**する（下の表の `chunk` 列がその値）。

**アプリ自身の 4 つのテキストを 1 回再描画したときの列数**（同じハーネス、64 列チャンク。
`tools/kasane_contract/test_span_count.c` が契約として固定している値）:

| テキスト（`apps/hello/main.js`） | span 呼び出し | 制御腕（変更前） | 狭い腕（出荷） |
| --- | ---: | ---: | ---: |
| `KASANE / JAVASCRIPT`（caption） | 32 | 27,824 | **912** |
| `Hello, World!`（display） | 64 | 36,544 | **2,496** |
| `KEY PRESSES: n`（caption、毎回書き換わる） | 24 | 22,912 | **864** |
| `ENTER +1    ESC HOME`（caption） | 32 | 30,592 | **960** |

**命令数**（`objdump`、`-Os`、対象ツールチェーン。`/tmp/spancheck/loopins.py`）:

| | 変更前 | 狭い腕 |
| --- | ---: | ---: |
| インクループ 1 列 | 70 命令 | **27 命令** |
| うち除算（`quou`） | 4 | **0** |

**契約スイート**: `tools/kasane_contract/run.sh`（sanitizer の腕を外した写し。この容器では
ASan 腕がループに入る）が全通し。`test_font.c` には「チャンクの左端がセルの途中に落ちる」
「そのセルに届かない `reveal`」の 3 ケースを足した。

### 5. 測っていないこと / 主張しないこと

- **実機のミリ秒**。上の 2.18 ms はこの変更の**前**の測定で、変更後の `render_ms` は
  まだ取っていない（`/dev/ttyACM0` がこのコンテナに見えない）。取ったら
  `tools/host_kasane_opt_ab.py`（13 腕）の `span_narrow` 列を読む。
- 括りの内訳と「何列歩いたか」の対応は 1 対 1 ではない。40 span/フレームという回数は
  わかるが、そのうち何回がどのテキストかはログからは決まらない。上の表の列数は
  「1 回の再描画がそのテキストに要求する量」であって「1 フレームの合計」ではない。
- jpfont（本文・全角）の字形読みが flash cache 越しにどれだけ高いかは未計測
  （survey §10-5）。この変更はその費用を減らさない —— 減らすのはインクループの列数。
- 制御腕をバイナリに残す代償はコードサイズ（`span` が 2 本書き分けになる）。
  flash の増分は `idf.py size` の実測を見ること。

## テキストを中身の範囲だけ描く（`extent`、2026-11-14）

下の画像（Shiki）が毎フレーム変わるゲームでは、HUD のテキストは帯ごと描き直される。そのたびに描画は**宣言した箱の全面**を 64 px ずつ `span` に通し、被覆バッファを 1 画素ずつ読み戻していた。空文字の箱も止まらなかった（`ksn_render.c` の判定は `visible` と `opacity` だけ）。上の段階 1〜3 は「損傷を変わった字まで絞る」側で、帯ごと描き直す場合の描く側の走査は残っていた。Fable のレビュー（描画経路の過剰の棚卸し）の最上位の項目。

### 置いたもの

- `ksn_text_port` に任意の `extent(ctx, draw, reveal, &w, &h)`（`ksn_ports.h`）。`span` がインクを置きうる範囲 = `bounds.x0` から reveal が通す字の送り幅の和、`bounds.y0` から 1 行の高さ（本文 12、caption 8、display 16）。字のインクは送り幅を越えず（`width <= advance`、豆腐も）、行の外は `span` が 0 を返すので、範囲の外は必ず 0。
- 描画はテキストの走査をこの範囲に切り、空の範囲は飛ばす（`g_ksn_text_extent`、既定 1）。同じバイナリの A/B の行（`KASANE_AB` の `text_extent`）。

### 検証

- 画素: STRESS の host の panel digest `a93618d1`、DERBY（MID）`ecf35bcd` が、スイッチの 0/1 と 8 行・4 行の帯で一致。DERBY の host 検査一式と `tools/kasane_contract/run.sh` も通過。
- `test_span_count.c`: 実フォントの `span` を 5 種の文字列（ASCII、豆腐の全角、空）× 3 フォント × reveal 0〜20 で、範囲の外にインクが無いことを 3,402,000 画素で確認。
- host（callgrind、STRESS、命令数の比だけ意味がある）: `span` 180.7 → 151.9 M（−16%）、`render_rects` の本体 119.0 → 87.3 M（−27%）、`extent` 自体 2.0 M。

### 実機（同じバイナリ、`sdkconfig.variant.kasane_ab`、d5e194c、実測）

30 フレームの窓ごとの `KASANE_PAINT`。`text_extent=0` の窓と、隣の全部 on の窓の比較。`render_ms` は場面が窓ごとに動くので、同じ窓の括りの計時（`span_cy`・`blend_cy`、30 フレームの合計、240 MHz）も並べる。

| 場面 | 窓 | span_n off / on | span_cy の差 | blend_cy の差 | 1 フレームあたり（括りの差から） |
| --- | --- | ---: | ---: | ---: | ---: |
| DERBY レース | 3 | 約 4,300〜5,600 / 450〜600 | +0.4〜1.4 M | +2.6〜3.6 M | **約 −0.45 ms** |
| DERBY パドック | 1 | 7,396 / 4,969 | +0.76 M | +1.97 M | **約 −0.38 ms** |
| STRESS | 2 | 3,882〜3,894 / 2,430〜2,486 | +3.4〜3.5 M | +1.4〜1.5 M | **約 −0.65〜0.7 ms** |

`render_ms` の直接の差（隣の on の窓と）は STRESS で +0.65〜0.85 ms、レースで +1.1 ms（場面が近い窓）。Fable の推定（レース −0.4〜0.6、STRESS −0.5〜0.8 ms）の範囲に入った。

**フレーム時間に届く分は別:** インラインの present は半帯ごとに `max(描画, 前の半帯の送信 192 µs)` を払うので、描画の節約は送信より重い半帯の分しかフレームを縮めない。半帯ごとの分布は測っていない。

### A/B の道具の修正

`KASANE_AB` の回転は、各窓で「その行だけ 0、他の行は 1」にする。`scale256` の行があったので、**すべての窓が出荷では取らない近似の色の腕（`scale256=1`）で走っていた**。行から外した（`app_session.c`）。それ以前の `KASANE_AB` の記録の窓は、出荷の設定ではない。

### 残り（Fable のレビューの A2・A3）

1 コマンド × 1 半帯の復号を 1 回にして行とチャンクで使い回す（A2）、2 値・不透明の字を帯へ直接書く口（A3）。パドックと STRESS はテキストの面積が大きく、ここが残りの大半（推定）。
