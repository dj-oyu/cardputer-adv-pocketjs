# managed Instanceから合成skinを評価する

`instance.evaluateSkin` はJSのjoint行列・bind geometry・weightを
[scalar skin core](skin-core.md) で計算し、[C描画接続](skin-render-bridge.md)へ渡す。
計算済みvertexを渡すだけのJS公開ではなく、palette生成とskinningがnativeで動く。
既存の `submit()` / `submitClipped()` とShiki frameへ提出する。

AYNM v1のcold dataと従来APIは変更しない。今回は**1回に1個の片面・holeなしmesh**を
置換する小さいAPIで、**評価済みpacket全体を置き換え、他nodeはcold geometryで評価する**。
別のmeshIdへ連続で呼んでもskinは蓄積せず、前のmeshはcoldへ戻る。複数meshは後続の明示[batch API](skin-batch.md)で同時評価できる。
skin付きwire format/importer、scene facadeの自動skin制御はまだない。
狐の原本・派生資産は使用せず、実機受入や狐3D完成を意味しない。

## 呼出し

```js
const instance = model.instance();
// All arrays are prepared/reused by the app. Resolve meshId during setup.
function draw() {
  instance.evaluateSkin(camera, light, meshId,
    jointGlobals, inverseBind, bindVertices, jointIndices, weights);
  P.beginMeshFrame(17, 2048, 200000, 1);
  instance.submit();
  P.commit();
}
```

引数は厳密に8個。通常Array、coercion対象object、Proxy、SharedArrayBuffer、
detached bufferは拒否する。読み取り専用なのでimmutableな通常ArrayBufferは受理する。
Float32の4byte、Uint16の2byteの実アドレス整列も検証する。typed arrayの`buffer` getterや
node IDの`valueOf`を呼ばず、借用中にJS callbackを実行しない。

- `camera`: Float32Array(9)、`light`: Float32Array(6)。通常evaluateと同じ。
- `meshId`: 整数node ID。元Modelのmesh vertex数と入力数が一致する必要がある。
- `jointGlobals`: Float32Array(12*J)、J=1..256。jointごとの行優先3×4 affine。
- `inverseBind`: Float32Array(12*J)。paletteは `global * inverseBind`。
- `bindVertices`: Float32Array(6*V)、V=3..32。各vertexはposition3、normal3。
  normalはfinite/nonzero、native coreで変形後に正規化する。
- `jointIndices`: Uint16Array(4*V)、`weights`: Uint16Array(4*V)。各vertexの4 weightは
  厳密に合計65535。zero weight slotも有効なjoint indexを必要とする。

joint globalとinverse bindは**対象mesh nodeのlocal空間**で作る。親子jointのglobalは
callerが先に合成し、skin後にModel側のnode/hierarchy TRSが一度だけ適用される。
`pocket.ayatsuri.compose(pose, parent, output)` と再利用するsubarrayでjointのposeを
更新できる。world jointからmesh-localへの変換はcallerの責任。

bindVerticesはcaller所有のrest geometryであり、Modelのcold geometryとの一致を
APIが推定・照合することはない。通常evaluateに戻すとcold geometryへ戻るため、
両者のbind/rest対応はappが確認する。topology・mask・材質はModelのcold tableを使う。
C bridgeと同じく、skin結果のpositionは±4096、normalはunitの範囲を検証する。

上限は実装で強制する入力上限であり、256jointをS3上で収容・高速評価できるという
主張ではない。反転は既存の片面cullに従い、windingを補正しない。特異blendは拒否する。
法線は混合後のlinear行列の逆転置であり、weightが面上で変化する実surfaceの
微分法線ではない。詳細とLBSの制約はscalar core文書を参照する。

## work、失敗、解放

入力を同期借用し、JS bufferやobjectの参照を保持しない。評価が戻った時点で
callerは入力を変更・解放できる。skin workはinstanceごとに独立してguest allocatorへ
課金し、最初の成功呼出しで保持する。必要容量は `64*V + 48*J B`、今回の6vertex/
2jointは480 B、最大入力は14336 B。joint global/inverse bindとJS view/objectは別費用。

一度確保したworkは同容量以下の呼出しで再利用し、普通のevaluateに戻しても保持する。
増量は旧workを残したまま候補を確保し、成功後に差し替えるため、増量中だけ旧容量と
新容量の両方が必要になる。無効入力、OOM、失敗した増量では候補を解放し旧容量を保つ。
`instance.stats(out).skinWorkBytes` は現在保持するpayload容量で、allocator overheadを
含まない。rigid-only instanceはこのworkを確保しないが、native actorの管理pointer/
sizeと共有prototypeの追加methodの費用はある。実際のbinding translation unitを
変更前後でcompileしDWARFの型sizeを確認したこのhostでは、native actorは208→224 B
（rigid-onlyも+16 B）。target ABIの値は未測定。

型・長さ・weight・joint・skin計算の不正やOOMでは、直前のready packetを維持する。
skinが計算できた後にC評価へ進み、camera/projection等が失敗した場合は、従来どおり
instance全体がnot-readyになる。正常再評価で復帰する。不正geometryの拒否時には
statsのevalCallsも変わらない。数値範囲の失敗はLIMIT_EXCEEDED、入力不正は
INVALID_ARGUMENT、OOMはQuickJSのallocation例外となる。

非emptyなskin評価は入力pointerが同じでも毎回計算する。直後に通常evaluateを呼ぶと
同じcamera/lightでもcold geometryへ戻る。submitは最新の評価済みpacketをコピーし、
その後の入力変更・instance/model破棄と独立する。提示ACKやIO失敗時のretryは既存runtimeの
責務である。明示disposeとGCのどちらでもworkを解放し、disposeは冪等。Modelを閉じても
既存instanceは利用でき、最後のinstance解放でcold copyも解放される。

## 実QuickJS host試験

```sh
python tools/ayatsuri/skin/run_js.py
# This ptrace environment cannot start LeakSanitizer:
python tools/ayatsuri/skin/run_js.py --no-leak-check
```

実QuickJSと製品のnative binding、skin core、C bridge、submit、Shiki snapshot、Kasane
compositorをリンクする。4/8行strip × row/spanの4条件、各240frameの往復pose更新を
検査し、遅延transportの借用strip、IO失敗後retry、完了前のpending保持も検査する。
Cから同じcore/bridgeで作ったoracle画像と比較するため、ここで新たにskin数式や
renderer全体の独立性を主張しない。それらの境界は先行core/C試験で別に検査する。

- strict型/長さ、無効weight/index、zero/nonfinite normal、特異/overflow、未参照jointの
  nonfinite、holes/両面拒否、SAB/detach/foreign buffer整列、immutable入力を検査。
- callback/setter中のbusy再入拒否とthrow後の復帰、初回workと増量のOOM、失敗後の
  ready/capacity保持、増量後の小さい入力/通常evaluateで容量を保持すること、
  別meshIdへの呼出しで前meshがcoldへ戻ること、独立した2個のskinned instanceを検査。
- 入力変更を評価後・submit前に行ってもpacketが変わらないこと、Model close後の利用、
  全source破棄後のpending frame描画、明示disposeとexpando cycleのGCを検査。

単mesh版を導入した時点の4条件のhost実測では、240frameのpose/native skin評価範囲のlinked allocationは
0回、submit/present範囲は初期frame確保等を含め合計18回。guestのframe時点の標本最大は
96793 B（設定上限160 KiB）。これはsource評価・OOM phaseを含む全時点のpeakではない。
初回OOM sweepは16失敗/17成功、増量失敗では480 Bを保持し、復帰時576 Bを保持した。
最終requested ledgerはlive bytes/blocksとも0。各条件の詳細は
`.cache/skin-js-host/rows*-direct*.log`、build入力/commandは同directoryの `commands.json`。

requested合計には比較用C Model/cold984 B/arena2532 Bとhost frameworkも含む。
JS view、cold、arena、skinWorkBytesはguest/requested合計の内数なので足し合わせない。
static panel/reference/ledger、stack、allocator overhead、shared library内部は対象外。
ASan/UBSan成功と、ptraceにより無効化したLeakSanitizerは区別する。S3 RAM、stack、
cycles/FPS、LCDの見た目、実キャラクター遮蔽は未検証。
