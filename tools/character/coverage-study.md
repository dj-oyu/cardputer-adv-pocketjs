# 点描・漫画トーン・ハッチングの小比較（hostのみ、未採用）

ユーザーの追加ヒントを受けた比較であり、連続陰影を全面的に置き換える決定ではない。`coverage_study.cjs`はruntime/nativeを変更せず、制御したscalar coverageと、任意の実native captureを材料に比較画像とJSONを作る。

```text
node tools/character/coverage_study.cjs
node tools/character/coverage_study.cjs --input .cache/character-native/frames/app_3612.ppm
```

出力先の既定は`.cache/character-coverage-study/`。`index.html`に比較を並べる。列順は連続blend、分散点描、集中型comic tone、単方向hatch、crosshatch。別の出力先は`--out <directory>`。

## どの入力に属する柄か

固有色/模様はbody UVへ付くalbedo。狐の白い尾先は光を逆向きにしても尾先の模様として残る。一方、この比較のcoverageは材質・形・世界光・視点の評価を受けた**後**に、連続値を画素の占有率や二つの色の配置で近似する手法。固定した点柄の濃淡で陰影を焼き込む方法とは異なる。

二つの用途を区別する。

- **縁のcoverage**: 既に求めたedge alphaを点の存在率へ写し、点が存在する位置では計算済み材質色を使う。固有色を壊さず、雲の縁を粒に溶かす候補になる。ただし低密度の1px点は240×135では硬く、雲の滑らかさより印刷物の印象へ寄る可能性がある。
- **陰影tone**: 計算後の明暗を二つの材質色等の面積比で表す。元のalbedo色を白黒へ一律変換するフィルターは、狐の模様や人の肌/服の意味を失うのでproduction案にしない。明部/暗部の二色を何にするか、鏡面/透過項をどの段に通すかを別に設計する必要がある。

hostの制御比較は仮想のpost-light scalar fieldを青い背景と白いcloud色の占有率に写したもの。設定図を実装した新しい雲モデルでも、native shaderの証明でもない。`--input`は合成済みRGB画像のluminanceだけを比較する説明図であり、実際のalpha・albedo・法線や光の内訳は復元できない。capture入力版をproduction shadingの再現と呼ばない。

## 手法と位相

| 手法 | 今回のhost試作 | 主な観察点 |
| --- | --- | --- |
| 分散点描 | 8×8の固定Bayer順位 | 細かい平均tone、孤立1px、明暗変化時の粒の出入り |
| comic tone | 8×8の中心距離順位 | 点が塊として育ち印刷調になる。輪郭の硬さ、周期の目立ち |
| hatch | 斜め線への距離順位、同順位は固定Bayer | 線幅と間隔、物体曲率との不一致、輪郭付近の切れ |
| crosshatch | 二方向の線までの小さい距離順位 | 暗部の密度、交点の重さ、細部や目との競合 |

全て同じ64段の占有順位を使い、uniformな8×8 tileでは各level/64の平均占有率を正確に保つ。これが知覚的に同じ明るさを保証するわけではない。

位相は固定値[3,5]で、毎frame乱数は使わない。screen固定では網目がLCDの格子へ安定して、物体がその下を通る印象になる。body固定では模様が体に追従するが、投影・伸縮で周期が変わりサブピクセルの切替、moiré、表面へ貼り付いた印象が出やすい。どちらも「光の結果の濃度」自体は毎回更新し、phaseの選択と陰影を固定することを混同しない。

hatchの方向をbody/world/画面のどれに固定するかも独立の美術判断。体の曲率に沿うhatchはUVや接線方向が必要で、今回の画面斜線だけでは検証していない。

## 実装候補と測定境界

採るならnative analytic rendererの既存per-pixelループでcoverageを選ぶ案が自然。JSからdotを一本ずつdrawして線分数/呼出回数を増やす案ではない。固有模様のpattern IDとcoverage style/phaseは別の入力にし、albedo patternを流用して意味を混ぜない。既定の連続blendを残し、明示した比較モードから試す。

副作用はpixel単位のしきい値操作と追加parameter。frameサイズ、ラスタ予算、帯signature/damage、opcode/packet互換、copy量をnative実装後に調べる。今回のhost実行時間からdeviceのfpsや費用を予測しない。

## 今回の独立検査と残る検査

実行した検査は、同入力/同phaseの再現、periodicity、uniform tileの平均tone/単調性、平行移動32step（0.25pxずつ）と圧縮伸長32step（1→1.2）のraw変更画素数、停止後の変更0。`report.json`へ条件を残す。

raw変更数は正当な運動・coverage変化・resamplingも含む。二値法の変更数が連続blendより小さいことを「ちらつきが少ない」と結論しない。body phaseの変更数が大きくても全てが不快なshimmerとは限らない。知覚的なちらつき/moireのPASS判定はまだしていない。

追加検査は、実nativeでbody-only/light-only/camera-onlyを同じ時計でcaptureし、停止2秒の全frame一致、光を滑らかに動かす濃度の段差、姿勢/scale変化での縞のうなり、目・鼻・輪郭の可読性を連続frameと実LCDで確認すること。screen/body phaseの比較は同一の材質・光・coverage値を入力して行う。production採用はこの比較とユーザーの美術判断の後でよい。
