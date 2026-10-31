# 設定図から取り直した狐顔の無陰影実証

既存モデルの頬幅や頭の楕円体係数だけを調整する方法は打ち切り、設定図から輪郭・眼・鼻・白毛境界を取り直した。既定FOX FACE／FOX SHEETと実機023559fは変更していない。これは**オフラインのアセット制作比較**であり、native rendererの画素・順序・実機性能の受入ではない。

![固定3D形状の無陰影投影と顔シルエット](../apps/fox-redesign-unlit-host.png)

上段は固有色のみ、下段は耳を除く顔の輪郭。yaw −35/0/+35°、240×135を最近傍2倍表示した。照明・ハイライト・テクスチャ画像は使っていない。眼は配置を判断するための平たいマーカーであり、完成した眼球・瞼・表情rigではない。

## 参照と作り直した要素

参照はユーザー設定図 `fox-character-sheet-volume-muzzle-v2.png`（Library `libfile_6be7d8a574e88191a1b4c8bb83517684`）。画像SHA256は `3c1d6d81ca613d501277898fc2bcda220931694a67687e294b9c9a7dc02870a5`。FRONT／3/4／SIDEの手動近似点を [fox_reference_landmarks.json](../../tools/character/fox_reference_landmarks.json) に保存した。中心点は概ね±3 px、毛先・額境界は±5〜8 px程度の見取りであり、自動抽出や正確な多視点復元ではない。各絵には非対称と投影不整合がある。

| 耳を除く正面顔の比率 | 参照の手動近似 | 新規authoring |
| --- | ---: | ---: |
| 外頬幅／額面〜顎高 | 1.578 | 1.571 |
| 眼中心間距離／外頬幅 | 0.3256 | 0.3258 |
| 額面から鼻まで／顔高 | 0.7523 | 0.7524 |

右列は**作成したランドマークの比率**であり、レンダリングから再測定した誤差でも品質スコアでもない。顔幅は頭蓋球ではなく頬毛の外端、額は前髪の毛先と区別した。

新しい面は旧モデルの頂点やローブを流用していない。顔の断面幅と奥行きprofileから連続する74頂点128三角形を作り、鼻へ続く部分を同じ面で前へ出した。頬に独立球を追加せず、固有色境界は頬下から中央鼻下へ湾曲する別データとした。耳根は頭へ重なる位置へ接続した。

![手動参照輪郭・ランドマークとの比較](../apps/fox-redesign-landmarks-host.png)

赤は手動参照輪郭と眼・鼻・顎の点、水色は新しい3Dのランドマーク。正面と斜めで確認する。参照3/4の実yawは未校正であり、右図のyaw35°との点距離を正確な投影誤差や合格値にしない。

## 実装と検証した境界

[生成スクリプト](../../tools/character/fox_asset_redesign.py) は標準Pythonと既存Node packerだけを使用する。

```text
python tools/character/fox_asset_redesign.py
python tools/character/fox_asset_redesign.py --out-dir .cache/fox-asset-redesign-repeat
```

出力は `model.json`／`model.aynm`／`authoring.json`／2枚のPNG／`proof.json`。2回の生成で全6ファイルのbyte一致を確認した。既存AYNM packerで16 node、8960 Bの固定native資産へコンパイルできた。資産SHA256は `13e9eabafb3931edf376f3188e2c1823cd582945fbee2618d72ff271c636687d`。native各meshの32頂点／64面制限内で事前分割し、計算した法線と固有色データも固定資産に含める。

表示PNGは制作確認用の**オフラインdepth-buffer投影**であり、製品へZbufferを追加したものではない。AYNMがcompileできた事実から、製品の面順・眼clip・morph・メモリ予算が通ったとは言わない。製品providerへは登録せず、JS appへの巨大な頂点graph展開も行っていない。後続は固定native資産を共有し、pose／表情を個体別mutable instanceに持たせる既存方針を維持する。

## 画像判定と次の受入

rootと独立画像レビューは、旧円頭・白頬球・円筒状の独立口吻から離れ、**同じ形状を固定して陰影実証へ進める土台**と判断した。製品造形の合格ではない。

- 輪郭：横の頬端と短い鼻下は設定図の方向へ進む。ただし頭頂が尖った釣鐘状に見える。
- 眼：参照を基準に配置を作り直したが、現マーカーの大きさ・瞼・眼窩への収まりは未受入。
- 口吻：斜めで鼻先が前へ出る。丸い量感が出るかは同geometryのclay／固定光で判定する。
- 白毛：水平な帯から曲線へ変えたが、斜めでは横長の扇にも見える。陰影で隠して合格にしない。

次は同じgeometryの単色clayと固有色表示、固定世界光のyaw −35/0/+35°を比較する。その後にnative rendererの面順・眼clip・差分一致・160 KiB・明示解放を検証し、通った面だけをmorphへ進める。現時点ではmorph、実機RAM・速度・画質、全身・他キャラ・2.5Dの完成は主張しない。

[既存頬造形の比較](fox-cheek-authoring-study.md) · [native mesh契約](../api/ayatsuri-native-mesh.md)
