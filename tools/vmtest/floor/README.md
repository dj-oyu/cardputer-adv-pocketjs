# tools/vmtest/floor — ゲストの起動床の計測

`docs/vm/builtin-floor-plan.md` の数字を作る道具。すべて WSL、実機には触れない。
コマンドと前提は同文書 §9。

```powershell
wsl -e bash tools/vmtest/floor/floor32.sh     # intrinsic ごとの床（実機レイアウト -m32 -malign-double）
wsl -e bash tools/vmtest/floor/lazyfloor.sh   # heap walk と F1/F2 の節約（floor32.sh の後）
wsl -e bash tools/vmtest/floor/lazyprobe.sh   # 出荷アプリでの組み込みオブジェクトの外れ回数
```

生成物は `.cache/vmtest32/`（-m32 の objects と floor32・lazyfloor）と `.cache/vmtest-floor/`
（計数入り `quickjs.c` の写しと lazyprobe）。リポジトリの `quickjs.c` は書き換えない。
