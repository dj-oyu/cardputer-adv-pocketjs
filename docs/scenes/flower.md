# FLOWER 背景

ホーム画面の第3の背景。関数曲面から実行時に生成した花を、森・光・雨とともに描く。頂点座標列やテクスチャは埋め込まない。少数の寸法から楕円体と開口した円錐帯を実行時に生成し、光線との交点を解析的に求める。植物学的に厳密な再現ではなく、240×135という小画面で特徴が伝わる関数アートを狙う。

形状生成は `main/scene/flower_species.c`、描画は `main/scene/flower.c`、共有部品型と上限は `main/scene/flower_parts.h`（`MAX_PARTS=56`）。森・光・雨は `main/scene/garden.c`。描画順は森林背景 → 花 → 雨滴 → 既存XMB。

PIEカーネル化の可否・コストの一般則は [pie-simd.md](../perf/pie-simd.md) を参照（FLOWER固有の実測は同ドキュメント §3（このコア上のスカラーコード、特に§3.6 bell_hitの除算巻き上げ・§3.7 ifloor/iceil）と §4.6・§7（装飾光線の実測）に集約してある）。この文書は「何を描いているか」「数字をどう動かすと何が起きるか」だけを書く。

## 種一覧

`FLOWER_SPECIES_COUNT` は14種（`main/scene/flower.h`）。40秒ごとに切り替え、1.2秒のディゾルブでつなぐ（`FLOWER_ROTATE_S` / `FLOWER_FADE_S`）。

| 種 | 表現 | 解析曲面部品数 |
| --- | --- | --- |
| すずらん (valley) | 曲がる茎、開口した釣鐘、広葉 | 31 |
| ヒマワリ (sunflower) | 放射状の花びら、黄金角の中心模様 | 36 |
| スノードロップ (snowdrop) | 下向きの3枚の外花被、緑の内側 | 20 |
| チューリップ (tulip) | 6枚の花被によるカップ、幅広い葉 | 15 |
| スイセン (daffodil) | 6枚の外花被、細い首から広がる副花冠と奥の花喉 | 19 |
| クロッカス (crocus) | 紫のカップ状の花、黄色い雄しべ、細葉 | 36 |
| カラー (calla) | 開いた非対称の仏炎苞と黄色い肉穂花序 | 11 |
| キキョウ (platycodon) | 5枚の紫の花被、膨らんだつぼみ | 21 |
| エキナセア (echinacea) | 桃色の舌状花14枚を各3個の重なった楕円体で構成。二次ベジェで付け根から立ち上がり外へ反って先端が垂れる | 52 |
| アネモネ (anemone) | 8枚の赤い萼片、黒い中心と雄しべ | 34 |
| ニゲラ (nigella) | 萼片10枚の八重型、直立する5本の花柱、分岐した苞 | 54 |
| セイヨウオダマキ (aquilegia) | 下向きの花、上へ曲がる5本の距 | 35 |
| フリチラリア・メレアグリス (fritillaria) | 丸い肩と控えめな裾の帽子形、深紫に薄紫の市松模様、2輪 | 23 |
| ハナショウブ (iris) | 3枚の幅広い外花被、小さな内花被、黄色い筋 | 25 |

最大使用部品数はニゲラの54（`MAX_PARTS=56`以内）。不正な種指定時はすずらんへ戻る。架空種は実装しない。

学名・参考資料の出典:

| ソース識別名 | 学名・参考資料 |
| --- | --- |
| `FLOWER_PLATYCODON` | [Platycodon grandiflorus — NC State Extension](https://plants.ces.ncsu.edu/plants/platycodon-grandiflorus/) |
| `FLOWER_ECHINACEA` | [Echinacea purpurea — Ohio State University](https://u.osu.edu/plants/echinacea-purpurea/) |
| `FLOWER_ANEMONE` | [Anemone coronaria — NC State Extension](https://plants.ces.ncsu.edu/plants/anemone-coronaria/common-name/anemones/) |
| `FLOWER_NIGELLA` | [Nigella damascena — NC State Extension](https://plants.ces.ncsu.edu/plants/nigella-damascena/) |
| `FLOWER_AQUILEGIA` | [Aquilegia vulgaris — RHS](https://www.rhs.org.uk/plants/100859/aquilegia-vulgaris/details) |
| `FLOWER_FRITILLARIA` | [Fritillaria meleagris — University of Oxford](https://herbaria.plants.ox.ac.uk/bol/plants400/Profiles/EF/Fritillaria) |
| `FLOWER_IRIS` | [Iris ensata — NC State Extension](https://plants.ces.ncsu.edu/plants/iris-ensata/) |
| 既存6種 | [RHS チューリップ](https://www.rhs.org.uk/plants/tulip)、[RHS クロッカス](https://www.rhs.org.uk/plants/29659/crocus-vernus/details)、[RHS スイセン](https://rhs.crocdn.co.uk/plants/_/narcissus-topolino/classid.1000000300/)、[Kew カラー](https://powo.science.kew.org/taxon/urn:lsid:ipni.org:names:89403-1/general-information) |

フリチラリアは多数の小花を穂状につける F. persica ではなく F. meleagris をモデルとする。ニゲラの青い部分は萼片で、実際の花弁は小さい（[Wikipedia「クロタネソウ」](https://ja.wikipedia.org/wiki/クロタネソウ)、[Nigella damascena](https://en.wikipedia.org/wiki/Nigella_damascena)）。フリチラリア専用の帽子形は既存の6区間ベル交差判定に別の輪郭係数を渡す（最大半径は基準の1.10倍）。スイセンの副花冠は右上へ開き、細い首（長さ0.32・半径0.16）と開口部（長さ0.38・半径0.35）を重ねた2曲面で表現、専用材質 `CORONA` が局所座標から光の筋を計算する（面上の発光風表現で、空間を照らす光源ではない）。クロッカスの市松模様は曲面の局所座標から計算し、画像や追加部品は使わない。全種の株元は背景の草の根元（134行目）へ合わせてある。

## カメラとショット（`main/scene/flower_shots.h`）

種が変わる間隔（40秒）のあいだ、カメラは1つのショットに固定される。テーブルに複数のショットを積めば種の途中でカットを割ることも構造上は可能。ショットは4つの数字で決まる。

| フィールド | 意味 | 範囲 |
| --- | --- | --- |
| `pitch` | カメラの高さ。負が下から見上げ、正が見下ろし | ±0.349 rad（20度） |
| `zoom` | カメラの寄り。**0以下で「株が窓に収まる倍率」**（種ごとに自動計算）。正の値は固定倍率 | 0以下、または 1.0〜4.0 |
| `aim` | 何を画面中央に置くか。株の高さに対する割合（0.0が株元、1.0が先端）。その位置が画面の65行目に来る | 0.0〜1.0 |
| `hold` | このショットを保持する秒数 | 合計が `FLOWER_ROTATE_S`（40秒）にちょうど一致 |

`aim`・`zoom` を行番号や画素の定数で書かない理由は同じ：**種の大きさ・背丈の差が、ショット間の差より大きい**。窓に収まる倍率は種によって0.80〜1.00に散らばり（nigellaが最小0.803、cropusが上限1.000）、定数1.00を書くと14種中13種がはみ出す。

### コストは `zoom` 単体では決まらない

`zoom` はカメラの前進であってレンズの拡大ではないので、大きくなった株は窓の外へ出ていく。`aim` を上げて頭部だけ画面に残す構図では、株の質量が「頭」にあるか「全体」にあるかでコストの向きが逆転する。`tools/flower_shot_cost.sh` が走査量（visits）と塗り面積（covered）を種ごとに出す。実測例（fitは窓に収まる倍率、他は `zoom`/`aim`）：

| 種 | fit | 1.55/0.72 | 2.20/0.94 | 1.55→2.20 |
| --- | --- | --- | --- | --- |
| valley（すずらん） | 6959 visits | 12162 | 6668 | **−45%**（寄せると安い） |
| echinacea | 6476 visits | 14627 | 20825 | **+42%**（寄せると高い） |

実機fpsも同じ`zoom`値で種ごとに大きく変わる（snowdropは`zoom`2.35で27.5〜29.0fps、echinaceaは同じ`zoom`で17.3fps）。**framingを変えたら、必ず全種で見た目とfpsの両方を確認する**こと。細い種（valley、snowdrop、fritillaria）は寄せすぎると識別できなくなり、頭が大きい種（sunflower、anemone、crocus、iris）は`zoom`2.35以上で頭が画面外に出る。

`pitch`は正射影カメラなので遠近感を生まない。奥行きによる縮小が無いため見上げの収束や三点透視は作れず、20度を超えると「カメラが動いた」ではなく「株が倒れた」ように見える。速度には無料（画素数を変えない）。`yaw`（水平回転）は実装が無く常に0——編集アプリは水平回転のUIを出してはいけない。

ファームウェアが拒否する定義：`pitch`絶対値0.349rad以下、`yaw`は常に0、`zoom`は0以下または1.0〜4.0、`aim`は0.0〜1.0、`hold`は4.0秒以上かつ`FLOWER_ROTATE_S`以下・合計が`FLOWER_ROTATE_S`に一致（誤差0.01秒）、隣り合うショットの`zoom`比は大きいほう÷小さいほうが1.16超（最後→最初の遷移も隣り合いとして数える）、`FLOWER_VIEW_MIN_S`は最も短い`hold`と一致。

動かす前にホストで確認できる：

```bash
bash tools/preview_flower_shots.sh   # 各定義を全14種でレンダリングしPNGの一覧を作る
bash tools/flower_shot_cost.sh       # 各段の画素コストを種ごとに出す
```

レンダラ内部の契約（CONTRACT）は `main/scene/flower_shots.h` 側にある。

## 森・光・雨（`main/scene/garden.c`）

garden.c は行ごとの整数演算で背景を近似する。光の密度は64pxと32pxの2層のValue Noiseを3:1で合成し、格子値は整数ハッシュ+Q8 smoothstepで補間する。LUT・画像・頂点配列は使わない。草木は区画内の位置・高さ・幅・傾き・葉の間隔をハッシュで決め、密度ノイズで群生と空白を作る。

雨滴の半径は3〜4px、既存画素を滴の中心へ寄せて参照する（背景に明暗差を置くことでレンズの揺らぎを見せる。追加の光線や物理屈折は使わない）。

### シーン遷移

40秒ごとの種交換時、旧レイアウトと新レイアウトの配置種を両方保持し、3秒のsmoothstepで重ね替える（`garden_row_blend`が480バイトの一時行バッファで両端点の高速パスを持つ）。以前は花が消えるフレームで森を即時交換していたため背景が飛んで見えたが、光・虫・風の時間は継続させたまま幾何だけを遷移させることで解決した。主光の水平オフセット・半幅・傾きも同じ遷移係数で移行する。花の1.2秒のディゾルブにも同じsmoothstepを使う。全画面暗転・花の二重描画は行わない。

検討した代替案：全画面クロスフェード（2枚目のフレームバッファ64,800Bが要る）、形状モーフ（種ごとにトポロジー・部品数・材質が違い中間形状が非現実的）、常時同じ森（周期的な再配置という要件を満たさない）。採用したのは「植生だけブレンドし、光は連続して動かす」案。

追加ストレージ：遷移状態12バイト、既存の共有garden frameに8バイト、一時行バッファは常駐しない。ブレンド中（時間の約7.5%）は植生を2回描画するが、光は2回描画しない。

### 装飾光線（decor rays）

既存のメイン光条の肩に、最大4本の一時的な暖色の開口部が浮かぶ。オフスクリーンの共通光源(240,-90)から発散し、直線で手前ほど広がる。トランク・樹冠・花がこの層を遮蔽する。メイン光条のコア（半幅/2−6px）はこのレイヤーから一切変更されない。

各機会は32秒ごとに新しい辺・入射角・幅・18〜26秒の寿命をハッシュし、1/4の確率でスキップする。4秒のソフトフェードで入れ替わりを隠すため、見える本数は変動する（常時2本固定には見えない）。開口部は0.5px/秒で右へ移動し、位置ノイズや主光条の揺れを継承しない。共有された移流Value Noiseフィールドが濃淡を制御する。行70〜101の間にある終端で最後の48pxがゼロへフェードし、下34行には光も影も落とさない。

基部半径は18〜25px、奥行きとともに広がり、共有ノイズで最大3px呼吸する。影は既存RGB565チャンネルに小さなQ8係数を掛けるだけ（青灰のストライプを描画しない）。in-scatteringは各画素の既存のメイン照明色に小さな暖色バイアスを加える形で、重なりは局所照明を継承する。単一の空間ディザ量子化の前に加算する、bounded integer transmissionの近似であり、volumetric transportのシミュレーションではない。

ノイズは行単位でサンプルし、画素単位ではない。各アクティブ行は画面クリップとメインコア除外の前で最大4×203画素を走査し、101〜134行は光を返さない。ヒープ・永続状態・追加画像バッファ・レイマーチは使わない。

`GARDEN_DECOR_RAYS=0` / `GARDEN_DECOR_EVENTS=0` でこの層・2つのイベントだけを比較用に無効化できる。実機フレーム時間は継続測定中（[pie-simd.md §4.6・§7](../perf/pie-simd.md)に実測あり：装飾光線の門番最適化で`rays` −1.46ms、列ごとの項を4列に1回にする近似で`rays` −5.0ms、13.6%の画素が近似で動く）。

参考にした資料：[NVIDIA GPU Gems 3 第13章](https://developer.nvidia.com/gpugems/gpugems3/part-ii-light-and-shadows/chapter-13-volumetric-light-scattering-post-process)（視覚的原理のみ採用、GPUのマルチサンプルラジアルブラーはMCUに実装していない）、[The Book of Shaders, fBM](https://thebookofshaders.com/13/)（独立した2オクターブ位置ノイズは実機レビュー後に共有単一移流フィールドへ置き換え、流れの一貫性を確保）。

## メモリ

- 森林モジュール（garden.c本体）の`.bss`/`.data`は0バイト。フレームパラメータ16バイトを既存の花用共有ブロックへ追加。
- 花モジュールの`.bss`/`.data`は84/16バイト（ESP32-S3向け単体コンパイル）。
- フェード用に自動変数の背景1行240バイトを使用（スタック、常駐配列ではない）。
- 雨滴の状態・行バッファ等は692バイト。
- シーン遷移の追加ストレージは12バイト＋既存共有フレーム8バイト。
- ここに挙げた数値は測定時点のもの。`capabilityを足すたびに動く`のはpie-simd.mdや他の数値と同じで、最新値は `tools/memlog.py --port --check` の実測を見ること。

## 検証・ホスト側の確認手段

- `tools/test_flower.c`（WSL/GCC、ASan/UBSan）：全14種×60姿勢の分割描画一致、描画境界、部品座標系、共有ブロックの解放・再利用、不正種指定時の代替。
- `tools/test_flower_div.c`：除算の近似・巻き上げのビット一致を全種×全位相で総当たり確認（[pie-simd.md §3.6](../perf/pie-simd.md)）。
- `tools/test_flower_floor.c`：`ifloor`/`iceil`が全floatビットパターンで`floorf`/`ceilf`と一致することを総当たりで確認（[pie-simd.md §3.7](../perf/pie-simd.md)）。
- `tools/test_bell_reject.c`：ベル早期棄却の取りこぼしゼロを全帯walkと突き合わせて確認。
- `tools/test_garden_transition.c` / `tools/test_garden_decor.c`：遷移の色・幾何の端点一致、行ガード、装飾光線の決定性・折り返し一致・メインコア不変。
- `tools/preview_flower.py` / `tools/preview_flower_shots.sh`：実際のC描画をホストでPNG化して目視確認する（LCD無しで見た目を確認する第一段階）。
- 実機での書き込み・FPS・物理的な見た目の確認は、これらのホスト検査を通した後に行う。
