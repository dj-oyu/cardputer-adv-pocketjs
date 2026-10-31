# 狐face固定資産

`fox_face.aynm` は既存AYNM v1の全29node資産。builtin名は `fox-face`。頭・眼球・虹彩・瞳・瞼・眉・鼻口吻・顎・耳を旧25nodeのIDを保ち、内耳・耳毛の4nodeを末尾へ追加する。clipの頂点と更新制御はJS側に残し、新しいgeometry形式やsectionは追加しない。

初期値はv4 authoringのFoxFaceModelの `create()` → `update({})` 後のneutral状態。口（ID7）は非表示、position `[0,14,22]`、scale `[1,Float32(.15),1]`。ほかは表示。rest TRS・geometry・正規化材質は既存utilityと同じF32、thickness/opacity/edgeSoftnessはF64を保持する。

authoringの正本はoffline専用の `tools/ayatsuri/model/fixtures/fox_face_v4.js`。実utilityで正規化し、既存 `pack_model.cjs` で符号化する。runtimeでJS定義graphを復元しない。旧 `fox_face_25.js` と `fox_face_v2.js`、`fox_face_v3.js` は変更せず比較用に保持する。

```sh
node tools/ayatsuri/model/export_fox_face.cjs --write
node tools/ayatsuri/model/test_fox_face.cjs
```

| ID | 名前 |
| --- | --- |
| 0 | fox-head |
| 1 | skull |
| 2, 3 | ear-outer-1, ear-outer1 |
| 4 | jaw-hinge |
| 5 | lower-jaw |
| 6 | cream-face |
| 7 | oral-volume |
| 8 | nose-bridge |
| 9 | eye-socket-1 |
| 10, 11, 12 | sclera-1, iris-1, pupil-1 |
| 13, 14, 15 | lid-arc-left-1, lid-arc-right-1, brow-1 |
| 16 | eye-socket1 |
| 17, 18, 19 | sclera1, iris1, pupil1 |
| 20, 21, 22 | lid-arc-left1, lid-arc-right1, brow1 |
| 23, 24 | lip, nose |
| 25, 26 | ear-inner-1, ear-tuft-1（parent 2） |
| 27, 28 | ear-inner1, ear-tuft1（parent 3） |

apertureのanchor→描画対象は `2→[2]`、`3→[3]`、`10→[10,11,12]`、`17→[17,18,19]`、`25→[25]`、`26→[26]`、`27→[27]`、`28→[28]`。exporterは照合用のclip情報も返すが、binaryへは保存しない。

資産は3620 B、SHA256 `923d5fa28889cf1614b3ac3dcb9410b7b28e018e5d9f6afabf64c4b251dc7149`。v4抽出元のLF正規化SHA256は `ac26c0ebaa8a03dc1c1764b003d341dcfd9b7439bd810ba054f5914763a9d17b`。Git checkoutの改行差は生成byteへ影響しない。旧25node資産の3148 B・SHA256 `3c296648c459ff0cc1b03849febfbd93f96e8cf64345ede2c28e8f7bde691888` は前候補の記録であり、現資産と混同しない。

native workは8496 B、Instance arenaは9192 B、geometric nodeは25（neutral表示24）。Model/Actor metadata・allocator整列・JS target配列・描画packetは別費用。構造値であり、guest peakや実機速度の測定結果ではない。旧7node＋JS19node方式は容量不足で未採用とし、製品asset/providerから外した。

眼球の奥行きを1.1・高さを8.5へ薄くし眼窩を(±11.5,-4.5,11)へ収め、虹彩の幅を5.8へ広げてlocal Zを1.0、瞳のlocal Zを1.5へ収めた。鼻筋の独立した球を細長い低いbridgeへ変更した。白い頬は中心口吻から左右の細い端へ連続する8lobeの同一材質field。耳は幅広いouterに、桃色の薄いinnerとcreamのroot tuftを重ねる。内耳のtransmission .64 / thickness .12は既存光源評価による様式化近似で、時刻による明滅ではない。edgeSoftness .08は解析曲面の縁の設定であり、凸clip境界へ追加AAを実装した意味ではない。耳の後描きが頭へ漏れないclip域・前後関係、最終画像と背面光の品質受入はruntime側の比較で確認する。

同条件のA/B描画比較から選択したv4 A3案は、低く薄い白頬とcompact taper、口内を支える連続した奥行きの下顎と口内半径X6.5。lip半径Xは6.8・rest Yは12.8、口内restは(0,14,22)へ揃える。29node/13lobeの配置とメモリ構造はv3から増やしていない。画像上の前後関係・口の接合は別に受け入れる。
