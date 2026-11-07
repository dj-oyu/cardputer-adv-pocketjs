# 実験用 native provider による一回評価

既定ビルドでは無効。`AYATSURI_MODEL_EXPERIMENTAL_PROVIDER` を定義したときだけ、
`ayatsuri_model_candidate_evaluate_provider` を利用できる。JS API、AYNM、既存の
`evaluate` / `evaluate_meshes`、その失敗契約、JS skin work上限は変更しない。

## 対象と所有権

信頼できる同期native C処理が、node-localのposition/normalを最大32頂点のslotへ
書く。Modelを一度だけ走査し、各meshのslotを消費してから次へ進む。callbackから
JSや外部user codeを呼んではならない。source入力、cold topology/material、camera、
light、Model、candidate、既に公開したframeをcallbackで変更しない。

meshはModel順で全部要求する。非表示nodeも計算対象で、subset指定やcoldへの
部分fallbackは提供しない。single-sided、holeなしのみ受理する。positionは有限で
各成分が[-4096,4096]、normalは有限で長さの二乗が1から1e-4以内であること。
親子TRSは既存の評価器で一度だけ適用する。

slotは32要素分をcallerが確保し、candidate/arena/Model/cold data/camera/lightや
ready frameと重ならないようにする。providerはslotと自身のprivate work/counterだけを
変更できる。contextとsourceは同期呼出し終了まで有効に保つ。APIはポインタを保持せず、
メモリ確保も行わない。

## candidateだけに使う

この入口は、公開済みactorを原子的に更新するAPIではない。owner/generation/busy、
slot、未対応mesh metadataは評価前に検証する。一度評価が始まった後にproviderや
数値処理が失敗した場合、candidateはnot-readyになり、途中のworkとstatsは残り得る。
既存batchの「不正geometryでは元ready packetを維持する」契約と混同しない。

callerは専用candidateから全packetをpending frame snapshotへコピーし、全件成功時
だけready frameを置き換える。copy失敗時は以前のready frameを保つ。表示・転送側が
保持するsnapshotはrelease/ACKまで再利用してはいけない。startup OOMの後始末、
frame pin管理、排他的な所有はcallerの責任。並行thread/ISR向けの原子操作ではない。

## host検査

```
python tools/ayatsuri/provider/run.py --opt 0
python tools/ayatsuri/provider/run.py --opt 2
python tools/ayatsuri/provider/run.py --sanitize
```

公開の手続き生成fanだけを使う。23meshの24frameを既存一括評価とpacket/pixel比較し、
4/8行band × 1/8行depth scratchを検証する。provider拒否、後半geometry不正、
projection/容量失敗、owner/generation、slotの不整列/alias/null、hole/double-sided metadata拒否、再入evaluate/dispose、
cold復帰、全face cull、親をhiddenにしたときの空snapshot、pin中の再利用拒否、
source解放後のsnapshot描画を含む。

検査用adapterはmesh packetだけを扱い、公開済みの完成品APIではない。テストの確保は
assertで止めるため、productionのOOM constructor検証は別途必要。sanitizer実行は
このhost環境向けにLSanを明示的に無効化し、ASan/UBSanを有効にする。requested allocation
ledgerが最後に0になることを検証するが、LSan成功や全プロセスのリーク証明とは言わない。

この検査に実asset、DQ、morph、透明material、ESP32-S3のbuild/実測、実機表示は含まれない。
