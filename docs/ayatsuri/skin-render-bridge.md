# 合成skinと既存mesh rendererのC接続

[skin scalar core](skin-core.md) の結果を、immutableなAYNM v1 Modelの
projection/shade/visibilityと既存opaque mesh rendererへ渡す最小のC接続である。
このC sliceは新しいwire section、skinのJS API、appの自動ロードを追加しない。
後続の[managed JS skin評価](skin-js.md)がnative skin計算とこのC接続を呼ぶ。狐資産・実機・
固定小数点/PIE作業は対象外。従来のrigid evaluate/submit経路を維持する。

## APIと座標系

`main/ayatsuri/ayatsuri_model_skin.h` の
`ayatsuri_model_instance_evaluate_meshes(instance, owner, generation, camera, light, meshes, count)`
は、`{node_id, vertex_count, vertices}` の配列を同期借用する。
`vertices` は `ayatsuri_skin_result` の配列であり、callerが事前に
`ayatsuri_skin_palette` と `ayatsuri_skin_evaluate` で作る。

変形後position/normalは**対象mesh nodeのlocal空間**で渡す。Modelの親子TRSは
この後一度だけ適用される。world空間で作ったjoint paletteをそのまま渡して
node変換を二重適用しない。world jointを使うcallerにはmesh-localへの変換が必要。
骨の探索、bind生成、weightのwire保存、motionとの自動接続はこのAPIの責務ではない。

- 元のvertex数と完全一致する置換だけを許可する。index、mask、材質はcold tableを使う。
  部分vertex更新・topology変更・追加geometryはできない。
- 今回の置換対象は**片面・holeなしmesh**に限定する。両面meshとhole付きmeshは
  明示的に拒否する。skin後の面の側判定やclip polygonの変形が未定義のまま、
  rigidなholeやownerのdeterminantだけを流用しないためである。
  同一Model内の置換しない両面/解析nodeは従来どおり評価する。
- positionはfiniteかつ各成分±4096以内、normalはfiniteかつ二乗長が1から1e-4以内。
  これはcold meshと同じ入力範囲である。重複node、group/nonmesh、数違い、null、
  不整列、instance/arenaと重複するspanを拒否する。確保容量とpointerの有効性、
  呼出し中の入力不変性はC callerの責任。
- 原本AYNM、共有Model、instance arenaのサイズは変えない。新しい永続geometry領域は
  確保しない。C instanceに内部cache flagを1個足す。今回の64bit hostでは既存paddingに
  収まり `sizeof(instance)=168 B` のままだが、target ABIへの外挿はしない。

```c
ayatsuri_model_mesh_geometry geometry = {mesh_id, vertex_count, skin_results};
/* Joint globals/inverse bind and skin results belong to the caller. */
if (ayatsuri_skin_palette(globals, inverse_bind, joint_count, palette) == AYATSURI_OK &&
    ayatsuri_skin_evaluate(palette, joint_count, weighted_vertices,
                          vertex_count, skin_results) == AYATSURI_OK) {
    status = ayatsuri_model_instance_evaluate_meshes(
        &instance, owner, generation, camera, light, &geometry, 1);
}
/* On success, copy ordinary mesh views into the caller's frame snapshot. */
```

## ready、失敗、所有権

全置換入力を評価前に検証する。不正geometryは既存packet・ready・statsを変えない。
評価開始後のcamera、transform、projection等の数値失敗は、従来evaluateと同じく
instance全体をnot-readyにする。部分成功のpacketをview/nextで読めない。
正常な再評価で復帰する。busy、owner、generation、disposeの検査も従来と共通。

nonempty batchは同じpointerでも毎回再評価する。外部bufferの変更をpointerだけで
cacheしない。overrideを省略したnodeにはcold geometryを使い、空batchは通常evaluateと
同じである。直前がoverride評価なら、通常evaluateも同じcamera/lightでcold geometryを
再構築する。その後の通常evaluateには既存cacheが効く。

借用geometryと記述子のpointerを保存しない。戻ったらcallerはskin workを再利用・
解放できる。返されるmesh view/packetはinstance arena内の現在値であり、次の評価や
破棄までの借用である。`ksn_mesh_append` がコピーしたsnapshotは別ownerであり、
geometry、instance arena、Model、cold blobをすべて解放した後も描画できる。

snapshotの容量/work超過ではそのappendが失敗する。複数nodeのbatch submit、
rollback、commit、transport retry、提示ACKは既存caller/runtimeの責務で、
この評価APIが暗黙に代行しない。反転した片面skinのwindingは補正せず、既存の
背面除去を適用する。skin normalの近似とLBSの制約はscalar core文書のままである。

## host受入と会計

```sh
python tools/ayatsuri/skin/run_render.py
python tools/ayatsuri/skin/run_render.py --opt 0
python tools/ayatsuri/skin/run_render.py --sanitize
```

`build_skin_render/` に、公開の6vertex stripと4vertex背景quadのAYNMを一度だけ生成する。
各frameでModelを生成し直さない。holes/sidedの追加fixtureは拒否検査だけに使う。
SDK・QuickJSを必要としないC harnessで、製品のModel evaluator、compact snapshot codec、
`ksn_mesh_append` と `ksn_mesh_render` を実際にリンクする。

120姿勢の往復240frame、各pause再評価、4/8行band × 1/8行depth scratchの4条件を検査。
別に組み立てたprojection/SNORM/shading packetとsnapshot bytes、RGB、depthを比較する。
band描画のRGBを保持したまま、別bufferへchunkを再描画し、RGB一致と全depth行を検査する。
skin scalarの数式は別のcore検査で独立比較し、ここではshader/rasterを共有する。
独立したrendererの正解画像や実モデルの見た目の受入ではない。各frameには十分な
前面色/背景色があり、restとは異なる画像が出ることも検査する。

非一様scaleを持つ親Rzと子Ryの合成について、行列builderを共有しない逐次S/R/T・
inverse-normal計算と位置・depth・照明を照合する。2nodeの同時置換、空batchでの解除、
skin後のtargets更新と通常evaluateによるcold復帰も検査する。

immutable cold bytes、独立instance、skin法線による照明変化、片面反転のcull、
不正batchの原子性、projection失敗後のview拒否と復帰、capacity/work失敗後の新規snapshot
再試行、全source解放後の描画とRGB/depth一致を検査する。transportやJS managed objectの
寿命検査は既存scene/motion試験の別範囲であり、このC試験から対応済みとしない。

64bit hostの実測:

- cold 984 B、instance arena各2532 B、C instance各168 B。arenaは既存の同fixtureと同じ。
- skin input 240 B、result 144 B、global/inverse bind/paletteの2joint×3組で288 B。
- snapshot各capacity 2048 Bに対し最大使用212 B。depth scratch 8行は7680 B。
- warm範囲（skin/評価/append/render/照合）のlinked malloc/calloc/realloc要求0回。
  2 instanceと比較用snapshot等を含むrequested live 19480 B、拒否検査も含むpeak 25616 B、
  最終live 0 B。部分費用はこのrequested合計の内数であり足し合わせない。
- static比較画像/depth配列、ledger、stack、allocator overhead、libc/sanitizer内部は別費用。
  S3メモリ適合やstack watermark、cycles/FPS、表示品質の実機値ではない。

このcloudではptraceによりLeakSanitizerが起動しないため、
`ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1` を付けてASan/UBSanを走らせる。
最終requested live=0の検査とLeakSanitizer成功は区別する。

## 未完の工程

managed JS bindingは[別slice](skin-js.md)で1meshの同期評価まで追加した。
AYNM skin tableのwire契約、asset importer、
armature/bind/rest対応、morph/表情、skin対応両面/holes、surface法線再計算、
実キャラクター姿勢/遮蔽/ヒープ/速度、実機表示は未実装または未検証。
このsliceは合成C描画接続の受入までであり、狐3Dの完成を意味しない。
