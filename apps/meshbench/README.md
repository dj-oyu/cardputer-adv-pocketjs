# Public native mesh benchmark

`local.meshbench` / `apps/meshbench/mesh_bench.js` は通常のapp登録と既存の
Model/Instance、`shiki.beginMeshFrame`、Kasane image compositorを使う専用synthetic。
通常のSTRESS/Shiki線分の性能とは別のworkloadである。メニューには追加しない。

## 固定入力と上限

- 公開source `generate.cjs` の4 quads、16 vertices、8 triangles、4 logical mesh records。
  平面の傾き・重なり・赤/緑/青/黄は固定。ambient-onlyの材質で色を固定する。
- AYNMは1,280 B、SHA256
  `43495668e4acc308672f9b3ae00784794c47b2b42c30f9c01fc0a2ab1bce4536`。
  character・画像・外部profile・runtime file I/Oはない。
- frame番号をf=1から数え、camera中心は
  `(112+(f-1)%16, 64+((f-1)>>4)%4)`。入力キーを使わない。
  同じ固定modelを毎frame evaluateし、submitする。heap pressure負荷を加えない。
- scopeは4096 B/candidate、work=200000、depth workspace=1行（960 B）。
  candidate+committed capacityは8,192 B。元の全体上限は変更しない。

`node apps/meshbench/generate.cjs --check` で生成JSの一致を検査する。
fixture変更時だけ同じコマンドの `--check` を外して再生成する。

## Windowとcounter

warmupは最初の30描画、測定は続く120描画。151回目のframe callbackでは新しいsubmitを
せず `MB END` を出し、その後は描画しない。hostは最後のpresent/ACK後にこのcallbackを
呼び、ENDが最後のsubmitより先に出ないことを検査する。app自体は終了せず、ownerのSTOPが必要。

- `MB READY`: fixture/build identityと上限。
- `MB SAMPLE_START f=30`: warmupの最後のsubmit直後。
- `MB SAMPLE f=60/90/120/150`: 測定30/60/90/120回の累積counter。
- `MB END n=120`: 測定callback完了。

`evaluate_cy` はevaluate、`submit_commit_cy` はbeginMeshFrame+submit+commitの
CPU cycle差の和。counter呼出し費用を差し引かない。raster/LCDは含まない。
各差はu32 wrap対応だが、1つの測定区間がcounterの1周未満であることを前提とする。

描画・送信は通常の `KASANE_PAINT turn_ms/render_ms/send_ms/bytes/bands/frames` を別に採る。
これは30描画窓なので、warmup境界に重なる最初の窓は除外し、測定区間内に完全に入る
窓だけを使う。native 120 samplesと同一の120描画平均であるとは主張しない。
実際の窓数とframe範囲、ELF identityを必ず残す。

## Host検証とメモリ

```sh
python tools/kasane_contract/run_meshbench_app.py
python tools/kasane_contract/run_mesh_memory.py
```

real QuickJS（hardened、guest limit160 KiB）・Kasane compositor・managed mesh APIを実行。
4/8行strip × direct rows on/off × 同期/遅延転送の8条件で各150 panelsを、独立に手組みしたquadの
入力経路から描く参照と全画素比較する。参照も同じpublic depth rasterを使うため、
raster自体の独立数学oracleではない。既存のcoverage/shader契約は別の回帰試験を利用する。
1回のpartial IO、retry、ACK、END後の追加submitなし、全破棄後の追跡allocation 0も検査する。

遅延4行経路は交互のhalf-stripを借用し、完了までの画素不変・再利用禁止と
mesh ACKより前のdrainを検査する。遅延8行経路はコピーする対照であり、
実機design-0が8行を借用するという意味ではない。これらは実C compositorの
transport callback試験で、SPI/DMA実機実行ではない。静的なtest panel/loan監視配列は
requested allocation ledgerの外であり、実機の必要メモリへ足し込まない。
hostのcycle関数はwrapを含むprotocol用stub。host cycle/FPSを実機へ外挿しない。
要求byteのpeakはリンクしたobjectのheap要求の計測で、allocator overhead、共有libcや
sanitizer内部、static（test panel/ledgerを含む）、stackを含まない。
guest counterは各frame後の標本であり、guestの瞬間peakではない。
容量・workspaceは追跡合計の内数。S3 static build memoryとは合算しない。
m32 layoutでの検証と、S3 runtime fitは未完である。

初回host4条件は全てdigest `924db725`。tracked requested peakは163,584 B、
最後のliveは129,671 B、guest frame後の標本最大は64,359 B。
snapshot capacityは8,192 B、workspace960 B、metadata204 B（host）。
各runの破棄後tracked live/blocksは0。ASan/UBSan/LSan検出なし。
独立内部reviewは登録・上限・oracle・counter・ACK/ENDを確認し、未解決指摘0件。

## 後続の性能ownerへの条件付き引渡し

通常のtracked defaults/partition、ESP-IDF 6.0.1、別builddir/sdkconfigを使う。
既存のO2/FP設定を保持し、broker API・security設定・renderer既定は変えない。
後続session案は60秒hard cap、ログ256 KiB以下、キー入力なし、`MB END`後にSTOP。
capまでにENDがなければtimeout/partialとして記録し、120 samples完了扱いにしない。

これは実行指示ではない。m32環境準備の拒否済みAPT readは停止したまま。
host前提の残件と、専任ownerの安全なsource-cache・同caller recovery確認が解決するまで
hardware job/fetch/flashを行わない。既存のfetch→build→flash後のholding ticketを
session.requestで消費する手順に従う必要があり、app_sessionをsubmit.afterへ直接渡さない。

## 起動時のfixture展開（2026-11-06）

1,280要素の数値配列literalを、`\\xNN` byte文字列と添字によるUint8Arrayへのコピーに
置き換えた。generatorは生成したdecoderを別のNode VMで実行し、modelOpenへ渡される
全byteがcold fixtureと一致することを検査する。fixture/hash、modelOpen以後の処理、
160 KiBのguest上限、renderer、app登録は変えない。sourceは4,654 Bから6,676 Bへ増えるが、
大量の数値literalのcompile用領域と一時JS配列を避ける。

同一64-bit host・real QuickJS・同一allocatorで旧sourceと新sourceを比較した結果:

- 起動JS_Eval中のguest usable byte peak: 114,592 → 63,104 B。
- 同区間の最大allocation/reallocation要求: 33,520 → 4,915 B。
- eval直前のusable byte: 両方44,700 B、eval後: 60,420 → 58,984 B。
- 8条件すべて150 panels、digest `924db725`、partial IO/retry/ACK/ENDと解放後ledger 0を確認。

`meshbench_eval_alloc.h`は4 B丸め・最小12 Bのusable lengthを模倣する。
usable peakはQuickJSのblockごとのquota overheadを含まない。host pointerは8 B、
JSValueは16 Bであり、実機layout、TLSFの断片化、block cache、realloc移動中の物理peakを
再現しない。これらの数値はS3実測ではない。最大要求の計測はallocatorへ届いた要求だけで、
先にQuickJSのquota判定で拒否された要求を含まない。

`python tools/kasane_contract/run_meshbench_app.py` は既定で起動時の単一要求に16 KiBの
上限を課し、巨大literalへの回帰を検出する。これはtestのstressorであり、製品の上限や
実機のlargest-free-blockではない。`--request-ceiling 0`は比較計測用に制限を外す。
LSanがptrace環境で実行できないhostでは`--no-lsan`を明示する。今回の8条件はこのoptionで
ASan/UBSanとallocation ledgerを検査した。LSan成功とは主張しない。

実機では以前、起動評価が`OOM first_req=33520 used=53280`で失敗した。
`native_launch`は起動失敗を`unsupported`へ変換するため、この表示だけでapp未登録と
判断してはいけない。今回のhost検証は実機での起動成功の代わりにはならない。
次は通常defaultsの別builddir/sdkconfig、同一fixture、`MB READY`から120 samplesの
`MB END`、STOPと復旧確認までを専任ownerが検証する。
