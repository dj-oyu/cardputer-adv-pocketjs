# Dynamic motion: host study

ファームウェアへ組み込まない机上/host試験。[設計草案](../../../docs/api/ayatsuri-dynamic-motion-draft.md)と[結果・限界](../../../docs/perf/dynamic-motion-host-study.md)を参照。標準Pythonだけで数値/CSV/APNGを作る。`plot.ps1`だけWindows標準.NET Chartingを使う。

```powershell
python tools/character/dynamics_study/test_signals.py
python tools/character/dynamics_study/study.py --out .cache/dynamics-study-final
& tools/character/dynamics_study/plot.ps1 -Folder .cache/dynamics-study-final
python tools/character/dynamics_study/render.py --folder .cache/dynamics-study-final
```

`results.json`は再現/有界/energyの検査と、未達の収束probeを分けて記録する。終了code0だけで全て合格と解釈しない。strict probeのFAILも本試験の成果である。各CSVは全16秒のsimulation time、APNGはpauseを含む先頭11秒のwall time/331枚。30sample/sの再生はdevice FPSではない。

`study.py`はbinary64の参照計算で、Pythonのfloat/object/dict/履歴を確保する。C/JS共通実装、無確保、native footprint、ESP32での速度を実証しない。`signals.py`のint16/u32配置もC実装候補の机上量で、Python objectの実サイズではない。ホストの並べ替え試験は並列化可能な依存境界を検証し、threadや多コア性能を測らない。
