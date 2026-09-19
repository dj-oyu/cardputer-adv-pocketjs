# 性能作業のバックログ

まだ手を付けていない、または着手したが結論が出ていない性能候補だけを置く。終わった項目・当たらなかった項目はここに残さず、知見として [pie-simd.md](pie-simd.md) 側の該当節に畳み込む。各行は出所（誰が・どのブランチ/issueで見積もったか）と現在の状態を明記する。

## カーネル・PIE化の候補

**着手した分の順序と根拠は [pie-opt-plan.md](pie-opt-plan.md)（`perf/pie-opt`）にある。**
下の表は未着手・未決のものだけを残す（T1 `bell` 帯棄却・T2 装飾光線の厳密カーネル・T3 MP3 FIR の
3行は plan 側を見ること。**うち T3 は `perf/mp3-fir` として本体線（`vm/main` 起点の直線10コミット+
補間の int32 化）へ着地済み ── 実測 +896 B flash / +16 B DIRAM、ホスト10本のハッシュは変更前と一致、
実機の時間だけ未確認**）。

| 項目 | 出所 | 状態 | メモ |
| --- | --- | --- | --- |
| canopy カーネルの追加削減（`wsr.sar 0` の重複、`dx` の直接保持、定数31の二重ロード、`f=(q*39322)>>16` を `EE.VMUL.U16` 1本に） | pie-simd.md §7 実装時の副産物 | 未着手 | `stalls.py` 静的解析ベース。§7 で canopy 全体が「差なし」から「−0.71ms」に変わった後なので、これらの追加削減が実測でどれだけ効くかは未検証。着手するなら canopy 単独のカウンタ（`garden_prof_canopy`）で測ること |
| テキスト描画のマスク合成の高速化（`coverage_index` の除算を加算に、全0行のスキップ） | issue #4、上流 PocketJS `backends/rgb565/src/lib.rs:715-745` | 未着手（スカラー最適化、PIE化は対象外と判明） | `scale=1` ではグリフ窓が6〜12pxで16px単位のブロックが成立せず、PIEは効かない（pie-simd.md §4.6 相当の結論）。除算8本→0本はホストで出力一致確認済み、全0行は全行の20.7%。上流の固定revision更新が必要で、`render-rgb565` の構造体サイズ検査により古いヘッダでビルドしたアクセラレータは黙って無効化される点に注意 |
| `sqrtf` を整数平方根に置き換え（FLOWER） | `perf/flower-decor` ブランチ、`docs/flower-optimisation-options.md` §3-5 | **却下（2026-09-19 実機 A/B）: 遅くなる** | 実装は出荷済みで `g_flower_fixed_sqrt`（既定 0、SPLIT3 の `sq=`）の裏にあるが、**この値を反転させるものがコードのどこにも無い**ので、誰も測っていない。2026-09-19 に home の FLOWER で SPLIT を読んだところ `sqrt=0.63 ms (1,490 calls, 102 cy)` / 別の種で `0.85 ms (2,132 calls, 96 cy)` で、**kernel 22 ms のうち 3〜4%**。旧記録の「1フレーム約4,000回・約2.1ms」はカメラのショットと種が違う時点の値で、通常のホーム静止時には当たらない。整数版自身も 16 ステップ回るので上限の勝ちは 0.3 ms 程度、対して B=8 は1フレームあたり数画素を反転させる。**絵のリスクに対して取り分が小さい。** 同一バイナリの窓ごと A/B で実際に測った結果、**`sqrtf` 115–124 cy に対し `flower_isqrt_q` は 191–195 cy**、`bell_hit` の6本も 157 → 179 cy で、**整数版のほうが遅い**。理由は逆アセンブル: この石の `sqrtf` は `sqrt0.s` シードの分岐ゼロ33命令（`__ieee754_sqrtf`）で、対する `flower_isqrt_q` は16反復の整数ループ＋両端の float 変換。詳細は [flower-shade.md](flower-shade.md) §6 |
| 装飾光線（decor rays）のPIE化 | `perf/flower-decor` ブランチ、`docs/flower-optimisation-options.md` §2-B | 未着手（設計上の壁が明文化されているのみ） | 32bit中間値の扱い、被覆の外で「書かない」を「マスクして書く」への変更、ditherのレーン化が壁。pie-simd.md §4.6 の実測（装飾のスカラーパスは命令数の床の約2.4倍かかる）を踏まえ、命令数を削る前にそのパスを単独計測してから着手すること |
| FPUの除算・平方根をNewton-Raphsonソフトウェア実装に置き換え | pie-simd.md §3.8 | 保留（着手見送り） | seed命令（`sqrt0.s`等）はハードウェアに実在しインラインアセンブリで呼べるが、`-mhard-float-*`フラグは無く手書きが要る。`__divsf3`の実測が推定の1/4だったため、着手コストに見合う根拠が今のところ無い |

## FLOWER シーンの候補（見た目が変わる可能性があるもの）

| 項目 | 出所 | 状態 | メモ |
| --- | --- | --- | --- |
| `bell_hit` の緯度帯数を6から減らす | 旧 flower-perf-handoff.md | 未着手・オーナー判断待ち | `FLOWER_BELL_BANDS`。コストはほぼ帯数に比例するはずだが、鐘の輪郭が折れ線近似になり見た目が変わるため「見た目が変わらないこと」を前提にできない |
| 判別式の符号だけで早期に帯を捨てる | 旧 flower-perf-handoff.md | 未着手 | 現状は `a`/`b`/`c` 計算後・`sqrtf`直前で`disc<0`を`continue`。帯の半径範囲から`disc<0`が確定する帯を先に落とせる余地があるかもしれないが未検証 |
| `bell_hit` のPIE化 | 旧 flower-perf-handoff.md | 未着手 | 6帯は独立でベクトル化の形はできるが、`sqrtf`がライブラリ呼び出しである以上そこは直列に残る。着手前に `stalls.py`/`test_kernels.py`/`run_models.py` の3層を通すこと |

## FLOWER の実測内訳（2026-09-19、home 静止、`sq=0 gate=1 tweaks=1 canopy=1`）

次に何を狙うかはこの表から選ぶ。出所は実機 COM3 の `garden: SPLIT` / `SPLIT2` / `SPLIT3`
（60 フレーム窓、ms/frame）。種とカメラのショットで動くので、2 つの窓を併記する。

| 項 | 種 10 / 29.9 fps | 種 13 | 内訳 |
| --- | ---: | ---: | --- |
| total（= PERF の kernel） | 21.60 | 23.71 | |
| ├ pixels（PIE ベクタ半分） | 6.36 | 6.45 | 既に PIE |
| ├ decor | 6.50 | 7.36 | veg 4.18 / rays 2.11 / rest 0.20 |
| └ ray（花そのもの） | 8.74 | 9.90 | span 7.31 + scan 1.13 + pre 0.11 + rest 0.19 |
| 　└ span の中 | | | shade 3.75（1,440 hits、624 cy/hit）、sqrt 0.63、visit 残り 2.90（6,834 visits × 102 cy） |

**単項で最大は `shade`**、次が `veg`（135 行 × 7,431 cy/行）。
`shade` は 2026-09-19 に初めて割った —— [flower-shade.md](flower-shade.md)。
死んだ `garden_dither` 呼び出しを消し、`garden_dither` をヘッダの `static inline` へ移して
**kernel 32.0 → 29.3 ms（種 0、−8.4%）、fps 26.4–27.4 → 28.2–28.4**。
残りは `shade` の `rest`（約 370 cy/hit、依存チェーン待ちかロードかは未測定）と
`norm`（145 cy、逆平方根の Newton 系列で上限 0.9 ms）。`veg` はまだ割られていない。
`visits` の 79% は miss（6,834 visits に対し 1,440 hits）。

## 参照

- [pie-simd.md](pie-simd.md)：カーネルを書くときの知識と、既に測った結果。
- [compiler-builtins.md](compiler-builtins.md)：リンクマップの「会員を取り込んだ理由」は1本しか出ないので、会員（cgu.13 = 10,244 B）を落とせるかはクロスリファレンス表で判断すること。`__fixdfsi` の参照を消しても 1 B も動かないという実測つき。
- `docs/flower-optimisation-options.md`（`perf/flower-decor` ブランチ）：装飾光線・`sqrtf`のPIE化候補の詳細設計。このリポジトリの `main` 系列にはまだ無い。
