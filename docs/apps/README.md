# アプリと機能の文書

[文書トップ](../README.md) / [過去の調査・実験](../archive/README.md)

仕様・設計にも将来案や当時の値が含まれる。採用範囲・既定値・未確認条件は本文の状態表示を優先する。

操作や設定はアプリの隣の README も参照: [DERBY WATCH](../../apps/derby/README.md)、[BIG WAVE](../../apps/bigwave/README.md)、[LCD CATCH](../../apps/lcdcatch/README.md)、[POCKET PET](../../apps/pet/README.md)。

## 仕様・設計・作業の入口

| 文書 | 種別 | 内容 |
| --- | --- | --- |
| [CHARACTER LAB](../../apps/character/README.md) | 試作・検証 | 人・キツネ・雲を同じ世界光/視点で比較する材質APIの縦切り。BODY/LIGHT/CAMERA/TEXTURE/EXPRESSIONの原因を分離し、hostの描画予算と静止更新を検査する。柔らかい材質表現と実機性能は未検証 |
| [CLOUD PROBE](../../apps/cloudprobe/README.md) | 有限診断 | 同じ完全な雲モデルでretarget・外乱・pause/replayを既存typed tapから実行 |
| [CLOUD ACT](../../apps/cloud/README.md) | 造形・演技の縦切り | 雲1体の主volume・顔・青い陰影と、気づき/喜び/減衰停止を共通APIで磨く専用app |
| [japanese-input.md](japanese-input.md) | 仕様 | SKK 日本語入力と `pocket.input.text` |
| [player-overlay.md](player-overlay.md) | 設計 | ホーム画面プレイヤー: オーバーレイの仕組みと費用の管理 |
| [pet-companion.md](pet-companion.md) | 仕様 | Pet Companion: PC 連携、API、実装メモ |
| [pet-asset-design.md](pet-asset-design.md) | 設計 | ペット画像の省容量化（PPT2 形式） |
| [lcd-catch.md](lcd-catch.md) | 設計・記録 | LCD CATCH: 固定セグメントの液晶ゲーム。Kasane の ref 32・コマンド 80 に収める設計、host の台本再生、実機で調整する項目 |
| [derby-watch.md](derby-watch.md) | 設計・記録 | DERBY WATCH: 線画の疑似 3D で観る競馬、再現できるレースのモデル、起動ごとの種、較正したオッズ、場面ごとの plan、host の全画素検証、`pocket.app.load` のチャンクへの分割 |
| [derby-ser-native.md](derby-ser-native.md) | 設計・記録 | DERBY WATCH の首振りの列 `ser()` を倍精度の C（`pocket.derby`、DERBY のセッションだけの面、capability `derby.series`）に写し `draw` を数値引数にした（S1b）: 境目の API、host の oracle（JS と C の draw の入力をビット単位で照合）、楕円コーナーの WIDE 2 が実機 15.9 → 24.6 fps、ゲスト −5.2 KB・評価の余裕 30.6 KB、画素・結果は全行一致（2026-10-02） |
| [megademo-story.md](megademo-story.md) | 設計・記録 | MEGADEMO — ASTRAL FIGHTER（2026-10-28 からの物語: 目覚め WAKE・出撃番号・記憶の濁り・猫のタマ）と、その前の NIGHT SIGNAL: 部屋と猫とモニターの物語として 5 章に組み直した台本、場面のつなぎ、Enter の小窓を物語の装置に使う設計、IV CORE（正二十面体の殻が開く 3D、グレーのフラッシュ・電波障害・めまいズーム）への差し替え、`pocket.app.load` の 4 チャンクへの分割（評価のピーク −39.5 KB）、メモリ（host m32）と実機の 90 秒の記録 |
| [megademo-shapes.md](megademo-shapes.md) | 設計・記録 | MEGADEMO の WAKE（3D の点＋透視のタマ）が実機 7 fps（JS のターン 130 ms）だったので、走査変換・投影・輪郭を C（`pocket.shapes`、MEGADEMO のセッションだけの面、capability `kasane.shapes`）へ移した: 倍精度ビルドで JS と draw がビット単位で一致する oracle、単精度の差、メモリ |
| [big-wave.md](big-wave.md) | 設計・記録 | BIG WAVE: 線分の疑似3Dで大波に乗るゲーム。世界固定の断面と 1/d の級数、host の台本とボット、全画素検証、負荷と上限の使用率、実機で詰める項目 |
| [backlog.md](backlog.md) | backlog | チュートリアルの見直し、オーバーレイの残り、日本語入力の残り |

## 計測・調査・実験の記録

| 文書 | 種別 | 内容 |
| --- | --- | --- |
| [derby-native-survey.md](derby-native-survey.md) | 設計・調査 | DERBY WATCH の首振り（楕円コーナーの WIDE 2）で native に下ろす候補 30 個の利得・費用・メモリ・工数・画素への影響、シナリオ別の fps（`ser()` に触らないと 20 fps に届かない、ウォーカーだけでは 27 fps の境目）、API の 3 案の比較と推奨（道の登録を共有する `path`／`pose`／`project`／`walk`）、実機で確かめる項目（2026-10-01、推定のみ・測定なし） |
| [derby-pan-depth-order.md](derby-pan-depth-order.md) | 修正・検証記録 | PAN 投影深度による描画順の再現条件と受入条件（2026-10-17）。 |
| [derby-venue-map.md](derby-venue-map.md) | 調査・計算 | 場内の大型画面が各カメラに映る割合。実機の測定ではない。 |
| [sd-async-design.md](sd-async-design.md) | 実験設計 | SD worker と再生 session の lease・停止・抜去時の共有安全契約。採用済み API と混同しない。 |
| [mp3-implementation.md](mp3-implementation.md) | 記録 | MP3 実装と実測 |
| [opus-feasibility.md](opus-feasibility.md) | 記録 | Opus 復号の実現性調査と、実装後の答え合わせ |
| [derby-corner-model.md](derby-corner-model.md) | 試算 | DERBY WATCH の楕円コース案: カーブだけの内外差とばらつきの乗数、枠・本命・入れ替わりへの効き、楕円用のオッズの再推定（host の Monte Carlo）、実装の結果（中・(c)、直線は全桁一致） |
| [derby-pan-camera-cost.md](derby-pan-camera-cost.md) | 記録 | DERBY WATCH の首振りカメラ・楕円コースの計算コスト: JS 演算と投影の単価（実機）、VM の Newton 逆数による投影、台数・自動ズーム・LOD 別のフレーム費用の見積もり（2026-09-30） |
| [derby-pan-memory.md](derby-pan-memory.md) | 記録 | DERBY WATCH の首振りカメラ（段階 3）が常駐させるゲストのヒープの内訳（関数・atom・plan の文字列）、事前コンパイルと行番号表の削除の効果の上限、規模を削る案と受け入れ条件との差（2026-09-30、host 実測）、flash の plan を載せた再挑戦で (e) の fps だけが不足（§10、2026-10-01 実機）、`vm/main` b184202 への追従とマージ前の再測定・楕円（段階 4）への引き継ぎ（§11）、楕円（段階 4）の実装・増分・曲線の首振りの fps を上げた手順と実機の表（§12、2026-10-01）、列を C にした S1b の増分（§14、2026-10-02） |
| [derby-wide2-profile.md](derby-wide2-profile.md) | 記録 | DERBY WATCH の楕円コーナーの首振り WIDE 2 の 1 フレームを、host の QuickJS の計装（`tools/vmtest/opprof/`）で命令・関数・配列・`Math` 単位に数え、実機の単価で µs に換算。実機の JS ターンとの一次式（4 構成で ±0.06 ms）、P5 の見積もりの検算（2026-10-01、host のみ） |
| [derby-finish-fx.md](derby-finish-fx.md) | 記録 | DERBY WATCH の FINISH のスローと HEAD ON のダスト（奥・中間・手前のボケ、馬群の範囲だけ）と手描き風の帯、スロー中の脚の位相の補間: plan 3 本（枠 31/32）、スクロールしても粒が保たれるスロットの列、既定 FINISH D・HEAD ON ①、調整前後と脚の補間の画と GIF、ser-native 統合後の実機の費用（FINISH JS +1.78 ms・render +0.48 ms、HEAD ON +0.71 ms、脚 +0.14 ms）、ゲストと評価の余裕（2026-10-02） |
| [derby-trig-cull.md](derby-trig-cull.md) | 記録 | DERBY WATCH の楕円の首振り: pose・三角関数の呼び出し元別の回数、キャッシュ・LUT・漸化式・固定点の表の比較、弦の culling と結合の本体数・画素差（画像・GIF）、弦の端を sin 9 個から対称性で作る変更（コードは tag `archive/trig-cull-B`。ゲスト +820 B で余裕を食うので vm/main には入れていない）。WIDE 2 の台は w −100・−14・−3（u=14）（2026-10-01、host のみ） |
| [derby-trig-cull-device.md](derby-trig-cull-device.md) | 記録 | trig-cull と WIDE 2 の u=14（w=−3）を実機で: 楕円コーナーの首振りの fps・JS・VM draw（4 構成、固定種）、ゲスト最大・評価余裕、u=14 の台の画像（2026-10-01） |
| [derby-lane-offset.md](derby-lane-offset.md) | 試作・記録 | DERBY WATCH の馬ごとの横位置（q26）: 楕円は脚質・区間・カーブの乗数で舵、後ろの馬はゆっくり揺れる、毎フレームの奥行き順。結果は main と全行一致、host m32 で評価のピーク +1.5 KB・生存ピーク +1.7〜2.8 KB（実機は推定のみ、2026-10-02） |
| [derby-projection-experiment.md](derby-projection-experiment.md) | 実験・既定 OFF | 実機受入は本文の未確認条件を参照。 |
| [derby-runner-affine-experiment.md](derby-runner-affine-experiment.md) | 実験・既定 OFF | 実機受入は本文の未確認条件を参照。 |
