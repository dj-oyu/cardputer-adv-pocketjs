# CLOUD ACT

`local.cloud`は、雲1体の「気づく → 喜ぶ → 落ち着く」を造形・材質・動きまで縦に検証するapp。既存3体CHARACTER LABを置き換えずメニュー末尾へ追加する。hone/ayatsuriの実装を複製せず、同じutilityと任意のblend moduleを使う。

## 今回の判断基準

ユーザーが既存labの造形/挙動を理想の5%程度と評価したため、診断機構の追加を止め、一体の見た目を優先した。[設定Page](https://chatgpt.com/space/page_0697a7f87a94819197f18a8b6d0210a3)の雲設定図を実画素で参照（Library ID `libfile_70115c5b39c88191aec74aaedd9079a2`、PNG SHA256 `15708e96c1a404974bf818bc1f5a1831bbca422450e40183dedca3bd6b86bbb8`）。図は目標で、動的照明の証明ではない。

以前は独立した卵形の境界陰影、前面lobeに埋まる小さな顔、見えにくい彩雲、rootの一様な往復変形が主だった。雲は主volumeと周縁の大きさ/深度を設計し直し、前面法線とalpha合成を共有できるため、最初の改善対象に選んだ。

- シルエット/厚み: 大きな梨形を主形にし、周縁の非対称lobeを奥へ置く。顔の前を別の球で覆わない。
- 顔: 大きな瞳、実際の照明へ応答する瞳のツヤ、眉、広がる口/舌/頬。表情と瞬き、視線を別入力で合成し、顔と手を胴体の伸縮から独立させる。
- 材質: 高明度白、青い環境光、世界の直接光と透過近似を分離。固定した照明textureや時間brightnessで代用しない。
- 手/彩雲: 手を身体から離し、彩雲も独立したworld配置にして空隙を保つ。中心と端で異なる変形を与える。
- 動き: 視線先行、予備squash、非対称の傾き/重心移動、手と彩雲の遅れ、上昇/open mouth、減衰settle。呼吸OFFでは8秒以後に完全停止し、seek順に依存しない。

8個までのlobeを使う[汎用前面ブレンド](../../docs/api/blended-surfaces.md)は雲専用rendererではない。薄い縁や複雑な内部密度、体積散乱、設定図の絵のような細かなfluffまでは再現していない。改善前後の画像を比較して調整中で、目標品質の受入済みとはしない。

[初版の改善前後と301sample動作画像、検証記録](../../docs/perf/cloud-quality-v1.md)を参照。どちらもHOST SIMULATIONであり、実機captureではない。

## 操作と検証

左右でBODY/LIGHT/CAMERA/TEXTURE/EXPRESSION/STILL、上でtick reset、下で呼吸ON/OFF。1体だけなのでEnterは同じmodel。frame時計は`tick*1000/30`の論理時刻で、実時間30fpsではない。BODYでは光/カメラ固定。LIGHT/CAMERAはその原因だけを変え、同じ静止形で照明/視点応答を分離する。

```sh
# 固定入力4枚（HOST SIMULATION）
python3 tools/kasane_contract/run_character_app_host.py --cloud --capture-dir .cache/cloud-capture
# 1体×6modeの1806frame、通常再生のhost資源門
CHARACTER_HEAP_LIMIT=163840 CHARACTER_NO_EAGER_GC=1 CHARACTER_FULL_REFERENCE=1 python3 tools/kasane_contract/run_character_app_host.py --cloud --m32
```

診断seek/pause/stepは既存labと同じ明示ロード入口。captureとapp_sessionの正式API同時利用は未対応のままで、host画像を実機LCD画像と混同しない。実機候補は同head CI/製品リンク/hostを確認したSHAで引き渡す。今回の新しい実機書換えは未実施。

[顔比率保持と彩雲の次段改善・3512529との比較](../../docs/perf/cloud-quality-v2.md)も参照。

[cap境界の陰影補正・柔らかな量感のローカル比較](../../docs/perf/cloud-quality-v3.md)では、32a3336を基準にnative修正と材質調整を分離して評価する。公開候補のCI/実機結果とは区別する。
