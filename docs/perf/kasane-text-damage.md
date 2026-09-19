# テキストの過剰再描画: 帯を名指しする invalidate と、帯の列範囲

2026-09-19。文字の再描画が広すぎる件を実機の計器で切り分け、3つの原因を順に直した記録。
§1〜4 が段階1（帯を名指しする invalidate）、§5 が段階2（damage が帯ごとの列範囲を持つ）、
§6 が段階3（TEXT の damage が変わった字だけになる）。
**段階2 だけではアプリのテキストは速くならず、3つ揃って hello の1桁更新が
11,520 B → 768 B、3.22 ms → 1.92 ms になる。** 残る場合は §7。

## 1. 何を測ったか

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

## 2. なぜ全画面だったか

編集欄は Kasane のコマンドではない。`pocket_text_overlay()` が
`app_session.c` の present 経路から、Kasane が描いた各帯の上へ直接合成している
（`pocket_text.h` の冒頭が書いているとおり、欄はホストのもので、ゲストは確定文字しか見ない）。
したがって欄の変化は **Kasane が自分のコマンド差分からは導けない damage** で、外から教える必要がある。

教える口は `app_force_redraw()` しかなく、その先は
`pocket_kasane_invalidate()` → `ksn_runtime_invalidate()` → `ksn_core_invalidate()` で、
`ksn_core_damage()` は `invalidated` を見て 17 帯を返していた。
`pocket_text_key()` は先頭で無条件に `dirty=true` を立てるので、
**受理されなかったキーでも全画面再描画**になる。

## 3. 置いたもの

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

## 4. 結果

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

## 5. 段階2: damage が帯の列範囲を持つ（2026-09-19）

計測Aが示していたのは、この invalidate では消えない構造のほうだった。
`command_bands()` は y しか見ず、`ksn_render.c` の帯ループは毎帯 240 px 全幅を塗り、
`transferred_bytes` も全幅で数えていた。

### 置いたもの

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

### 実機で分かった2つの落とし穴（どちらも実測）

1. **端数の窓は転送経路を落とす。** 最初の実装（窓 184 px）は
   `bytes 11,520 → 8,832` と減ったのに **render 1.55 → 1.65 ms、send 1.67 → 1.88 ms と悪化**した。
   詰め直した行が 32 B 境界にも 32 B 倍数にもならず、board のバイトスワップが PIE から
   スカラーへ落ち、背景塗りも `fill_blocks` から外れていた。
   そこで**窓を 16 画素へ外側丸め**する（16 画素 = 32 B = カーネル1ブロック）。
2. **ほぼ全幅の窓は割に合わない。** 全幅経路は RAMWR を開いたまま流すので再アドレス指定が
   0 コマンド（`board.c` の `next_row`）。狭い窓は帯ごとに 3 コマンド払う。
   閾値 `KSN_NARROW_MAX = 192`（80%）を超える帯は全幅のまま送る。

### 結果

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

## 6. 段階3: TEXT の damage が変わった字だけになる（2026-09-19）

`ksn_core_damage()` は `memcmp` で文字列の違いの**有無**しか見ず、違えば bounds 全体を
damage にしていた。`KEY PRESSES: 5` → `6` は caption 1 文字 6 px である。

### 置いたもの

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

### 結果

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

## 7. 追補: 長さが変わる書き換えも「箱」ではなく「中身」まで（2026-09-19）

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

## 8. まだ箱全体になる場合

- **ネイティブ画面**（home / overlay / picker / editor）は Kasane が所有していないので
  この経路に乗らない。`docs/kasane/kasane-astra-plan.md` の CP27–30。
- **`advance` を持たない text port**（ホスト試験の広いアーム）。実機は `ksn_font_port` が
  持っているので該当しない。
