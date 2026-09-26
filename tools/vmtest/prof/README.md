# tools/vmtest/prof — ゲストの JS のターンを命令数で見る

`stress_prof.sh` は `apps/stress` を `tools/test_stress_app.c` で回し、callgrind で命令数を取る。
実機の配置（`-m32 -malign-double`）、`-O2`、ファームの既定の VM 設定。命令数は**同じ走行の中での比率**としてだけ
使う（ミリ秒にはならない。描画は PIE の代わりにスカラーで走るので、ホストでは描画がほぼ全部を占める）。

```bash
# valgrind はインストール不要（sudo が無い WSL 向け）: .deb を展開して使う
mkdir -p ~/vg && cd ~/vg && apt-get download valgrind && dpkg-deb -x valgrind_*.deb root
wsl -e bash tools/vmtest/prof/stress_prof.sh /tmp/sp          # 640 KiB・900 フレーム（test_stress_app の既定）
STRESS_HEAP_LIMIT=163840 STRESS_FRAMES=299 ...                  # 実機のヒープ設定・LV1 だけ（test_stress_app.c）
perl ~/vg/root/usr/bin/callgrind_annotate --inclusive=yes /tmp/sp.cg
```

JS のターンは `test_stress_app.c:eval` の包含コスト（`frame()` と、その後のジョブの消化）。描画は
`pocket_kasane_present`。WSL の `/tmp` は呼び出しの間に消えることがあるので、残したい結果は `.cache/prof/` へ写す。
