# ayatsuri 投影可視面 utility

`apps/lib/ayatsuri_visibility.js`を必要なappだけが明示loadする最小数値utility。Model形式や既定キャラクターを変更しない。投影はapp/sceneが行い、時計・描画先・frame開始/公開も所有しない。

```js
const faces = AyatsuriVisibility.create({
  vertexCount: 30, indices: authoredIndices, order: 'centroid-depth'
});
const visible = faces.evaluate(projectedXY, viewDepth);
if (visible.triangleCount)
  pocket.hone.gouraudPatch(projectedXY, visible.indices,
    colorsA, colorsB, mask, holeXY, holeCounts, visible.triangleCount);
faces.dispose();
```

- `vertexCount`は整数1..32、`indices`はordinary Uint16Arrayの1..64面。生成時だけtopologyを1回コピーし、呼出し元による後の変更から隔離する。
- `evaluate(xy, depth)`はordinary Float32Array[2n]/[n]。xyは有限±4096px、depthは有限のview座標、正が観察者側。投影後area2>1e-5を表、<−1e-5を裏、それ以外を縮退として分類する。裏面のindexを反転して表面へ偽装しない。
- `order:'authored'`（既定）は元の面順。`'centroid-depth'`は3頂点depthのbinary64和を昇順、同値なら元の面ID昇順。遠い面を先に描く**明示的heuristic**であり、交差面/循環遮蔽/任意meshのZ解決ではない。使用assetと角度域で別途検証する。
- 戻り値は同じborrowed descriptorを再利用する。`indices`は固定容量Uint16Array、`triangleCount/backfaceCount/degenerateCount`は最後の成功結果。書込み禁止。次の成功evaluateで内容が更新され、失敗なら前の結果を全byte維持する。cloneしたimmutable snapshotではない。全て裏/縮退ならtriangleCount=0で成功、描画は呼ばない。
- evaluateは動的確保/配列view生成/描画/JS callbackを行わず、入力を保持しない。独立selector間でmutable stateを共有しない。通常の実行中に同一bufferを他threadから変更することは許さない。
- disposeは冪等。再入中disposeは拒否。selector/resultを保持したままでも内部のtopology/output/backend参照を切り、その後のevaluate/getterは拒否する。呼出し元が既に取得した生typed array参照のRAMまで強制解放はできない。

## 共通C/JS境界

Cは`main/ayatsuri/ayatsuri_visibility.h`の`ayatsuri_visibility_select`。JS bridgeは`pocket.ayatsuri.selectVisible(xy, depth, sourceIndices, outputIndices, counts, order)`。countsはwritable Uint32Array[3]、orderは整数0(authored)/1(centroid-depth)。outputはwritable Uint16Arrayで全入力面数×3以上。全capacityを出力領域としてalias検証し、書いた先頭以外のtailは保持する。

C/JS共通で全入力（未使用頂点/裏面を含む）を検証し、範囲外index、非有限、短出力、outputとinput/outputの重なりを拒否。出力/countsは一括成功時だけ更新する。入力間aliasは許可。Cは同期借用・自然整列のcaller-owned span、JSはShared/detachedを拒否し、foreign backingの非整列も拒否する（snapshotをmemcpyするhone描画とは異なる）。readonly inputは可、readonly outputは不可。JS失敗はINVALID_ARGUMENT（既存VMのdetached例外はそのまま伝播）。CはINVALID/RANGE/OVERLAPを区別。buffer/owner/clockを保持しない。

C作業領域は最大64面のID列64 B＋depth key列512 Bと小さなspan/counts、heap確保0。固定selectorのtyped backingはsource/output各6t B＋counts12 B、constructor検証用xy8n B/depth4n Bは一時。JS object/配列header、stack、native描画snapshot、constructor peakは別会計。重心sortは最大64面のstable insertion、最悪O(t²)。authored順はO(n+t)。性能改善値やESP32実測値ではない。

## 境界

頂点法線から求める照明色とbody-local albedo maskは選択処理の対象外で、元の頂点番号へ対応したまま保つ。Gouraudは選択index先頭だけsnapshotする。眼のclip、別packetの鼻や毛、scene間の描画順はapp側の明示構成が必要。一般の深度/遮蔽完成APIと呼ばない。screen-linear補間の制約は[shapes](shapes.md)と同じ。
