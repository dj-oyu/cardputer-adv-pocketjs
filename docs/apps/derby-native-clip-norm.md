# DERBY clip正規化係数cache の通常OFF収容

PR14 `44f9c56121b5aab4fb2eca0fcf9fbe54d833a276` の係数cacheを移植する。
`POCKET_DERBY_CLIP_NORM` はOFF既定。OFFでは元のou関数・state layoutを保ち、
host -O2 objectがmainと完全一致することを確認した。

ONでは楕円viewで `sqrt(f*f+25600)` を一度保存し、margin mとの積は元の順序のまま。
getterやdraw hookがviewに再入してfを変えた場合はouで再計算する。NaNでも保守的に
再計算する。static stateの追加は2 doubles、16 bytes。straight viewではrefreshしない。

```
python -B tools/games/test_derby_clip_norm.py
python -B tools/games/run_derby_clip_batch.py --out artifacts/clip-batch
```

抽出した実predicateを元の式と180万件比較し、signed zero、巨大値、Inf、NaN、
reentry、syntheticなsqrt回数を検査する。実アプリの4mode matrixには明示的な楕円MIDを
含め、clipとrunner batchの組合せをdraw/pixel/work/raceで比較する。target側は
CMake modeと、ELF中のsource-scoped view objectの実sizeを、同じtarget compilerで
作ったlayout-only objectと照合する。layout用objectはfirmwareにリンクしない。

[PR14](https://github.com/dj-oyu/cardputer-adv-pocketjs/pull/14) の元診断枝の単体実測は
この移植の実機受入ではない。PR15の観測差と加算しない。実機速度・heap/stack・LCD・
組合せ受入は未完了。通常OFF収容と有効化を分け、PR19の固定candidateは変更しない。
