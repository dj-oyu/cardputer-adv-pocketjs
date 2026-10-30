# 狐の顔v2: 眼窩・口吻・内耳

状態: 顔の小さな造形反復。全身、動的演技、実機画質の完成ではない。比較基準は`b83327b2385ad6fc7fad598a8a5d0783f280999d`。[前段の容量試行とclip契約](fox-native-face-study.md)を保持し、診断実機候補`fde49fb`とは別候補にする。

## 同じ条件で変えたもの

camera/世界光/4表情/yawは前段と同じ。camera位置(0,0,400)、target0、zoom1.3、center(120,78)、世界光(-.5,-.4,.8)。画像は240×135の**HOST SIMULATION**を2倍に拡大したもので、実機captureやFPSではない。

- 眼窩を(±11.5,-4.5,11)へ、眼球のZ半径を3→1.5へ変更。虹彩・瞳も薄くし、共有の3D開口と同frameで評価する。開口の上部は瞼で覆い、開眼の縁は薄いfur色、笑顔の縁は暗色にする。
- 作者が指定したカメラ相対`viewYaw`に対し、遠側の開口幅を`1-.4*max(0,side*sin(viewYaw))`で連続補正する。左右で同時に眼を消さず、閉眼だけをEMPTYにする。これは一般の頭部depth遮蔽を解決した意味ではない。
- creamの連続8lobeを低く細くし、中心口吻から外側へ細まる形へ。鼻筋の独立した橙の球を薄いbridgeへ、小さい鼻へ変更。口内は唇・顎に近いZ=22へ寄せ、開口時の位置・傾き・高さを連動させる。
- 耳を幅のある5点輪郭へ変更し、桃色の薄い内耳とcreamの根元毛を左右各1個追加。内耳は後描きが額へ広く漏れないclip域に制限する。根元毛のforeground重なりは作者による指定で、汎用Z bufferを追加しない。

## 資産・所有権・費用

AYNM v1のまま、元25nodeのIDを保ち内耳・耳毛4nodeを追加した29node。[資産正本](../../apps/fox3d/assets/README.md)はoffline専用`fox_face_v2.js`で、元`fox_face_25.js`は比較用に保持する。binからJS定義graphへ戻さない。Modelは固定flash byte列を共有し、Instance・target・開口は個体所有。dispose/共有中保護/失敗時snapshotの契約は変更しない。

| 項目 | 前段25node | 顔v2・29node |
| --- | ---: | ---: |
| bin B | 3148 | 3620 |
| 個体raw slot B | 7440 | 8496 |
| 個体arena B | 8040 | 9192 |
| 開口/clip対象node数 | 4/8 | 8/12 |

これらは構造値。guest peak、ESP free/largest、stackとは別。native arenaも同じguest会計capへ課金する。新しい毎frameの配列作成や形状graph再生成は加えず、材質色patchは再利用して色が変わる時だけ適用する。全frame alloc0や実機高速化は未測定であり主張しない。

## host受入

通常4表情×yawの12枚に、4表情×pitch±10°×yawの24枚、neutral×世界光±150°×yawの6枚を加える。背面光以外は比較の基準光を維持する。その後のyaw sweep、光のみの移動、停止、resetを含め237frameを2runtimeで実行する。42枚のcaptureはrunごとに独立したfolderへ出し、寸法・byte長・反復hashを検査。全frameで差分/全描画referenceを照合する。

```sh
node tools/character/test_ayatsuri_clip.cjs
node tools/ayatsuri/model/test_fox_face.cjs
python3 tools/kasane_contract/run_fox_app_host.py --m32 --capture-dir .cache/fox-v2-final
python3 tools/kasane_contract/run_fox_app_host.py --capture-dir .cache/fox-v2-final-asan
```

32bitは163840B制限、通常GCでframeごとの強制GCなし。64bit ASan/UBSanは容量上限なし。最大要求はguestへのrequested byteで、usable/free/largestや断片化と呼ばない。更新中の一時値、定常値、解放後の値を混同しない。

最終の32bit hostは237frame×2 PASS、要求peak140120B、最大単一要求9199B、quota/allocator拒否0、終了guest0。42枚の反復hashと全frameの差分/全描画は一致。前段25nodeの要求peak126992B/単一8047Bと比べて費用は増えている。表現追加とdriverの費用を含む比較であり、高速化や低メモリ化とは呼ばない。

64bit ASan/UBSanも237frame×2 PASS、要求peak199124B/単一10632B、拒否0、終了guest0。両profileともsource依存hashの試験前後不変を確認した。これはcapなしの64bit安全性試験で、device容量やESP heapの実測ではない。

[基準25node](../apps/fox-face-host.png)、[v2の同条件12枚](../apps/fox-face-v2-host.png)、[v2のpitch/背面光12枚](../apps/fox-face-v2-pitch-backlight.png)を保存した。残りの表情pitch captureを含む全42枚とsource hashはhost試験の結果folderへ出す。CIの指定済みgenerated artifact pathではhidden `.cache`も保存対象にし、前段の未アップロードを解消する。

## 視覚判断と未達

初期の独立画像レビューでは、耳の識別性、眼の突出と遠側白目、頬の厚さの改善を確認した。yaw±35°・掲載neutralのpitch±10°では明らかな漏れ・反転・部品消失なし。背面光では頭部が暗くなる一方、耳の暖色が残る。ただし、物理透過の正しさや実機LCDの品質を保証する試験ではない。

耳はまだ板状、根元毛は塊状、眼と縁には貼り付け感、下頬・顎には皿状の形が残る。斜めの口内接合を確認してから、次は眼を包む頬面と口角を優先する。細粒を増やすだけで柔らかさを達成したとは扱わない。胸毛・尾は今回の顔実証外。

口内をZ24→22へ寄せ、開口時の移動と縦scaleを抑えて傾けた最終画像では、独立した暗い楕円の前方突出が減ったと独立レビューで確認した。ただしyaw±35°の口端には平たい貼付感が残り、口腔・唇の立体的な接合の完成とはしない。掲載最終contactのSHA256は`36045737683fd54ee5d1333f18bd6df3e51a58712a953b7972144ae868e4be97`。
