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
- **既知の退行（承知の上）:** `run.sh --trace`（asan）と `--vm-seg-size 2048`（o2）で `gc_threshold_device.js` が落ちる。常駐セグメントがジワジワ型OOMの「残り」を動かし、catchした後の `print` 自体がOOMする。期待値は書き換えていない。通常の `run.sh` 4バリアントでは通る。詳細は [tools/vmtest/README.md](../../tools/vmtest/README.md) の既知の脆さの節。

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

## 4. L2c: 中断・再開の進捗

L2cの本体（`rt->vm_susp`/`vm_yield:`/`vm_resume:`）は未実装（backlog.md #7）。以下は済んでいる段の結果。

### 4.1 段A: async関数のフラット化（[vm-L2-design.md](vm-L2-design.md) §11、実測(host)、`vm/l2c`）

段A1（挙動不変の下拵え）: 全関所不変（44/44、7,501/194/0、G1 0.000、予算10/10、`--force-yield` 3/44）。

段A2（本体、`flat_async_call:`/`async_flat_return:` 実装後）:

| 関所 | flat | -recur |
| --- | --- | --- |
| G1（async） | NOT_PROPORTIONAL | PROPORTIONAL |
| `budget_probe.sh`（`deep_async_recursion`） | `budget_hits=0` + `InternalError`（ヒープ枯渇。D38の帰結） | `RangeError`（Cスタック検査） |
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
