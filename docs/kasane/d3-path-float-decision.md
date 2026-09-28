# D3/D3a: path と float IR の採否

2026-09-28 の host gate: [`test_proc_path_float_decision.c`](../../tools/kasane_contract/test_proc_path_float_decision.c) を MinGW GCC 15.2.0 の `-O0` と `-O2` で実行。両方通過した。COM3はこのgateで使用していない。`run.sh` にも同じ試験を追加した。

## 代表path

2本の`CUBIC`を異なる色で順に発行し、最後に`LINE`を足す。第2曲線の制御点は毎frameの型付き入力で変更する。合計33線分の順序、曲線間と最後の線分のpen連続性、全画面と逆順8行帯の画素、最終stateを比較した。入力変更後も第1曲線の16線分は不変で、第2曲線の画素だけが変化した。第2曲線の制御点を座標範囲外にした場合は、第1曲線の16線分を生成した後、frameを確定せず停止する。通常と失敗の両ケースで、scalar VMと登録済みplanのstatus、pc、論理step数、register、pen、線分が一致した。

`CUBIC`は全8registerを読み、penと描画を更新し、座標・線分・ラスタ上限で失敗しうる。登録時解析はこれらを効果と失敗として記録し、純粋算術の再配置・融合へ混ぜない。現時点で採用するpathの範囲は、有界なnative `CUBIC`、`MOVE`、`LINE`、`PLOT`の逐次合成と、型付き入力による図形パラメータ更新である。適応分割、塗り規則、stroke join、任意pathの一括PIE化は採否を保留する。曲線を1デバッグstepで生成する既存の境界も維持する。

## float と PIE

float VMの`0.4999 + 0.0002`を`PLOT`へ渡すと、座標の`lroundf`によりx=1になる。同じ半画素平行移動を現在の独立Q14点列で表すと、契約上floorしてx=0になる。暗黙変換は最終画素を変える。`ADD`の途中非有限値失敗と`PLOT`の座標失敗も、解析とVMの順序に残さなければならない。

したがって一般float描画IRを現行Q14 PIE点列へ暗黙に下ろす案は棄却する。floatの登録済みplanは逐次scalarとして使い、PIEは明示的に型付けされた独立Q14点列に限る。後者は既存の8点以上、16バイト整列、値域・alias契約に従って選択する。float専用PIE kernelを採用するには、命令ごとの丸め、途中非有限値、stepと失敗snapshot、描画順を保持する意味論と、pack/unpackを含む実機費用を先に決める必要がある。このgateはその性能を測ったものではない。

prefix/IIRの型付き依存行PIEの実機費用は[ロードマップ](dynamic-rendering-roadmap.md)の6形状で既に測定済みであり、このpath/float gateから一般式への自動選択閾値を広げない。
