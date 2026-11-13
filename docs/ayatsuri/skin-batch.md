# 明示batchによる複数mesh skin評価

`instance.evaluateSkins` は複数meshを一度にskin計算し、既存のC batch評価へ一回で渡す。
[単mesh版](skin-js.md)の `evaluateSkin` は変更せず残す。どちらも評価済みpacket全体を
置き換え、呼出し間でskinを蓄積しない。batchに指定しなかったnodeはcold geometryへ戻る。

AYNM v1、Modelのimmutable cold data、既存のsubmit/commitは変更しない。穴と両面の
skin overrideは引き続き拒否する。GLB skin importer、実armature/weight変換、眼の
procedural material変換は別工程であり、この合成成功で変換損失を隠さない。
私有狐資産、PIE、実機/flash、CI accessは変更していない。

## packed typed API

```js
// Rows: [nodeId, vertexCount, jointCount]. Pools follow this row order.
const batches = new Uint16Array([leftId, 6, 2, rightId, 4, 1]);
instance.evaluateSkins(camera, light, batches,
  jointGlobals, inverseBind, bindVertices, jointIndices, weights);
P.beginMeshFrame(17, 2048, 200000, 1);
instance.submit();
P.commit();
```

厳密に8引数。camera/lightは通常evaluateと同じFloat32Array(9)/(6)。残りは次の通り。

- `batches`: Uint16Array(3*M)。各行はnode ID、vertex数V、joint数J。重複nodeは禁止。
  MはModelのnode数以内（Model自体の上限256）。各行V=3..32、J=1..256。
- `jointGlobals`、`inverseBind`: Float32Array(12*ΣJ)。行ごとの行列列を連結する。
  同じjointを別meshでも使う場合も、今回はそれぞれの行に必要な行列を渡す。
- `bindVertices`: Float32Array(6*ΣV)。各vertexはposition3/normal3、行順に連結。
- `jointIndices`、`weights`: Uint16Array(4*ΣV)。各vertexの4slot。indexは**その行の
  palette内の0..J-1**で、全体poolへのindexではない。weight合計はvertexごとに65535。

全poolの長さは合計数に厳密一致させる。各行でV/Jが異なってよく、node順とdescriptor順も
異なってよい。各meshのvertex数はcold Modelと一致し、結果はそのmesh nodeのlocal空間で
渡される。親子TRSが後で一度だけ適用される。bind/restの対応はcallerの責任。
座標範囲、inverse-transpose normalの近似、反転時cullは単mesh/C coreと共通。

空batchは `Uint16Array(0)` と5個の適切な空poolを渡す。通常evaluateとしてcold geometryへ
戻り、既存skin workの容量は保持する。空descriptorに非empty poolを付けることは拒否する。

## 所有権と入力安全性

descriptorをobject/Arrayとして読まない。すべてstrict typed arrayからnative primitiveで
取得し、getter、iterator、数値coercionを挟まない。call引数がbufferをrootとして保持し、
借用中にJS callbackを実行しない。ArrayBufferの整列と長さ、detach、SABを検証する。
immutableな通常ArrayBufferは読み取り入力なので受理する。

input同士は同一または一部重複したArrayBuffer/viewを使ってよい。例としてidentityの
jointGlobalsとinverseBindを共有できる。APIはJS側のどの入力にも書かず、instance所有の
scratchへコピーして計算する。成功後も失敗後もJS pointer/referenceを保存しない。
戻ったら入力・descriptorの変更/解放が可能で、現在のpacketは影響を受けない。

## 強制するメモリ上限

**合計joint数ΣJは256以下、scratchの要求確保bytesは16384以下**。
後者には以下をすべて含める。

- packed入力のnative copy: `40*ΣV B`
- skin結果: `24*ΣV B`
- palette: `48*ΣJ B`
- C geometry記述子: `sizeof(ayatsuri_model_mesh_geometry)*M`
- 記述子を整列させる余白: `_Alignof(ayatsuri_model_mesh_geometry)-1`

したがって判定式は `64*ΣV + 48*ΣJ + M*sizeof(descriptor) + alignment-1`。
ABI依存の項も実際のcompile時の値を使う。256jointと最大vertex数の組合せが常に
収まるという意味ではない。上限超過はLIMIT_EXCEEDEDで、候補確保前に拒否する。

workは単mesh版と同じinstance所有bufferを再利用する。今回の64bit hostではV=6/4、
J=2/1の2meshで823 B（descriptor16 B/align8）。V=32/32、J=127/127は16327 Bで受理し、
J=128/128は16423 Bなので拒否する。小さいV=6/4でΣJ=257のケースは要求式では
16384未満だが、独立した合計joint上限で拒否する。これも試験する。

小さいbatch、空batch、通常evaluate、既存evaluateSkinへ戻してもcapacityは縮めず、
`stats.skinWorkBytes` が保持する要求bytesを返す。dispose/GCで解放する。
増量時は旧workを保持して候補を別に確保するため、成功前の一時peakは**旧容量+新容量**。
各容量は上限内でも、増量中の2本合計まで16384以内ではない。allocator overhead、JSの
元pool/view、native actor、cold/arena/frame snapshotは別費用で、全体S3適合は未検証。
今回native actorのfieldは増やしていない。

## batch全体の失敗契約

1. strict型/長さ、descriptor、重複、合計上限を検証する。
2. private workで全paletteと全skin結果を作る。後半meshが不正でもC評価を始めない。
3. 一回の `ayatsuri_model_instance_evaluate_meshes` へ全geometryを渡す。このC APIも
   geometry全体を検証してから評価する。
4. 成功した時だけ新しいwork候補を所有bufferにする。失敗なら候補を解放する。

型・pool・weight・joint・geometry不正、skin数値失敗、OOMでは直前のready packetと
保持capacityを維持し、先頭meshだけを公開しない。再利用workのprivate内容が途中まで
書き換わっても、既存packetは別のinstance arenaにあるため変わらない。

**C評価に入った後のcamera/projection/transform数値失敗は別**で、従来契約どおり
instance全体をnot-readyにする。部分packetをsubmitできず、正常な再評価で復帰する。
既にframeへコピーしたsnapshotは影響されない。instanceのbusy/closed/owner検査と
callback中の再入拒否は既存と共通。submitのrollback、commit、transport retry、提示ACKは
引き続き既存runtimeの責務である。

## 実QuickJSから描画するhost検査

```sh
python tools/ayatsuri/skin/run_batch.py
# LSan cannot start under this cloud's ptrace; ASan/UBSan stay enabled.
python tools/ayatsuri/skin/run_batch.py --no-leak-check
```

公開の6vertex/2joint stripと4vertex/1joint quadを同時変形し、実QuickJS、native binding、
C bridge、Shiki snapshot、Kasane compositor、遅延transportを通す。4/8行strip × row/spanの
4条件、各240frame往復。比較用C経路は同じskin core/rendererを共有するため、ここで独立した
skin数学/renderer正解を主張しない。前段のcore試験に独立した数式チェックがある。

- 異なるV/Jとpalette offset、descriptor逆順、input部分alias、immutable descriptor。
- 重複node、後半meshの不正weight/index/normal、nonfinite、特異行列、型/長さ、
  descriptor detach、SAB、foreign buffer不整列、busy setterとthrow後の復帰。
- late-invalidの直後に再評価せずsubmitした画像が直前batchとbyte一致。
- 初回OOMと単mesh480 B→batch823 Bの増量OOM。失敗直後のsubmit画像が旧単meshと一致し、
  旧capacity/readyを保つ。復帰後は両meshを描画できる。
- 独立actor、subset/empty/単meshへの切替、descriptor/入力変更を評価後submit前に行うこと、
  Model closeと全source破棄後のpending描画、expando cycle GC、最終requested ledger全解放。
- 上限近傍の合法2×32vertex fixtureと、要求bytes/合計jointの独立した上限拒否。

最終4条件ではwarm評価のlinked allocationは0回、submit/presentは240frame合計18回。
guestのframe標本最大104160 B（設定160 KiB）、requested peak197721 B、最終live bytes/blocksは0。
初回OOM sweepは13失敗/12成功。これらはhost実測であり、targetの値ではない。

実測のwork容量、warm allocation、guest frame標本、requested peakは
`.cache/skin-batch-host/rows*-direct*.log`、build command/入力hashは `commands.json` に出す。
比較用C arena/blob、host shimはrequested合計に含まれ、static panel/ledger、stack、
allocator overhead/shared library内部は含まれない。guestのframe標本は全評価時点peakではない。
LSan無効をleak検査成功とは言わない。

## 実assetへの次工程

この上限で任意の大きな実モデルが収まるわけではない。GLB armature/bind/weightの
明示import、mesh分割時のjoint remap、node-local座標対応、material変換と失敗診断、
実assetのメモリ会計/遮蔽/姿勢が次の別工程である。budgetを越えるassetを逐次単mesh評価で
累積できるかのように扱わない。穴・両面・morph・実眼material・実機性能も未完のまま。
