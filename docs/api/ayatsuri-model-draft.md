# ayatsuri: 共有Modelと個体Instance

状態: 7d21b302を対照とする比較Cから、明示回収と製品CLOUD MEMORYへの組込みを進める試作。通常CLOUD PROBEの既定経路は維持し、native資産経路の実機確認は未完。既存のJS動的作成経路は維持する。

[host比較結果](../perf/cloud-native-model-study.md)では単体accounted peakを156124→110572 Bへ削減し、301frame画像ハッシュ一致を確認した。製品のnative cloud切替とこの経路の実機検証は未完である。

## 合意した責務

固定の形状・材質初期値・親子関係・target bindingはbin化でき、native Modelが直接参照する。個体ごとの姿勢・速度・可視性・材質override・評価cacheはmutable Instanceへ分離する。JSは演技と目標の指示を所有し、どの資産がnative共有かを明示して使う。全面native化・全面mesh化は要件ではない。解析楕円体/blendと将来の頂点meshを用途で併用する。

今回の縦切りは既存cloudの19node（group3、ellipsoid13、blend3、合計14lobes）。同じ固定世界光・カメラ・入力・clockで、既存JS作成Aとnative Cを比較する。過去のbin→JS再構築Bの不採用をnative共有一般へ拡張しない。

現実装のModel Instanceは描画pose/world/cacheを所有する。速度・運動位相は既存の個体別follow actorが所有し、共有Modelには入らない。これらを単一arenaへ統合した状態ではなく、演技と描画の個体状態を分離して接続した最小比較である。

## 所有権と更新順

CのModelは検証済みreadonly spanを借用する。spanとModel構造体は全Instanceより長生きし、可変cacheを共有しない。Instanceは呼出側が確保する8-byte整列arenaを所有し、ownerとgenerationで操作を検証する。ファイル読み込みやArrayBufferはflash直参照とは異なる。JS `modelOpen(ArrayBuffer)`は検証前にguest会計内へsnapshot copyし、可変backingへの借用を残さない。埋込みstatic span用C入口はprocess/firmware寿命の資産に限定する。

試作JSは `pocket.ayatsuri.modelOpen(buffer)`、Modelの `node(name)` / `instance()` / `dispose()` とInstanceの `targets(...)` / `evaluate(camera, light)` / `submit()` / `readPose(...)` / `stats(out)` / `dispose()` を持つ。通常利用者は `AyatsuriNative.scene({model, camera, light})` の配置・材質操作を使い、typed offsetやdirty maskを管理しない。低層target batchは同期borrowで全件検証後に適用し、拒否時に一部だけ書き換えない。同値targetは再評価を増やさない。Modelをcloseしても既存Instanceの資産寿命は保持し、新Instance作成を拒否する。

評価完了→提出→既存app-owned commitの順。packet viewは次のmutationまでの借用でありimmutable snapshotではない。既存native描画への提出ではsnapshot copyを残す。失敗したcandidateと、既にcommittedの画像は別である。描画budget失敗でcandidateが使用不能になった場合はnew beginから再試行し、旧committed画像を維持する。submitのappend数rollbackだけでcandidateがcommit可能へ戻ると保証しない。

## 数値と試作形式

ellipsoidの固定2色領域を使うassetだけ、required section8を追加できる。[material領域のwire/所有権](material-regions.md)を参照。従来のModel blobとInstance work量は維持し、Model descriptorは8B増える。C headerの構造体サイズが変わるためC callerは同じheaderで再buildする。既存binの全書換えや、blend共通座標のmask対応を意味しない。

Model.disposeは新規利用をcloseし、生存Instanceは保護する。close済みかつ最後のInstanceが退役した時点で、copy版cold blobを同期的にguest allocatorへ返す。保持中のModel tombstoneやdisposed scene/handleがあってもcold blobを保持しない。static版はspanの参照を外し、flash自体はfreeしない。Modelをopenのまま保持した場合は再利用のためcold blobも保持する。最後のJS参照のfinalizerも保険として働く。GC cycleのfinalizer順に依存しないC側参照管理を持つ。

`model.stats(out)`はclose後も呼べ、`closed`、`instances`、`ownedBytes`、`staticBytes`、`released`を返す。後二つのbytesは現在attach中のspanで、`released=true`はdetach済みを意味する。static flashの物理回収やESP heap増加の保証ではない。Instance.disposeは個体arenaを返す。native scene.disposeは自分のInstanceとJS buffer/参照を切るが、借用Modelを勝手にcloseしない。二重disposeは成功、disposed handleの操作は拒否する。既存描画packetはsnapshotなので、cold回収にpresent待ちやsleepを加えない。

製品の `pocket.ayatsuri.modelBuiltin('cloud')` / `modelBuiltin('fox-face')` は検証済みの組込みAYNMをModelとして開く。固定byte列は直接参照し、巨大なJS定義graphへ戻さない。unknown名と無効形式は公開前に拒否する。汎用FS loaderや任意assetの認可設計まで含まない。[有限メモリ比較](../perf/cloud-memory-fixture.md)は同一firmware内でJS定義Aと組込みCを選択する。

## native評価済み投影と凸clip提出

狐の固定資産はAYNM v1のまま保持し、clipのunit-local頂点と表情制御はJS utilityが所有する。顔v2は旧25nodeのIDを保ち内耳・耳毛4nodeを追加した29node。次のInstance入口は、既存Cの`ayatsuri_model_instance_next`が返す評価済みpacketとhoneのclipped ellipsoidを組み合わせる。数値評価器やasset形式をもう一つ作らない。

| JS入口 | 型と意味 |
| --- | --- |
| `readProjection(id, out)` | idは範囲内整数。outは通常Float32Array/8。evaluate済みで可視のellipsoidのscreen8を同期copyしoutを返す。group/blend/hidden/未評価は拒否、失敗時out不変 |
| `submitClipped(ids, counts, xy)` | idsはUint16Array/k、countsはUint8Array/k、xyはFloat32Array/(16k)。k<=node数、id重複なし。1行は最大8点の画面XY。countは0または3..8。指定idは可視ellipsoidのみ。全行を検査してから提出 |

全backingは通常ArrayBufferで、shared/detachedを拒否する。Cは呼出中だけborrowし、JS/C pointerを返したり描画まで借り続けない。readProjectionは32Bの明示copyでありゼロcopyではない。clip行はscene所有の固定bufferを再利用する。未使用座標は描画へ使わず、native packetのreserved/tailはゼロ化する。idsの並びで描画順を変更せず、元のnode順を維持する。k=0は通常submitと同じ描画。

閉眼/poseによる非表示は、bindされた眼・耳自体をhiddenにせずcount0（EMPTY）で指定する。hiddenなbound nodeや祖先はこの最小入口では対応外で、無言でclipを外さない。通常の非bound nodeの可視性は従来どおり。

型・全行・重複・clip値の不正はbegin前に拒否する。提出中のnative append失敗は既存markへcount/raster/extを戻し、旧committed表示を変更しない。ただし混合する通常ellipsoid/blendの予算失敗は従来どおりcandidateをpoisonし得るため、次beginから復帰する。単独hone `ellipsoidClippedPacked`の強いNOT_APPLIED契約と混同しない。

高層利用者は[optional clip utility](ayatsuri-clip.md)でnode handle/apertureを使う。dirty mask、id tableのstride、転送準備をアプリへ要求しない。C利用者は同じinstance iteratorのborrow期限と既存hone C snapshot入口を使う。

C arenaは8-byte整列を要求する。JS adapterはguest allocatorの4-byte整列だけを前提に元allocationへ7 B足して確保し、整列後のspanを核へ渡す。解放は必ず元allocationへ戻す。targetのopacityはN個のLE binary64値を持つbyte spanで、double alignmentを要求しない。JS公開引数の型はFloat64Arrayのまま、同期borrowと全件validationを保つ。

F32/U32入力は4-byte整列を要求し、JS adapterも外部ArrayBuffer由来の未整列viewを拒否する。型名だけを根拠に未整列pointerをCへcastしない。U8とF64 byte spanにはこの4-byte制約を課さない。

AYNM v1試作はLE、offset/length付きsection directory。nodes、rest TRS、radii、local lobes、materials、target bindings、ASCII名のsectionを持つ。未知required sectionは拒否、optionalは境界検証してskip。長さ・積・offsetのoverflow、重なり、stride/count、親の順序、ID、finite値を検証する。1MiB、256nodes、64sectionsが現試作上限で、製品予算の保証ではない。

保存TRSはF32、計算中間と丸め順は既存共通C核とJS実装に合わせる。thickness/opacity/edge softnessは既存JS Numberの量子化結果を保つためF64を保持する。一律のF32化やPIE用量子化を同時に導入しない。packed保存形式と将来のPIE実行layoutは同一でなくてよい。毎frameの詰め替えや余分な全state copyを増やさない。

## 比較の合格条件

現試作は変更なしの再評価を省くが、受理した変更では全nodeを再走査する。局所target・色のみ・光のみを最小依存範囲だけ再計算する最適化は未完で、カウンタで現コストを測ってから進める。native化そのものを差分更新の完成と呼ばない。カメラ入力は現試作ではF32配列であり、一般のJS Numberカメラとの完全同義性は未保証。比較は両方へ同じ明示F32倍率を与える。

同じQuickJS経路の数値/packet/画素一致を基準とし、Nodeの異なる数学実装だけをoracleにしない。2Instanceの非干渉、owner/dispose/再入、全件atomic検査、OOM復帰、旧committed画像維持を確認する。初期化peak・定常guest・native heap・BSS・stack・最大単一要求・packet snapshot copyを別記し、guest外へ移した費用を消えたRAMと呼ばない。host値は実機性能ではない。効果がなければ製品経路へ採用しない。

## 後続: 凸多角形と必要時のmesh

ユーザー合意の追加方式。ayatsuriはauthoringしやすいconvex polygon部品を持ち、honeは軽量convex fillと、必要時のtriangle描画を担う。既存 `shapes.project/fill/outline` は存在するが、UV付きtriangle、morph、補間法線の新しい契約は未実装。固定形状のtriangulationはoffline/bin作成時の候補とし、UV/morph/法線補間が必要な形だけ内部meshへ進める。今回のnative Model比較には実装を追加しない。

変形後の凸性・winding・面積・自己交差・三角形反転を検査またはauthoring制約で保証する。凹化や縮退を任意fan分割で安全と扱わない。境界外/不正変形のbatchは一部公開せず拒否し、旧committed frameを維持する。近接near-plane処理と許容誤差は後続の具体fixtureで固定する。

最初のfixture候補はrigidな耳飾りと、根元が頭へ追従し先端が遅れて曲がる前髪の房。形状の有効性と動的追従の因果を分けて確認する。狐2.5Dの画像/warp、HOME背景利用、PIEは別の後続工程である。

3D本体にもview-dependentな輪郭morphを併用する。3D anchor/depthとカメラ依存の見た目補正を分離し、顔方向とexpressionから頬毛・瞼・口・前髪などを補正する候補。2.5D専用には限定しない。最初は狐の頬毛か人の前髪の小fixtureで、occlusion、本体との干渉、適用角度域、切替連続性、法線と世界光の整合を調べる。これらは未解決であり、投影へmorphを加えるだけで正しい前後関係や照明になるとは保証しない。

長期のasset制作は狐を先行し、角度×表情の目標図から、3Dで保持する部分、morphする輪郭、遅延揺れ、動的照明を分けて検討する。現cloudのAYNM section群を唯一の長期形式として固定せず、mesh/index/UV/morph等は別sectionで拡張できる設計に留める。既存モデルは比較用に残す。これは後続制作仕様の方針で、今回mesh実装を追加したことを意味しない。

制作順はまず狐の3D版を仕上げ、1キャラずつ進める。3体の3D版が揃ってから2.5D画像パーツ版へ着手する。3D版でview-dependent多角形morphを使う方針は維持し、残り2体の順序は今固定しない。共通native Model比較はこの制作順とは独立に継続できる。

狐の後続最小実証は頭・3D鼻口吻・片眼（球/clip/上下瞼）・眉・片頬毛/耳根・尾一節。neutral/smile/surprise/jawOpenを独立controlとしblink/gazeと併用、初回camera検証範囲はyaw±35°/pitch±10°とする。rest/index/UV、expression delta/view corrective、material mask、anchor/parent、depth/occlusion、clip、許可範囲を分離する。camera相対方向から決定的に補間し、最終surfaceからattachment/clip/normal/depthを同frameで確定する。遠側眼は三角形反転で消さず遮蔽または空clipにする。目標画の焼込み陰影をalbedoへそのまま移さない。

合格条件は240×135での表情可読性、角度×表情sweep、反転/縮退/自己交差/凹化、毛根浮き/目漏れ/口吻と鼻の前後順序、固定光下のpose回転と固定poseでのlight回転。±90°のside目標画や小実証の合格を、その角度への対応済み・狐全体の完成とは扱わない。初回合格後に顔全体・胸毛・尾へ拡張する。
