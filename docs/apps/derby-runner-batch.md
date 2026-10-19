# DERBY runner submission の通常OFF収容

PR15 `3a641d4d996938339b44fe735aba876ad031b3a2` のPAN専用batchを、
PR18後の通常mainへ移植する。`POCKET_DERBY_RUNNER_BATCH` はOFF既定。
通常アプリJS・描画順・race stateを変更せず、ON時だけ生成PAN chunkと
native `runners` を使う。root CMakeの診断ON presetsは持ち込まない。

入力は8×5 doubles、320 bytesのArrayBuffer。使用行をdraw前に一括検査し、
pointerを保持せずsnapshotする。stale handleの拒否順・既存draw失敗の扱い・
rinのsin式とdouble演算順序は元PRと同じ。計測専用hooksは収容していない。

検証コマンド:

```
python -B tools/test_derby_elf.py
python -B tools/games/test_derby_clip_norm.py
python -B tools/games/run_derby_clip_batch.py --out artifacts/clip-batch
```

matrixはbaseline / clip / batch / combinedを通常seed全3tierと明示的な楕円MIDで
比較し、draw入力・順序・frame CSV・pixel hash・race結果の一致を要求する。
追加の楕円combined oracleでは、実batch呼出し、negative checks、2000 fuzz入力、
procedural float化前のdouble一致を必須とする。oracleは通常hash比較と別に扱う。

CIの4 firmware組合せはCMake cache、runner native symbol、埋込PAN、BIN/ELF pair、
SDK、PROJECT_VER=headを検査する。clipの実ELF state layoutも対象compilerで検証。
これらは実機の速度・heap/stack・LCD受入ではない。

clipとの併用以外のaffine / course-cache / projectionとの組合せは未受入なので
構成拒否する。PR19の固定candidateが処理されるまでDraftを維持し、merge前に
結果mainとの統合・同一head CI・独立レビューをやり直す。

## 元の比較枝の証拠

[PR15](https://github.com/dj-oyu/cardputer-adv-pocketjs/pull/15) は診断用比較枝。
通常設定での今回の移植とはbaseもgeneratorも異なる。元の単体実測は1boot内3従属
反復で、PR14との累積効果や独立boot再現性を証明しない。元枝は変更せず保持する。
