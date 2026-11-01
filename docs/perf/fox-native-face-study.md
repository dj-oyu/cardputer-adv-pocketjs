# 狐3Dの顔・native資産・凸clipのhost検証

状態: FOX FACEの最小縦切り。全身や自然な演技の完成、実機画質・速度の受入ではない。診断実機候補`fde49fb`は固定し、この変更は通常版のTRACE既定をOFFへ戻す。

## 比較の条件

同じ25nodeの顔を、JS定義から作る方式とAYNM native共有資産から作る方式で比較した。固定assetと個体poseを分け、JSは表情・配置・開口を指示する。[API契約](../api/ayatsuri-clip.md)、[操作](../../apps/fox3d/README.md)、[資産内訳](../../apps/fox3d/assets/README.md)を参照。

画像は240×135の**HOST SIMULATION**。cameraは位置(0,0,400)、target(0,0,0)、zoom1.3、center(120,78)。世界光は(-.5,-.4,.8)。neutral/smile/surprise/jawOpen×yaw -35/0/+35度の12枚に、pitch±10度、連続yaw、固定姿勢での光移動、停止、resetを加え、231frameを2runtimeで試す。各frameの差分描画を全描画referenceと比較し、停止時の画素一致・無効入力不変・終了時guest解放も検査する。

64bit ASan/UBSan試験は容量制限なしの安全性・画像対照。32bit試験だけが163840B制限で、SSE2/F32 profile、通常GC、frameごとの強制GCなし。host時間をESP32時間やFPSへ変換しない。

## 容量の試行と採用判断

以下はguest allocatorへの**要求byte**のhighwaterと最大単一要求。ESPのusable/free/largestや断片化ではない。失敗行は起動/生成の途中までのpeakなので、成功版との削減率は計算しない。

| 構成 | requested peak B | 最大単一要求 B | 160KiBでの結果 |
| --- | ---: | ---: | --- |
| JS固定定義25node | 146952 | 8496 | quota拒否6、frame開始前に失敗 |
| native7node＋JS19node | 147056 | 8496 | quota拒否7、起動失敗 |
| 同構成を遅延生成 | 146912 | 8496 | entry評価成功、初frameの眼生成でquota拒否9 |
| native25node、目の最終調整前 | 126616 | 8047 | 231frame×2、quota/allocator拒否0、終了guest0 |
| native25node、最終の丸い開口 | 126992 | 8047 | 231frame×2、quota/allocator拒否0、終了guest0 |

allocator自身の拒否は各失敗構成でも0。物理heap不足という意味ではない。全25nodeをnative資産へ移し、SourceA/blend JS moduleと固定定義graphの復元を省いた方式を採用した。native Instanceも同じguest会計capへ課金される。

AYNM本体は3148B、SHA256 `3c296648c459ff0cc1b03849febfbd93f96e8cf64345ede2c28e8f7bde691888`。4group/18ellipsoid/3blendの25node、個体raw slot7440B、arena8040B（整列余裕を含む要求8047B）。assetはflash側共有、arenaは個体別可変RAM。JS wrapper・clip buffer・描画candidate/committed snapshotは別に残る。全部をゼロコピーにした方式ではない。

## 画像と境界

目の最終調整前には、全25node native版と元JS25node版の12枚が全画素一致した。最終調整は意図的な画質差で、開眼を尖った菱形から8点の丸い開口へ変更し、開眼時の硬い屋根状の瞼線を消した。全smile時の瞼端点と、瞼以外のrig制御は対照と一致させる。これ以後の画像を旧版との全画素一致とは呼ばない。

同じ条件の[調整前12枚](../apps/fox-face-host-before.png)と[調整後12枚](../apps/fox-face-host.png)を保存した。いずれもnative host描画を2倍に拡大した画像で、実機captureではない。最終版は32bitで要求peak126992B/単一8047B、64bit ASan/UBSanでは要求peak176992B/単一10632B（capなし）。両profileとも231frame×2、差分/全描画、反復画像一致、解放guest0。64bit量を実機予算へ転用しない。

nativeの`readProjection`は開口ごと32B同期copy、`submitClipped`は固定typed tableを同期snapshotする。bound nodeは可視ellipsoidに限定し、閉眼はEMPTYで扱う。一般Z bufferや頭・頬との自動遮蔽は追加していない。

低層のASan/UBSan・32bit検査はEMPTY/両winding/凹・自己交差・縮退拒否、入力不正の不変性、snapshot、damageと転送失敗repairを含む。旧shaderの64frame/2073600画素も一致した。native Instanceの1000回warm method呼出はguest alloc0（recorder経路）。これを実renderer込みの全frame alloc0や実機高速化の証明にはしない。

## 再実行と証拠

```sh
node tools/character/test_ayatsuri_clip.cjs
node tools/ayatsuri/model/test_fox_face.cjs
python3 tools/kasane_contract/run_fox_app_host.py --m32 --capture-dir .cache/fox-capture-final
python3 tools/kasane_contract/run_fox_app_host.py --capture-dir .cache/fox-capture-final-asan
```

結果は`.cache/fox-app-{m32,asan}/result.json`。各runの12画像を独立したfolderへ出し、P6の寸法/長さと反復hash一致を検査する。capture省略時の画像反復比較はN/A。compile/chunk/asset依存hashを試験前後で照合し、古いPASSを開始時に消す。固定資産の生成は`export_fox_face.cjs`、lowclipの検査は`run_proc_ellipsoid.py`と`run_pocket_proc_qjs.py --clip-only`、共有資産寿命は`run_model_contracts.py`が担当する。

## 未達

設定図に対してまだ顔だけの造形試験であり、頬毛の柔らかい輪郭、全身、胸毛・大尾、自然な表情遷移と身体演技は未完。新clipは解析曲面の切抜きで、汎用mesh/UV/skin、一般遮蔽、追加AAの完成ではない。実機の画質、frame分布、peak/free/largest、stackはこの候補では未測定。host PASSと別に受け入れる。
