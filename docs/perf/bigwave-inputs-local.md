# BIG WAVE 入力配列の検証と有限実機診断

製品変更は main `e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267` に対する `apps/bigwave/big_wave.js::draws` の5式だけ。`concat` による数値入力の組立を同じ順の配列リテラルへ置換した。rock/rider guard、他のアプリ変更、PR42/44、永続構造・予算変更を含めない。独立Draft PRで検証し、実機測定前に性能改善として採用しない。

## 確認済みの範囲

native O2 の40組を2回実行し、title/ride の JS CPU は約7–9%、frame 呼出しから native present 完了までは約4–5%短縮した。この区間には検査用の時計・sample保存・pending job・end_turnも含み、製品scheduler/LCD/FPSではない。pauseの小さい悪化傾向も全rawに保持する。

通常窓120frameは対象組立720回、成功割当59回・charged bytes3168B/frame減。ソース+69B、bytecode+44B、drawsのoperand stack11→12 slots、全体最大33 slotsは同じ。nativeの評価後heapは+40B、compile peak156640Bは同じ。32bit/ESP32の値ではない。

全ゲーム再生は7279最終panel全byte・native draw列・51events/29checksが一致。ソースから抽出した5式をsh両分岐・±0/非有限値など240件でObject.is比較した。通常builtin/private dense数値配列が前提。任意のconcat/species差替えやOOM発生位置の同値は主張しない。

```sh
node tools/experiments/bigwave-inputs/boundary.mjs
python tools/experiments/bigwave-inputs/verify.py
python tools/experiments/bigwave-inputs/bench.py --pairs 40
python tools/experiments/bigwave-inputs/test_device.py
```

計時と別のbuild/observerを同時に実行しない。各trialは新context、40初期frame＋開始操作＋40warm＋120frame、AB/BA、2組破棄、window前後にGC、window内はautomatic GC。native heap cap524288B。m32は163840B/初期GC81920で同じscriptの`--m32`を使う。

## CIとビルド

専用workflowは同じnative入力に対するO2・m32 (`-malign-double`、163840 B上限) の互換性/40組A/B、通常製品と有限ride診断のbaseline/candidateを独立buildする。ESP-IDF6.0.1・Node24・製品既定設定を使い、partition一致・INFO Application information・security設定とELF/map/bin/offsetのhashを保存する。CIの成功と実機の成功は別で、実機性能・heapは未測定。

上記host結果はnative64であり、m32の結果は専用CI artifactを参照する。旧PR63のartifactは別候補のため、この入力配列候補のfirmwareとして使わない。

## 最小の有限実機診断

`prepare_device.py`は別のclean checkoutだけを変更する。通常製品には計測を常駐させない。baselineは上記main、candidateはこの独立branchのsource。双方を同じtoolchain/configで別buildする。実際のsource/hashとflash offsetは各CI artifactのexperiment.json/flasher_args.jsonを参照。

```sh
python tools/experiments/bigwave-inputs/prepare_device.py --checkout /path/to/disposable-checkout
# 通常のESP-IDF6.0.1環境で、対象checkoutごとに実行。flashしない。
idf.py --version
idf.py -B build_inputs -D SDKCONFIG="$PWD/build_inputs/sdkconfig" -D IDF_TARGET=esp32s3 -D PROJECT_VER="SOURCE_HEAD_12_CHARS" build
idf.py -B build_inputs size > build_inputs/size.log
```

PROJECT_VERには対象checkoutの実head先頭12文字を指定する。製品のsdkconfig.defaults/partition/security/INFO設定を使い、通常buildと同じ依存準備を行う。ELF/map、sdkconfig、bootloader/partition/app bin、flasher_args.json、diagnostic.json/patch、SHA256一覧を保存する。offsetは生成されたflasher_args.jsonを正とし、過去artifactから転記しない。通常boot gateのINFO Application information/version/ELFを照合する。

workloadは `bigwave-inputs-ride-120-v1`。通常begin_run/app_tick/ui_task/scheduler/LCD/GUEST_ALLOCATORを通し、40title＋開始2turn＋40bot warmの82準備turn、その後120ride frame。physicsや状態を飛ばさず、JS botは既存host botのread-only判断を再利用。同期setup loopなし。24秒の協調deadline、watchdog/guard/heap/stack/securityは不変更。hostでは通常FIFO bot・診断baseline・診断candidateが203最終panel全byte一致、202frame/state1/x50.909642993、exception/present/oracle失敗0。

120 unsigned sample配列は480B。正確なmedian_x2とnearest-rank p95をFW内で計算し、生sampleは送らない。診断7行＋cap付きapp log16行、約202owner frame（30Hzなら約6.7秒の見込み）で、既存30秒/64KiB受信/256行32KiB保持を変更しない。実機での期限内完走は未確認。

実機担当の合否条件はfull source headの`BW_INPUTS_DIAG START`、pointer_bytes4、frames120/ride120/other0、`END status=complete complete:true`、`EXIT running=0`。timeout/app_error/input_mismatch/present_pendingは無効。marker付きJS wall時間とowner_tick合計、要求割当回数/要求bytes、frame境界の空きheapを比較する。境界heapはtransient peakではない。実機操作・flash・MCP設定変更・保存領域書込みはこの作業では行わない。
