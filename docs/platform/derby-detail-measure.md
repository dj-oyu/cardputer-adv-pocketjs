# DERBY WATCH 詳細計測の再構成

2026-10-19。旧担当環境の WIP ZIP / CHECKPOINT は取得できていない。本変更は指定仕様と exact source からの再構成であり、旧コードと同一とは主張しない。対象は計測であり、描画・コピー・転送の最適化は実装していない。

比較元は baseline `0c52600fc34deafbadc53bc48e4fc98931e404e4`、native `44f9c56121b5aab4fb2eca0fcf9fbe54d833a276`、batch `3a641d4d996938339b44fe735aba876ad031b3a2`。native と batch は独立した比較版。既存 checkout は変更せず、新しいローカル branch/worktree で扱った。AGENTS.md / 関連 .agents/skills は選択環境・exact treeに存在せず、CLAUDE.md を参照した。

## 計測の契約

一般optionの `POCKET_DERBY_DETAIL_MEASURE` は既定 OFF。この診断revisionのroot設定はONを指定し、OFF対照は `-DPOCKET_DERBY_DETAIL_MEASURE=OFF` で選ぶ。ON には既存の MEASURE / FIXED_WORKLOAD が必要。固定 fixture の seed / camera / 390→510 の120 completed contexts / 3 dependent repetitions / END / STOP / HOME、250ms watchdog、180秒 session、最大8step/frameでreturnする未計測tailを維持する。通常アプリの JS は変更しない。詳細OFFの生成JSと主要5 CオブジェクトのO0 .text を exact base と比較する。

26区分と13イベントの固定配列を使い、heap割当・大きなhistogram・ISRからのstack操作を追加しない。各repの測定窓で開始し、終了時にsnapshotを確定、tail後に512B未満の行で出力する。出力失敗・不正scope・clock逆行は品質不合格。abort / app stopで計測をcancelする。

C scopeはcleanup属性でreturn時に閉じる。JSは生成fixtureのtry/finallyと専用bindingで同期scopeを張る。QuickJS park時はJS stackを退避し、rendererとpaceを分離、resume時に休止区間を開始時刻から除外する。継続を含むscopeは1 invocationとして数える。owner task以外とISRは状態に触れない。inclusiveは子を含み、selfは同じ階層の子区間を除く。どちらも実行中のpreemptionを含むelapsedであり、CPU専有時間ではない。

JS_FRAME.selfはsimulation / shot・runner pose-projection / PAN sort / runner提出 / HUDと、計装したnative部分を除いたframe残余。未分割のbinding引数処理等を含み、純JS interpreter時間とは呼ばない。NATIVE_SERIESはview/serのnative区間。PROC_BUILDの子を VM setup / zero / instruction copy / validate / match / run / segment appendに分ける。描画はprepare / backdrop / compositeと、image span / band raster / span copyに分ける。

TX_SYNCはrendererの同期present call全体。TX_COPY / TX_SUBMIT / TX_REAPは既存P0と同じowner側のswap・queue・get-result境界を使う。blocking fallbackではTX_SUBMIT自体が完了待ちを含む。TX_REAPは実際にin-flightがあるget-resultだけを数える。P0 ISRやhistogramは有効化・拡張しない。純transfer時間、DMA占有時間、実bus trafficは測定していない。submit/reap elapsedをそれらと混同しない。

## byte/eventの意味

schemaの順序は `tools/games/derby_detail_schema.json`。

- BRIDGE_SNAPSHOT_BYTES: batch bindingの320B snapshot memcpy。1repは120回、38,400B。
- VM_ZERO_BYTES: 実際の `sizeof(*vm)` zero量。ホストABIでは1,016B。targetの996Bという構造見積もりをホスト実測値で代用しない。
- VM_CODE_COPY_BYTES / SEGMENT_COPY_BYTES: 実際のinstruction配列 / scratch→candidate memcpyの論理量。
- BAND_CLEAR_BYTES: band初期色clearだけ。segment rasterの全store量を表さない。
- SPAN_RGB_BYTES / SPAN_ALPHA_BYTES: cache→spanのRGBコピー / alpha fill量。
- SHARED_OUTPUT_BYTES: 最終damaged bandの論理出力範囲。全blend storeや多重上書きの総量ではない。
- TX_COPY_BYTES: TX panel bufferへ書いた論理量。in-place blocking swapには別panelコピーがないため0。
- TX_PAYLOAD_BYTES / TX_SUBMITS / TX_REAPS / TX_FAILURES: 成功したpixel payload量 / submit試行 / 実reap / 失敗回数。LCD command引数bytesはpayloadに含めない。

ホスト描画oracleは通常経路に加えてdebug/reference VMを実行するため、VM zero/copy/run集計にはその追加仕事も含まれる。host portはboard.cを呼ばず、TX_COPY / TX_SUBMIT / TX_REAPとpayload bytesが0になる。実board関数は別のSPI stub試験で確認し、実機I/O計測とは区別する。

## 厳格な受入

詳細parserは既存fixed全trial parserを併用し、stage/eventの欠落・重複・順番・version/schema・session/rep混在・不可能な時間・fault・既知のJS/native仕事欠落・byte観測欠落・STOP/HOME欠落を拒否する。`run_derby_fixed.py --verify-only` も詳細有効フラグに従って同じparserを走らせ、保存JSONとlogの詳細値を比較する。READY/3 summaries/END/STOP/HOMEとtransport境界の受入には、同runnerのpinned PR47 collectorによるmemory-only試験も併用する。hostのSTOP/HOMEはshell modelであり実機HOME受入ではない。

`record_verification.py` は将来のartifact検証に備えて詳細mode/native symbolsと生成watch/view/play/PANのELF埋込を検査する。今回の試験はsynthetic ELFのみ。この文書の初回ホスト検証時点ではfirmware build/linkや実機操作は実施していない。公開後のCI・実機受入結果は個別PRと検証artifactに記録する。

## ホスト再現

3版の対応patchを各exact baseへ適用する。依存はNode、GCC、Python/jsonschemaとCMake/Ninja。ホスト検証では CMake 4.4.4 / Ninja 1.13.2 を使用した。Git fetchはproxy CONNECT 503だったため、認可されたGitHub読取経路でblob/tree/commitを取得しSHA1を照合した。ローカル履歴は3つのexact commitをshallow境界として保持し、それ以前の全historyは取得していない。

```sh
python3 tools/test_derby_detail.py
python3 tools/test_derby_detail_generation.py
python3 tools/test_derby_detail_board.py
python3 tools/test_derby_detail_proof.py
python3 tools/test_derby_fixed_protocol.py
python3 tools/test_derby_fixed_workload.py
python3 tools/test_derby_fixed_integration.py
python3 tools/games/run_derby_fixed.py --out /tmp/detail-off
python3 tools/games/run_derby_fixed.py --detail --out /tmp/detail-on
python3 tools/test_derby_detail.py --log /tmp/detail-on/trial.log
python3 tools/games/run_derby_fixed.py --verify-only --out /tmp/detail-on
python3 tools/games/run_derby.py --measure
```

3版の同一意味とoverhead対照は次で再現する。fastはO2・sanitizer無し、通常はASan/UBSan。cacheはfast/ASan・detail/OFF・m32ごとに分離する。`--m32` は環境の32bit依存が必要で、今回実行していない。

```sh
python3 tools/games/compare_derby_detail.py \
  --baseline BASELINE_DIR \
  --native NATIVE_DIR \
  --batch BATCH_DIR \
  --out /tmp/detail-comparison
# 同じ引数に --fast を加えるとホストoverhead対照。
# --verify-only は保存済み6実行を再検証する。
```

同一画素/全strip同値はホストportが転送直前に前panel状態と比較する観測で、バッファも描画も変更しない。3,888,000 pixels中3,499,706同値、2040 strips中449全同値を各repで確認した。この比率から差分転送の節約量・速度向上は結論しない。

## 未実施

実機受入は単独担当・登録済みbrokerの明示planでのみ行う。targetの詳細ON/OFF overhead、正確なELF/map/heap/stack、純bus transfer、実画面とUSB collectorは未検証。hostの3repは同一process内のdependent replayで、独立boot比較やtarget性能の証拠に使わない。最終の結果・hash・patch・再現ソースは外部checkpointのREPORT/JSONを参照する。

## 診断出力のreturn境界

詳細ONは測定窓とbounded tailの完了後、各論理reporting frameから最大1行だけUSBへ出してreturnする。26 stage＋13 event＋detail END、coarse summary、最終whole-trial ENDを分け、最後のvalid ENDにも独立したframeを使う。各USB callのbounded waitを40回以上1native callへ積み上げない。詳細OFFの生成fixtureは元exact版のまま。遅い成功transport（1call110ms）で最大1write/return、42の拒否位置、最終valid前のsession timeoutを試験する。完全raw stream中のInternalError/RUNAWAY/LOADSTALL等もstrict detail parserで拒否し、valid END後の障害を黙って合格にしない。

各qualifying出力の直前にsession残時間250msを確保し、不足時はDF_TIMEOUTで不合格にする。終端write開始時には180秒以内でも成功write中に境界を跨ぐケースを含め、valid ENDの遅延確定を拒否する。既存USB callの100ms＋tick rounding上限を変えない。
