# Kasane — 読む順番と判定の前提

Kasane は Cardputer ADV（240×135、PSRAM なし）の固定容量 UI 基盤。ここには**現行の判断に必要な設計契約、コードから復元しにくい選択理由、検証の到達範囲**だけを置く。過去の試行を時系列で追う必要はない。

1. [architecture.md](architecture.md) — owner、表示、source、System、失敗時の契約。新しい利用側を作るときに読む。
2. [decisions.md](decisions.md) — 採用・不採用の理由と、再検討時に満たすべき条件。最適化案を出す前に読む。
3. [verification.md](verification.md) — 実機の固定ゲート、確認済み範囲、測定上の注意。
4. [roadmap.md](roadmap.md) — 現在の実装・実測・未達を分離した、再評価可能な作業一覧。

`design-schema.json` と `design-example.json` は制作時の**旧構想の機械可読例**として残す。現在の `mount({version:1,...})` の完全なランタイムスキーマではない。ランタイムの型・容量・APIの厳密な真実は `main/ui/kasane/ksn_schema*`、`main/pocket/pocket_kasane.c` と契約試験にある。文書の数値は対象 image と workload を伴う場合だけ測定値として扱い、設計予算・推定と混同しない。

## 一目で分かる現在地

- `mount` の汎用 descriptor、型付き slot、native source、dirty-node、APP/SYSTEM の lease と repair は実装されている。メニュー登録済みの通常アプリ4件（hello・imucal・pet・companion）とdeskclock/music overlayは native mount 経路。bridgeもソースは移行済みだが現行imageには埋め込まれず、出荷アプリには数えない。Kasane core はアプリ名や音楽の意味を知らない。
- music はアプリ側に音声・入力・文言判断を残し、表示を native presenter へ移した。music 固有の light 3矩形だけを PATCH する高速経路は採用済み。全計画 PATCH は利益を確認できず不採用。
- 「producer が公開した text → 各 core destination が受理するまで」は該当実測経路で1 copy。producer 原データから LCD 描画完了までの**無条件の全経路1 copy以下は必須目標にしない**。同一payloadの不要な複製を減らす努力目標とし、2 bank・返却済み borrowed pointer の寿命・実測利益を優先して判断する。
- dirty-node の CPU 利益と部分転送・画素一致は確認したが、音楽 overlay は native 背景のため多くの frame が全17帯・64,800 Bを転送する。CPU改善とLCD改善は別物。
- hostの全画素一致、注入 `KSN_IO` 後の全帯修復、音声共存の複合試験はある。ただし実LCD GRAM読戻し・実SPI故障・全条件同時・全アプリ/全曲網羅は未証明。旧music repair試験の1回はsend最大が固定上限を6 µs超え、不合格のまま保存。位相分類をOFFにした後続の独立boot×2は同じ固定線を全run通過したが、故障注入付き診断imageの結果を全probe OFF製品imageの実機性能とは呼ばない。

`roadmap.md` は「製品として使える」と「選択的な最適化」を分ける。限定経路の1-copyを全経路の達成と呼ばず、copy回数のみを理由に安全な所有権契約を崩さない。
