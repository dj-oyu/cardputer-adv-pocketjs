# 旧UI（Rust core + taffy + rgb565）と Kasane の構造比較

2026-09-17、Fable によるソースコードベースの分析（読み取りのみ）。問いは「今の実装の差」ではなく、**両方を最適化しきったときに構造的な差が残るか**。

凡例: **実測(map)** は `build_before_ports`（旧UI入り）と `build_mr_ship`（Kasane のみ）のリンクマップの live セクション集計。**実測(doc)** は [kasane-guest-memory.md](kasane-guest-memory.md) と [kasane-guest-memory-reduce.md](kasane-guest-memory-reduce.md)。それ以外は算出または推定と明記する。旧UIのソースは `.cache/pocketjs/engine/`（taffy 0.11）、`components/pocketjs_ui_qjs/`、削除前の `main/pocket/pocket_ui.c` / `main/text/jsfont.c`。行番号は分析時点のもの。

## 結論

| 軸 | 最適化しきった後に残る差 | 判定 |
| --- | --- | --- |
| ゲストヒープ | 描画コマンドあたりの JS オブジェクトは両者 0 にできる。残るのは refs 保持の数百 B 程度（推定） | 実装で埋まる |
| ネイティブ常駐 | Kasane はバンク2面 ≈ 8 KiB 固定、旧UIはノード数依存（C の固定配列 flex にすれば同桁） | ほぼ同等、性質が違う |
| **最大連続確保** | **Kasane は定数 9,916 B（実測(doc)）、旧UIはデータ依存で 29,648 / 59,296 B ＋フォントアトラス ~24 KiB** | **構造的に Kasane 有利** |
| CPU / フレーム | 静止は同等。一部更新の差は旧ホストの実装不足（レンダラは領域限定描画を持っていた）。レイアウトは Kasane が JS に転嫁 | 実装で埋まる（flex を除く） |
| flash | 旧 ≈ 265 KiB、Kasane ≈ 48 KiB（実測(map)）。主因は機能幅で、Rust 固有（fmt/panicking/raw_vec）は 2,881 B のみ | 実装で埋まる |
| **失敗の仕方** | **旧は abort で、段の大きさ分の連続空きを常に空ける必要。Kasane は返り値＋先行確保で余裕 0** | **構造的に Kasane 有利** |

**taffy を消したのは正しい。** 334 KiB の DRAM でゲストが 160 KiB を持つ機体で、データ依存の 29.6 / 59.3 KiB の連続確保を、失敗時 abort で要求する部品は成立しない。失ったのはネイティブの flex レイアウトだけで、固定サイズの C で取り戻せる。

## 軸ごとの根拠

### 1. ゲストヒープ
- 旧: `ui.createNode` は int32 を返す（`ui_qjs.c:285`）ので、ノードあたりの JS オブジェクトは 0 が下限。名前空間は mount 時に 25 関数などを eager 生成（`ui_qjs.c:700-770`）。`JSFONT_WRAP` が `ui.setText` を JS 閉包で包み、毎回 `String(t)` を作る（`jsfont.c:126-128`）。
- Kasane: tx ごとに ticket・tx・modal の3オブジェクト（scene 経路は ticket 無し）、描画コマンドごとに ref ラッパ1オブジェクト、呼び出しごとに spec オブジェクトと bounds 配列の一時（個数は未計測）。
- 下限は両者ほぼ同じ。Kasane も int ハンドルとフラット引数にすれば旧 createNode と同形。移植で増えた +13.9〜15.4 KiB（実測(doc)）の主因はアトムと関数で、シーンコントローラの C 化で −6.2 KiB 済み。

### 2. ネイティブ RAM と最大連続確保
- 旧:
  - `Vec<Node>` は倍々成長（`tree.rs:43-96`）。taffy の木は構造が dirty になるたびに `clear()` してゼロから再構築（`layout.rs:4-8, 408-431`）。
  - taffy 0.11 の NodeData は Style ＋ measure cache 9本 ＋ final cache（`taffy-0.11.0/src/tree/cache.rs`）で約 1,853 B/ノード（ホスト計測、`pocket_ui.c:63-71`）。これが 29,648 / 59,296 B の単一連続確保の段になる。
  - DrawList（`Vec<u32>`）を毎フレーム作り直し、DamageTracker が前フレームを丸ごと複製（`damage.rs:290,341-344`）＝描画リスト2面。
  - フォントアトラスは約 24 KiB 常駐、再構築ピークは約3倍（`jsfont.h:19`、`jsfont.c:24-26` の自己申告からの算出で ~73 KiB）。
- Kasane:
  - ヒープは1確保 9,916 B（実測(doc)。算出内訳: コマンド帯 2×96×32 = 6,144、テキスト帯 2×1,024、制御と state ≤ 3,072）を評価前に確保し、core 自身は確保しない（`ksn_core.h:56`）。任意の cache は ≤ 4,096 B。
  - 静的 .bss は `ksn_*.c` 合計 20,288 B（実測(map)）。`decoded` 6,600、blend LUT 2×2,048、row_table 976、anchor 308、`ksn_pet.c` cache_rows 8,192。
  - フレーム中のヒープ確保はゼロ（scratch はスタック ≤ 512 B）。
- 構造差: 固定容量は起動前に予約できるが、倍々に伸びる Vec は予約できない。

### 3. CPU / フレーム
- 旧: 毎フレーム木を歩いて DrawList を作り直す（子 Vec を階層ごとに clone、ヒープ churn）。relayout は dirty のときだけ。ただしホストは region_count>0 なら17帯すべてを描いて送っていた（`app_session.c:1367-1396`）ので、一部更新＝全面更新（64,800 B）。Rust 側の領域限定 `render_damage` は未使用だった。
- Kasane: 静止はコマンド 32 B × n の memcmp で mask 0 なら即 return。一部更新は変化したコマンドの8行帯だけを再描画・転送。レイアウトパスは無い（座標は絶対）。
- 構造差: 旧はレイアウトをネイティブで払い、Kasane は JS で払う。フォントは旧が RAM アトラスの blit、Kasane が flash から帯ごとの span（RAM を CPU に振った）。

### 4. flash（実測(map)、live .text）
- 旧: ui_core.a 204,302（うち taffy 50,243、pocketjs_core 133,229）＋ rgb565.a 45,326 ＋ pocket_ui.c 9,110 ＋ ui_qjs.c 3,970 ＋ jsfont 811 ＋ render_accel 1,210 ≈ 265 KiB。
- Kasane: ksn_*.c 33,629 ＋ pocket_kasane.c 14,264 ＋ ksn_font.c 1,241 ≈ 48 KiB。
- 差の主因は機能幅（3D・スプライト・テクスチャ・コンポジタ・480×272 汎用）。

### 5. 失敗の仕方
- 旧: 確保失敗・panic は `abort()`（`ui-cabi/src/lib.rs:137-146`）。ホストは事前検査で防ぐしかなく、`layout_block() + LAYOUT_MARGIN 8,192` を常に空けておく必要があった。実際の余裕は 23.5 KiB しかなく、`UI_SAFE_NODES 15` でノード数を抑えて運用していた（`pocket_ui.c:27`）。
- Kasane: 全操作が `ksn_result` を返し、JS には BUSY / LIMIT / OOM の PocketError。builder の失敗は原子的に取り消され、表示中のバンクは保たれる。必要な余裕は 0 だが、未使用スロット分を常に払う。

## Kasane が構造的に不利な点
1. **バンク2面の原子性 ≈ 8 KiB 固定。** 前バンクをハッシュにしても、差分描画が旧コマンドの帯を汚す（`ksn_core.c:817`）ので旧 bounds は残す必要があり、削れるのは約 −2.5 KiB（推定）。
2. **レイアウトが JS 側。** flex が要るアプリはインタプリタで座標を計算する。
3. **`decoded` / LUT / row_table の約 11.7 KiB は CPU との交換。** 外せば −11.7 KiB、代償は帯ごとの再デコード（139 命令 ＋ memset、`ksn_render.c:87-99`）。旧UIの描画リスト2面と同じ種類の代価。

## 旧UIから取り込める要素（未着手）
- **ネイティブ flex レイアウト。** 木とキャッシュを持たず、submit 時に固定配列を1回走査してコマンドの座標へ解決する形にすれば、ヒープは 0 で済む。
- **テキストの幅測定と折り返しのネイティブ実装**（旧 `ui_qjs.c:611-643`）。Kasane 側にあるかは未確認。
