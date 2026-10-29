# JS APIの文書

[文書トップ](../README.md) / [過去の調査・実験](../archive/README.md)

仕様・設計にも将来案や当時の値が含まれる。採用範囲・既定値・未確認条件は本文の状態表示を優先する。

## 仕様・設計・作業の入口

| 文書 | 種別 | 内容 |
| --- | --- | --- |
| [common-api.md](common-api.md) | 仕様 | 共通 JS API `pocket.*`。冒頭に節ごとの実装状況（BLE 以外は実装済み）。`main/pocket/` はこれの実装 |
| [filesystem-api.md](filesystem-api.md) | 仕様 | `pocket.fs`。`app:` / `assets:` / `sd:` の3ボリュームとも実装済み |
| [hone-ayatsuri.md](hone-ayatsuri.md) | 正式方針・移行 | honeの汎用低層と、JS/Cを持つayatsuriの責務・命名・現状対応・共通評価核への工程 |
| [shapes.md](shapes.md) | 仕様 | `pocket.shapes`（capability `kasane.shapes`、名乗ったアプリだけ）: 射影行列で 3D の点を写す `project`、多角形・楕円から穴を抜いて塗る `fill`、隠す形つきの `outline`。手続き面のフレームへ直接描く |
| [character-utility.md](character-utility.md) | 実験版 | キャラクターの配置・材質・身体座標の模様・世界光・視点を分離するJS utility。縦切りの実装範囲・依存更新・資源と未達品質を明示 |
| [backlog.md](backlog.md) | backlog | BLE、Opus の採否、SD の帯域、PC bridge の wire 形式など |
| [app-control-v1.md](app-control-v1.md) | 仕様 | HOME の native app-control と broker の共通契約。 |
