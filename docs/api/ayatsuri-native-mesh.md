# ayatsuri native mesh（最小縦切り）

固定の形状・topology・材質を事前コンパイル資産へ、可変pose/visibilityと評価済みpacketをInstanceへ分離する。既存Model/Instanceを拡張し、別のmesh専用所有権体系を作らない。JSは共有Modelの明示load/close、個体生成/dispose、演技targetを指示する。JS定義から動的に作る既存経路も維持する。

この初版はbounded Gouraud面であり、全mesh engine・画像texture warp・skin・頂点expression morph・完全IK・一般Z遮蔽は未実装。既定狐assetの置換を意味しない。キャラクター3Dを1体ずつ進め、2.5D画像版は後続とする。

## 作者側と呼出側

offline `tools/ayatsuri/model/pack_model.cjs` の `compile(manifest)` に次のnodeを渡す。rest pose、材質11係数、名前・親などは従来と同じ。これはNode上の制作形式であり、実行時にこのobject graphを復元しない。

```js
{
  index: 1, name: 'face', parent: 0, kind: 'gouraudMesh', enabled: true,
  pose: [0,0,0, 0,0,0, 1,1,1],
  color: 0, thickness: 1, opacity: 1, edgeSoftness: 0,
  material: [.4,.6,.025,4,.08,1,.73,.44,0,0,0],
  mesh: {
    vertices: [-1,-1,0, 1,-1,0, 0,1,0],
    mask: [-1,-1,1], indices: [0,1,2], holes: [],
    order: 'centroid-depth', albedoA: 0xf800, albedoB: 0xffff,
    thickness: 175
  }
}
```

normalsは任意のF32 unit vector×頂点数。省略時は各face crossをsource順にF32保存で累積して正規化する。degenerate triangle、未定義normal、sparse/非有限/range違反は拒否する。albedoは既成RGB565 U16で保存し、従来のRGB888丸めへ再変換しない。maskはbody-local signed scalarで、光を動かしても毛色境界は動かない。同じsurface normalから2固有色をshadeする。

実行時は既存 `pocket.ayatsuri.modelBuiltin(name)`（組込みreadonly span）または`modelOpen(buffer)`（owned immutable copy）、`AyatsuriNative.scene({model,...})` を使う。mesh nodeの `place` / `visible` と親子TRSは既存target batchを共有する。meshへのCOLOR/OPACITY更新はnative batch全体を未適用で拒否し、黙って無視しない。JS scene handleもsetter段階で拒否し、先に積んだpose targetやready状態を汚染しない。`instance.nodeKind(id)`（C: `ayatsuri_model_instance_node_kind`）で0=group/1=ellipsoid/2=blend/3=meshを取得する。これはpacket kindとは別。owner/generation・alive/busy・index・out aliasを検証し、失敗時out不変。Model close後も生存Instanceはqueryでき、個体dispose後は拒否する。初版はopaque固定材質なのでcold共通opacity=1/edgeSoftness=0/thickness=1を要求する。実際の厚みはmesh descriptorのU8、固有色はdescriptorのA/Bであり、共通colorはmeshに使わない。

表情を既存の眼・瞼などの個体poseへ割り当てることはできるが、meshの頂点morphを実装したという意味ではない。今回の狐比較はneutralの配置・毛色・曲率に限定する。

## Cold ABI と互換性

AYNM v1 little-endian。sections1..8の既存bytes/意味を維持し、meshを持つ資産だけ以下のrequired sectionsを追加する。4つは一組で、未知required sectionを旧readerが拒否する。optional unknown sectionを勝手にmeshとして読むことはない。

| section | stride B | 内容 |
| --- | ---: | --- |
| 9 | 32 | mesh descriptor |
| 10 | 28 | XYZ F32×3、rest normal F32×3、mask F32 |
| 11 | 6 | local vertex ID U16×3 / triangle |
| 12 | 12 | 穴のmodel-local XYZ F32×3 |

section9はoffset0/2/4/6/8のU16でvertexStart/count、triangleStart/count、holeVertexStart。byte10は穴数0..2、11はorder 0 authored/1 centroid-depth、12/13は各穴点数3..8（非使用は0）、14/16はalbedo A/B U16、18は厚みU8、19..31はreserved zero。startは各section内のrow IDで、ポインタではない。

上限は3..32頂点、1..64面、穴0..2×3..8点。XYZ/mask/穴座標は有限±4096、normalはunit長二乗誤差≤1e-4。全cold rowを検証し、section長/stride/offset/countのoverflow-safe bounds、予約値、local index、重複index、3D縮退、親・material参照を検査する。Cはunaligned blobもbyte単位で読む。Model open失敗はoutを変更せず、JS load失敗は既存Model/表示へ影響しない。

cold node kind=3と、iteration packet kind=4は別値。既存packet kind=3はmasked blendであり変更しない。既存cloudのnode work6144 B/arena6600 Bと旧asset bytesを維持する。Model metadataのsection表は8→12 entriesで32 B増えるが、allocator会計やguest peakの増分をその値だけで説明しない。

## 評価と描画

Instanceは共通world/TRS84 B、既存Gouraud packet cache1040 B、projected XY8n B、viewDepth4n Bを持ち、mesh slotを8 B単位へ丸める。cold XYZ/normal/topology/materialを個体へ丸ごとコピーしない。30頂点ならslot1488 B、別途既存node stateが必要。arena自体は8-byte整列、各slotは4-byte整列にもなり得るため、doubleの直接cast配置はしない。

投影はF32係数とF32 multiply/add、深度はview空間。normalはworld linearのinverse-transposeで変換して正規化、SNORM16化は`floor(x+.5)`のtie規則を使う。JS `shade`とModelの数値shadeを同じpure-C headerへ集約した。F32保存順やRGB565丸めを変更する場合は、資産移行の比較とは分けて検証する。

[可視面選択](ayatsuri-visibility.md)を使い、source windingを保持する。negative scaleで表裏が変わってもindexを自動反転しない。可視面0は成功でiteratorがskipする。centroid-depth順は同mesh内のpainter heuristicであり、別packetや一般的な自己遮蔽を解決しない。Modelのnode順がpacket間の描画順となる。

穴は同じ最終変換で投影する。可視面がある場合、穴のstrict convex・非縮退・非自己交差を評価時に検査する。失敗はiteratorを無効化し、前にsnapshotされたcommitted frameは維持する。部分更新された内部workをpublish可能な前frameと呼ばない。submit/submitClippedの双方で既存Gouraud snapshotを使い、scene全体の提出失敗は既存markへrollbackする。mesh自身をellipsoid clip targetにすることはできない。

評価・更新・提出は同期、毎更新heap確保なし。現Modelは変更があると全nodeを再評価するため、局所dirty最適化を完成したとは扱わない。light変更はalbedo/maskを変えずshadeへ反映し、pose/camera変更でprojection/normal/selection/穴を再評価する。packetは次のmutation/evaluate/disposeまでのborrowed storageで、immutable snapshotは描画frame側が所有する。

## 寿命と試験境界

共有Modelはreadonly、各Instanceのarena・pose・light/camera・packetは独立。Model closeは新Instanceを拒否し、生存中の既存Instanceは保護する。最後の個体退役時にowned cold copyを回収、static資産はdetachしてflashに残す。保持されたdisposed scene/handleから不要なcold資産へ参照を残さない既存契約を維持する。提出済みframeはsnapshotなので、資産の解放に架空のframe待ちは追加しない。

core/real QuickJS試験はshared2個体、owner/generation、close/disposeの両順、retained tombstone、submit後disposeの画素維持、無効target batch、失敗rollback、OOM後の同runtime回復、no-op/通常反復の無確保を対象にする。m32 160 KiB、host ASan/UBSan、製品link、同head CI、実機の結果は区別する。

狐の5条件（移行のみ・眼のみ・毛色のみ・形状のみ・組合せ）はhost-only compile flag下で比較する。未採用5資産を通常製品へ無条件に常駐させない。明示的に選んだ1条件のmetadataだけをJSへloadし、鼻/眉/口/眼outline/耳clip補助に残るJSとnative workの費用も測る。採用前の資産を既定狐へ上書きしない。
