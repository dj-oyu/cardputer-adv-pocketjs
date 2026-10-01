# 手続き型描画 IR の host 実験

2026-09-27。これは D3 の設計を決めるための実験であり、JS API や製品の命令セットではない。[ロードマップ](dynamic-rendering-roadmap.md)の D1/D2 の frame 寿命・描画面契約はまだ作業中。実機での性能推定には使わない。

## 実行した例

[`test_procedural.c`](../../tools/kasane_contract/test_procedural.c) は次の処理を13命令で表す。`INPUT` は1フレームにつき一括で渡した数値を読む。`REPEAT` は native 実行器が反復し、JS→native 呼出しを各点に発生させない。

```text
r0 = 10                 # x
r1 = 1                  # dx
r2 = input[0]           # baseline
r3 = 12                 # amplitude
r4 = 0.1                # frequency
repeat 100:
    r5 = r0 * r4
    r6 = sin(r5)
    r6 = r6 * r3
    r7 = r6 + r2
    line_to(r0, r7, white)
    r0 = r0 + r1
```

`ksn_proc_begin` は入力と検証対象の命令を VM 所有領域へコピーし、`ksn_proc_step` は1命令ずつ進める。デバッガは `last_pc`、レジスタ、loop stack、出力数をその都度読める。`ksn_proc_run` は同じ step を完了まで呼ぶ。描画中に JS や VM を実行せず、完了時だけ `frame.ready` が立つ。`ksn_proc_render_band` は固定された線分を渡された RGB565 下地へ命令順に重ねる。

host 実行では 100 sample → 99 segment、706 VM step。`--trace` の10 step目で `pc=9, x=10, y=70.10, depth=1, emitted=0`、18 step目で `pc=10, emitted=1` を確認できる。最初の `line_to` はペン位置を決め、2点目から線分を出す。フル画面と逆順8行帯の画素は一致し、次フレームの入力変更後も旧フレームを再描画できた。後の `plot` が先の色を上書きする逐次順序も確認した。

## 今回見えた境界

- 命令64、レジスタ16、入力8、loop深さ8、実行10,000 step、生成1,024線分、線分のラスタ総歩数8,192を暫定上限とし、超えたフレームは公開しない。不正命令、非有限数、座標の範囲外も拒否する。（2026-09-29 にレジスタ8→16、入力4→8、loop深さ4→8へ緩和。命令・step・線分・ラスタは据え置き。）
- JS adapter（`pocket.kasane.procedural`）の上限: 登録plan 32本（`unregister(handle)` で解除でき、空いた slot は再利用するが handle 番号は再利用しない）、型付き点列 2〜128点、点列の変換後座標は VM と同じ −480〜720（範囲外は `INVALID_ARGUMENT`）。`draw(handle, inputs)` は 0〜8 要素を受け、不足分は 0 で埋める。`draw(handle, a0, …, a7)`（2 つ目の引数が数）は配列を作らずに数を受け、末尾の `undefined` は渡さなかったのと同じ（2026-10-02、[derby-ser-native.md](../apps/derby-ser-native.md) §5）。
- 上記の最悪時 heap（計算値、xtensa-esp32s3-elf-gcc の `sizeof`）: plan 872 B ＋ 点列 1,040 B（16 B 整列のため `malloc` は +15 B）を 32 本で 61,664 B。これとは別に、面ごとの候補・確定 frame（各10,246 B、2面で4枚）と共有 scratch 1枚・VM 996 B。点列領域は点数によらず128点分を確保するので、40点のバッチも 1,040 B を使う（旧64点上限では 528 B）。登録時解析 `ksn_proc_analysis` はスタック上で 2,248→2,952 B に増えた。いずれも実機での heap・stack 実測はしていない。
- 線分列は命令順とフレームの再描画を簡単にする一方、host 上で最大容量のフレームは 10,246 B、検証済み命令コピーと loop 対応表を含む VM は 952 B を要した。次に実測するのは、実際の線分数、行ごとの描画コスト、旧・候補フレームの同時保持量。関数画素や FLOWER をこの IR の線分列へ展開することは決めていない。
- 8行帯の描画は各線分の縦範囲を調べ、交差する線分だけを走査する。帯ごとに一覧を再走査するため、線分が多い場合は行 index や別の native kernel の方がよい可能性がある。
- VM は `begin` 時に検証対象の命令を所有コピーにする。JS 登録を作る段階では長寿命の native program と mount/unmount の管理が別途必要。現行 Kasane の APP 80命令枠へ点列を展開しない。

Windows host での再現例（MSYS2 UCRT64 GCC、`.cache` を TEMP/TMP に設定）：

```text
gcc -std=c11 -O2 -Wall -Wextra -Werror -Imain/ui/kasane main/ui/kasane/ksn_procedural.c tools/kasane_contract/test_procedural.c -lm -o .cache/test_procedural.exe
.cache/test_procedural.exe --trace
```

継続試験は `tools/kasane_contract/run.sh` にも登録した。

## 表現を変えた構造 probe

[`probe_procedural_patterns.c`](../../tools/kasane_contract/probe_procedural_patterns.c) で、出力数を増やす代わりに10の小さな構造例を試した。O0 と O2 の host 実行で同じ結果を得た。

```text
gcc -std=c11 -O2 -Wall -Wextra -Werror -Imain/ui/kasane main/ui/kasane/ksn_procedural.c tools/kasane_contract/probe_procedural_patterns.c -lm -o .cache/probe_procedural_patterns.exe
.cache/probe_procedural_patterns.exe
```

| 例 | 結果 | 分かったこと |
| --- | --- | --- |
| 3×4 格子 | 12点、loop深さ2、55 step | ネストと内側座標のリセットは現在のレジスタ操作で表せる。 |
| 前回値を積分する軌跡 | 7線分 | 反復間に数値状態を持ち、逐次的に `LINE` できる。 |
| 2本の離れた path | `MOVE` で接続線を作らない | パスの切断は表せる。 |
| 脱出時間型の実数軌道 `z = z*z + c` | 旧固定16回では `c=2` が28 step目に非有限となり、フレーム全体が無効。`BREAK_IF_GT` なら2反復目で終了して公開でき、`c=-1` は16回完走する。色レジスタ版 `PLOT` は各反復回数を RGB565 値として線分へ固定する。 | **条件付き終了と計算色の欠落を実験IRで解消。** 旧失敗も対照として残した。 |
| 2フレームの粒子積分 | 同じ入力だけを再度渡すと両方 x=12。`ksn_proc_state` をC側に置き、成功後の state を次の `begin_state` に渡すと x=12→14。 | **native 側で状態を継続可能。** state の所有者と表示失敗時の進行規則は統合時に決める。 |
| step 中の命令変更 | 借用元の2命令目を y=20→30 に変えても、VM が begin 時にコピーした命令で y=20 を描く。 | **検証と実行が同じ命令列を読む。** 長寿命登録の所有権は別段階。 |
| 入力で sample 数を 3→5 | 旧即値 `REPEAT` は出力3点。`REPEAT_REG` へ替えると同じプログラムと入力5で出力5点。入力0では本体を飛ばす。 | **入力駆動の有界反復を追加。** 回数は整数0～255、総10,000 step 上限は維持。 |
| 候補フレームの失敗 | 生の frame を同じ領域で上書きすれば旧 frame が消える。後述の surface は別スロットと ticket で保持し、失敗時に旧 frame を修復する。 | **host の surface 境界で対処。** Kasane core との統合は未了。 |
| 点の移動 | 生の `render_band` で旧パネルへ重ねると旧位置も残る。後述の surface は旧/新 bounds を damage に含め、下地から再合成する。 | **host の surface 境界で対処。** |
| 交差する近遠2本の線 | 提出順を入れ替えると交点は緑↔赤へ変化する。 | 2D の逐次描画としては正しい。3D の前後関係には別の depth 値と比較、または depth を扱う native kernel が要る。 |

制御の小さな拡張として `REPEAT_REG` と、レジスタ a>b の場合に最内ループを抜ける `BREAK_IF_GT` を追加した。0回 skip、ネスト内だけの break、ループ外 break の登録拒否、小数・256回の拒否を確認した。`PLOT_COLOR_REG` / `LINE_COLOR_REG` は整数 0～65535 のレジスタ値を RGB565 として固定する。最大 step・線分・ラスタ歩数は以前のまま。

次の実装判断は Kasane core への frame/ticket/damage 接続と、native state の進行を表示成功・失敗からどう独立させるかである。3D depth や FLOWER の下地参照は線分IRへ直に入れるか専用 native kernel に分けるか比較する。これらの probe は現行 2D の逐次順序を不合格とするものではない。ここで opcode を製品仕様として確定しない。

## 候補・確定フレームの host 実験

[`ksn_procedural_surface.c`](../../main/ui/kasane/ksn_procedural_surface.c) は完成した IR フレームを2つの所有スロットへコピーし、世代 ticket を返す。候補の転送完了までは旧確定を保持する。途中失敗なら候補を破棄し、旧確定と固定下地から全画面を再生成する。修復前の次候補は受け付けない。描画時は必ず下地 callback を先に呼び、移動点の旧位置を消す。damage は旧・新両方の線分 bounds から8行帯と列範囲を求める。

[`test_procedural_surface.c`](../../tools/kasane_contract/test_procedural_surface.c) は、候補の所有コピー、古い ticket の拒否、移動点の旧/新 damage、1帯転送後の失敗、旧フレームの画素一致修復、初回候補失敗時の下地修復を確認した。O0/O2 host で通過。これは Kasane core/LCD への接続試験ではない。最大容量のフレーム2スロットだけで 20,492 B を占めるため、FLOWER と動画を同時に持つ設計へ無条件に加えない。実際の線分数に応じた可変保持、再導出可能な状態だけを再計算する方法などを比較する。

[`ksn_procedural_layers.c`](../../main/ui/kasane/ksn_procedural_layers.c) は複数 surface の旧確定または候補を下から順に再生する。固定下地は帯ごとに一度読む。変更する下層の上に旧確定の上層を重ねるため、下層の更新で上層が消えない。現在の画素は不透明 RGB565 だけで、透明や depth は未定義。2面の最大フレームスロットだけで 40,984 B を使う。

[`ksn_procedural_present.c`](../../main/ui/kasane/ksn_procedural_present.c) はこの合成結果を Kasane の backdrop callback に渡す host 接続。core の APP/SYSTEM 命令は通常の順で上に重なる。候補と core の提出が重なる場合は core 提出を旧確定の上で先に表示し、次回に候補を表示する。部分転送に失敗した候補は破棄し、次回 core の全画面 repair に旧確定面を供給する。転送前の失敗では候補を再試行できる。`test_procedural_layers` と `test_procedural_present` が O0/O2 host で画素、帯 damage、最初の転送失敗、SYSTEM 遮蔽、提出の順序、転送前の再試行、画面外だけの更新を確認する。実機接続、列範囲 damage、メモリ予算は未検証。

[`test_procedural_pattern_matrix.c`](../../tools/kasane_contract/test_procedural_pattern_matrix.c) は波形、二重反復の格子、画面外から交差する線、同じ座標での色変更、消去、画面外だけへの移動、転送失敗時の旧フレーム復元を組み合わせた。候補の全画面再生と逆順8行帯が一致し、確定との差がある画素はすべて旧・新 bounds の damage に含まれることを O0/O2 host で確認した。実機の[表示・負荷診断](procedural-device-probe.md)は同じ波形と格子を使う。
