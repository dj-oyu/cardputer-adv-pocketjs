# 狐face固定資産

`fox_face.aynm` は既存AYNM v1の全25node資産。builtin名は `fox-face`。頭・眼球・虹彩・瞳・瞼・眉・鼻口吻・顎・耳を元の順に保持する。clipの頂点と更新制御はJS側に残し、新しいgeometry形式やsectionは追加しない。

初期値は25node版FoxFaceModelの `create()` → `update({})` 後のneutral状態。口（ID7）は非表示、position `[0,15,24]`、scale `[1,Float32(.15),1]`。ほかは表示。rest TRS・geometry・正規化材質は既存utilityと同じF32、thickness/opacity/edgeSoftnessはF64を保持する。

authoringの正本はoffline専用の `tools/ayatsuri/model/fixtures/fox_face_25.js`。実utilityで正規化し、既存 `pack_model.cjs` で符号化する。runtimeでJS定義graphを復元しない。

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

apertureのanchor→描画対象は `2→[2]`、`3→[3]`、`10→[10,11,12]`、`17→[17,18,19]`。exporterは照合用のclip情報も返すが、binaryへは保存しない。

資産は3148 B、SHA256 `3c296648c459ff0cc1b03849febfbd93f96e8cf64345ede2c28e8f7bde691888`。抽出元の保存byte SHA256は `0bb4e5e8c79f32df9f6a003483738ee9b3f0f506ff142a1b1662015f72650503`、LF正規化後は `ee4dfdddf14b88da8d97bf8c6758644de89fc4e7f343ff15372c926e19039ffa`。Git checkoutの改行差は生成byteへ影響しない。

native workは7440 B、Instance arenaは8040 B、geometric nodeは21（neutral表示20）。Model/Actor metadata・allocator整列・JS target配列・描画packetは別費用。構造値であり、guest peakや実機速度の測定結果ではない。旧7node＋JS19node方式は容量不足で未採用とし、製品asset/providerから外した。
