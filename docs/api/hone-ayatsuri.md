# hone / ayatsuri — 役割と命名・移行契約

2026-10-29のユーザー決定。まず名称と責務を固定し、公開面の移行と検証を別工程で進める。ここでC APIの実装完了やHOME対応を宣言しない。

## 二層と言語の関係

**hone**は汎用の低レイヤー描画・変形API。骨格アニメーション専用ではなく、射影、面、輪郭、法線/材質/光の数値評価、解析的曲面等を扱う。キャラクター名、瞬き、芝居、appの開始停止を含めない。狭い保存型、借用範囲、snapshotの所有権、資源上限を明示する。

**ayatsuri**は配置・階層・材質・テクスチャ変形・演技を扱う共通フレームワーク。**JS APIとC APIの両方**を持つ構成にする。C APIをhoneの別名としない。JSは視覚に対応するobject/handle、Cは明確なowner/allocator/lifetime/資源制御を与え、座標・単位・評価順・失敗時の意味を共有する。

目標構成は `appまたはHOME環境adapter → ayatsuri（JS adapter / C API、共通評価核）→ hone → kasaneの候補frame/提示`。時計、入力、描画先、begin/commit/ACKと資源ownerは環境adapterが所有する。HOME対応は低APIの設計実装後に行い、現JS appのrealmをHOMEへそのまま持ち込めるとはしない。

## 現状からの対応表

| 旧/現行の識別子 | 正式名・移行方針 | 状態 |
| --- | --- | --- |
| `pocket.shapes.project/fill/outline` | `pocket.hone.project/fill/outline`、既存shapesを互換として保持 | 同じnamespaceのaliasを実装、同head検証へ |
| 新設の`pocket.shapes.shade/ellipsoid` | `pocket.hone.shade/ellipsoid`を新利用例の正規表記へ | 同上、古い呼出しも保持 |
| Cの`pocket_shape_*` binding | JS bridge内部。公開hone C面と区別して段階移行 | 既存名の一括置換はしない |
| `ksn_proc_*` | kasaneのframe/renderer内部。honeのpacket提出先 | 無関係な識別子は改名しない |
| JS `Character.scene` | `Ayatsuri.scene` | 新公開名へ移行、旧名の互換範囲を明示 |
| JS `CharacterMotion.track/sample/performance` | `AyatsuriMotion.track/sample/performance` | 軽量な別chunkを維持し、material engineを強制ロードしない |
| `CharacterLab/CharacterModels` | 試験app固有。`AyatsuriLab/AyatsuriModels`へ関連範囲だけ移行 | キャラクター種類の意味はmodelに残す |
| Cのayatsuri scene/handle/evaluate/submit | `ayatsuri_*` | 今回は意味/所有権設計、C共通核の実装とJS接続は未完 |

既存`kasane.shapes` capabilityは互換性を維持する。初期hone endpointも同じmanifest gateを使い、新しい権限やcapability名を増やさない。`pocket.hone === pocket.shapes`で、honeを先に読んでも旧namespaceを一度だけ構築し、同じmethod/resource ownerを参照する。古いエラーのoperation名`shapes.*`も互換のまま残す。既存JS、ホストshim、firmwareの入口を照合して実装状態を更新する。filenameとC内部名の全置換を完了条件にしない。

## 共通の意味契約

- 座標はx右/y下/z手前、角度rad、親子変換は列ベクトルの`T*Rz*Ry*Rx*S`。世界光・カメラと身体UVの固有模様を分離する。非一様scaleの法線は逆転置。保存float32・計算中間・縮退/overflow拒否は[現縦切り仕様](character-utility.md)と揃える。
- 変更→評価→候補へ描画→公開→提示ACKの順序。更新なし/色/UV/光/カメラ/親子変形の依存は両言語で同じ。高層へoffset/dirty flag/転送準備を押し付けない。
- 同期評価の再入を拒否する。二重disposeは可能、破棄済みhandleは拒否。失敗した評価を描かず、nativeの最後の成功画像を保持する。別instanceの光/視点/workを共有しない。隠れたtimer/sleep/乱数を持たない。
- 入力dataは副作用のない通常object/配列、Cは長さ/型付きspan。借用keys/packetと所有workを分ける。JSとCで同じ意味のfixtureと許容誤差を使い、似た数式を独立に育てない。

## ayatsuri C APIの設計境界（未実装）

公開prefixは`ayatsuri_`、opaqueな`ayatsuri_scene`/scene所属handleを使う案とする。`create(config, allocator, out_scene)`、`add(scene, desc, out_handle)`、`place/material/texture/light/camera`、`evaluate(scene,time)`、`submit(scene,hone_sink)`、`destroy(scene)`がJSの操作に対応する。具体的なABI、構造体packing、arenaサイズ照会は実装レビューで固定する。

allocatorとsceneは環境ownerに属し、同一owner taskからのみ操作する。handleはscene識別と世代で他scene/破棄済み利用を拒否する。setterはscalar/configを取り込み、評価workはscene所有、submitは同期借用しhone側の候補へ小packetをsnapshotする。提示ACK前のsource解放/書換えを許す設計にしない。native失敗時にC/JSのどちらが候補を破棄するかをadapter契約で一意化する。

現在のJS評価器は縦切り試作で、Cフレームワークはまだ存在しない。次段階で評価核を共有化し、JS adapterとC利用側が同じ座標・法線・材質・seek・失敗fixtureを通ることを条件にする。CとJSを別々の完成実装として公開してから差異を吸収する進め方は採らない。HOMEでは前景UI、app起動/復帰、停止/解放、共有strip/資源衝突を別途検証する。

関連: [低APIとpacket](shapes.md)、[現utilityとライフサイクル](character-utility.md)、[計測と制約](../perf/character-materials.md)。正式名称への変更は機能・品質・実機検証の合格を意味しない。

## 反復の記録単位

仕様の意図、実装済みの入口、同headのsimulation/実測事実、未検証を別に記録する。[実装と計測記録](../perf/character-materials.md)では対象commit・条件・指標・失敗原因・修正後比較を追い、実機で制約/raceが出たら契約まで戻って見直す。仕様→実装→simulation/実機→仕様の反復であり、名称変更や一枚の静止画だけで完了としない。
