# Kasane — 読む順番と判定の前提

Kasane は Cardputer ADV（240×135、PSRAM なし）の固定容量 UI 基盤。ここには**現行の判断に必要な設計契約、コードから復元しにくい選択理由、検証の到達範囲**だけを置く。過去の試行を時系列で追う必要はない。保存資料は [archive の索引](../archive/README.md)、関連文書は末尾の一覧から探す。

1. [architecture.md](architecture.md) — owner、表示、source、System、失敗時の契約。新しい利用側を作るときに読む。
2. [decisions.md](decisions.md) — 採用・不採用の理由と、再検討時に満たすべき条件。最適化案を出す前に読む。
3. [verification.md](verification.md) — 実機の固定ゲート、確認済み範囲、測定上の注意。
4. [roadmap.md](roadmap.md) — 動的描画（旧 D0〜D6）の行方と、現在の実装・実測・未達を分離した再評価可能な作業一覧。

`design-schema.json` と `design-example.json` は制作時の**旧構想の機械可読例**として残す。現在の `mount({version:1,...})` の完全なランタイムスキーマではない。ランタイムの型・容量・APIの厳密な真実は `main/ui/kasane/ksn_schema*`、`main/pocket/pocket_kasane.c` と契約試験にある。文書の数値は対象 image と workload を伴う場合だけ測定値として扱い、設計予算・推定と混同しない。

## 一目で分かる現在地

- `mount` の汎用 descriptor、型付き slot、native source、dirty-node、APP/SYSTEM の lease と repair は実装されている。メニュー登録済みの通常アプリ4件（hello・imucal・pet・companion）とdeskclock/music overlayは native mount 経路。bridgeもソースは移行済みだが現行imageには埋め込まれず、出荷アプリには数えない。Kasane core はアプリ名や音楽の意味を知らない。
- music はアプリ側に音声・入力・文言判断を残し、表示を native presenter へ移した。music 固有の light 3矩形だけを PATCH する高速経路は採用済み。全計画 PATCH は利益を確認できず不採用。
- 「producer が公開した text → 各 core destination が受理するまで」は該当実測経路で1 copy。producer 原データから LCD 描画完了までの**無条件の全経路1 copy以下は必須目標にしない**。同一payloadの不要な複製を減らす努力目標とし、2 bank・返却済み borrowed pointer の寿命・実測利益を優先して判断する。
- dirty-node の CPU 利益と部分転送・画素一致は確認したが、音楽 overlay は native 背景のため多くの frame が全17帯・64,800 Bを転送する。CPU改善とLCD改善は別物。
- hostの全画素一致、注入 `KSN_IO` 後の全帯修復、音声共存の複合試験はある。ただし実LCD GRAM読戻し・実SPI故障・全条件同時・全アプリ/全曲網羅は未証明。旧music repair試験の1回はsend最大が固定上限を6 µs超え、不合格のまま保存。位相分類をOFFにした後続の独立boot×2は同じ固定線を全run通過したが、故障注入付き診断imageの結果を全probe OFF製品imageの実機性能とは呼ばない。

`roadmap.md` は「製品として使える」と「選択的な最適化」を分ける。限定経路の1-copyを全経路の達成と呼ばず、copy回数のみを理由に安全な所有権契約を崩さない。

## 仕様・設計・作業の入口

| 文書 | 内容 |
| --- | --- |
| [入口](README.md) | 読む順番、現在地、機械可読の旧スキーマ例の扱い |
| [Shiki 仕様書 v1.5（2026-11-02 凍結）](shiki-spec.md) | Shiki の規範: 固定小数の型と演算、線の式の命令、データ領域（欄ごとの面、`EACH`・`FETCH`・`PUT`・`LOOKUP`）、登録時の検査、`@shiki` の記法と Q 形式、JS API・ヒープ・capability、検証の要件、移行の順番 |
| [Shiki v2.0 の草案](shiki-v2.md) | v1.5 からの変更の草案: 旧 API（float の plan）の廃止と移行の順、データ領域を番号で読み書きする `LOAD`・`STORE`、draw の番号を読む `DRAW_INDEX` とアプリの状態の領域、描画の並びを残す手引き、PIE のベクトルの命令を仕様に、決めることの一覧、SLOW のレビューと対応（2026-11-02、凍結前） |
| [Shiki の JIT](shiki-jit.md) | ベクトルの区間を登録時に PIE の機械語にして RAM で実行する（案 A、2026-11-02 決定・実装済み）: パイプラインを空けている原因の理屈、実行できる RAM とメモリ保護、生成するコードの形、アセンブラから作るエンコーダと検査、段ごとの実機結果（§11〜§23）。§24 に旧 shiki-pie.md（PIE の事実の表、ベクトルの命令・列からの描画・SoA・端数の決定、試作の実機の照合と時間） |
| [Shiki JIT とメモリ配置（図解、HTML）](shiki-jit-overview.html) | Shiki の JIT とその機械語の置き場所を、一続きの問いとして読む図解（各章の冒頭に既知の事柄との対応）。インタプリタと JIT、実行できる RAM、Shiki の区画への相乗り、飽和チェックの除去、レジスタキャッシュ、RTC fast memory の拒否、56 KiB の実験、区画の固定長の表、作業領域の廃止、DERBY の engine_ms の推移と今の 48 KiB の区画（shiki-jit.md §11〜§23、2026-11-03） |
| [Kasane の描画パイプライン（図解、HTML）](pipeline-overview.html) | フレームバッファを持たない理由、1 ターンの流れ（`frame()` → Shiki の beginFrame/draw/commit → 17 本の帯の合成 → SPI の DMA）、作業台の帯と転送用バッファ 1 本の重ね、`KASANE_PAINT` の各欄、帯の重ね合わせの順、Shiki の線分の並び（candidate・committed、帯の hash、作業領域の廃止）、DERBY の区画の内訳と、並びを 1 本にする予定（c4d7a6c 時点、2026-11-03） |
| [Shiki の draw の束ね（作業中）](shiki-bundle.md) | draw 1 回の固定費の段ごとの実測と、複数の handle を 1 回で描く API |
| [Shiki](shiki.md) | 高速化用の命令セットと API（旧 procedural・plan・IR・VM と grid IR）の総称 Shiki の決定、旧名との対応表、API の棚卸しと問題点 |
| [境界と契約](architecture.md) | APIの考え方、owner、source、表示失敗、overlay、System |
| [判断台帳](decisions.md) | トレードオフ、不採用案と再検討条件 |
| [検証](verification.md) | 固定実機ゲート、実測の達成範囲、計測の落とし穴 |
| [ロードマップ・残タスク](roadmap.md) | 動的描画（旧 D0〜D6）の行方（出荷・Shiki へ置換・保留）と、Kasane v1 の実装・実測・未達を分けた再評価用の表 |
| [面ごとの線分上限](surface-segment-cap.md) | `createSurface({maxSegments})` で面1のフレームを n 本分に縮める API、1 面の確保量（256 本で 5.4 KB、既定 1,024 本で 21.5 KB）、DERBY WATCH の評価直後に面1が確保できる境目と plan 登録への影響（2026-09-30 実測）|
| [JS 風の言語 → 手続き型 IR](js-to-ir.md) | plan を名前と式で書くコンパイラの試作（`tools/kasane_ir/`、手書き 13 本と出力一致、命令 −3%）、plan の文字列と `prog()` のゲスト常駐（評価後 7.1 KB、段階 3 で 8.1 KB。文字列を詰めても −1.6〜2.4 KB）、native の plan が命令数に依らず 872 B である点、flash の plan を id で登録する設計と優先順位、plan を普通の JS の関数（`@plan`）で書きビルドが詰めた IR へ置き換える実装（DERBY で実装、Node 16 以上がそのときだけ要る。2026-09-30 host・実機実測、推定）|
| [組み込みの plan を flash に置き名前で登録する](flash-plan.md) | `register('derby.crowd', args)` の API と誤り、表の生成器（`emit_rom_plans.mjs`）、引数を begin で当てる設計、`ksn_proc_rom_plan`（40 + 4 × 引数 B）、DERBY で native 9.9 → 1.2 KB（推定）・ゲスト −3.1〜5.7 KB（host 実測）、配列登録との全件一致の試験。firmware への組み込みと DERBY の 14 本の移行、実機でターン内の最小 +18 KB・評価の余裕 +4.8 KB・登録 1,252 → 570 µs・描画 +9〜17%（2026-10-01）|
| [pixel-small-window.md](pixel-small-window.md) | 小窓の動的 pixel image。寸法・session・更新の制約。 |
| [file-stream.md](file-stream.md) | KSV1 file stream の試作、SD grant と動画スロットの契約。 |
| [grid-js-notation.md](grid-js-notation.md) | 登録時に JS から int16 grid IR を生成し、native で反復する記法。 |

## 計測・調査・実験の記録

| 文書 | 内容 |
| --- | --- |
| [Shiki の画像を帯へ直接書く・横線の塗り](image-rows.md) | 不透明・等倍の画像を 32 画素ずつではなく帯の行で直接書く `read_rows`（`ksn_ports.h`）、覆われた帯の背景の塗りの省略、帯のラスタで横線を 2 画素ずつ塗る `g_ksn_proc_hspan`。STRESS の実機の同じバイナリの A/B で描画 11.80 → 9.34 ms、さらに 9.58 → 7.38 ms。送出の待ちがそれぞれ +0.63・+1.47 ms 増え、1 フレームの合計は 12.7 → 10.3 ms。LCD の DMA が次の床（2026-11-04）|
| [手続き型描画の上限緩和の実機検証](procedural-limits-device.md) | plan の動的登録・解除、新上限の全画素一致、最悪時 heap、step 時間、ターン予算で組み立て中のフレームが消える不具合とその修正（実効フレーム予算 8 ms→250 ms）、仕様上限の棚卸し（2026-09-29 実測） |
| [MEGADEMO Act II の上限と実機の負荷](megademo-limits.md) | 手続き面の上限を叩く場面（ねじれ廊下・Apple II 風 ZENITH）の設計の芯、実機で初めて動かした結果（ZENITH で落ちていた）、破綻の境界（面の確保・plan の同時数・回転読み出し・登録時間・ゲストヒープ）、決めた `KN`/`LOAD` と既定段階、場面切替の最初のフレーム（2026-09-29 実測。LIMIT は 10-28 に外れ、その表は落とした） |
| [megademo-limit-scenes.md](megademo-limit-scenes.md)・[megademo-device-limits.md](megademo-device-limits.md) | 上の megademo-limits.md へ統合した旧文書の移動案内。MEGADEMO の JS のコメントが旧パスを引くため残す |
| [DERBY WATCH の背景は何が重いか](derby-background-cost.md) | 観客席・観客・柵・刈り目の plan を外した差（カメラ×段階）、帯描画の費用モデル（1 線分・1 帯・1 画素）、VM の SIN が引数約 201 超で約 7〜9 倍遅い発見、画素キャッシュと透明な面の効果の上限とメモリ（2026-09-30 実測）。追記に観客をフレームごとに半分描く試作（q27、出荷しない）の画・GIF・実機の費用 |
| [観客を「模様の線」と「タイル」で描く（試作）と、模様線 P24 の本実装](crowd-primitives-design.md) | 手続き描画の新しい命令 `LINE_PATTERN`（24 ビットの模様と 2 色目、遠近の弦）の設計・試作（タイルとノイズ線も）、実機の帯の単価（水平 0.061〜0.088 µs/画素、見積もりの 1.0〜1.4 倍）、本実装で変えたこと（パラメータのエントリの行を負に、色 A を命令に）、DERBY の観客の置き換えの費用（横見の VM draw + 帯 −0.27〜−1.18 ms）と画・GIF（`docs/apps/derby-crowd-p24-*`、2026-10-01）|
| [plan を命令数ぶんだけ確保する](plan-sized-alloc.md) | `ksn_proc_sized_plan`（40 + 12n B、以前は 872 B）、命令数ごとの heap のブロック、DERBY の 25 本 22.5 → 9.7 KB とターン内の最小 +11.4 KB、首振り段階 3 の LOADSTALL の解消、断片化（`lg`）、flash の id 登録への見通し（2026-09-30 実測）|
| [楕円・首振りの描画を速くする設計の考察](oval-pie-design.md) | 曲線 93 ms の内訳（`pose()` が 1 回 12 個の三角関数で 56%、計数と推定）、Möbius 変換と Newton の段数の条件、円弧の回転の漸化式（float32 の誤差）、有理ベジエ・チェビシェフを棄却した理由、VM の DIV の効果（+0.3 fps）、PIE が向く所と向かない所、区間の表（P0）と native のウォーカー（P5）の順序と合格基準、host の数値実験 `tools/games/ovalcost/` |
