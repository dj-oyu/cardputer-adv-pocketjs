# テキストの過剰再描画（1）: 帯を名指しする invalidate

2026-09-19。文字の再描画が広すぎる件を実機の計器で切り分け、3つの原因のうち
**1つ目だけ**を直した記録。残り2つは未着手で、§5に条件と見積もりを書く。

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

## 5. 残っている2つ

計測Aが示すのはこの修正では消えない構造で、こちらのほうが**全アプリの全テキスト更新に効く**。

- **damage に X が無い。** `command_bands()` は y しか見ず、`ksn_render.c` の帯ループは
  毎帯 `fill565(pixels, 240*rows, …)` で全幅を塗り、`transferred_bytes` も全幅で数える。
  直すには帯ごとの `x0/x1`（17×2×int16 = 68 B）を damage に添え、帯ループの塗り・命令の
  x クランプ・転送をその範囲へ閉じる。`board_present()` は現在 CASET 固定・全幅
  ストリームで再ウィンドウを避けているので（`board.c` の `next_row` の節）、
  狭い窓では帯ごとに CASET/RASET/RAMWR の 3 コマンドを払うことになる。非同期経路の
  byte swap が既に `tx_buf` へ写しているので、**その 1 パスで狭い行を詰められる**。
  display port は `present` を壊さず `present_rect` を**追加**する形にすれば、
  `tools/kasane_contract/` の約 20 本の `present` 実装を触らずに段階導入できる。
- **TEXT の damage が文字列全体。** `ksn_core_damage()` は
  `memcmp(old->text+offset, next->text+offset, length)` で違いの有無しか見ず、
  damage は bounds 全体になる。新旧を scalar 単位で前から比べ、最初に違った位置までの
  advance 合計を x0 にすれば、カウンタは末尾数桁だけになる。長さが変わる場合は
  「最初の差異〜bounds 右端」で打ち切るのが安全側。`reveal` は旧値〜新値の列だけ。

この2つが揃うと計測Aの 1 桁更新は 3帯×240px=11,520 B から 3帯×8px=384 B になる**見込み**で、
これは面積からの算術であって実測ではない。順序は X を先にすること —— X が無いと
TEXT 側を narrow しても damage は 1 画素も減らない。
