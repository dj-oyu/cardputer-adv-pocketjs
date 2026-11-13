# アプリと機能の文書

[文書トップ](../README.md) / [過去の調査・実験](../archive/README.md)

仕様・設計にも将来案や当時の値が含まれる。採用範囲・既定値・未確認条件は本文の状態表示を優先する。

操作や設定はアプリの隣の README も参照: [DERBY WATCH](../../apps/derby/README.md)、[BIG WAVE](../../apps/bigwave/README.md)、[LCD CATCH](../../apps/lcdcatch/README.md)、[POCKET PET](../../apps/pet/README.md)。

## 仕様・設計・作業の入口

| 文書 | 種別 | 内容 |
| --- | --- | --- |
| [japanese-input.md](japanese-input.md) | 仕様 | SKK 日本語入力と `pocket.input.text` |
| [player-overlay.md](player-overlay.md) | 設計 | ホーム画面プレイヤー: オーバーレイの仕組みと費用の管理 |
| [pet-companion.md](pet-companion.md) | 仕様 | Pet Companion: PC 連携、API、実装メモ |
| [pet-asset-design.md](pet-asset-design.md) | 設計 | ペット画像の省容量化（PPT2 形式） |
| [lcd-catch.md](lcd-catch.md) | 設計・記録 | LCD CATCH: 固定セグメントの液晶ゲーム。Kasane の ref 32・コマンド 80 に収める設計、host の台本再生、実機で調整する項目 |
| [derby-watch.md](derby-watch.md) | 設計・記録 | DERBY WATCH: 線画の疑似 3D で観る競馬、再現できるレースのモデル、起動ごとの種、較正したオッズ、場面ごとの plan、host の全画素検証、`pocket.app.load` のチャンクへの分割、楕円コースのカーブ補正とオッズの較正（host の Monte Carlo、中・(c)）、馬ごとの横位置 `lz`（q26）、首振りの描画順の受入条件、場内マップの大型画面の見え方 |
| [derby-integer.md](derby-integer.md) | 設計 | DERBY WATCH の JS から double を無くす: 受け入れ条件（opprof の float の組は列挙した境目だけ、静的検査 0、境目は int だけ）、前の計測（float の組 776〜1,250/ターン）、double の発生源、QuickJS で int のままでいる規則、2 進の固定小数の m、割り算の行き先（定数の逆数・`pocket.derby` の C・整数の長除法。Shiki に除算は足さない）、段階（表示が先、モデルが最後）、SLOW のレビューと対応（2026-11-01、実装前） |
| [derby-ser-native.md](derby-ser-native.md) | 設計・記録 | DERBY WATCH の首振りの列 `ser()` を倍精度の C（`pocket.derby`、DERBY のセッションだけの面、capability `derby.series`）に写し `draw` を数値引数にした（S1b）: 境目の API、host の oracle（JS と C の draw の入力をビット単位で照合）、楕円コーナーの WIDE 2 が実機 15.9 → 24.6 fps、ゲスト −5.2 KB・評価の余裕 30.6 KB、画素・結果は全行一致（2026-10-02） |
| [derby-runners-native.md](derby-runners-native.md) | 設計・記録 | DERBY WATCH の走者（`legs`・`run`）と HEAD ON の馬の 1 フレームの仕事（投影・間引き・並べ替え・`drawMany`）を `pocket.derby.runners()` 1 回に下ろした（api-reorg P4b）: r5sim の内訳（走者の JS 約 18k カウント/ターン）、Shiki の状態の領域と `drawList` を採らなかった理由、r5sim のレースの場面 −8.8〜−18.3%・確保 78 → 66 回/ターン、host の全フレームの画素一致、同じバイナリの A/B（up で切り替え）（2026-11-13） |
| [derby-ser-cost.md](derby-ser-cost.md) | 記録 | P4b の後の重い CAM WIDE 2（draws 約 50・`pat=` 約 40、実機 11.5 ms）の内訳: 増分の 89% は `pocket.derby.ser()` の倍精度の計算（観客の `pat=` の draw は JS ではなく `ser()` から出る）、r5sim の `pat=`・WIDE の単位別の行・`R5SIM_DERBY_PROF` の関数別・区間別の計装、ビット一致のままの小さい案（`ou` の `sqrt` −2.8%）と float32/整数化の見込み（約 3 ms、推定、仕様の変更）（2026-11-13、実装なし） |
| [derby-ser-division.md](derby-ser-division.md) | 設計 | `pocket.derby.ser()` の除算と倍精度をやめる設計（実装前）: 演算ごとの値段と除算の割合（約 4 割、推定）、案の比較（ビット一致のままの弦の表、float32、整数、構造の変更）、整数（Q16・Q30・64 ビットの積）の手順と host の試作の実測（倍精度と 0.1 px 以内）、oracle の契約の改定（1 px の許容差・不変条件・整数の C と PIE のビット一致）、PIE の扱い（32 ビットの乗算が無いので弦の除外の核だけ、実機の同一バイナリ A/B）、段階 S0〜S4、利用者の判断事項 |
| [megademo-story.md](megademo-story.md) | 設計・記録 | MEGADEMO — ASTRAL FIGHTER（2026-10-28 からの物語: 目覚め WAKE・出撃番号・記憶の濁り・猫のタマ）と、その前の NIGHT SIGNAL: 部屋と猫とモニターの物語として 5 章に組み直した台本、場面のつなぎ、Enter の小窓を物語の装置に使う設計、IV CORE（正二十面体の殻が開く 3D、グレーのフラッシュ・電波障害・めまいズーム）への差し替え、`pocket.app.load` の 4 チャンクへの分割（評価のピーク −39.5 KB）、メモリ（host m32）と実機の 90 秒の記録 |
| [megademo-shapes.md](megademo-shapes.md) | 設計・記録 | MEGADEMO の WAKE（3D の点＋透視のタマ）が実機 7 fps（JS のターン 130 ms）だったので、走査変換・投影・輪郭を C（`pocket.shapes`、MEGADEMO のセッションだけの面、capability `kasane.shapes`）へ移した: 倍精度ビルドで JS と draw がビット単位で一致する oracle、単精度の差、メモリ |
| [big-wave.md](big-wave.md) | 設計・記録 | BIG WAVE: 線分の疑似3Dで大波に乗るゲーム。世界固定の断面と 1/d の級数、host の台本とボット、全画素検証、負荷と上限の使用率、実機で詰める項目 |
| [backlog.md](backlog.md) | backlog | チュートリアルの見直し、オーバーレイの残り、日本語入力の残り |

## 計測・調査・実験の記録

| 文書 | 種別 | 内容 |
| --- | --- | --- |
| [mp3-implementation.md](mp3-implementation.md) | 記録 | MP3 実装と実測 |
| [opus-feasibility.md](opus-feasibility.md) | 記録 | Opus 復号の実現性調査と、実装後の答え合わせ |
| [derby-camera-history.md](derby-camera-history.md) | 記録 | DERBY WATCH の首振り・投影カメラの経緯（Shiki 以前、2026-09-30〜10-19）: 計算コストの見積もり（JS 41.8 µs/点、VM の Newton 逆数、観客の単価）、段階 3 のゲストヒープの内訳と受け入れ条件、楕円（段階 4）の実装・曲線の fps を上げた手順・評価の余裕とゲスト最大の経過（CLAUDE.md が引く値）、WIDE 2 の 1 フレームの命令プロファイルと較正式（実機 = 1.253 × 推定 + 4.22 ms）、trig-cull と u=14 の実機結果、ネイティブ化の候補調査（S1b の根拠）、既定 OFF の `POCKET_DERBY_COURSE_CACHE`・`POCKET_DERBY_PROJECT_BATCH` の実験 |
| [derby-pan-camera-cost.md](derby-pan-camera-cost.md) | 移動 | スタブ。→ derby-camera-history.md §1（JS のコメントが引くため残す） |
| [derby-finish-fx.md](derby-finish-fx.md) | 記録 | DERBY WATCH の FINISH のスローと HEAD ON のダスト（奥・中間・手前のボケ、馬群の範囲だけ）と手描き風の帯、スロー中の脚の位相の補間: plan 3 本（枠 31/32）、スクロールしても粒が保たれるスロットの列、既定 FINISH D・HEAD ON ①、調整前後と脚の補間の画と GIF、ser-native 統合後の実機の費用（FINISH JS +1.78 ms・render +0.48 ms、HEAD ON +0.71 ms、脚 +0.14 ms）、ゲストと評価の余裕（2026-10-02） |
| [derby-trig-cull.md](derby-trig-cull.md) | 移動 | スタブ。→ derby-camera-history.md §7（JS のコメントが引くため残す） |
