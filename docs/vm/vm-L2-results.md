# VM L2 計測結果

対象: [vm-L2-design.md](vm-L2-design.md) の各段が実装された時点で測った数字と関所の通過結果。**設計・決定はここに書かない**（vm-L2-design.md 参照）。**未着手の段の予測値もここには置かない**（backlog.md）。

値はすべて計測日・コミット・ビルド（host/device、asan/o2、変種名）を明記する。ホストは `JSValue` 16B・ポインタ8B、実機は `JSValue` 8B・ポインタ4Bなので、**バイト単位の絶対値をホストと実機の間で混同しない**。

## 1. L2a: フレームセグメント（2026-09-13、`vm/l2a`）

実装は `quickjs-vmstack.h`（ヘッダオンリー）と `quickjs.c` の差分。`CONFIG_POCKET_VM_SEGFRAMES`（実機）/ `-alloca` バリアント（ホスト）で旧経路へ戻せる。

### 1.1 関所（実測(host)、統合後に再走）

| 関所 | 新経路（segframes） | 旧経路（alloca） |
| --- | --- | --- |
| `run.sh` asan / o2 | 43/43 / 43/43 | 43/43 / 43/43 |
| G1 `bytes_per_call` | **528.000** | **672.000** |
| `run.sh --force-yield` | 3 pass / 40 fail | 3 pass / 40 fail |
| Test262 | 7,501 / 194 / regressions 0 | 同左 |
| G6 `verify_all.sh` | exit 0 | — |
| ファーム DIRAM / flash | **115,468**（不変）/ 1,554,032 | 115,468 / 1,553,648 |

旧経路が 672.000 を正確に再現することが、ビルド時スイッチで戻せることの実証になっている。差144B/段はホストの `sizeof(JSStackFrame)` 72 + alloca 64 を16に丸めた値と一致。両方とも verdict は PROPORTIONAL（C再帰が消えるのはL2bなので、L2aでは正しい）。

`--force-yield` が新旧とも 3/40（後に corpus 追加で 3/44,3/45 と変わる。§3参照）で同一なのは、L2aが変えたものではないことの確認。

### 1.2 メモリは純増する（実測(host)、計算値は実機換算）

- 常駐セグメント1本（ホスト実測4,143B = 4096+ヘッダ32+整列15。実機は計算値4,127B）。最初の呼び出しから `JS_FreeRuntime` まで居座る。深さが境界を跨げば+1本キャッシュ。
- `JSRuntime` に `JSVMStack` が+20B（実機、計算値）。
- 内部余白は `resident_max − live_max` で、浅いファイルでは常駐1本のほぼ全部（3.5〜4.0 KiB）。
- 減ったのはCスタックだけ（144B/段）。ui タスクのスタックは静的確保なので、この減少がRAMとして戻るのはL2b でC再帰が消えてから。
- 静的DIRAMは不変、flash +360B。

### 1.3 修正したバグと既知の退行

- **直したバグ（Test262が発見）:** pop の判定に `b->func_kind == JS_FUNC_NORMAL` を使っていたが、`JS_FUNC_ASYNC` で組まれたバイトコード関数が通常経路で呼ばれて `done_generator:` に抜ける経路があった（モジュール本体、[vm-L2-design.md](vm-L2-design.md) §10 参照）。フレームが積まれたまま残り、`JS_FreeRuntime` の表明で abort していた（`language/eval-code/direct/export.js`/`import.js`、regressions 2）。セグメントの生存範囲で判定する形に変えて解決。
- **既知の退行（承知の上）:** `run.sh --trace`（asan）と `--vm-seg-size 2048`（o2）で `gc_threshold_device.js` が落ちる。常駐セグメントがジワジワ型OOMの「残り」を動かし、catchした後の `print` 自体がOOMする。期待値は書き換えていない。通常の `run.sh` 4バリアントでは通る。詳細は [tools/vmtest/README.md](../../tools/vmtest/README.md) の既知の脆さの節。（backlog #5の修正で同ファイルはOOMしなくなり解消、§4.19）

### 1.4 未実施だった検証（実施済み分は本文書の他節、未着手分は backlog.md）

- 完了条件6の後半（アロケータ比較で内部余白・外部断片化を分ける）は台帳07の時点では未実施 → backlog.md #1。
- 標準セグメントサイズ4096Bの根拠は §5（D42/D43）で見直し済み。
- 実機での初回動作確認は §2（L2b）の実機比較で実施。

## 2. 予算の窓とD10の着地（2026-09-13、実測(host)）

D10（[vm-L2-design.md](vm-L2-design.md) §9）実装前に「3つの期待値を1バイトも変えずに通る予算値がそもそも在るか」を確認した。

フレーム1個の大きさは実測と一致する式（`alloca_size = sizeof(JSValue)*(arg_allocated_size+var_count+stack_size) + sizeof(JSVarRef*)*var_ref_count`、`frame_total = round_up(sizeof(JSStackFrame)+alloca_size, JS_VM_FRAME_ALIGN)`）で決まる。`sizeof(JSStackFrame)` はホスト72/実機48。

| | 下限（これより大きくないと期待値が壊れる） | 上限 | 窓 |
| --- | --- | --- | --- |
| host | 約0.1 MB（`deep_recursion.txt` の `depth>1000`） | 64 MiBのヒープ | 3桁の余裕 |
| 実機 | 約480B（`seg_oom_boundary` の大確保の時点） | ゲスト上限160 KiBより小さいこと | 2桁以上の余裕 |

1フレームあたりのバイト数は走行条件で104〜120Bの幅がある（`--stack-limit` を変えた2回の走行で一致しなかった）ため、上表の下限は桁の目安であり確定値ではない。

### 2.1 D10着地の実装値（2026-09-13、`vm/l2b`）

出荷値は変更なし: host 7 MiB（`--profile host`）、実機 `stack_limit = 20*1024`（`app_session.c`）。どちらも上表の窓の中。

`budget_probe.sh`（`--stack-limit 512M` でCスタックのガードを退け予算だけを残す走行）: 実測(o2) 10項目すべてOK。予算だけを残した走行でhostは深さ70,575、実機プロファイルは深さ195で `RangeError`、`seg_oom_boundary` は `InternalError` のまま。asanでも10項目すべてOK（asanはulimitを上げないため、hostの予算を512Kにして深さ5,039で `RangeError`）。

関所: コーパス43/43（asan・o2・asan-alloca・o2-alloca）、Test262 7,501/194/regressions 0、G1は528.000（segframes）/672.000（alloca）でともにPROPORTIONAL、selftest ok、`--force-yield` 3/40不変、期待値ファイル無変更。

未計測: 呼び出しごとの加算・比較コストの速度影響（`timing.py` はL2bのフラット化と合わせて計測、§3.3参照）。

## 3. L2b: フラット呼び出しの着地（2026-09-13、`vm/l2b`）

実装は `JS_CallInternal` の `flat_call:` ブロック、共有プロローグ、`JS_SF_FLAT` 復帰と `quickjs-vmstack.h`（`JSVMLink`、`JS_SF_*`、`JS_RET_*`）。スイッチは `CONFIG_POCKET_VM_FLATCALLS`。対象は通常のバイトコード関数への `OP_call`系（[vm-L2-design.md](vm-L2-design.md) §10）。

### 3.1 H7実験: モジュール本体の扱い（実測(host)、`-O1`、segframes）

「床の判定を `func_kind` に頼れるか」を確認する実験（コードは戻した）。結果: モジュール本体（`js_inner_module_linking` が hoisting 済み宣言の初期化のために通常呼び出しする `JS_FUNC_ASYNC` の関数）だけが非NORMALで通常経路に来ることを確認。**床の判定は `func_kind` に頼らずセグメントの生存範囲で行う**（結論は[vm-L2-design.md](vm-L2-design.md) §10 に反映済み）。

### 3.2 H5: 3経路の関所行列

| 経路 | `build.sh` | Kconfig | 何が違うか |
| --- | --- | --- | --- |
| alloca | `asan-alloca`/`o2-alloca` | `SEGFRAMES=n` | L2a以前。Cスタックにフレーム |
| segframes再帰 | `asan-recur`/`o2-recur` | `SEGFRAMES=y, FLATCALLS=n` | L2a。C再帰は残る |
| segframesフラット | `asan-flat`/`o2-flat` | `SEGFRAMES=y, FLATCALLS=y` | L2b |
| 無印 `asan`/`o2` | | Kconfigの既定を写す（フラットが既定） | 無印の関所=出荷経路の関所 |

### 3.3 費用と速度（実測(host)、`8833514`〜）

フレームごとにlink 4B（ホスト8B）。`JSStackFrame` は48Bのまま（`caller_ctx`/`l2_flags`/`ret_shape` は隙間に収まる。実機コンパイラの `_Static_assert` で確認）。

| 関所 | フラット | segframes再帰 | alloca |
| --- | --- | --- | --- |
| G1 `bytes_per_call`（2000/4000） | **0.000/0.000 NOT_PROPORTIONAL**、selftest ok | 528.000 PROPORTIONAL | 672.000 PROPORTIONAL |
| `budget_probe.sh`（o2・asan） | 10/10、出荷値で `budget_hits=1〜2`（host深さ57,342、device 158） | 10/10、`budget_hits=0` | 予算なし |
| コーパス（asan・o2） | 44/44 | 44/44 | 44/44 |
| Test262 | 7,501/194/regressions 0 | 同左 | 同左 |
| `--force-yield` | 3/41 | 同左、出力43件バイト一致・safepoint数一致 | — |
| G6 `verify_all.sh` | exit 0 | | |

既定onにした理由: (1) 上の関所がすべて通り3経路とも期待値無変更、(2) `stack_limit=20KiB` がフラットではJSの深さに対する予算だけを意味するようになった（§2の窓の中）、(3) 戻す手段が `CONFIG_POCKET_VM_FLATCALLS=n` の1行。

**呼び出し経路の速度差は検出できなかった**（実測(host)、`timing.py -n 20`、flat/recur/alloca を交互2周）。周回間のぶれ（最大約30%）が経路間の差より大きく、主張できない。同一バイナリでの切り替え比較（別ビルドの配置差と交絡しないもの）が無いと速度差は主張できない。実機の速度は未計測。

### 3.4 実機での比較: main と vm/main（実測(device)、2026-09-13）

main 3607e84（改変なし）と vm/main 983eabe（L2b、フラット既定on）。`benchmark_app.py`/`memlog.py --port`/`smoke_device.py`/`capture_home.py` で main→vm→main→vm の交互2周、効果音off。

| | main | vm/main | 差 |
| --- | --- | --- | --- |
| `idle_free` | 277,172 | 277,060 | −112B |
| `app_free`（hello実行中） | 120,980 | 116,600 | −4,380B |
| `app_largest`（最大連続ブロック） | 69,632 | 65,536 | −4,096B |
| `js`（JSヒープ使用量） | 86,356 | 90,523 | +4,167B |

- アイドル時−112Bは静的DIRAMの差（115,356→115,468）と一致。
- JSヒープ+4,167Bは§1.2の見積もり（常駐セグメント4,127B + `JSRuntime` +20B + 確保ヘッダ）とほぼ一致。
- **最大連続ブロックはCLAUDE.mdのtaffy段差（34ノード以上で59,296B）への余裕を10,336B→6,240Bに減らした**（§5.3で42/43適用後の値に更新）。

| `PAINT`（hello、ms、各周10サンプル） | main | vm/main | 差 |
| --- | --- | --- | --- |
| `turn_ms` | 1.656 | 1.708 | +0.05（+3%） |
| `kernel_ms` | 3.10 | 3.23 | +0.13（+4%） |
| `render_ms` | 7.69 | 7.67 | −0.02 |
| `send_ms` | 7.28 | 7.29 | 同じ |

`kernel_ms`（描画カーネルはL2a/L2bで一切未変更）が4%動いたのが陰性対照。`turn_ms` の+3%も同じ大きさで、CLAUDE.mdの「命令キャッシュ配置だけで15%動く」の床の内側 — コードの変化による差とは言えない。安定性: 両方とも `SMOKE_OK 20`、故障回復6種OK。未計測: 呼び出しが多いアプリでの差、深い再帰を含むアプリでの予算20KiBの効き方、同一バイナリでの切り替え比較。

### 3.5 同一バイナリ内のflat/recur速度比較（2026-09-16）

`CONFIG_POCKET_VM_CALLBENCH`（既定n、FLATCALLS必須・YIELD禁止）で、idle runtimeの
通常call/method callを既存の`JS_CallInternal`再帰fallbackへ切り替える口を追加した。
通常ビルドではフィールド・分岐・APIを前処理で除去する。これは同じflatインタープリタの
dispatch比較であり、コンパイル時にFLATCALLSを外したL2a全体との同一性は主張しない。
async第一同期区間もselectorの対象だが、速度測定は同期呼び出しだけ。

`tools/vmtest/callbench.c`をhost/device共通で実行。独立のPATH検査で17回のnative callbackを呼び、
live frame中の切り替え拒否とCフレーム位置を確認する。速度は4負荷×8組ABBA/BAABの128標本/run。
各blockはGC後に同じ関数を2回ウォームアップ、3回目の`JS_Call`だけを計測し、毎回戻り値を照合する。
コンパイル・GC・ログ出力は区間外。runtime標準allocator、ヒープ160KiB、実機Cスタック上限20KiB。
guest allocator・スケジューラ・描画を含まない。集計器は128標本の完全性・重複・順序・値・PATH・PASSを検査する。

実機は240MHz、`-Os`、YIELD/FAIR/PROBE=n、SEGFRAMES/FLATCALLS/CALLBENCH=y。
image SHA256 `9d90ff8e0a9bc4ba34514dd69ad364278ebcacb9656d633aa54fe547647245d7`、
app 2,145,264B、DIRAM 123,324B、flash 1,558,752B。ELFでselectorとbenchの残存も確認した。
COM3の同じimageで2回、計256標本。各組のflat平均時間/recur平均時間の中央値（小さい方が速い）は以下。

| 負荷 | 比率 run1 / run2 | flat時間中央値 run1 / run2 (ms) | recur時間中央値 run1 / run2 (ms) |
| --- | --- | --- | --- |
| callなしのloop 20,000回 | 1.00023 / 0.99985 | 93.011 / 93.005 | 93.002 / 93.009 |
| 通常call 20,000回 | 1.03997 / 1.04017 | 161.897 / 161.927 | 155.667 / 155.659 |
| method call 20,000回 | 1.03161 / 1.03256 | 228.380 / 228.538 | 221.311 / 221.354 |
| 16段再帰×1,000回 | 1.05414 / 1.05419 | 72.781 / 72.736 | 69.024 / 69.010 |

**実測(device): この同期小関数群ではflatは3.2〜5.4%遅い。高速化したとは主張しない。**
対照loopの組ごとの比率は2run全体で0.99792〜1.00074。
PATHの16段Cスタック増加は両runともflat 0B、recur 4,864B（304B/段）。
診断用分岐を含む今回imageの値であり、§4.11の別imageの288B/段と混ぜない。
flatの価値はC再帰依存を除くことと中断可能な鎖を作ることにあり、ここでは既定を変更しない。
総アプリ速度、async速度、異なる関数形状・最適化フラグへの一般化はしない。

実測(host): o2/ASanでPATH・戻り値・反復解放が成功（16段recur増加9,728/15,872B、flat 0B）。
host時間は対照loopも揺れ、速度結論には使わない。通常o2とcallbench既定flatは各67/67、
selector=recurのclosure/generator/promise/job重点9件はo2/ASan各9/9。
通常o2の`nm`にselectorなし、診断オプション指定は拒否。集計器の正常系と欠落等の拒否テストも合格。
生ログは`.cache/vmtest/callbench-{host,host-2,asan,device-1,device-2}.log`、
再現手順は`tools/vmtest/README.md`。実機比較後は元のapp領域を復元し書込hash一致、
通常smoke 3周・故障回復6種・`HOME_READY`まで確認した。NVS・storage・font領域は変更していない。

### 3.6 復帰時のcall inputs遅延復元の試作（2026-09-16、既定OFF）

`CONFIG_POCKET_VM_LAZY_INPUTS`を追加。flat returnで`argc/argv/this/new.target`を即時復元せず、
無効フラグだけ置く。読者は`push_this/special_object/rest/check_ctor/init_ctor`の5 opcode入口。
必要時に現在のsf・floor・async所有者・TCO保持スロットから復元してその命令へ直接戻る。
命令の再dispatch・debug dump・poll・確保・JS実行は増やさない。default引数のcall後にrest等を
生成する場合も復元する。通常push・flat async・TCO reuseは入力が既知なので有効化する。
VM resumeは既存の完全復元を維持。JSフレームやバイトコードの構造は増やさず、C activationにboolを持つ。

CALLBENCHにはruntime毎の即時復元対照を追加し、USB `U` / host `--inputs`でlazy/eagerを比較する。
同一image SHA256 `9c93f7818caae77904010da5a4c9a2e7a9ad0f212c55dfa40d0067ecca716f8a`、
app 2,145,888B、DIRAM 123,324B、flash 1,559,340B。240MHz/-Os、YIELD=n。
Kconfig追加後の単なるbuildでは古い設定が残ったため、reconfigure後のLAZY_INPUTS=yと
sourceより新しいengine objectを確認したimageだけを実機へ書き込んだ。
2run×128標本、各組のlazy平均/eager平均の中央値（§3.5と同じ手順）は以下。

| 負荷 | 比率 run1 / run2 | 時間短縮率 |
| --- | --- | --- |
| callなしloop | 1.00015 / 1.00020 | 有意な改善とは扱わない |
| 通常call | 0.98496 / 0.98522 | 1.50% / 1.48% |
| method | 0.98810 / 0.98829 | 1.19% / 1.17% |
| 再帰 | 0.96542 / 0.96605 | 3.46% / 3.39% |

実測(device): 16段のCフレーム位置差は両方式とも0B。これは復元処理が費用の一部である証拠だが、
flat全体の遅さの全原因を特定したものではない。旧imageとの時間の引き算はしない。
今回imageの`JS_CallInternal`は`entry a1,0x130`、再帰は`call8`、復帰は`retw.n`。
Cフレーム304Bを戻るたびコピーするわけではなく、VMの汎用的な状態再構築とは処理が異なる。

実測(host): 最終コードのo2-callbench/asan-lazyコーパス各68/68。
asan-lazyはYIELD/TCOも有効な実験変種。毎中断GCの重点8件に加え、64KiB VM予算のtco_guards
（686中断/再開）が成功、寿命・GC・terminate/discardは900ケース成功。
既存Test262部分集合4,099ファイルは7,501 pass/194 fail/0 skip、baseline退行0。
この試作時点では追加対照・通常buildの検証前なので既定n。後続の検証と既定yへの採用は§3.7。

検査系でも修正: 毎中断GCのtco_guardsを7MiB host予算で走らせると300秒timeoutになり、
旧run.shが無条件に「ASan startup hang」として再試行した。2回目を明示停止し、完走とは記録しない。
runnerのmain到達markerをstderrへ出す方式にし、到達後のtimeout/crashは再試行しない。
実際のrun.shをfake runnerで動かす5条件で分類を検査した。
64KiBでの再走は、深さを広げる検査ではなく、fallbackの予算拒否・解放を検査するもの。

生ログ: `.cache/vmtest/lazy-{device-1,device-2,host-final}.log`、
`lazy-{o2-corpus-final,asan-corpus-final,asan-yield-final,asan-tco-final,asan-marker,test262,lifecycle}.log`。
実機は元appを書込hash一致で復元し、smoke3周・故障回復6種・HOME_READYを確認。保存領域は変更していない。

### 3.7 遅延復元の通常構成検証と採用（2026-09-16）

実測(host): 診断・YIELD・TCOなしの`o2-lazy-flat` / `asan-lazy-flat`でコーパス68/68、
セグメント成長6方針×6負荷の36比較も両方成功。o2のG1は通常/asyncとも深さ2000/4000で0B/段、
D10予算検査は11/11。重点コーパスのtail呼出しをspreadから固定arityへ改め、実際にTCO reuseを
通る形にした後もeager/lazyの出力は一致し、asan-lazyの毎中断GC・64KiB予算で98中断/再開が成功。

固定Test262 `72faf8ec1445c55149615e8b35187830783aba1a`の追加範囲は
`language/{statements,expressions}/{function,class}`、`language/arguments-object`、`built-ins/Function`。
9,913ファイル、19,307判定を即時復元o2とo2-lazy-flatで直接対照し、両方19,259 pass/46 fail/2 skip。
全判定と失敗詳細を含む結果ファイルが一致（SHA256 `0872f9fa4a1b02c83eb456cede7eedd6b0fa6c38846a050473f0944c30690282`）。
既存baselineの範囲外なので「baseline退行0」だけを根拠にせず、対照ファイルの完全一致を確認した。
共通失敗とskipは残り、Test262全体の合格を意味しない。

実測(device build): 独立`build_vm_lazy_release`のSDKCONFIGでLAZY_INPUTSだけをn→yに変更。
両方SEG/FLAT=y、CALLBENCH/PROBE/YIELD/TCO/SELFTEST=n。root設定は変更していない。

| 資源 | 即時復元 | 遅延復元 |
| --- | ---: | ---: |
| app image | 2,143,456B | 2,143,584B |
| Flash Code | 1,557,652B | 1,557,784B |
| 静的DIRAM | 123,308B | 123,308B |
| JS_CallInternal Cフレーム | 304B | 304B |

ELFに診断用selector/benchmarkが含まれないことも確認。image SHA256は即時復元
`6514a93d0c951462fca284202671ce262b9e8babbd881e3b58b2062713701a63`、遅延復元
`db55575c449e67fde7873ae230105113cb1b4f75fe038aa16d7c0b29c43dd32a`。
COM3で両imageのmemlogを実測し、idle free269,232B、app free128,564B、largest79,872B、JS86,571Bで一致、予算内。
これは当該アプリの標本で、任意負荷の断片化の保証ではない。遅延復元版のsmoke20周・故障回復6種が成功。
10/20周の停止後free269,232B・largest147,456Bも一致した。

採用判断: §3.6の同一image速度対照、追加互換検査、静的RAM増加なし・Flash Code +132Bを根拠に
LAZY_INPUTSを既定yへ変更する。無印host変種も揃え、`-eager`で旧経路を検査可能に保つ。
既存SDKCONFIGの明示nは上書きしない。YIELD/TCO/FAIRの既定は変更しない。
別image間の速度比較はしておらず、flatがC再帰を上回ったとは主張しない。
採用後に無印o2とo2-eagerを再ビルドし、両方コーパス68/68を確認した。
検証後は元app（SHA256 `b7a41c57626e45b6ade42d7c752d0f2909c03eda75f582690b1774b87659f073`）を
書込hash一致で復元し、smoke3周・故障回復6種・HOME_READYを確認。app以外の領域は書き込んでいない。

ログ: `.cache/vmtest/lazy-release-{corpus,asan-corpus,g1,g1-async,budget,segments,asan-segments}.log`、
`lazy-extra-{eager,release}-results.txt`、`lazy-extra-{eager,release}.log`、`lazy-release-memory.jsonl`、
`lazy-{default,eager}-corpus.log`。

## 4. L2c: 中断・再開の進捗

L2cの本体は段3bまで部分実装済み（backlog.md #7）。以下は済んでいる段の結果。

### 4.1 段A: async関数のフラット化（[vm-L2-design.md](vm-L2-design.md) §11、実測(host)、`vm/l2c`）

段A1（挙動不変の下拵え）: 全関所不変（44/44、7,501/194/0、G1 0.000、予算10/10、`--force-yield` 3/44）。

段A2（本体、`flat_async_call:`/`async_flat_return:` 実装後）:

| 関所 | flat | -recur |
| --- | --- | --- |
| G1（async） | NOT_PROPORTIONAL | PROPORTIONAL |
| `budget_probe.sh`（`deep_async_recursion`） | `budget_hits=0` + OOM記録 + exit 2（エラー生成・catch到達も保証できない。§4.12） | `RangeError`（Cスタック検査） |
| Test262 | 7,501/194/regressions 0（不変） | 同左 |
| `--force-yield` | 3/45 | 同左 |

段A3（実機ビルド）: `_Static_assert` 通過、`memlog.py`、`smoke_device.py --cycles 20` 実施。§5.3のD42/D43適用前の中間状態のため、実機の`app_largest`余裕の値はD42/D43後の§3.4改訂値（6,240B）を参照。

深いasync再帰の実際の壊れ方（D40が固定した値）: flatで深さ77（1段約1.9KB、ホスト`--profile device`）でヒープが尽き、エラーオブジェクトを作れず理由`null`、巻き戻し中に23段のawait登録が失敗して未処理の拒否が残りexit 2。D42/D43適用後は深さ82・未処理26に変わった（§5.3）。

Stage Aの攻撃で見つかった二重解放（`JS_NewPromiseCapability` の2つ目のresolve関数の確保失敗で解放済み `resolving_funcs[0]` を再解放）は上流由来のバグとしてパッチ修正済み。報告書は `reports/upstream/` に保存（上流へは未送信）。

### 4.2 段1・段2: L2cの関所とガード（実測(host)、`8d025ff`/`0020773`/`21c12a4`）

`quickjs.c` は未変更。段1は `quickjs-vm.h/.c` にpass-through実装（`JS_VMCall`/`JS_VMEval` は `JS_Call`/`JS_Eval` そのもの、`JS_VMSuspended` は常に偽）。段2はコーパス15本を追加しyield無しでbless。

| 関所 | 結果 |
| --- | --- |
| コーパス8変種（段1/段2） | 47/47 / 62/62 |
| `--force-yield`（o2） | 3/59。失敗行は `safepoints_yieldable=1 stops=1 resumes=0: rule: bytes` |
| `budget_probe.sh` o2-flat/o2-recur/asan-flat | 11/11 ×3 |
| `stack_probe.sh` 2000 o2 | NOT_PROPORTIONAL、selftest ok |
| `oom_canary_probe.sh` o2/asan | 5/5 ×2 |
| Test262 asan | 7,501/194/regressions 0（4,099ファイル） |
| ファームのビルド（既定構成） | 通る |

### 4.3 段3a: ホスト所有SEG鎖の保存・再開（実測(host)、2026-09-16）

`CONFIG_POCKET_VM_YIELD`（既定n）を追加し、`JS_VMCall` / global の`JS_VMEval`から入ったSEG床と、その上のフラットSEG・最初の同期区間にいるflat asyncフレームを、分類Aの7地点で保存・再開する本体を実装した。yieldは例外を作らず、`cur_pc` / `cur_sp`と鎖を公開して`JS_EXCEPTION`を返す。再開時は同じフレーム鎖を`restart:`へ戻す。床は実行開始時に関数・`this`・`new.target`・渡された全引数を所有するため、yield地点での確保はない。モジュール本体はD17rどおり対象外。

停止中のSEGフレームは`JS_MarkContext`からmarkし、鎖の途中のflat asyncフレームは既存の`async_func_mark`へ分担する。最初の同期区間を終えたasyncフレームは、まだasync所有者側の再開囲いを実装していないため`MAY_YIELD`を落とす。

| 関所 | 結果 |
| --- | --- |
| o2-yield 通常コーパス | 63/63 |
| o2-yield `--force-yield` | 63/63。全対象で`safepoints_yieldable == stops == resumes` |
| 最大の強制再開回数 | `bench_loop` 6,000,001回、`bench_calls` 2,692,537回 |
| asan-yield 強制中断（寿命・例外・async混在の代表10件） | 10/10 |
| asan-yield `--gc-on-yield`（async混在、closure、深いSEG保持） | 3/3 |
| 既定offのo2コーパス | 63/63 |
| ESP-IDF 既定offビルド | 成功。app 2,143,040 bytes、最小app領域32%空き、DIRAM増加0 |

未実装は分類B、async/async-generatorが床になる再開、D36の保留ジョブ、Terminate/Discard、実機統合。したがって出荷既定はまだoffであり、本節はL2c全体の完了を意味しない。

Test262の読み違いを1件記録する: 作業ツリーの `.cache/test262` が固定リビジョンのcheckoutではなく欠けたコピーだったため、一時的に6,511/191という誤った基準を報告した。ジャンクションで繋ぎ直して7,501/194に復帰。**作業ツリーを作るときは `.cache/test262` もジャンクションで繋ぐこと**（[vm-branching.md](vm-branching.md) の既存の一覧に追加すべきもの）。

### 4.4 段3b: 分類Bとホスト鎖の終了・破棄（2026-09-16、`ab71b71`からの差分）

呼び出しのpush完了後にも強制yieldできるようにした。呼び出し前のwatchdogポーリングはそのままで、分類Bは二度目のwatchdogポーリングを行わない。`JS_VMTerminate`は次のresumeで捕捉不能な終了へ入り、エラーの確保もできないOOM時には専用ビットでcatch/finallyを迂回する。`JS_VMDiscard`は内側からSEGを解放し、ヒープ上のflat asyncフレームは持ち主へ返す。`JS_FreeRuntime`も同じ破棄経路を使う。停止中の`JS_ExecutePendingJob`は先頭を取り外す前に拒否する。

強制yieldの早期returnがwatchdogのカウント更新を飛ばしていた問題も修正。分岐だけの無限ループが9,999回のresume後にwatchdogで終了することを固定した。

| 関所 | 結果（host、ASan/UBSan） |
| --- | --- |
| コーパス `--force-yield` | 63/63、全対象で中断数と再開数が一致 |
| `lifecycle.sh asan-yield` | 22中断位置×5モード=110成功、watchdog検査も成功 |
| 5モード | 再開、Terminate、Discard、runtime解放、OOM下のTerminate。各中断でGC、キュー拒否と保持、捕捉変数の寿命を検査 |
| Test262 通常/強制yield | 両方7,501成功・194既知失敗、4,099ファイル、退行0 |
| 既定offのo2コーパス | 63/63 |

実機コンパイラのELF型情報: `sizeof(JSRuntime)=344`、`JSStackFrame=48`、`JSAsyncFunctionData=104` bytes。終了フラグはoffset 173で、入口トークン172と床の値176の間の詰め物に収まる。フレームは増えていない。yield時とDiscardは確保を行わない。async/async-generator所有床、保留job、要求ビットと実機スケジューラ接続は次工程。

**実測(device、ESP32-S3、240 MHz、ESP-IDF 6.0.1):** `CONFIG_POCKET_VM_YIELD=y, POCKET_VM_SELFTEST=y`、独立した`build_vm_l2c_selftest/sdkconfig`でビルドし、COM3の実機へアプリ領域のみを書き込んで確認した。

| 関所 | 結果 |
| --- | --- |
| USB `L`（同じ`lifecycle.c`） | 110ケース×3周すべて成功、watchdogも3周成功 |
| 各周の空きヒープ | 全周とも前後269,320→269,320B。終了後の最大連続ブロック163,840B |
| 検査時間 | 5,861,834 / 5,861,447 / 5,872,546 µs（runtime生成と検査内の待機を含む。VM速度比較ではない） |
| 通常アプリ smoke | 起動・終了20周、故障回復6種すべて成功。10/20周目の空き269,232B、最大連続147,456Bで一致 |
| 実行中メモリ（`memlog --check`） | `idle_free=269232 app_free=128524 app_largest=79872 js=86603`、予算内 |
| hello `PAINT` 10標本の平均 | `turn_ms=1.82 render_ms=7.91 kernel_ms=3.19 send_ms=1.94`（sound設定変更なし、速度差の主張には使わない） |
| 検証ビルド | app 2,147,328B、DIRAM 123,308B、領域32%空き |
| 既定offビルド | app 2,143,024B、DIRAM 123,308B、成功。`JSRuntime=312B`なのでyield有効時+32B。off時はDiscard呼び出しもコンパイルから除外 |

元の実機は別系統のKasane版（image version `vm-pre-stack-74e704d-131-g0ad90`）だった。変更前もsmoke20周・故障回復6種は成功。描画系が異なるため、この実機との差をエンジンの性能差・省メモリ効果として扱わない。検証前にアプリ領域3MiBを読み出し、image checksum/hashと実機digest一致を確認して保存した。stub経由のreadが途中で止まったため、ROM経由の`--no-stub`で読み出し・書き込みを行った。

検証後は保存したアプリを復元し、書き込みhash照合、Kasaneのsmoke3周と故障回復6種、`HOME_READY`まで確認した。設定・辞書・フォント・storage領域は書き換えていない。実機で検査したselftestバイナリのSHA256は`8a55812544e031579c2c1349adac71d2242bdf51bee2fec9392a1ff968ae75d4`。

### 4.5 L2c: async所有床と保留job（2026-09-16）

async/async-generatorのheap所有床を保持・再開・破棄できるようにした。Promise reaction、thenable、microtask、FinalizationRegistryのjob入口を囲い、完了tailとauxを必要な間だけruntimeで所有する。jobを再enqueueせずFIFOを維持する。async handlerの初期区間も中断できるため、先に返されたPromiseをtailまで保持する（独自speciesのresolveがJSである場合にも対応）。内部async generator継続に挟まるnative frameは、実際の呼び出し元が無い時だけ一時的に外す。

`JS_ExecutePendingJob`の保留戻り値2はschedulerで未完了として扱い、resumeとtailの完了後に一度だけ数える。検査中、`vmrun`の旧受け口が中断のたびにジョブ予算をリセットし、停止指定を越えて実行する不備を検出して修正した。FAIRの仮想ホストイベント境界もopcode中断数ではなく従来の予算区切りに維持する。既存期待値は変更していない。

ホストの寿命検査は7所有形態×22〜23地点×5モード、計790ケースに拡張し、ASan/UBSan下ですべて成功。GCを各中断で実行するasync/held-job重点検査12件も成功。全コーパス強制yieldは互換順序・FAIRとも64/64、通常と強制yieldのTest262はいずれも7,501成功・194既知失敗、退行0。既定offのo2コーパス64/64。追加の`yield_job_tails`はspecies、thenableのresolve後throw、reject、async generatorの連続要求とfinallyの出力をoff版で固定した。全コーパスへの毎中断GCは大規模benchmarkの費用が大きいため途中で止め、重点検査と全地点寿命検査でGCを行った。

**実測(device):** COM3へアプリ領域のみ書き込み、同じC寿命検査790ケース×3周=2,370ケースとwatchdog検査3周すべて成功。各周とも空き269,320→269,320B、最大連続159,744B。所要46,308,916 / 46,341,436 / 46,309,214µs（検査内の待機を含み、VM速度ではない）。通常アプリsmoke20周と故障回復6種も成功し、空き269,232B・最大連続147,456Bは10/20周で一致。`memlog --check`: idle269,232、app128,500、largest79,872、JS86,635B、予算内。

ELF型情報は`JSRuntime=376`、`JSStackFrame=48`、`JSAsyncFunctionData=104`B。runtimeは段3b比+32B、yield無効比+64Bで、フレームあたりの増加なし。実機検査imageはSHA256 `7e3c8139b78c9867f4c3ba01bca9da63e329f6acda68903b51de062bfd6477f4`、app2,149,216B、DIRAM123,308B。これはschedulerの戻り値2接続修正前のimageであり、直接VM APIを使う寿命検査と通常アプリを測ったもの。scheduler接続修正はホストで検査し、実機ゲストの時分割接続は引き続きbacklog #11。既定offビルドも成功（app2,143,120B、DIRAM123,308B）。

実機の変更前imageが保存済みbackupと一致することをdigest照合した上で検査を開始した。検査後は元のKasane版アプリを復元し、書き込みhash一致、smoke3周・故障回復6種、`HOME_READY`を確認。設定・辞書・フォント・storageは変更していない。

### 4.6 D27: 別スレッドからの中断要求（2026-09-16）

`JS_VMRequestYield` / `JS_VMClearYield`を追加。要求のatomic byteだけをproducerが操作し、A/B境界で受理する。禁止床では消さず、実際に中断した時に合流した要求を消す。通常整数の`interrupt_counter`をtimerが変更する当初案はdata raceになるので撤回し、カウンタのatomic RMW化も避けた（design §11.8）。要求フラグはruntime offset155のpaddingに入り、`sizeof(JSRuntime)=376B`のまま。実機逆アセンブルでRequest/Clearは`memw`＋`s8i`、ロック関数呼び出しなし。通常A地点にatomic loadが追加される費用の同一バイナリ比較は未実施で、性能向上は主張しない。

ホストASan/UBSanで、要求の合流・Clear・native map callbackでの保持を検査。JS実行中にpthreadから要求を送る10ケースも成功。寿命検査790ケース、強制yieldコーパス64/64、既定offのo2コーパス64/64、強制yield Test2627,501成功・194既知失敗・退行0。既定offの実機ビルドも成功（app2,143,120B、DIRAM123,308B）。

**実測(device):** ESP one-shot timerから、既に実行中の無限ループへ要求を送って中断し、その後Terminateする検査10回×3周=30回すべて成功。要求API単体検査と寿命検査790×3も成功。全周とも空き269,320→269,320B、最大連続159,744B。総所要45,970,243 / 45,953,387 / 45,947,522µs（待機込みで速度比較不可）。image SHA256 `99fae8df5eb581c4c3e7985b4596c73f0a64ae5fb4beb2d3e8dc42c7f9d563ef`、app2,150,928B、DIRAM123,308B。

通常アプリsmoke20周・故障回復6種も成功。`memlog --check`はidle269,232、app128,500、largest79,872、JS86,635Bで予算内。FAIR強制yieldも64/64、通常Test262も7,501/194・退行0。検査後はdigest確認済みの元Kasane版アプリを復元し、書き込みhash一致、smoke3周・故障回復6種、`HOME_READY`を確認した。

これは要求の配送とエンジン受理の検査であり、通常アプリのターンtimer・guestの再開状態・Back停止処理の接続はまだ含まない（backlog #11）。

### 4.7 D24r/D28r: guestの再開・実行ターンtimer（2026-09-16）

guestに`suspended`とorigin（FRAME/JOB_HELD/JOB_ASYNC）を追加し、`work_pending`で継続を選ぶ。frameの呼び直しを拒否し、保留jobはtail完了後に一度だけ数える。FAIRも中断中はpumpしない。frame/continue APIの内側だけで残り予算のone-shot timerを張り、出口で解除する。callbackはguestポインタを保持せず、共有runtimeスロットのロックで解放競合を避ける。Backターンはtimerを無効化し、`pocket_app_reset`は中断鎖を先にTerminate/Resumeする。Cスタック不足でasync ownerの再開が拒まれた場合はDiscardで閉じる。

累積frame時間と従来250ms相当のguardを追加（閾値の調律実測は未完了）。drainの締切と実行時間を分離した。従来の`budget.elapsed`はターン開始からを測り、frameやresumeを含んでいたので、call開始からへ訂正して二重計上を防いだ。`sched_clock.c`で先行処理を除くこと・32bit wrap・count modeでclockを読まないことを固定した。

**実測(device、互換順序、yield有効):** guest APIを使う4起点×4モードの16ケース×3周、100µs timerでの完走/leave相当/途中停止各3周、既存790ケース×3周とproducer/watchdog検査が成功。100µsで2万反復を完走する再開数は1,211 / 1,215 / 1,224回。leave相当でtimerを止めた後は全周1回で完了し、finallyは1回。各周の空き269,288→269,288B、最大連続159,744B。総所要48,367,648 / 48,357,622 / 48,376,749µs（待機・GC込み、速度比較不可）。初期検査の1千回上限では16,421反復・累積109,609µsで止まったため、進捗を確認した上で上限を1万回へ変更した。期待する最終値と副作用は変更していない。

検証image SHA256 `9d08a4af17ffe59d49698b3922e387ec3f62fe0a46875c8f9123dc497ea09ad9`、app2,155,760B、DIRAM123,340B（前段比+32B、runtime slot/lock/deadlineの実サイズ20Bと整列）。ELF型情報: guest168B、yield無効guest152B、ESP timer本体32B（システムallocator分は別）。timerは初回の実行ターンで1つだけ作り、guest終了時に解放。`JSRuntime=376B`とフレーム48Bは据え置き。

通常アプリsmoke20周・故障回復6種成功（frame無限ループとjob無限連鎖を含む）。空き269,200B・最大連続147,456Bは10/20周で一致。`memlog --check`: idle269,200、app128,452、largest79,872、JS86,635Bで予算内。

**実測(device、FAIR、yield有効):** 独立した`build_vm_l2c_fair`でguest16ケース、timer3モード、既存790ケースとproducer/watchdog検査が成功。timer完走は1,298再開、leave相当は1再開。空き269,288→269,288B、最大連続159,744B、所要48,645,058µs（速度比較不可）。smoke20周・故障回復6種成功、10/20周の空き269,200B・最大連続147,456Bは一致。`memlog --check`も互換順序と同値で予算内。image SHA256 `0e3f6162a5e4de0f65a3d3d08130f5c9729f769d2e17d48ba7d8656032235674`、app2,155,824B、DIRAM123,340B。

検証後、保存済みの元Kasane版アプリ3MiBのみを0x10000へ復元し、書き込みhash一致、smoke3周・故障回復6種、`HOME_READY`を確認した。パーティション表・NVS・フォント・保存領域は書き換えていない。

ホストの強制yieldコーパスは互換順序・FAIRとも64/64、Test262強制yield7,501成功・194既知失敗・退行0、既定off o2コーパス64/64。既定offのfirmwareもビルド成功（app2,143,424B、DIRAM123,308B）。実際のBackによる保存/stop-hook順序の専用検査、H14、閾値調律、既定yieldを有効化する総合関所は残る。

### 4.8 Back／stop hookのホスト経路検査（2026-09-16）

SELFTEST専用USB `M`を追加。通常frame・保留Promise job・asyncの3起点について、初回`app_tick(0)`後の中断を必須にし、実物の`app_tick(0x2000)`→`app_request_stop()`→`app_stop()`を通す。再開本体、Back内の保存相当マーカー、そのPromise完了、stop hook、そのPromise完了をC側の記録で順序比較する。期待値は`123456`。Backターンなしで中断中に停止する対照では`156`を要求する。診断はホーム・非実行時に限定した（既存`L`も同様）。通常ビルドには追加コードを含めない。

**実測(device):** 互換順序・FAIRとも6ケース×3周、全件成功。両構成とも初周の空き269,272→269,184B、2/3周は269,184→269,184B。初回88B差の原因は未特定であり、完全な無リークの証明とはしない。実際のNVS保存や物理キー入力は未検証（本検査はNVSへ書かない）。これらを本検査の成功で閉じない。

検証image SHA256: 互換`57e392b360f4535adfa46d56dfb65b2eb594679fddf0c1b8a480770e1c7e0b3d`（app2,156,848B）、FAIR`9efa1ff99db0afcc643f184295207d39fe1da7f41d38daba6338b203a1b3eeef`（app2,156,912B）。DIRAMは両方123,356B、記録用static4Bと整列を含む診断専用増分16B。

既定off構成もビルド成功し、DIRAM123,308B・flash1,557,616Bは前段と同値。検証後は元Kasane版アプリを復元し、書き込みhash一致・smoke3周・故障回復6種・`HOME_READY`を再確認した。

### 4.9 H14: 中断時のフレーム保持量（2026-09-16）

実際のpark地点に計測を追加。`susp_bytes_max`は生存SEGフレームの整列済み容量、`susp_async_frames`は鎖の非SEGフレーム数の独立した最大値。総ヒープ保持量ではない（定義はdesign §11.7）。検査をarmしたときだけ追加走査し、検査用`JSVMState`は24B増。runtimeフィールドは増やさない。

**実測(host/device):** 同一の7起点×5終了モード・790地点で全件成功。各起点は5モードとも同じ最大値だった。

| 起点 | host SEG容量(B) | device SEG容量(B) | 非SEGフレーム数（両方） |
| --- | ---: | ---: | ---: |
| 同期tree→async middle | 1,224 | 644 | 1 |
| await後→tree | 1,080 | 568 | 2 |
| async generator→tree | 1,080 | 568 | 2 |
| Promise handler→tree | 1,224 | 644 | 1 |
| async handler→tree | 1,080 | 568 | 2 |
| thenable→tree | 1,240 | 652 | 1 |
| microtask→tree | 1,208 | 636 | 1 |

同期無限ループの対照ではasync数0、park数と再開数9,999の一致を要求。ASan/UBSanの全コーパス強制yield64/64成功、レポートのpark数＝再開数も64件で一致した。実機ではguest16ケース、timer3モード、producer10回も成功し、空き269,272→269,272B、最大連続159,744B。全検査所要47,756,305µs（待機・GC込み、速度比較不可）。

実機image SHA256 `558f84f5695671978d34c8b90233346b4286cd0a86663edf506e0816a59eab31`、app2,157,584B、DIRAM123,356B（前段と同値）。既定yield無効ビルドも成功し、app2,143,424B・DIRAM123,308Bは同値、mapのflashは1,557,624B（+8B）。この検査鎖の値を一般アプリの分布や最適なセグメントサイズの根拠とはしない。実アプリの分布・外部断片化・速度比較は別途残る。

検証後は保存済みの元Kasane版アプリを復元し、書き込みhash一致、smoke3周、故障回復6種、`HOME_READY`を確認した。永続データ領域への書き込みは行っていない。

### 4.10 ジョブから本体へのネイティブ経路監査（2026-09-16）

vendored `quickjs.c`の`JS_EnqueueJob`全呼び出しを列挙した。内部のjob種は次の5種で、単なるCフレームを安全な床と取り違えないことを確認した。

| job種 | 本体への経路／中断の扱い |
| --- | --- |
| Promise reaction | `JS_VMCallJob`。通常JS handlerは保留tail、async handler初区間はheap owner＋保留tail、内部await継続はownerに委譲 |
| thenable resolve | `JS_VMCallJob`。then本体を保留し、resolve/rejectの後処理は中断禁止 |
| microtask | `JS_VMCallJob`。本体終了までjobを保留 |
| FinalizationRegistry | `JS_VMCallJob`。cleanup callbackの本体を保留 |
| dynamic import | `JS_LoadModuleInternal`→`JS_EvalFunction`→module実行。初区間は入口トークンなし。await後はPromise reaction経由のasync ownerで再開 |

`js_async_from_sync_iterator_next`はCのmagic function入口なのでトークンを失効させ、iteratorのnext/return/throw・done/value getter・PromiseResolveを含むC後処理中はparkしない。unwrapは新規iterator resultを作るだけ。内部async generatorのawait継続だけは「native frameの下に呼び出し元が無い」と確認して外す既存の専用経路を通る。通常のgenerator.next、module依存の完了callback、外部Cからの再入はnative境界を残したままなので中断不可。この監査は組込みjob経路の確認であり、外部埋込み側が独自に登録するjobの保留tailを自動生成するものではない。

`yield_async_from_sync`を追加し、通常完了・break・next/getter例外・thenable拒否・不正return値・async generatorのyield*からのreturn/throwを固定した。非中断o2基準とASan/UBSan強制yield、FAIRで出力・終了コードが一致。558地点でpark/resumeし、毎中断GCでも成功。不正return値は基準版でも未処理拒否を報告してexit=2になるため、その既存挙動を保存した（規格適合性の修正ではない）。

`yield_dynamic_import`と`.mjs` fixtureは同一moduleの二重import、評価1回、トップレベルawaitを検査し、204地点の強制yield・毎中断GC・FAIRでも基準と一致した。この工程ではエンジンの経路は変更していない。実機には書き込まず、前工程で復元した元ファームウェアのまま。

現ソースでo2を再ビルド後、全コーパスはo2・ASan/UBSan強制yieldとも66/66成功。FAIR・毎中断GCは新規2ケースを重点検査した。design §1.1の古い「L2c本体待ち」表記も実績と残件に訂正した。最大中断遅延の実機測定・通知なし性能比較・断片化は依然未完了である。

### 4.11 スタックHWMの意味とG1実機対照（2026-09-16）

IDF v6.0.1 `FreeRTOS-Kernel/tasks.c`の`prvTaskCheckFreeStackSpace`は、タスク作成時の`0xa5`塗りが残る領域を走査する。`window_reset()`は集計変数を戻すだけで、タスクのスタック塗りを戻さない。Xtensaの`StackType_t`はuint8なので、この構成の値はbytes。従来の`stack_hw_min`はUIタスク生涯の最小余裕であり、各アプリの使用量ではなかった。

ログ互換性のため旧欄を残し、STATICに`stack_scope=task_lifetime stack_unit=bytes`を追加。深さサンプルは`__builtin_frame_address(0)`による現在のC計測関数のフレーム位置`depth_fp`も記録する。絶対位置そのものは空き容量ではなく、同一呼び出し列の深さ間差だけを比較する。追加配列はprobe専用64B。通常ビルドのUIタスク32768Bは変更しない。

**実測(device):** `build_vm_stack_flat`と`build_vm_stack_recur`は独立sdkconfigでPROBE=y、SEGFRAMES=y、yield無効。後者だけFLATCALLS=n。再起動なしで`B deep_recursion`を各2回、base条件・4秒ずつ採取。

| 構成 | 深さサンプル | Cフレーム位置の全幅 | 隣接深さ間の増分/段 | 2回目のHWM |
| --- | --- | ---: | ---: | --- |
| flat | 1,2,4,8,16,32,64,128,256 | 0B（両回） | 0B（両回） | 全地点23,788B |
| recur対照 | 1,2,4,8,16,32,64 | 18,144B（両回） | 288B（全区間・両回） | 全地点8,348B |

recurの初回HWMは23,788→21,340→12,124Bと下がったが、2回目は深さに関係なく8,348B。現在のCフレーム位置は両回とも同じ288B/段なので、過去の使用履歴がHWMの読みを隠すことを実証できた。flatのゼロ幅は対照で感度を確認した上でのG1追認である。この負荷は構成ごとに限界深さを探索して実行深さを変えるため、frame所要時間を速度比較に使わない。

採取開始時に古いHOME_READYを拾って最初の試行が欠けたログは除外。USB-open待ちを1.5秒にし、q前に入力バッファを空にした。`vm_stack_report.py --curves 2`は1回しかないログを実際に拒否し、完全なflat/recur各2回は成功。解析の単体検査3件も成功。完全ログは`.cache/vm/stack-flat-complete.jsonl`と`stack-recur-complete.jsonl`。

image SHA256: flat `70d945090f30a2b534192c146f8de320a6844939050e618b85478ec9005cee5d`（app2,152,816B）、recur `be0d92f49dde2fab8038d232b2dd4f314d20450996b7f51a4d3a4fbdd1a441df`（app2,151,232B）。DIRAMは両方129,372B。recurはsmoke1周・故障回復6種も成功した。これはスタック回収可能量の証明ではなく、解析・native再入・割り込み等を含むUIタスク全体の予算を縮小する根拠にはしない。

検証後は元Kasane版アプリを復元し、書き込みhash一致・smoke3周・故障回復6種・`HOME_READY`を確認した。

### 4.12 async再帰のガードとTest262対照の再検証（2026-09-16）

現行ソースから`build.sh o2-recur` / `build.sh o2`で再ビルドし、各`budget_probe.sh`は11/11成功。既存の変種別期待値が既に終了条件を固定していたため、未着手のまま残っていたbacklog #4を整理した。hostの`--profile device`による制限模擬であり、実機の深さではない。

| async再帰 | C再帰版 | flat版 |
| --- | ---: | ---: |
| 到達深さ | 15 | 82 |
| budget_hits | 0 | 0 |
| OOM記録回数 | 0 | 54 |
| 未処理拒否 | 0 | 26 |
| 安定出力 | sync-try none / caught RangeError / exit=0 | sync-try none / exit=2 |

flat版では今回も外側catchの出力自体がない。ヒープ枯渇由来という判定は確保を伴わないOOM記録に依存し、`InternalError`や`null`がcatchへ届くとは主張しない。深さ・OOM回数・拒否数は観測値で、期待値として固定しない。

Test262は固定revision `72faf8ec1445c55149615e8b35187830783aba1a`の既存subset（4,099ファイル）を両版で実行。各7,501成功・194既知失敗・0 skip・baselineからの後退0。結果ファイルは`.cache/vmtest/test262-results-o2{,-recur}.txt`。これは64MiBヒープ/7MiBスタックの意味論検査で、デバイス制限の検査を代替しない。async関数・async generatorの宣言/式、async arrow、awaitの取得済みディレクトリには`recurs|RangeError|stack.?overflow`の大文字小文字を無視した検索一致なし。ただしcheckoutはsparseであり、クラスのasyncメソッド等の未取得範囲を含め「Test262全体に依存テストがない」とは結論しない（backlog #5）。

### 4.13 Test262全体からのasync・再帰関連監査（2026-09-16）

§4.12の未取得範囲を展開し、同じ固定revisionの全53,582 JavaScriptテスト（`_FIXTURE`除外）からasync/await本文・async機能名、または再帰/スタック深さ制限の文言に一致する8,332ファイルを抽出。ファイル名やディレクトリに依存させず、クラス/privateメソッド、オブジェクトメソッド、stagingも含めた。`async_audit.py`がrevision・追跡ファイルの無変更・非sparseを検査し、Git archiveを展開せずストリームで読む。WSLからWindows上の全小ファイルを個別stat/readする初版はI/O待ちのためテスト開始前に終了し、同じ対象の連続読み取りへ切り替えた。

実測(host、`--profile host`、既存ハーネス): `o2-recur` / `o2`とも**14,340成功・1,359失敗・98 skip、全15,797判定の種類と詳細に差0**。共通失敗は新たな成功として扱わず記録する。skipは既存ハーネスが提供しないfeature/flagで、未実行の意味。既存subsetのbaselineは書き換えていない。再帰等に言及する267候補も選択に含む。全体の`stack.{0,30}(overflow|limit|depth)|maximum.{0,20}stack`検索はURIErrorテストのStack Overflowサイトへの参考リンクだけで、async名のファイルの`recurs`は構文規則の説明だった。文言検索だけでなく、選択ケースの実行比較を根拠として、このrevisionの監査ではasync再帰制限変更の影響を検出しなかったと結論する。無言の深さ依存や任意プログラムの不在証明ではない。

再現: `git -C .cache/test262 sparse-checkout disable`後、両版をビルドし`python3 tools/vmtest/async_audit.py --output .cache/vmtest/async-audit.json -j 8`。抽出条件の単体検査3件成功。同一変種を2回比較する誤用は非0で拒否。JSONには全対象名、再帰候補、個別判定、差分、バイナリSHA256を保存。今回のrecur `4638ad71b6975278890acead17be84c61099ffb3187f74efe34b73fee85ee061`、flat `806d37cfe5226f9fbf4e020d0658f4b1bd5c1fd13165e4bc4b23669aefbc0460`。実機コード・既定設定はこの工程では変更していない。

### 4.14 継続ターンのメモリ採取と出力（2026-09-16）

backlogのL1範囲外項目4を現行ソースで再監査。`app_tick`とoverlayの継続経路には既に`vmprobe_continuation_sample`が接続されていたが、採取周期のcounterがセッション間で引き継がれ、継続だけが長く続くとwindowを出力しない穴が残っていた。セッション開始/終了で周期をリセットし、継続経路でも1秒経過時にwindowを出力する。frame/call/drainの時間やjob数を継続ターン用に捏造せず、従来どおりframe側で回収する。メモリは8ターンごとの低頻度標本であり、実行中の真の最大値の保証ではない。

採取元を判別できる`frame_heap_n` / `continuation_heap_n`をwindowへ追加。診断用counterは8B、全体のDIRAM増分は整列込み16B。通常のPROBE無効ビルドには追加しない。hostのログ読取は旧形式にも対応。`test_vmprobe_continuation.py`は実際のC関数本体を偽時計と採取/出力stubで動かし、8回周期、継続だけの1秒出力、window境界での周期保持、セッションリセットを検査する。メモリだけのwindowの読取を含む2件と既存stack読取3件が成功。allocatorや実機時計をstub検査で保証したとは扱わない。

独立`build_vm_stack_flat`（PROBE/SCHED/SEGFRAMES/FLATCALLS=y、YIELD/FAIR=n）をビルドしCOM3でF/base（async_generator）を8秒×2回、各回再起動して採取。image SHA256 `184d09d20c339c945a360a0309e90433a392a883e58c3d8b8a63c2a2e9618fcc`、app2,152,960B、DIRAM129,388B、flash1,560,660B。生ログ`.cache/vm/continuation-memory.jsonl`は各8window、通常採取16回ずつ、継続採取87/86回、drainrun drop=0。全windowで継続採取を確認し、js_used最大89,415B、free最小121,692B、largest最小79,872B。要約器は既存仕様で各回のseq0を除くため14windowと表示する。今回のFはframeも進むため、frames=0の継続専用windowの出力境界はhost検査の根拠に限る。旧版との性能比較や512/4096Bの最適性の証明には使わない。

検証後に元Kasane appを復元し、書き込みhash一致、起動3周・故障回復6種・HOME_READYを確認。変更対象はapp領域のみで、NVS/storage/パーティションは書き換えていない。

### 4.15 USB Back入力と再起動を跨ぐ保存（2026-09-16）

SELFTEST専用のUSB `Y`（書込待機）/`Z`（読取・後片付け）を追加。
`device_back_storage.py --stage write`は起動後にUSB `q`を送り、`usb_stroke`→`KEY_BACK`→
通常のアプリ入力処理→`app_tick(0x2000)`を通す。専用owner `vm.back.selftest.20260916`の
`back-save-20260916`だけを使い、存在確認と`ifRevision:0`で既存キーへの上書きを拒否する。
読取側は値全体とrevision=1が一致した場合だけ診断キーを削除し、不在まで再確認する。
通常buildに追加のコード・入力割当は含まれない。プロトコルのホスト単体検査5件も成功。

実測(device、互換順序、YIELD/LAZY_INPUTS=y): 専用image SHA256
`f8bb9d0746a14eb580ae4e208e5438ea2c17ba608bd47f6219a3a41f084220fa`、
app2,159,072B、静的DIRAM123,356B、Flash Code1,567,192B。
初回writeで待機/Back/保存Promise完了/stop hook/そのPromise完了の`1,2,3,4,5`が順番どおり。
続いてesptoolのROM接続・hard resetを実行し、起動後の新guestから読取/削除/不在の`6,7,8`が成功。
電源断耐性や物理キーボード走査の検査ではない。また、このwrite負荷は中断中であることを要求しない。
中断中の順序は別の既存USB `M`で同じimageの3起点×2終了方式×3周、全18ケースを再確認した。
Mの初回free269,240→269,152B、2/3周は269,152→269,152Bで、従来の初回88B差は引き続き未帰属。
この初回時点ではUSB入力・中断・実保存を同時に組み合わせた検査は未実施（下記で追加）。
FAIRでの実保存は後続の下記検証で確認。物理入力確認は残る。

通常アプリsmoke20周・故障回復6種成功、10/20周のfree269,152B・largest147,456Bが一致。
memlogはidle269,152B、app128,388B、largest77,824B、JS86,635Bで予算内。
診断キーは削除済み。NVS namespaceメタデータは残りうるが、パーティション消去はしていない。
ログは`.cache/vmtest/back-storage-{write,reboot,read,smoke,suspended}.log`、`back-storage-memory.jsonl`。
最後に元app（§3.7の退避SHA256）を書込hash一致で復元し、smoke3周・故障回復6種・HOME_READYを確認。
復元時のログは`back-storage-restored-smoke.log`。診断キーの削除以外の保存データは変更していない。

**複合条件の追加検証:** `Y`の通常frameを診断専用の明示yieldループにし、実際にparkした後の
marker10を待ってUSB Backを送る。受信時にもsuspendedを必須にしてmarker11を出し、
UIタスク専用Cフラグで待機を解除してから通常leave再開処理へ進む。JSへ再入して解除しない。
ホストprotocol検査は、中断確認欠落・中断なしBackの拒否も含む7件に拡張し成功。
実測(device、互換順序): image SHA256 `0a6aa7c1e6112fa68d9e520eaf9539f16f3cc01bcf5b30f3376fd57a04af3390`、
app2,159,440B、DIRAM123,356B、Flash Code1,567,448B。
`1,10,11,2,3,4,5`の順序、hard reset後の`6,7,8`が成功。診断キー削除済み。
通常アプリsmoke20周・故障回復6種も成功（10/20周free269,152B、largest147,456B）。
ログ`.cache/vmtest/back-parked-compat-{write,reboot,read,smoke}.log`。
これはFRAME起点の明示yieldとUSB入力の複合検査であり、timer中断・全起点・物理キーを含む検証ではない。
複合検査後も元appへhash一致で復元し、smoke3周・故障回復6種・HOME_READYを確認した。

**FAIR構成での複合検査:** 同じソースの独立SDKCONFIGだけをFAIR=yへ変更して実施。
image SHA256 `8269a67749eabf9a0473c8f37c690a00a7846fb3db531ff8945d7d9385829d73`、
app2,159,504B、DIRAM123,356B、Flash Code1,567,516B。
`1,10,11,2,3,4,5`の順序、hard reset後の`6,7,8`が成功し、診断キー削除済み。
smoke20周・故障回復6種成功（10/20周free269,152B、largest147,456B）。
memlogはidle269,152B、app128,388B、largest77,824B、JS86,635Bで予算内。
ログ`.cache/vmtest/back-parked-fair-{write,reboot,read,smoke}.log`、`back-parked-fair-memory.jsonl`。
FAIRの既定nは変更していない。物理キー確認は引き続き未実施。

### 4.16 暴走ガードの停止時間採取（2026-09-16、調律は継続）

`device_runaway.py`を追加。USB `3`の無限frameと`6`の無限job連鎖を各3回実行し、
対応する起点のRUNAWAY報告が1件だけあることとAPP_STOPPEDを必須にして生ログとJSONを出す。
抽出器は起点違い・欠落・重複を含む5件のホスト単体検査で確認した。
§4.15の最終互換/FAIR imageを使い、250,000µsの定数は変更していない。

| 実測(device) | frame累積µs（3回） | drain累積µs（3回） |
| --- | --- | --- |
| 互換 | 256899 / 256857 / 256895 | 250377 / 250531 / 257673 |
| FAIR | 257016 / 256933 / 256984 | 257301 / 257117 / 258315 |

frameの超過は6.86〜7.02ms、drainは0.38〜8.32ms。8msターン後の検査という構造と整合するが、
あらゆる負荷の最悪停止時間を保証する値ではない。host側の送信開始→APP_STOPPED受信は391〜422msで、
USB・起動・表示待ち・終了を含む。firmwareの累積時間も実行区間の経過時間で、プリエンプトを除いたCPU時間ではない。
本採取は通常ホームから開始し、背景mode1/DEMO・音声通知等が動く条件。統制した同一image内の
速度比較ではないので、FAIR/互換の速度優劣やjob件数あたりの性能には使わない。
両構成とも全6回で終了を確認。正常な長いframeの閾値近傍・負荷競合・誤停止の検査は未実施で、
これだけで閾値調律を完了扱いにはしない。ログ`.cache/vmtest/runaway-{compat,fair}.log`。
FAIR保存検査と本採取の終了後、元appをhash一致で復元し、smoke3周・故障回復6種・HOME_READYを確認。
復元後ログは`runaway-restored-smoke.log`。通常設定のFAIR/YIELD既定と暴走閾値は変更していない。

次の正常負荷検査用にSELFTEST専用の有限frame診断（2万/4万/10万回の整数加算）と
`device_finite_frame.py`を追加した。完走時は総和を照合し、停止時はframe由来のRUNAWAYと
APP_STOPPEDを要求する観測ツール。ループ回数は§3.6の2万回約92msを目安に選んだが、
別image・実アプリの費用は未測定であり、完走すべき境界をこの数字で断定しない。
この追加時点では有限診断の実機採取は未実施（次節でFAIR構成を採取）。
既存の無限負荷採取や通常アプリsmokeを代わりの証拠にはしない。

### 4.17 有限frameの実機採取（2026-09-16、互換/FAIR）

SELFTEST/YIELD/FAIR/LAZY_INPUTS=yのimage SHA256
`a94add02c8c79a71b95d698967e57c24947274413cd006afebbac47db521d961`、
app2,159,984B、DIRAM123,356B、Flash Code1,567,796B。閾値は250,000µsのまま。
`device_finite_frame.py`を3回実行（別serial open。2/3回のuptimeは起動直後へ戻っているため、
同じ起動状態での連続寿命検査とは扱わない）。各回で以下の結果が一致した。

| 反復数 | 観測結果 |
| --- | --- |
| 20,000 | 完走、総和199,990,000 |
| 40,000 | 完走、総和799,980,000 |
| 100,000 | frame暴走ガードで停止、累積256,899 / 256,970 / 256,904µs |

4万回はHELLO_FRAME_PRESENTED→完了ログの差だけでも261/262/262ms。
これは描画・待機・ログを含む区間でCPU時間ではないが、250msを超える壁時計時間を許して
有限frameが完走する例にはなる。完走frameの累積実行時間は現在のログでは取得できておらず、
閾値まで何µsの余裕があったかは断定しない。10万回は有限でも停止するため、「有限なら必ず完走」
という判定をこのガードへ持ち込まない。ログ`.cache/vmtest/finite-fair-{1,2,3}.log`。
FAIR側の初回証拠であり、この時点では互換順序・競合条件・閾値近傍の確認は残る。
検証imageのsmoke3周・故障回復6種成功後、元appをhash一致で復元し、同じsmoke3周・故障回復6種と
HOME_READYを確認した。ログ`finite-fair-smoke.log` / `finite-restored-smoke.log`。

**互換順序での追試:** 検証SDKCONFIGのFAIRだけをnへ変更し、同じ診断ソースをビルド。
image SHA256 `f0da067f90caa22b6505fb3fb7ced6068acf4dbd248294f2cce268a601315610`、
app2,159,920B、DIRAM123,356B、Flash Code1,567,728B。
別serial openの3回すべてで2万/4万回は同じ総和で完走し、10万回は累積256,751 / 256,798 / 256,484µsで停止。
4万回のHELLO_FRAME_PRESENTED→完了は全回261ms。FAIRと同じ定性的結果だが、別imageなので
小さな時間差をモードの性能差へ帰属させない。検証用imageのsmoke3周・故障回復6種も成功。
ログ`.cache/vmtest/finite-compat-{1,2,3}.log`、`finite-compat-smoke.log`。
これでこの3負荷の互換/FAIR対照は採取済み。競合条件と閾値近傍の確認、総合関所は残る。
互換側の検証後も元appを書込hash一致で復元し、smoke3周・故障回復6種・HOME_READYを確認した。
復元後ログ`finite-compat-restored-smoke.log`。閾値と通常設定は変更していない。

### 4.18 完走frameの累積時間（2026-09-16、互換順序）

SELFTEST/YIELD限定で、指定した次のframeがJS_VMCallまたはJS_VMResumeから戻った際、
既存の累積`frame_us`を消去する前に一度だけ記録する口を追加。
`device_finite_frame.py --timing`は総和ログだけで終了せず、正常復帰マーカーとその順序も検査する。
ホスト検査5件成功。通常ビルドにはフラグ・分岐・ログを追加しない。

実測image SHA256 `66955a6a59910cfddb4cdce7c1b37a3615abc0b484582ae9c920631049838d22`。
SELFTEST/YIELD/LAZY_INPUTS=y、FAIR=n、app2,160,160B、DIRAM123,356B、Flash Code1,567,908B。
1回の採取で2万回は総和199,990,000・累積103,575µs、4万回は総和799,980,000・累積206,034µsで正常復帰。
10万回は累積257,136µsでframeガード停止。ログ`.cache/vmtest/finite-timing-compat.log`。
4万回は250,000µsまで43,966µsの差があったが、1標本であり安全余裕の下限とは扱わない。
値はpark中のホスト待機を除く累積壁時計時間で、プリエンプションとnative完了ログの費用を含む。
`VM_FRAME_COMPLETE`自身のログ出力は計時後。音声・ネットワーク競合を制御した測定ではない。
候補のsmoke3周・故障回復6種成功（`finite-timing-smoke.log`）。閾値と通常設定は未変更。
元appをhash一致で復元後もsmoke3周・故障回復6種・HOME_READYを確認
（`finite-timing-restored-smoke.log`）。競合・閾値近傍・総合関所は引き続き未完了。

### 4.19 循環ゴミのGC閾値（backlog #5、2026-09-16、`vm/l2-memory-safety`）

**不具合**: quickjs-ngの`malloc_gc_threshold`は初期256 KiB、GC後は生存量×1.5。ゲスト上限160 KiBより先に来ないため、循環ゴミは一度も回収されずOOMになっていた。`js_malloc_rt`は確保失敗時にGCしない。

**修正（2か所）**:
- `quickjs.c` `js_gc_effective_threshold`: 上限があるとき、`js_trigger_gc`の比較に使う閾値を「上限−上限/32」（160 KiBで155 KiB）で頭打ちにする。保存値をGC後にクランプする案は、満杯近くで計算された値が次のGCまで残るので不採用。余白を0にする（上限−1）案も不採用: 判定はオブジェクト生成時だけで、その間のshape・プロパティ配列の確保が最後の数バイトを越える。どちらも`gc_threshold_near_limit.js`がOOMのままだった（実測(host)）。
- `guest.c`（写しの`vmrun.c`も同じ）: `JS_SetMemoryLimit`直後に初期閾値を上限の半分（80 KiB）へ**下げる**。キャップだけでも回収は起きるが、その場合ゲストは共有DRAMを155 KiBまで使ってから回収する。下げるだけなので64 MiBのhostプロファイルは上流と同じ時機。
- GCが走る地点は上流と同じ`JS_NewObjectFromShape`の`js_trigger_gc`だけで、確保フック内からは呼ばない。変わるのは頻度だけ。

**ホスト（実測(host)、WSL、出力は/tmp）**:
- 修正前（HEAD `8ef5b25`のコピー）: `gc_threshold_device.js`は循環262個でOOM、`gc_threshold_near_limit.js`もOOM（asan/o2とも2件FAIL）。修正後: 100,000個完走（GC 465回）、上限寄りも完走（GC 669回）。
- コーパス8変種（asan/o2 × 無印/recur/flat/alloca）全合格（69件、allocaは既存skip 1）。Test262 asan/o2とも7,501 pass / 194 fail、regressions 0。`budget_probe.sh o2` 11/11、`oom_canary_probe.sh` o2/asan 5/5（`gc_threshold_device`はOOM例外リストから外し、oom=0を縛る側に移した）。`run.sh --trace`、`--vm-seg-size 2048`でもGC系2件は合格。
- GCストレス: 別コピーで`FORCE_GC_AT_MALLOC`（全オブジェクト生成でGC）を有効にしたasanコーパスは68/69、残る`bench_promise`は300秒タイムアウト（exit=124）で、ASan報告・クラッシュは無し。L2a/L2b（セグメント・フラット呼び出し）下の任意のオブジェクト生成地点でのGCの安全性の傍証であり、`--force-yield`中断中の毎回GCは既存の§4.10の検査に依る。
- 時間: o2コーパス（GC系2件を除く）の合計は修正前792〜841ms、修正後795〜804ms（各3回）で差は雑音内。**最悪ケース**: 生存量がキャップを超えた状態（OOMまで埋めて8個だけ解放）でオブジェクトを2,000個作ると、修正前0ms・修正後42ms（毎回GC）。ホスト値であり実機値ではない。

**実機（実測(device)、COM3、前=`vm/main`と同一の`8ef5b25`を別worktreeでビルド、後=`build_gcthr`）**:
- `smoke_device.py --cycles 20`: 前後ともSMOKE_OK 20・故障回復6種。
- `memlog.py --check`: DIRAM 144,588B不変、idle_free 248,752B・app_free 139,320B・app_largest 94,208B・js 93,902Bで前後同値。Flash +76B。
- `benchmark_app.py --samples 12`（hello、音設定は変更せず）: turn_ms 0.64→0.62、render_ms 1.55→1.54、send_ms 1.61→1.60。悪化なし。
- helloは生存量が小さく、フレーム中にGCが走らないので、上のbenchmarkは前後比較になっていない（下の追加計測で確認）。

**実機の追加計測（実測(device)、2026-09-16）**: 前=`8ef5b25`、後=`6fa205d`。それぞれに計測パッチ（コミットしない。`js_trigger_gc`のGC回数・時間、`malloc_size`のピーク・GC後の生存量、`app_tick`1回の時間の0.25ms刻みヒストグラム。`app_stop`で`GCPROBE`行を出す）を当てて、別worktreeでビルドした。
- 対象の2本: 5本（hello／IMU CALIBRATION／POCKET PET／PET COMPANION／Kasane demo）を無操作で20秒ずつ走らせ、ゲストヒープのピークが大きい2本を選んだ。IMU CALIBRATION 129,562B、POCKET PET 127,735B（以下Kasane demo 115,654B、companion 111,977B、hello 99,921B）。
- 手順: 無操作で30秒走らせて終了。p99は`app_tick`時間の最近順位法で、0.25ms刻みの上端。

| ビルド | アプリ（周回数） | frame max | frame p99 | GC回数（うちフレーム中） | GC 1回 |
| --- | --- | --- | --- | --- | --- |
| 前 | IMU CALIBRATION（6） | 26.9〜27.5ms | 20.75〜21.75ms | 0（0） | — |
| 後 | IMU CALIBRATION（14） | 27.0〜28.8ms | 20.75〜22.75ms | 各1（0） | 1.03〜1.22ms |
| 前 | POCKET PET（6） | 26.9〜56.2ms | 7.5〜8.5ms | 0（0） | — |
| 後 | POCKET PET（6） | 26.5〜58.4ms | 7.75〜8.75ms | 各1（0） | 1.03〜1.18ms |

- 修正後のGCは、アプリ起動時のソース評価中に走る1回だけ（初期閾値80KiBを越えた時点）。フレーム中のGCは0回で、max/p99の差は周回間のばらつきの範囲。PETの56〜58msのmaxは修正前にもある。
- 毎回GC領域（155KiB超）: GC後の生存量は最大81,881B、ピークは129,562Bで、2本とも届かない。無操作での値で、操作中の生存量は測っていない。

**ハング1件（未解決）**: 修正後の計測ビルドを焼いた後、12回目の起動（5本を順に→キー操作ありの2本→交互の3周目のIMU CALIBRATION）で機体が応答しなくなった。USB-JTAGは列挙されたままで出力は無く、esptoolも接続できず、物理リセットで復帰。パニックのログは取れていない。切り分け（いずれも再現せず）: 前の計測ビルドで12回（IMU 6）、後の計測ビルドで20回（IMU 14）、同じビルドでハングした順序を再生して14回（キー操作ありのIMUを含む。PETは無操作。当該の12回目も通過）、計測パッチ無しの`build_gcthr`で14回（IMU 12）。発生は1回だけで、修正・計測パッチ・既存の問題のどれが原因かは特定できていない。関係ないとは判断していない。前の計測ビルドの1回目でもホスト側のシリアルエラー（ClearCommError）が1回出たが、機体は生きていて別の事象。キー操作ありの計測をPETでも1回行ったので、`pet.v1`の保存内容が変わった可能性がある（元の値は控えていない）。

**残る懸念**: 上記のハング1件（未解決）。生存量が上限の31/32を超えたアプリは、オブジェクト生成のたびにGCする（上記hostの最悪ケース）。修正前はその状態から数回の確保でOOMだったが、循環ゴミを作らず上限際で長く生きるアプリは修正前より遅くなりうる。実機での該当アプリの有無とGC 1回の費用は未確認。

### 4.20 ゲスト確保の実長報告と realloc のその場伸長（backlog #8(b)・#9、2026-09-16、`vm/l2-memory-safety`）

**変更**: `guest.c`（実機）は確保ヘッダを廃止し、`js_malloc_usable_size` に `heap_caps_get_allocated_size()`（tlsf の実ブロック長）を返す。`realloc` は `heap_caps_realloc()`。
- QuickJS は `usable − 要求` を slack として使う（`js_realloc2`・文字列連結の高速経路）。要求サイズを返していた間は slack が常に0で、伸長のたびに realloc になっていた。
- tlsf の実長は要求を4Bに切り上げ、最小12B、分割できない余り最大15Bまで（`adjust_request_size`・`block_can_split`）。`malloc_limit` はこの実長で課金されるので、160 KiB は実際に渡した量に近づく。同じ理由で、上限に当たるバイト位置はヒープの空きブロック配置にも依存するようになった。
- 失敗時の意味論: tlsf 内の realloc は失敗しても元ブロックを解放せず、別ヒープへの移動は新ブロックができてから旧ブロックを解放する（IDF v6.0.1 `heap_caps_base.c`・`tlsf.c` を読んで確認）。`js_realloc_rt` の前提どおり。
- `heap_caps_get_allocated_size` の実機コスト: 48Bの生存ブロックへ2万回呼んで 1回 316〜454 ns（計測ビルド、実測）。ヘッダを読むだけだった旧経路との差で、malloc・free・realloc 1回ごとに1〜2回かかる。下のフレーム時間には差が出ていないので、ヘッダは戻していない。

**ホスト（`vmrun.c`）**: ヘッダは残し、実機の実長を**模型**で持つ（4B切り上げ・最小12B。分割できない余りは模擬しない）。realloc は、模型の長さに収まればその場で返す（縮小で余りが16B以上なら長さも縮める）。超える伸長は常に移動する（隣の空きをホストは知らないので、その場伸長は模擬しない）。
- **模型の最初の版には誤りがあった**: 移動時に旧**要求**サイズしかコピーせず、QuickJS が slack に書いた内容を落として、コーパス24件がクラッシュ・破損した（ASan `compute_stack_size` の SEGV）。実機の `heap_caps_realloc` は旧ブロック長までコピーするので、実機コードの誤りではない。ホスト側を旧ブロック長のコピーに直した。
- コーパスの調整3件（期待値は変えていない）:
  - `oom_resolving_functions{,_module}.js`: `--fail-alloc` の対象（`js_create_resolving_functions` の2つ目の確保）が1つ前へずれたので、計装して 1353→1352、1290→1289 に置き直した。alloca 版は修正前から 1352 を指しておらず（対象は1352、指定は1353）、今も隣を指す。
  - `memory_device.js` の array-oom: `new Array(1<<18).fill(0)` は 1.5 倍ずつ伸びて上限の 388B 手前まで這い寄る（163,452/163,840B、host）。そのため InternalError を作れるかがバイトの偶然で決まり、今回 `null` に変わった。単発の大きな確保になる `Array.apply(null,{length:60000})` に置き換えた。
- 結果: コーパス8変種全合格（alloca は既存 skip 1）、Test262 asan/o2 とも 7,501 pass・regressions 0、`budget_probe.sh o2` 11/11、`oom_canary_probe.sh` o2 5/5。**asan は 4/5**: `limit_rejected_by_accounting`（`--heap-limit 100K` で push し続ける）が、上限際で既知の上流 UAF（backlog #6、`build_backtrace`→`can_add_backtrace`）を踏むようになった。修正前の asan では同じ検査が通る。検査の値は動かしていない。

**実機（実測(device)、前=`b54b71d`、後=作業ツリー。同じ計測パッチ〈コミットしない〉を当てて別worktreeでビルド。無操作30秒×各3回）**:

| アプリ | | 起動後の空き / 最大連続 | js課金 | 走行中の最小空き | realloc / 移動 / コピーB | frame max / p99 |
| --- | --- | --- | --- | --- | --- | --- |
| IMU CALIBRATION | 前 | 85,164 / 31,744 | 106,371 | 66,824〜66,848 | 5,543〜5,550 / 全数 / 252,805 | 27.2〜27.5 / 20.75〜21.0ms |
| | 後 | 91,560〜91,596 / 31,744 | 108,932〜108,980 | 73,820〜73,868 | 5,507〜5,520 / 3,602〜3,942 / 142,740〜170,828 | 27.2〜27.3 / 20.75ms |
| POCKET PET | 前 | 82,196〜82,224 / 36,864 | 112,548 | 77,568〜77,584 | 1,413 / 全数 / 104,757 | 26.3〜56.5 / 8.5ms |
| | 後 | 89,116〜89,132 / 45,056 | 115,440〜115,456 | 84,504〜84,520 | 1,371〜1,372 / 570〜579 / 62,636〜62,964 | 26.6〜55.2 / 8.5ms |

- 空きは IMU で +6.4〜7.0 KiB、PET で +6.9 KiB。PET の最大連続は +8 KiB。コピー量は IMU −32〜44%、PET −40%。移動は IMU で約3割、PET で約6割減った。realloc の呼び出し回数は IMU −0.8%、PET −2.9% で、slack で消えた分は少ない。
- js 課金は +2.6〜2.9 KiB。実長課金になったぶんで、同じアプリの課金は約2〜3%増え、上限にそのぶん早く近づく。IMU は課金ピークが 1.5×82K を越えて、起動時の GC が1回から2回になった（2回目は GC 前 123K、どちらも評価中で、フレーム中の GC は0回）。
- `memlog.py --check`（hello、素のビルド。前=`b54b71d` を worktree でビルド、後=`build_rp`）: app_free 139,296→145,092B（+5,796）、app_largest 94,208→102,400B、js 93,902→96,068B。idle_free は同値。静的 DIRAM +120B（`xtensa_vectors.S.obj` に計上、原因は未確認）。
- `smoke_device.py --cycles 20`: 後のビルドで SMOKE_OK 20・故障回復6種。
- 手順の誤りを1件記録する: 修正前の memlog を最初は `build_gcthr` で採ったが、`idf.py flash` が作業ツリーの変更込みで再ビルドしていたので、前後とも修正後の値になっていた。上の前の値は、変更を含まない worktree のビルドで採り直したもの。

### 4.21 backtrace 組み立て中の OOM：UAF と CallSite 二重解放（backlog #6、2026-09-16、`vm/l2-memory-safety`）

**§4.20 の訂正**: `memory_device.js` の array-oom（`new Array(1<<18).fill(0)`、少しずつ伸びて上限で OOM）を単発確保に替えた件の説明が不十分だった。この元の形は #9 の後、o2 では `null`（捕捉され、後続も正常）だったが、**asan では下の UAF** だった。当時は o2 の差分しか見ておらず、置き換えで UAF の再現をテストから消していた。`memory_device.js` は単発確保のまま、元の経路は新ケース `oom_creep_backtrace.js` で守る。

**不具合1（上流 e1c1e416、#1469）**: `build_backtrace(ctx, error_val, ...)` は `rt->current_exception` を借用参照で受け取る（`JS_CallInternal` の `exception:` ラベル、パーサ、正規表現）。中の確保が上限で失敗すると `JS_ThrowOutOfMemory`→`JS_Throw` が `rt->current_exception` を解放する。巻き戻し中はそれが唯一の参照なので、その後の `can_add_backtrace(error_val)`・`JS_DefinePropertyValue` が解放済みを読んでいた。修正は、入口で `error_obj = js_dup(error_val)` を取り、出口で解放する形（上流の移植、`JS_ToObject` の無関係な1行は除外、`49edcc4`）。
- 回帰 `oom_creep_backtrace.js`（device プロファイル）: ヒープを小オブジェクトで上限まで埋め、r=0〜47 個解放してから3段下で大きな確保をする。当たる窓が狭いので余白を掃引する形にした。修正前の asan で UAF、修正後は48回とも捕捉（null 9回、`#info`）。
- 別モデル（sonnet）の敵対的レビュー: この修正は正しく完全（解放は1回、借用参照の残存使用なし、8呼び出し元すべてに効く）。同じ関数に次の不具合を見つけた。

**不具合2（上流 c846cb13）**: `Error.prepareStackTrace` を設定していると、`build_backtrace` は CallSite の配列を作る。配列への挿入が失敗した場合、`JS_DefinePropertyValueUint32` は失敗時も値を消費する（`JS_DefinePropertyValue` → `JS_FreeValue`）のに、呼び出し側でもう一度 `JS_FreeValue(v)` していた。さらに `js_new_callsite` が `csd` の値を移したのにクリアせず、後始末のループが二重に解放していた。修正は上流の移植（`780cd25`）。
- 回帰 `oom_callsite_double_free.js`: 上限際の余白掃引（最大1,200回×3通り）では、CallSite の確保が失敗するか全部収まるかで、この分岐に届かなかった。両分岐を計装して `--fail-alloc` を掃引し、1458 を index 0 の挿入時の配列伸長として特定した（alloca 版は番号が違うので skip）。修正前の asan/asan-recur/asan-flat で UAF、修正後は `returned object`。

**検証（最終状態）**: コーパス8変種全合格（71件。alloca は skip 2）、Test262 asan/o2 とも 7,501 pass・regressions 0、`budget_probe.sh o2` 11/11、`oom_canary_probe.sh` asan/o2 とも 5/5、`known/oom_backtrace_uaf.js` は asan で UAF なし。実機 `build_uaf`: smoke 20周・故障回復6種。

### 4.22 上流の memory-safety 修正の追加移植と棚卸し（2026-09-16、`vm/l2-memory-safety`）

v0.14.0（`3c051980ab`）以降に上流へ入った use-after-free / double free / OOM 系の修正を読み、こちらの該当箇所を確認した。上流1コミット＝こちら1コミットで移植している。

| 上流 | 内容 | 判定 | こちら |
| --- | --- | --- | --- |
| e1c1e4163e / c846cb1364 | build_backtrace の UAF / CallSite 二重解放 | 移植済み | §4.21（`49edcc4`・`780cd25`） |
| d98ff101c6 | `.length` を伸ばした fast array への push | 移植 | `8af66ab`。回帰 `array_push_length_hole.js`: 修正前は `hasOwnProperty(1)` が真、`Object.keys` に穴が出る（o2 では未初期化スロットに `3` が見えた）。ASan は未初期化読みなので検出しない。修正後は正しい |
| 49131a6315 | Promise.withResolvers の OOM 時二重解放 | 移植 | `0a869c7`。回帰 `oom_with_resolvers.js`（`--fail-alloc 1370`、1250〜1400 を修正前 ASan で掃引し 1370/1371 が該当）: 修正前は asan/recur/flat で UAF、修正後は捕捉 |
| 776d724cfa | resolving functions 生成時 OOM の UAF | 不要 | `js_create_resolving_functions` は独自修正（失敗時に [0] を解放して UNDEFINED にする）で同等。async 関数・async generator 側の3つの呼び出し元（`js_async_function_settle_core` 23293、`js_async_generator_await` 23552、`js_async_generator_completed_return` 23647）は、失敗時に `resolving_funcs` を解放しない |
| 7955cfd49e | 中断中コルーチンの closure 経由 UAF | 移植 | §4.23（分析時点では分析のみ。上流テスト3本が asan / recur / flat / alloca の全変種で UAF を再現。移植設計とリスクは backlog #13） |
| 4369dd6488 | detach 済み ArrayBuffer の二重解放 | 不要 | ファイナライザは detach 後 `free_func(NULL)` を呼ぶだけ。ファームは free_func に NULL を渡している（`ui_qjs.c:770`） |
| 396e1e0b4f | JS_FreeCStringUTF16 と slice 文字列 | 不要 | ファームから使っていない API |
| 05b2db95d9 / d0c2272126 | (Async)DisposableStack の UAF | 該当なし | 機能自体がない |
| a65377157e ほか5件、ef7a3a748b | 整数オーバーフロー、循環 re-export | 記録のみ | backlog #14 |

リーク系の修正（9b58030f60 など）は範囲外。

**7955cfd49e の分析（移植前、別モデルの敵対的レビュー済み）**:
- **機構**: closure がコルーチンのローカルを捕捉した open var_ref は GC オブジェクトではない。holder の mark（`js_bytecode_function_mark`、オブジェクトの VARREF プロパティ、`js_mapped_arguments_mark`）も detached しか辿らない。そのため closure → コルーチンの辺が cycle collector から見えず、中断中コルーチンが生存中に回収される。
- **L2 との関係**: フレームは移動しない（`stack_frame`/`pvalue` の書き込みは `get_var_ref` と `close_var_ref` だけ）。async 関数のフレームは flat 経路でもヒープ埋め込み（22112）。中断中の SEG フレームは `js_vm_mark_suspended` が context の子として mark するので、この修正の対象外で、穴もない。穴は L2 前からのもの。
- **移植設計**: JSStackFrame に上流の `cur_gc_obj` ポインタを足すと 48→52B で、`sizeof(JSAsyncFunctionData)==104` のアサートが壊れ、全 JS フレームも +4B になる。代わりに空きバイト（offset 38、全変種で空きを確認）に `coro_kind` を置き、所有者は container_of で求める。generator 用に `JSGeneratorData` へ逆ポインタを足す。`is_coro` は JSVarRef の open 側 union のパディングに置く（上流の新しいヘッダ配置は前提にできない）。
- **壊しうる不変条件**:
  - 上の2つのサイズ（アサートを新設する）。
  - `mark_children(VAR_REF)` の `assert(is_detached)`（7599）を緩める。
  - `gc_obj_list` の一貫性。
  - **再入**: `close_var_refs` で所有者の参照カウントが0になり、同じフレームへ再入しうる。L2c の Discard に固有ではなく、通常完了（`async_func_free` 22952）でも起きる。ガードは `close_var_ref` 自体に置き、22952 と 22556 の両方の呼び出しに効かせる（レビューで訂正）。
  - 毎中断GC・force-yield・asan-tco との組み合わせ。

移植は §4.23。上の「再入」のガードは、レビュー後の設計で「到達不能、assert で不変条件として書く」に改めた（§4.23）。

### 4.23 中断中コルーチンの closure 経由 UAF：上流 7955cfd49e の移植（backlog #13、2026-09-17、`vm/l2-memory-safety`）

上流の設計（コルーチンのローカルを捕捉した open var_ref を GC オブジェクトにし、所有コルーチンへの参照を持たせる）をそのまま使い、置き場所だけ変えた。

| 上流 | こちら | 理由 |
| --- | --- | --- |
| `JSStackFrame.cur_gc_obj`（ポインタ） | `coro_kind`（1B、パディング）＋ `js_coro_owner()` が container_of で所有者を求める | 48→52B を避ける。`JSStackFrame==48`（新設）と `JSAsyncFunctionData==104`（FLATCALLS 以外にも新設）のアサートで固定 |
| — | `JSGeneratorData.generator`（逆ポインタ、+4B） | generator だけ Data から JSObject へ戻る手段がなかった。async generator は既存 |
| `JSVarRef.is_coro`（ヘッダ隣） | open 側 union のパディング | 空きがない。close 時の `value` 書き込みで潰れるので、`close_var_ref` は `js_dup` より前に所有者と `is_coro` を読み、全読者は `is_detached` を先に判定 |

レビュー（別モデル、敵対的）で必須とされ、入れた5点:
1. `coro_kind` は**所有 JSObject の生成後**に立てる（generator・async generator は生成直後、async 関数は `is_active=true` の後、flat 経路も同じ）。プロローグ（`OP_initial_yield` まで）の mapped arguments・既定引数 closure は通常の open var_ref のまま。失敗経路は kind 0 のまま閉じる。
2. SEG ブロックも JSVarRef も zero 化されないので、フレーム生成の全箇所（`JS_CallInternal`、`async_func_init`、TCO の再利用）で `coro_kind=0`、`get_var_ref` で `is_coro` を必ず書く。
3. リスト操作: `close_var_ref` は `is_coro` なら `add_gc_object` しない（既に登録済み）。`free_var_ref` の open 経路は「slot を NULL → `remove_gc_object` → 所有者 release → free」の順（release が arg_buf を解放しうる）。
4. 再入: `close_var_refs` 走査中に所有者が解放される経路は、open var_ref 自身が参照を持つので到達不能。上流の `close_var_ref` 冒頭にある「既に detached なら return」は入れず、`close_var_ref` の release 地点に置いた assert「REMOVE_CYCLES 以外では release 前の refcount>1」で不変条件として書いた（専用の release ヘルパ `js_coro_release` には置いていない）。
5. `mark_children(VAR_REF)` の `assert(is_detached)` を、open なら `is_coro` を assert して所有者を mark する形に緩めた。holder 側の mark（`js_bytecode_function_mark`・VARREF プロパティ・`js_mapped_arguments_mark`）は `is_detached || is_coro` を辿る。

**回帰**（実測(host)。コミット `5e6d244` のファイルを、修正前＝vm/main から `aa602e1` の quickjs.c 変更だけを逆適用したビルド、修正後＝vm/main で実行。2026-09-17 に測り直した）:

| ケース | 修正前（asan / -alloca / -recur / -flat / -yield / -tco） | 修正後 |
| --- | --- | --- |
| `coro_closure_gc.js` 全体（上流3本、async generator の closure と循環） | 6変種すべて heap-use-after-free | asan・-alloca とも一致、ASan なし |
| `coro_prologue_gc.js` のうち、プロローグだけで捕捉する単独ケース（generator・async generator の arguments と既定引数 closure、逃げた既定引数 closure） | 6変種すべて鳴らない | asan で一致、ASan なし |
| `coro_prologue_gc.js` のうち、プロローグの捕捉と本体の closure が同じフレームにある循環（generator） | 6変種すべて heap-use-after-free | asan で一致、ASan なし |
| 同上（async generator、ジョブを2段送ってから GC） | 6変種すべて heap-use-after-free | asan で一致、ASan なし |

`coro_prologue_gc.js` の単独ケースは修正前にも穴がなく、1. の順序を守るガードとして置いている。`coro_kind` をプロローグ前（`async_func_resume` の前）に立てる変異を一時コピーに入れると、asan ビルドで UBSan が `js_coro_owner` の NULL 参照（`JSGeneratorData.generator` がまだ無い）を報告して落ちる（実測(host)）。

**訂正（2026-09-17）**: この節の初版は、`coro_prologue_gc.js` 全体を「修正前も鳴らない」、`coro_closure_gc.js` を「修正前に鳴るのは asan・-alloca だけ」と書いていた。どちらも誤り。
- 前者はテストの世代差による。初版の計測は、並行セッションが混在循環2件を足す前の版で行った。
- 後者は変種漏れによる。初版は2変種しか回していなかった。
- 並行セッションのエージェント coro-uaf の実測（「混在循環は修正前に6変種すべてで UAF、単独ケースは鳴らない」）が正しい。

**関所**（実測(host)、コミット済みの `aa602e1` をビルド。修正前は HEAD=`75cc6c7` のコピー）:

| 検査 | 修正前 | 修正後 |
| --- | --- | --- |
| コーパス asan・o2・-recur・-flat | — | 各 75/75 |
| コーパス -alloca（asan・o2） | — | 72/72＋skip 3（既存） |
| `--force-yield`（asan-yield、全件） | 73/75（`coro_closure_gc`・`seg_oom_boundary`） | 74/75（`seg_oom_boundary`） |
| `--gc-on-yield --force-yield`（asan-yield、中断・コルーチン・TCO 関連27件） | 25/27（`coro_closure_gc`・`tco_guards`） | 26/27（`tco_guards`） |
| asan-tco（全件） | 73/75（`coro_closure_gc`・`seg_oom_boundary`） | 74/75（`seg_oom_boundary`） |
| asan-tco `--gc-on-yield`（同27件） | 26/27（`coro_closure_gc`） | 27/27 |
| `tco_probe.py`（n=100000 gc=0、n=100 gc=1） | OK | OK |
| Test262 asan / o2 | — | 7,501 pass、regressions 0 |
| `budget_probe.sh o2` / `oom_canary_probe.sh` asan・o2 | — | 11/11 / 5/5・5/5 |

修正前後で結果が変わったのは `coro_closure_gc` だけ。修正後も残る2件は、修正前と差分がバイト一致の既存失敗で、この移植とは独立（原因は未調査、backlog #15）:
- `seg_oom_boundary`: -yield / -tco / `--force-yield` で2回目の OOM が `InternalError` でなく `null`。
- `tco_guards`: `--gc-on-yield --force-yield` で 300 秒のタイムアウト（exit=124）。

毎中断GC＋強制中断を全件で回す最初の試行は、ベンチ系が 300 秒上限まで走って片側1時間を超え、Windows 側のメモリ不足でジョブが止められた。そのため関連27件に絞った（§4.10 などと同じ運用）。

**実機**（実測(device)、COM3、hello）: 修正後の `build_coro` で smoke 20周・故障回復6種 OK。`memlog.py --check` を、修正前 `build_uaf`（`75cc6c7` と同じコード、同じ作業ツリーのパス）と修正後 `build_coro` で比べた:

| | 修正前 | 修正後 | 差 |
| --- | ---: | ---: | ---: |
| Flash | 1,611,164 | 1,611,484 | +320 |
| 静的 DIRAM | 144,708 | 144,708 | 0 |
| `idle_free` / `app_free` / `app_largest` / `js` | 248,720 / 145,092 / 102,400 / 96,068 | 同値 | 0 |

- Flash の比較元は揃える必要がある。並行セッションの草稿の +272B は、別 worktree（`.claude/worktrees/coro-head/build_base`、1,611,216B）を比較元にしていた。同じコードの `build_uaf` は 1,611,164B で、ビルドディレクトリのパスによる 52B の差を含むので採らない。`build_coro` の map は後で同じソースから再ビルドされ、今は 1,611,488B（`build_uaf` 比 +324）。
- 同じ草稿にあった `app_free` −68B・`js` +68B は、こちらの測定（差 0）では再現していない。
- generator 1個あたり +4B（`JSGeneratorData.generator`）は、generator を使わない hello では測れていない。

**コミット `aa602e1` の本文の訂正**: 本文の「close_var_ref guards re-entry」は誤り。コミットされた `close_var_ref`（quickjs.c:18551〜）には再入ガード（detached なら return）が無い。代わりに release 地点で `assert(rt->gc_phase == JS_GC_PHASE_REMOVE_CYCLES || js_coro_owner(sf)->ref_count > 1)`（18575 付近）を置き、「`close_var_refs` 走査中の再入は、open var_ref 自身が所有者の参照を持つので到達不能」という不変条件を形にしている。これは別モデルレビューの結論（ガードではなく release 前 refcount>1 の assert で書く）と一致し、assert は専用の release ヘルパではなく `close_var_ref` の release 地点にある。経緯: 同じ作業ツリーで並行セッションが同じ移植を進めていて、その quickjs.c の編集（ガードの削除と assert への置き換え、FLATCALLS 以外向けの `JSAsyncFunctionData==104` アサートの追加、コメントの拡張）が、気づかれないまま `aa602e1` に入った。本文はその前の版の説明のまま。履歴は書き換えず、ここで訂正する。

## 5. D42+D43: フレームセグメントの線形化（2026-09-13〜14、`vm/segsize`）

### 5.1 実機のフレーム使用量（実測(device)、プローブビルド、hello他6本を各4秒走行）

| アプリ | 深さ | `live_max` | `frame_max` | セグメント数 | `seg_mallocs`/`frees`/`reuses` |
| --- | ---: | ---: | ---: | ---: | --- |
| hello | 4 | 456 | 188 | 1 | 1/0/0 |
| `sync_loop` | 2 | 148 | 100 | 1 | 1/0/0 |
| `deep_recursion` | 269 | 20,460 | 92 | 6 | 360/358/118 |
| `closures` | 2 | 172 | 100 | 1 | 1/0/0 |
| `promise_chain` | 2 | 148 | 92 | 1 | 1/0/0 |
| `io_wait` | 2 | 160 | 84 | 1 | 1/0/0 |
| `async_generator` | 2 | 148 | 84 | 1 | 1/0/0 |

普通のアプリは固定4,096Bのうち150〜460Bしか使っていない。ホスト（o2、40件中）でも深い再帰主体の8件以外は1,392B以下で1本に収まった。`stack_hw_min=23,788`（32,768B中の未使用）は全アプリで同値だった（読みの妥当性はbacklog.md #9）。

### 5.2 実装で見つかったこと（実測(host)、o2）

D42（線形成長 `min(JS_VM_SEG_FIRST×n, JS_VM_SEG_MAX)`、`FIRST=512`/`MAX=4096`）だけを入れた状態: 常駐は1本収まる35件で4,143→567B（−86%）。一方 `bench_calls`（深さ31）で `seg_mallocs` 2→129,940、`seg_reuses` 89,103→1 — キャッシュ1本では戻り→再度潜るの往復で位置ごとに大きさが違う分が合わない。キャッシュ8本の対照実験で `bench_calls` は5回、`deep_recursion_device` は13→9回に収まり、原因はキャッシュの本数だと確定した。

同時に、`deep_async_recursion`（D40の固定値）で外側の`catch`が`null`を表示しなくなり（深さ77→82）、`gc_threshold_device` で最後の `#info` 行を作る余裕が無くなった。どちらもヒープを使い切った後の余りがバイト単位で動いた結果。

### 5.3 D43適用後（実測(host)、`vm/segsize`）

D43: ターンの間はキャッシュ本数無制限、ジョブキューが空になった時点（`JS_VMStackTrim()`）で全部返す。

| ファイル | 深さ | `seg_mallocs`（D42前/D42のみ/D43） | 常駐ピーク（B） | `held_max`（D43） |
| --- | ---: | --- | --- | ---: |
| 深さ1本の35件 | 1〜3 | 1/1/1 | 4,143/567/567 | 567 |
| `sync_loop`等2本の10件 | 2〜10 | 1/2/2 | 4,143/1,646/1,646 | 1,646 |
| `bench_calls` | 31 | 2/129,940/5 | 8,286/7,955/7,955 | 7,955 |
| `deep_recursion_device` | 159 | 5/13/9 | 20,715/23,023/23,023 | 23,023 |
| `seg_closure_survives` | 3,002 | 6,457/6,667/124 | 501,303/500,388/500,388 | 500,388 |
| `seg_generator_frames` | 1,502 | 1,269/1,368/64 | 252,723/251,328/251,328 | 251,328 |
| `deep_recursion` | 57,343 | 3,812/3,836/1,824 | 7,544,403/7,557,088/7,557,088 | 7,557,088 |

全ファイルで `held_max` = 常駐のピーク（キャッシュがターン中に一度生きた量を超えない、D43の主張どおり）。関所: コーパス8変種47/47、`--force-yield` 3/44、`stack_probe.sh` 0.000 NOT_PROPORTIONAL、`oom_canary_probe.sh` 5/5×2、Test262 asan 7,501/194/regressions 0。

固定を2つ変更（記録している性質は変えていない）: `gc_threshold_device.js` の `#info cycles_before_oom=` 行を`try`で包んだ。`expected/deep_async_recursion.txt`（D40）は「同期tryに届かない」「終了コード2」だけに変更（深さ77→82、未処理23→26。原因はヒープ枯渇時点のカナリア`oom>0`で縛る、出力の完全一致では縛らない）。

### 5.4 実機（実測(device)、hello、main/vm-main（L2b）/vm-segsize（D42+D43）の3周比較）

| | main | vm/main（L2b） | vm/segsize（D42+D43） |
| --- | ---: | ---: | ---: |
| `idle_free` | 277,172 | 277,060 | 277,044 |
| `app_free` | 120,980 | 116,600 | 120,212 |
| `app_largest` | 69,632 | 65,536 | 63,488 |
| `js` | 86,356 | 90,523 | 86,943 |
| `turn_ms` | 1.656 | 1.708 | 1.78 |

空きは戻った（L2bで失った4,380Bのうち3,612Bが戻り、mainとの差は−768B）。**最大連続ブロックは戻らず、さらに−2,048B: taffy 59,296B段への余裕は6,240→4,192B**（原因未確認、backlog.md #8）。`turn_ms`の差は配置差の範囲で帰属できない。安定性: smoke 20周・故障回復6種OK、ホーム画面29.5〜30.3fps（モード0〜2）。

プローブ統計: hello `resident_max=547 held_max=547 trims=0`。予算まで毎フレーム潜る`deep_recursion`系ワークロードは `seg_mallocs` 360→821、`seg_reuses` 118→0、`trims=117`（D43設計どおりの費用: ターン間に20KiBを抱えない代わりにターンごとに取り直す）。`stack_hw_min` は26,492（前回23,788）で今回もアプリをまたいで同一（backlog.md #9）。

**計測の副作用（記録）:** `benchmark_app.py --sound off` は設定を戻さない。本節の計測後、実機は無音のままだった。`--sound on` で復元済み。

### 5.5 同一バイナリでのD42成長サイズ比較（2026-09-16、host）

従来の`--vm-seg-size`はFIRST=MAXの固定サイズにし、cache_maxも1へ変えるため、D42/D43の初期サイズと上限だけを比較する口ではなかった。`--vm-seg-growth FIRST MAX`を追加し、cache方針を保持したまま、最初のフレームより前にサイズだけを指定する。FIRSTは16B整列、MAXはFIRSTの整数倍とし、無効値・生存chain/cacheは状態を変えず拒否。固定サイズ指定との併用もエラーにする。既定512/4096Bと旧固定サイズAPIの動作は変更していない。

`segment_growth_check.c`で範囲・整列・倍数・大きな値・live/cache拒否・失敗時不変性・旧APIの切り上げを検証。`segment_growth_probe.py`は同一runnerで6workload×6方針を既定と比較し、JS出力と終了値の一致、指定の反映、統計の存在を要求する。`o2`と`asan-yield`で各36比較成功、ASan/UBSan報告なし。既定o2の全コーパス67/67も成功。強制yieldのサイズ掃引ではなく、ASan版も通常実行である。

主な実測(host、保持ピークB / segment確保回数):

| FIRST/MAX | closures | 深いflat calls | bench_calls |
| --- | ---: | ---: | ---: |
| 256/1024 | 1701 / 3 | 540378 / 502 | 6017 / 7 |
| 256/2048 | 1701 / 3 | 527250 / 254 | 5706 / 6 |
| 512/2048 | 1646 / 2 | 526884 / 252 | 7443 / 5 |
| 512/4096（既定） | 1646 / 2 | 500388 / 124 | 7955 / 5 |
| 1024/4096 | 3182 / 2 | 500278 / 122 | 6309 / 3 |
| 4096/4096 | 4151 / 1 | 502271 / 121 | 8302 / 2 |

上限を小さくするだけでは深い鎖の保持量と確保回数が増える。初期1024Bは浅いclosuresでは約倍、短いfib再帰では小さくなるため、単一ケースから最適値を決めない。各workloadのlive最大値は方針間で同じ。device profileの深い再帰も20,448Bで一致したが、これは64bit host上の20KiB予算検査でありESP32-S3の使用量ではない。

生結果は`.cache/vmtest/segment-growth{,-asan}.json`（全42run/変種、runner SHA256付き）。o2 SHA256 `42a7e12bed623e78645572b93e2478ff41b23a8475cceab78f98edd1343e3f00`。これは容量/確保回数比較で、速度測定や実機の最大連続空き/外部断片化の証明ではない。次工程は同じ設定口を実機プローブに接続して分布と配置を測ること。実機と既定設定は本工程では変更していない。

### 5.6 D42実機サイズ比較の初回（2026-09-16）

PROBEのみのUSB選択（G/H/I/J/K/O）を追加し、ゲスト生成直後・最初のJSより前に§5.5の設定口へ接続。選択はatomicな次セッション用状態であり、現在動いているruntimeを変更しない。起動時はpolicy3=512/4096B。`VMSEG APPLY`の戻り値と、終了時の実際の`#info vmstack`のFIRST/MAXを採取器が照合する。従来はstop待ちの途中で捨てていた終了時統計もJSONLの`segment`へ保存する。fake serialによる終了時回収・拒否値の検査2件、継続/stackの既存5件を通過。

同一`build_vm_stack_flat` image SHA256 `93b954fbd0c132f9ef12cc1fe38953cffb3573362ec1fb17136a1156633a39ea`（app2,153,392B、DIRAM129,404B、flash1,560,972B）をCOM3へ書き、B/F×6方針を各4秒・各回再起動、base条件で実行した。初回のみの12runで、反復性・順序効果はまだ未検証。全runで適用値と最終統計の一致、早期終了なしを確認。

| FIRST/MAX | B保持ピークB | B確保回数 | F保持ピークB | B/F最大連続空きの標本最小B |
| --- | ---: | ---: | ---: | --- |
| 256/1024 | 22821 | 2201 | 291 | 81920 / 79872 |
| 256/2048 | 21994 | 1345 | 291 | 81920 / 79872 |
| 512/2048 | 21924 | 1101 | 547 | 81920 / 79872 |
| 512/4096 | 22843 | 856 | 547 | 81920 / 79872 |
| 1024/4096 | 22773 | 612 | 1059 | 81920 / 79872 |
| 4096/4096 | 24786 | 490 | 4131 | 77824 / 75776 |

Bのpush数は全方針24,834、深さ269、live最大20,460B、最大frame92Bで一致。Fはlive最大148B・最大frame84B、segment確保1回、push94〜95回。Fの浅い鎖では初期256Bが保持量を減らすが、helloなど他のアプリも含む最適性は未確定。固定4096Bで最大連続空きが4096B減ったことは今回の配置差であり、旧§5.4の反対方向の差の原因を特定したものではない。保持ピークはターン中のsegment容量統計、空きヒープは低頻度標本なので同時点の値とも限らない。

生ログ`.cache/vm/segment-device-{0..5}.jsonl`。再現例: `python tools/vm_l0_capture.py --port COM3 --workloads BF --conditions base --seconds 4 --reps 1 --segment-policy 3 --out .cache/vm/segment-device-3.jsonl`。サイズ以外は同一バイナリだが、今回の短い1反復だけで速度差は主張しない。既定512/4096Bは維持し、残る工程は他アプリと反復・順序を含む分布/配置比較。

検証後は元Kasane appへ復元し、書き込みhash一致、起動3周・故障回復6種・HOME_READYを確認。app領域以外（NVS/storage/パーティション）は変更していない。

### 5.7 反復行列の採取入口の修正（2026-09-16、行列未完了）

§5.6と同一バイナリで、6方針×7対象×正逆2順序の84runを企図した。A〜Fに通常のUSB決定キー`e`を足してhelloを起動する方式は不適切だった。逆順のpolicy5/4/3/2で`e`のstop応答と最終segment統計が得られず、policy2の追加診断ログではSDの`PICK 2 folders`、`GRANT DECLINED`とoverlay実行を確認した。ホームの選択状態に依存する入口であり、helloを指定した証拠にならない。SDアクセス許可は与えていない。forwardの`e`に統計があってもhelloとしての採用判断には使用しない。

旧ログ`.cache/vm/segments-repeat-{0,1}-{policy}.jsonl`は調査用に残す。正順42試行と逆順28試行までで、hello終了統計なし4件、後続14runは未実行。A〜Fの60runには適用値と終了統計があるが、84run行列成功とは扱わない。サイズの既定値も変更しない。

診断PROBE限定の`X`を追加し、他の診断と同じ`APP_ID_DEFAULT`選択経路から`hello_start`を明示して起動する。これは通常キーバインドや出荷設定を変えない。採取器は`e`を受け付けず、`X`はbase条件のみ。採取失敗を表示するだけで終了コード0だった穴も修正し、失敗時はserialを閉じて1を返す。直近ログも失敗の説明に残す。

`vm_segment_report.py`は新しい`segments-repeat-v2-*`だけを既定で読み、84runの完全性、window重複、適用サイズ、終了統計、標本数とdropを検証する。legacyの不完全な行列を成功扱いしない。単体検査は集計5件・採取3件・継続2件・stack3件の計13件成功。X追加ビルドはapp2,153,408B、DIRAM129,404B（増加0）、flash1,560,976B。X入口の実機確認と新行列の採取は次工程である。

調査後は元Kasane appを復元し、書き込みhash一致、起動3周・故障回復6種・HOME_READYを確認。app領域以外は変更していない。

### 5.8 代表7負荷の反復比較とサイズ決定（2026-09-16）

X入口をAの後に実機確認してから、hello(X)/A〜F×6方針を正順・逆順で各1回、計84条件実行した。全条件base、各3秒・毎回再起動、同じimage SHA256 `4e825e62ec5fa7981b5ddd32decbcbfce1e9e7c199e149961eb8852a05e3aeb5`（§5.7のX追加build）。これは各負荷の最大frame/live容量分布であり、すべてのframeサイズの頻度ヒストグラムではない。

最初の集計は標本不足で失敗した。採取期限がWINDOWヘッダとS行の間に来ると、stop待ち前の`Collector.close()`がcurrentを消し、後続のS行が捨てられることを再現。closeをstop待ち後だけにし、期限がヘッダ直後に来るfake serial検査を追加した。不完全だった23条件だけを同一imageで取り直し、正常な61条件はそのまま使用した。元ログを編集せず、再採取は`segments-repair-v2-*`へ別保存。`merge_repairs`は元が不完全な条件だけを丸ごと置換し、正常な条件の置換・再採取の欠落・firmware metadata不一致を拒否する。最終検査は**84run・225window成功**、設定一致、早期終了/dropなし、各frame/call/drain/jobsの標本数一致。修正と取り直しのため、連続した2周がそのまま全件成功したという意味ではない。

実測(device、2回の範囲):

| FIRST/MAX | hello保持ピークB | A/C/D/E/F保持ピークB | 再帰B保持ピークB | 再帰Bの確保回数 |
| --- | ---: | ---: | ---: | --- |
| 256/1024 | 838 | 291 | 22821 | 1625〜1643 |
| 256/2048 | 838 | 291 | 21994 | 993〜1004 |
| 512/2048 | 547 | 547 | 21924 | 813〜822 |
| 512/4096 | 547 | 547 | 22843 | 639 |
| 1024/4096 | 1059 | 1059 | 22773 | 452〜457 |
| 4096/4096 | 4131 | 4131 | 24786 | 362 |

live最大は全方針でhello456B、A148B、B20460B、C172B、D148B、E160B、F148B。最大単体frameはhello188B、A/C100B、B/D92B、E/F84B。helloはFIRST256で2segment、512以上で1segment。再帰Bのpush数は18,338〜18,541なので、時間窓内の呼び出し回数にはわずかな差がある。確保回数を実行速度そのものとは扱わない。

**決定: 現行のFIRST512/MAX4096Bを維持する。** FIRST256は単純負荷で256B節約するが、helloの保持ピークを291B増やして2segmentに分割する。FIRST1024以上は浅い負荷の常駐余白を増やす。MAX2048は再帰ピークを919B減らす一方、確保回数が約27〜29%増える。hostの深い鎖でも2048上限は保持量・確保回数とも増えた（§5.5）。典型的なUIアプリを1本に収め、浅い負荷の余白と深い鎖の確保コストを均衡させる出荷方針として512/4096を採る。あらゆるアプリの大域的最適値を証明したものではなく、負荷構成が変われば再測定する。

配置の観測も容量と分離する。helloのlargest標本最小はpolicy3で69,632B、policy4で65,536〜67,584B。同じpolicy4・総空き112,548Bでもlargestが2,048B違った。旧§5.4の単発差をD42由来と断定する根拠にはならず、旧差の原因特定は残る。全7負荷中のlargest最小はpolicy0〜4で62,464B、固定4096で59,392B。taffy59,296Bとの差は名目3,168B対96Bであり、今後の追加確保やヘッダ/整列分まで保証する余裕ではない。heap値は低頻度標本、segment保持ピークとは同時点とは限らない。

再現集計: `python tools/vm_segment_report.py --repair-pattern 'segments-repair-v2-*.jsonl'`。元は`.cache/vm/segments-repeat-v2-*`、取り直しは`.cache/vm/segments-repair-v2-*`、統合した出所と84条件の要約は`.cache/vm/segments-repeat-v2-summary.json`。速度改善の数値は主張しない。既定値・JS挙動は変更せず、backlogのサイズ再検証項目2を完了とする。

最終のhost採取/集計検査15件成功。実機は元Kasane appへ復元し、書き込みhash一致、起動3周・故障回復6種・HOME_READYを確認。app以外の領域は書き換えていない。

## 6. 関数ソースを保持しない（2026-09-17、`vm/strip-fn-source`）

上流の QuickJS は、関数を1つ解析するたびにその全文を `js_strndup` で複写して `JSFunctionBytecode.source` に持つ（内側の関数の本文は親の複写にも入るので二重に持つ）。読むのは `Function.prototype.toString` とデバッグ出力だけ。Kasane 移植の計測（[kasane-guest-memory.md](../kasane/kasane-guest-memory.md)）で、この複写がアプリあたり 2.5〜7.1 KiB と分かったので、出荷の既定で作らないようにした。

- **切替**: `CONFIG_POCKET_VM_STRIP_FN_SOURCE`（`main/Kconfig.projbuild`、既定 y）。`quickjs.c` の複写3箇所（通常関数、式本体のアロー、クラス）を飛ばす。n にすると上流と同じ。
- **失うもの**: JS 関数の `toString()` は、上流にもともとある「ソース無し」の分岐を通って `"function " + name + "() {\n    [native code]\n}"` を返す（`js_function_toString`）。この分岐では `func_kind` が更新されないので、async・ジェネレータ・アロー・クラスも接頭辞は `function ` になる。呼び出し、エラーメッセージ、スタックトレース、行番号は変わらない。

### 6.1 実機（実測(device)、vm/main 同一ソースで切替だけ変えた2ビルド、各2回）

`build_sfs_before`（`=n`）と `build_sfs_after`（既定 y）。数値は起動時の `MEM` 行（`memlog.py` が読むのと同じ行）で、2回の差は 50 B 以内。

| アプリ | js 前→後 | 差 | 実行中 free 差 | largest 前→後 |
| --- | ---: | ---: | ---: | ---: |
| hello | 96,090→93,310 | −2,780 | +2,750 | 102,400→106,496 |
| imucal | 108,972→104,232 | −4,740 | +4,652 | 31,744→38,912 |
| companion | 103,008→100,432 | −2,576 | +2,540 | 51,200→57,344 |
| pet | 115,448→111,324 | −4,124 | +4,092 | 45,056→49,152 |

`memlog.py --check`: DIRAM +0、idle free +0、running free +2,740（hello）、`MEMLOG_OK`。`smoke_device.py --cycles 20` は前後とも `SMOKE_OK 20`（故障回復6種 OK）。app バイナリは 2,211,536→2,211,408 B（−128 B）。Kasane 版アプリでの効果（計測では 4.8〜7.1 KiB）は統合後に測る。

### 6.2 ホストの関所（実測(host)）

各変種 X を、同じフラグで切替だけ外した `X-keepsrc` と並べた。

| 関所 | 既定（strip） | `-keepsrc` |
| --- | --- | --- |
| コーパス o2 / asan / asan-recur / asan-flat / o2-eager | 75/75 | 75/75 |
| コーパス asan-alloca | 72 合格・3 skip | 同じ |
| コーパス asan-yield / asan-tco / asan-lazy | 75/75 | 74/75（`seg_oom_boundary`） |
| asan-yield `--force-yield` | 75/75 | 74/75（`seg_oom_boundary`） |
| asan `--budget-jobs 3` | 75/75 | 75/75 |
| Test262 部分集合 asan / o2 | 7,501 合格、退行 0 | — |
| oom_canary o2 / asan | OK | — |
| budget_probe o2 / o2-recur | OK（11/11） | OK（11/11） |

`seg_oom_boundary` は backlog #15 の既存の失敗で、`-keepsrc` 側（＝変更前）で落ちる。既定で通るのは、ヒープのバイト配置が変わって2回目の OOM でエラーオブジェクトを作る余地が残ったからで、**直ったのではない**。#15 は未着手のまま。

**テストの扱い**

- `special_calls` の toString 行: 期待値を分けた。`expected/` は出荷の既定（名前だけの形）、`expected-keepsrc/` はソース全文。`run.sh` は `-keepsrc` 変種で `expected-keepsrc/` を優先する（`--fair` と同じ仕組み）。行を外さなかったのは、両方の挙動を関所に残すため。
- `--fail-alloc` の番号: 目標の手前で解析される関数の数だけ試行番号が前にずれる。番号を動かして何かが落ちるのを待つのではなく、`-keepsrc` で元の番号を失敗させたアロケータのトレースを取り、既定ビルドで失敗レコードの種類と大きさ、直前 40 件の大きさがすべて一致する番号を探した（o2 と asan-alloca の両方で同じずれ）。`oom_resolving_functions` 1352→1351、`oom_callsite_double_free` 1458→1455、`oom_with_resolvers` 1370→1369、`oom_resolving_functions_module` 1289 は変化なし。`-keepsrc` は先頭5行の `// vmrun-keepsrc-flags:` で元の番号を使う。alloca の skip 指定に `-keepsrc` 名も足した。
- `budget_probe.sh` の `deep_async_recursion`: flat では外側の catch が表示されるかどうか（`caught null` の行）を差分から外し、情報行（`outer=`）にした。この行はヒープに残るバイト数だけで決まり、D42/D43 で消え、今回また現れた（深さ 80→81）。拘束するのは「同期 try に届かない」「終了コード 2」「OOM カナリアが鳴った」で、変更前と同じ。-recur の `caught RangeError` は C スタック検査の結果なので引き続き差分に入れる。

**Test262 の toString（部分集合の外、`built-ins/Function/prototype/toString`、o2）**: 既定 150 合格／10 不合格、`-keepsrc` 158／2。新しく落ちるのは 5 ファイル×2 モード: `method-computed-property-name`、`private-method-class-expression`、`private-method-class-statement`、`private-static-method-class-expression`、`private-static-method-class-statement`。名前が計算プロパティや `#x` になり、代替の文字列が NativeFunction の文法に合わない。そのほかの toString テストは `[native code]` 形も受け付けるので通る。逆に `line-terminator-normalisation-LF` は既定でだけ通る。

再現: `bash tools/vmtest/build.sh o2 && bash tools/vmtest/build.sh o2-keepsrc`、`python3 tools/vmtest/test262.py --variant o2 built-ins/Function/prototype/toString`（チェックアウトに `git -C .cache/test262 sparse-checkout add test/built-ins/Function/prototype/toString` が必要）。
## 7. §4.23 の既存失敗2件の原因（backlog #15、2026-09-23、`vm/main`）

どちらも VM の不具合ではなかった。1件はテストを直し、もう1件は時間の伸び方を測って記録した。

### 7.1 `seg_oom_boundary`: テストがヒープを上限まで詰めていた（実測(host)、device プロファイル）

`null` は、OOM の後に InternalError そのものを確保できなかったときの上流の挙動である（`JS_ThrowOutOfMemory` は入れ子の OOM を投げない）。

- OOM 時点の充当は `used=162,500`（-keepsrc）/ `162,180`（既定）で、上限 163,840 B に対して残りは約 1.3 KB だった。失敗した要求は `new Array(1 << 14).fill()` が 1.5 倍ずつ伸びる途中の realloc（57,552→86,320 B）。テストの冒頭コメントは「1回で上限を超える大きな要求」と説明しているが、実際には上限に忍び寄る形だった。`memory_device.js` は同じ理由で既に `Array.apply` に替えてあった（backlog #9 のとき）。
- 1回目と2回目の間に残るブロックを、アロケータのトレースとスタック表示で調べた。
  - 1,079 B は VM スタックのセグメントで、2回目に再利用されるので余裕を減らさない。
  - 183 B の `build_backtrace` の文字列を含む5ブロック 439 B は、1回目の Error と読める。トップレベルの catch 束縛がフレームの slot に残っていて、2回目の試行中も生きている。
  - 残りの 462 B（8ブロック）の中身は特定していない。
  - このため2回目の余裕は1回目より狭い。-keepsrc は関数ソースの分だけ 320 B 狭く、エラーを作れなかった。
- トレースを付けると GC 観測用オブジェクトの分だけ配置がずれ、失敗しなくなる。印を入れてソースを数バイト変えても同じだった。§6.2 の「配置が変わっただけ」はこの感度のことである。
- 修正: `Array.apply(null, { length: 60000 })` で配列を1回で要求する（host 960,000 B）。OOM 時点の充当は約 105,000 B に下がり、余裕は約 58 KB になった。期待出力は変えていない。
- 関所: `seg_oom_boundary` 単体で 22 通りすべて合格した。内訳は asan / o2 / asan-recur / asan-flat / asan-alloca / asan-yield / asan-tco / asan-lazy / o2-eager の各既定と -keepsrc、および asan-yield / asan-tco（両モード）の `--force-yield`。

### 7.2 `tco_guards`: 毎中断 GC が深さに対して2乗（実測(host)、asan-yield、`--profile host --gc-on-yield --force-yield`）

`wide(100000)` と `big(100000)` は予算の RangeError まで再帰する。`--force-yield` は呼び出しごとに中断し、`--gc-on-yield` は再開のたびに `JS_RunGC` を呼ぶ。GC は中断中の全フレームを辿るので、1回の費用が深さに比例し、全体は深さの2乗になる。

| `--vm-budget` | safepoint | 時間 |
| --- | ---: | ---: |
| 256K | 2,248 | 0.4 秒 |
| 512K | 4,332 | 2.2 秒 |
| 1M | 8,500 | 7.1 秒 |
| 2M | 16,838 | 31.0 秒 |
| 既定（7 MiB、打ち切りなし） | 58,518 | 366 秒 |

既定の予算でも完走し、出力は期待値と一致した。366 秒は別のビルドと並行して測った値。§4.23 の exit=124 は `run.sh` の 300 秒打ち切りで、ハングではない。予算拒否の検査は、`tools/vmtest/README.md` のとおり `VMTEST_VMRUN_FLAGS='--gc-on-yield --vm-budget 64K'` で行う。

## 8. YIELD 既定化の判断材料（backlog #11、2026-09-23、`vm/l2c-integration`）

`vm/main` の `afb2c89` を基点に、worktree で `build_y`（`CONFIG_POCKET_VM_YIELD=y`、FAIR/TCO は n）と
`build_n`（既定、YIELD=n）を作って比べた。どちらも同じソース・同じ `sdkconfig.defaults` で、
違いは YIELD の1行だけ。COM3 へ交互に焼いて測った。

### 8.1 実アプリの frame 累積時間（実測(device)、`build_fprobe`）

frame ガードは `VM_FRAME_RUNAWAY_US`（250,000µs）に対して1回の `frame()` の累積実行時間を見る。
その分布を取るため、完了した frame ごとに最大値と閾値超過数を数える計測パッチを当てた
（コミットしない。`guest.c` の2つの完了地点と `app_session.c` の `app_stop`。`FPROBE` 行を出す）。
各アプリを無操作で30秒走らせ、Back で終了したときの1行を読む。

| アプリ | frame 数 | 最大µs（無操作2回 / キー操作1回） | 20ms 以上 | 50ms 以上 |
| --- | ---: | --- | ---: | ---: |
| HELLO WORLD | 911 / 916 / 960 | 262 / 344 / 990 | 0 | 0 |
| IMU CALIBRATION | 901 / 897 / 951 | 6,283 / 5,702 / 5,822 | 0 | 0 |
| POCKET PET | 893 / 894 / 892 | 47,426 / 18,404 / 18,314 | 1回目のみ1 | 0 |
| PET COMPANION | 887 / 888 / 889 | 3,307 / 3,183 / 3,637 | 0 | 0 |
| KASANE DEMO（`K`） | 851 / 851 | 5,605 / 5,490 | 0 | 0 |

- 最大は POCKET PET の 47.4ms で、閾値の 5.3 分の1。2回目以降は 18.4ms で再現しないので、
  起動直後の一度きりの費用と読める（何の費用かは特定していない）。
- 多ターンに跨る drain は全アプリ・全回で 0 件（`drain_max_us=0`）。`VM_RUNAWAY_US` 側は
  この5本では一度も働く場面が無い。
- キー操作の回は 0.5 秒ごとに `e`/`u`/`d`/Enter を送った。効果音が鳴るので音声との競合を含む。
  POCKET PET と PET COMPANION は保存データを変えないため無操作のままにした。
  KASANE DEMO はキーでデモ自体が終わるので、この回の値は無い。
- 無操作と軽いキー操作だけの条件であり、Wi-Fi 通信・音声再生・SD 読みを統制した競合条件では
  測っていない。**この5本で余裕が5倍あることは、あらゆる負荷での安全余裕の証明ではない。**

### 8.2 通知なし通常経路の性能（完了条件#7の後半）

**実機（実測(device)、hello、`benchmark_app.py`、音設定は変更せず）**: 2ビルドを交互に2回ずつ焼き直した。

| | turn_ms | render_ms | send_ms |
| --- | ---: | ---: | ---: |
| `build_n`（YIELD=n） | 0.17 / 0.17 / 0.17 | 0.90 / 0.91 / 0.91 | 0.67 / 0.68 / 0.68 |
| `build_y`（YIELD=y） | 0.22 / 0.22 / 0.22 | 0.93 / 0.91 / 0.93 | 0.68 / 0.68 / 0.68 |

JS ターンが +0.05ms（+29%）で、3回とも同じ値だった。render と send は動かない。
ビルド間の命令キャッシュ配置で15%動きうる（CLAUDE.md）が、差はそれより大きく、
全サンプルで一定で、同じ経路の render/send が動いていない。hello の1フレームは 30fps で
33ms あるので、+0.05ms は1フレームの 0.15%。**hello 以外のアプリでは測っていない。**

**ホスト（実測(host)、`timing.py -n 15`、`o2` と `o2-yield` を2巡）**: 同じ実行ファイルの
2巡の中央値が最大2割動く環境で、YIELD 有効が遅い方向へ出た項目は無かった
（`bench_loop` は 68.5/74.9 に対し 55.2/59.8）。**この差も雑音の幅を超えないので、速くなったとは言わない。**

**静的（実測、同一ソース）**: Flash Code 1,398,088→1,401,872 B（+3,784）、
静的 DIRAM 159,644→159,676 B（+32）、image 1,955,676→1,959,620 B。

### 8.3 残り

物理キーでの入力確認、統制した競合条件（音声再生・Wi-Fi 通信・SD）での frame 時間、
閾値近傍の誤停止検査、総合関所。既定値（YIELD/FAIR/閾値）の決定はそれらの後。

## 9. 最大連続ブロックの2 KiB変動は現行ファームでは再現しない（backlog #8、2026-09-23、`vm/l2c-integration`）

§5.4 の単発差（taffy 59,296 B 段への余裕 6,240→4,192 B）を D42 に帰属できなかったのは、
§5.8 で「同じ policy・同じ総空きでも largest が 2,048 B 違う」標本があったためだった。
その揺れを現行の `vm/main`（`715ffb5`）で探した。**16標本で一度も動かなかった。**

| 採取 | 条件 | 結果 |
| --- | --- | --- |
| `memlog.py --port --check` ×6 | 既定ビルド、hello、毎回再起動 | 6回とも `idle_free=233,792 / app_free=136,988 / app_largest=94,208 / js=88,708`（完全一致） |
| VM_PROBE の WINDOW 行 ×12 | hello を1回走らせ、1秒窓ごと | 12窓とも `heap_free_min=130,704 / heap_largest_min=86,016` |
| 負荷を変えて再起動 ×6 | X/B/X/B/A/X | 同じ負荷なら毎回同じ値（X: 130,704/86,016、B: 149,736/106,496、A: 150,404/106,496） |
| 競合条件 W（UI＋音声＋Wi-Fi）×4 | hello、毎回再起動 | 4回とも X と同じ値 |

- 走行中（窓ごと）にも、再起動を跨いでも、負荷が同じなら値は動かない。2,048 B の揺れは観測できない。
- **比較対象そのものが無くなっている。** 6,240→4,192 B は「taffy が要求する 59,296 B の単体確保に対する余裕」であり、
  その Rust UI コアと taffy は 2026-09-17 の CP24–25 で出荷物から削除された（[kasane-progress.md](../kasane/kasane-progress.md)）。
  59,296 B を1回で確保する利用者は今のファームに存在しない。
- 2026-09-16 のファーム（関数ソース保持あり、旧UI経路あり、Kasane 以前の画面）で見えた揺れの原因は、**特定していない**。
  現行で再現しないという事実であって、当時の原因が分かったのではない。同じ現象が再び出たら、その時点のバイナリで採り直す。

再現: `python tools/memlog.py --map <build>/cardputer_pocketjs.map --port COM3 --check` を繰り返す。
窓ごとの値は `CONFIG_POCKET_VM_PROBE=y` のビルドで `VMPROBE WINDOW` 行を読む。

### 8.4 閾値の近くと競合条件（実測(device)、2026-09-23）

**閾値近傍**: SELFTEST の有限 frame 診断の反復数を 44,000 / 48,000 / 52,000 へ差し替えた
計測専用ビルド（コミットしない。既定は 20,000 / 40,000 / 100,000）で、各2回。

| 反復数 | 結果 | 累積µs（2回） |
| ---: | --- | ---: |
| 44,000 | 完走 | 219,821 / 219,827 |
| 48,000 | 完走 | 239,623 / 239,657 |
| 52,000 | frame ガードで停止 | 255,946 / 255,916 |

閾値 250,000µs に対し、**239.6ms（96%）の frame は4回とも完走し、停止時の行き過ぎは 5.9〜6.0ms**。
8ms ターンの後に検査する構造と整合する。再現性は2回の差で 6〜36µs。
USB へ 20ms ごとにバイトを送る負荷では 239,699µs（+約 50µs、0.03%）で、CPU の競合にはならない。

**競合条件**（`vm_l0_capture.py`、workload B=deep_recursion、各12〜15秒、VM_PROBE ビルド）:

| 条件 | frame 中央値 | p95 | 最大 |
| --- | ---: | ---: | ---: |
| base | 1.22 ms | 1.29 | 1.32 |
| audio（440Hz トーンを連続再生） | 1.24 ms | 1.44 | 3.36 |
| wifi（リンク＋3秒ごとの GET） | 1.32 ms | 1.64 | 9.39 |

ガードは横取りされた時間も数えるので、この増分はそのまま余裕を削る。中央値で +2%（audio）/ +8%（wifi）、
**1フレームの最悪値で +2.0ms（audio）/ +8.1ms（wifi）**。§8.1 の最も重い実アプリのフレーム（POCKET PET 47.4ms）へ
足し合わせると 50〜59ms で、250ms までなお4倍以上ある。ただしこれは**別の負荷で測った増分を別の負荷へ当てた推定**であり、
実アプリを競合条件下で測った値ではない。倍率で外挿していないのは、横取りは長いフレームほど比例して増えるのではなく
期間あたりの CPU 取り分として乗るため。

**条件スクリプトの欠損（記録）**: `apps/vmprobe/condition.js` の UI 条件（mask 1）は旧 `pocket.ui` を呼ぶ。
CP24–25 でその API は削除されたので、`ui` と `all` の条件は現在そのまま走らせても UI 負荷を作らない。
本節が audio と wifi を単独で使ったのはそのため。直すなら `pocket.kasane` で書き直す。

### 8.5 総合関所（実測(device)、2026-09-23、`d54f753`）

`tools/system_full_test.py` を `--cycles 30` で通した。**FULL_PASS、全10段階成功。**
firmware SHA-256 `68b23d90ef141ca8a338bb0e011e807afc6d8166bb111ffa51902bce0c8c9cda`、
報告は `.cache/system-full-backlog11/report.json`。

| 段階 | 秒 |
| --- | ---: |
| system-host / kasane-native / kasane-js（sanitized・optimized） | 29.7 / 238.4 / 9.6 / 7.2 |
| firmware-build / taffy-exclusion / flash | 260.5 / 1.9 / 16.1 |
| device-animation / device-system / device-lifetime（30周） | 12.0 / 7.7 / 23.8 |

30周すべてで free 219,800 / largest 77,824 B の一定値。`--cycles` は既定の100ではなく30。
報告が対象外と明記するもの: 実LCD・音声の物理確認、SNTPの実時刻変更、sleep電流、全周辺機器と
ファイルシステムの故障系統。この関所は L2c の既定値そのものを検査するものではなく、
**YIELD=n の出荷構成が壊れていないこと**を示す（ビルドは `sdkconfig.defaults` のまま）。

### 8.6 L2c を既定にする（2026-09-23、`main/Kconfig.projbuild`）

`CONFIG_POCKET_VM_YIELD` を `default n` から `default y` にした。根拠は §8.1〜8.5 の実測で、
値段（JSターン +0.05ms、Flash +3,784B、DIRAM +32B）と、持ち込む frame ガードの余裕
（実アプリ最大 47.4ms に対し閾値 250ms、239.6ms は完走・256.0ms で停止）を測ったうえでの切り替え。
`POCKET_VM_TCO` と `POCKET_VM_FAIR` は n のまま — 前者は `Error.stack` と再帰上限を、
後者は JS から観測できるジョブ順序を変えるので、値段ではなく互換性の判断が要る。

新しい既定での総合関所（`--cycles 30`）: **FULL_PASS、全10段階**。
firmware SHA-256 `9abbb52c8acb1937…`、30周すべてで free 219,768 / largest 77,824 B。
報告は `.cache/system-full-yield-default/report.json`。YIELD=n の同じ関所（§8.5）との差は
free −32 B で、静的 DIRAM の +32 B と符合する。

ホスト側も既定を写した（`tools/vmtest/build.sh`）。無印の `asan` / `o2` が YIELD 有効になり、
旧経路は新しい接尾辞 `-noyield` で作る。`-alloca` と `-recur` は FLATCALLS を落とすので
YIELD も一緒に落とす（Kconfig の `depends on` と同じ規則）。コーパスは `asan` 75/75、
`asan-noyield` 75/75。

### 8.7 実アプリ（hello）を競合条件下で（実測(device)、2026-09-23、VM_PROBEビルド）

§8.4 は合成負荷で測った増分を実アプリへ当てた推定だった。実アプリそのものを条件下で回せるようにした:
`app_session.c` の条件適用を `'X'`（hello）にも広げ、**UI ビットだけは落とす**
（hello は自分の Kasane scene で APP 層を持つので、条件側が2枚目を建てることになる）。
`vm_l0_capture.py` の「hello は base だけ」という拒否も、`ui` を含むときだけに変えた。
どちらも `CONFIG_POCKET_VM_PROBE` の中で、出荷ビルドには入らない。

| 条件 | frame 中央値 | p95 | 最大 | 完了遅延 p95 |
| --- | ---: | ---: | ---: | ---: |
| base | 0.09 ms | 0.16 | 0.21 | — |
| audio | 0.16 ms | 0.33 | 2.22 | 0.25 ms |
| wifi | 0.25 ms | 0.71 | 8.46 | 7.68 ms |
| all（audio＋wifi） | 0.27 ms | 1.74 | 9.90 | 7.80 ms |

- **最悪の1フレームは 9.90ms**で、frame ガードの 250ms まで25倍ある。
- 増分（base比 +9.7ms）は §8.4 が deep_recursion で測った +8.1ms と同じ桁で、
  「横取りは期間あたりの取り分として乗る」という読みと整合する。
- §8.1 の最も重い実アプリのフレーム（POCKET PET 47.4ms）は**この条件下では測っていない**。
  同じ増分を当てれば約 57ms（4.4倍の余裕）だが、それは推定のまま。
  PET と COMPANION は probe の入口を持たないため、測るには同じ拡張をもう一段必要とする。

### 8.8 POCKET PET と PET COMPANION を競合条件下で（実測(device)、2026-09-23、VM_PROBEビルド）

§8.7 で残っていた「いちばん重いフレームを持つアプリ」を測るため、probe の入口を2本足した
（`'<'` POCKET PET、`'>'` PET COMPANION）。この2本は**自分の manifest で起動する** —
capability の注入（`pet.companion`）と保存の持ち主が manifest で決まるので、hello の身元では
どちらも得られない。UI ビットは hello と同じ理由で落とす。probe ビルド限定で、出荷には入らない。
**この2本は自分の保存データを普通に書く**（PET の autosave）。

| アプリ | 条件 | frame 中央値 | p95 | 最大 |
| --- | --- | ---: | ---: | ---: |
| POCKET PET | base / audio / wifi / all | 0.95 / 1.00 / 1.08 / 1.14 ms | 3.71 / 3.85 / 4.04 / 4.26 | 4.17 / 4.17 / **12.90** / 9.79 |
| PET COMPANION | base / audio / wifi / all | 0.23 / 0.28 / 0.36 / 0.41 ms | 0.41 / 1.76 / 0.67 / 2.77 | 2.65 / 3.13 / 3.46 / **10.34** |

- **最悪の1フレームは PET の 12.90ms**（Wi-Fi 条件）で、frame ガードの 250ms まで19倍ある。
  hello（§8.7、9.90ms）、deep_recursion（§8.4、9.39ms）と同じ桁で、増分は横取りの取り分として乗る。
- §8.1 が FPROBE で見た PET の 47.4ms はここには出ていない。あちらは起動直後を含む30秒の全フレーム、
  こちらは12秒の窓。**起動直後の重いフレームと競合条件を同時に捉えた測定ではない。**
  最悪同士を足すと 47.4＋9.7≒57ms で、それでも4倍以上の余裕になる（これは推定）。
- 条件下では空きが減る: PET の `heap_free_min` は base 109,388 → all 38,600 B、
  `largest` は 65,536 → 29,696 B。Wi-Fi のリンクと HTTP がゲストと同じ DRAM を使うため。
  **ゲストの上限（160 KiB）に触れてはいないが、余裕は Wi-Fi 条件でいちばん薄くなる。**
