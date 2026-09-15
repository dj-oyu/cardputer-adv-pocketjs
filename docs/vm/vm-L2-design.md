# VM L2 設計: 移動しない VM スタックと中断・再開

対象: [quickjs-freertos-vm-spec.md](quickjs-freertos-vm-spec.md) §7（L2）。方式は仕様どおり **L2a（非移動セグメント）→ L2b（明示フレームと単一実行ループ）→ L2c（セーフポイント・保存・再開）** の3段。

この文書は**現時点で決定していること**（何を、なぜ）を書く。事実の根拠は台帳（[vm-ledger/](vm-ledger/)）、実測値と関所の通過結果は [vm-L2-results.md](vm-L2-results.md)、末尾呼び出し最適化（TCO、未着手）は [vm-tco-design.md](vm-tco-design.md)、未着手・未確認の項目は [backlog.md](backlog.md) にある。決定は D1〜D43 の番号で管理し、コード側のコメントがこの番号と本書の節番号を引用する（§15 決定表）。**D番号は固定。節番号は本書の版で変わりうる** — コードから `sec.N` で参照している箇所は、この文書の改版ごとに突き合わせて直す。

## 実装状況（2026-09-15 時点）

| 段 | 内容 | 状態 |
| --- | --- | --- |
| L2a | 非移動フレームセグメント | 実装済み（`CONFIG_POCKET_VM_SEGFRAMES`） |
| L2a 追補 | セグメントサイズの線形成長・ターン単位キャッシュ（D42/D43） | 実装済み |
| L2b | 通常関数のフラット呼び出し | 実装済み（`CONFIG_POCKET_VM_FLATCALLS`、既定 on） |
| L2b 拡張 | JS から呼ぶ async 関数のフラット化（D31〜D38） | 実装済み |
| L2c | 中断・再開の関所とガード（`vmrun` の受け口、コーパス） | 実装済み（pass-through。`JS_VMResume` は常に例外を返す） |
| L2c 本体 | `rt->vm_susp` / `vm_yield:` / `vm_resume:` の実装 | **段3aまで実装済み**（ホスト所有SEG床、分類A、GC保護。`CONFIG_POCKET_VM_YIELD=n`が既定。async/job所有と破棄はbacklog.md #7） |
| L2c 実機統合 | 要求ビット・guest 側3状態・Back ターン対応 | 未着手（backlog.md #11） |
| TCO | 末尾呼び出しでのフレーム再利用 | 設計下書きのみ（[vm-tco-design.md](vm-tco-design.md)） |

---

## 1. 完了条件と関所

仕様 §7 の完了条件は7項目ある。判定する仕組みがリポジトリに無いまま実装だけを進めると、「何倍速くなった」「何バイト余分に保持している」という結論が計測点の抜けだけで誤りうる（[vm-L1-report.md](vm-L1-report.md) が実例）。**判定できない条件は、満たしたかどうかを言えない。** そのため実装より先に3つの判定機構（G1・G5・G6）を作ってある。

### 1.1 完了条件の対応表

| # | 完了条件（仕様§7） | 判定手段 | 現状 |
| --- | --- | --- | --- |
| 1 | 純粋な JS の深い呼び出しで C スタック使用量が深さに比例しない | G1（§1.2） | 判定機構あり。L2b（フラット化）以降で NOT_PROPORTIONAL（[vm-L2-results.md](vm-L2-results.md) §3.3） |
| 2 | 監査済みセーフポイントで停止し、再開後の値・副作用・例外が非中断実行と一致する | `--force-yield` + バイト比較 | 関所は動く（[vm-L2-results.md](vm-L2-results.md) §4.2）。L2c 本体が無いので実際の中断・再開はまだ判定できない |
| 3 | 同期 JS の途中に別 JS を挟まない。中断で共有オブジェクトの状態が不意に変化しない | 同上 | L2c 本体待ち |
| 4 | 中断状態での GC・OOM・終了・例外・finally・クロージャ保持 | 同上 + コーパス | ジョブ境界での該当ケースはコーパスが既に固定（`gc_threshold_device.js`、`try_finally.js`、`closures.js`、`stop_with_queue.js` 等）。opcode 粒度の中断状態は L2c 本体待ち |
| 5 | 正確な最大中断遅延を主張するなら、最長の中断禁止区間も測定する | G5（§1.3） | 判定機構あり。§2 の N1〜N5 が対象、G5 が実測している |
| 6 | セグメント追加・境界越え・返却で値とクロージャの参照が壊れない | **G6**: `tools/vmalloc/verify_all.sh`（[台帳07](vm-ledger/07-segment-g6.md)） | 判定機構は完成（アロケータ単体で36トレース＋故障注入5種）。実物のVMフレームをこのアロケータの上で検査するのはL2a実装後の課題（未着手、backlog.md #1） |
| 7 | 各確認地点の直前・直後に中断要求を発生させ、命令や副作用の重複・欠落がない／通知なし通常経路の性能低下も測る | 前半: `--force-yield`／後半: `timing.py` | 後半は既存インフラで足りる。前半は#2と同じ前提 |

### 1.2 G1・G5・G6

- **G1（Cスタック比例性）**: 再帰の各段でスタックポインタ相当（ローカルのアドレス）を記録し、深さNと2Nで消費が2倍にならないことを判定する（「溢れない」ではない）。ホスト（vmrun）で判定、実機は `uxTaskGetStackHighWaterMark` で追認。
- **G5（最長中断禁止区間）**: 中断を要求した時点から実際に停止した時点までをVM側でカウントし、その**最大値**を出す（平均・中央値では完了条件を満たさない）。ホストは `vmrun --gaps` / `corpus/g5_gaps.js` が、連続する停止機会（§2のA/B地点・ENTER・LEAVE）の間隔をns単位で最大値と上位8件（始点・終点の関数名付き）で出す。実機側は時計を渡す口のみで未実装。
- **G6（セグメント参照整合性）**: セグメント方式のアロケータ（`tools/vmalloc/adapter_segment.c`）を36トレースで再生し、追加・境界越え・返却のたびに全生存ブロックの内容・非重複・所属を検査する。故障注入5種が全部捕まることを同じスクリプトが要求する。詳細は[台帳07](vm-ledger/07-segment-g6.md)。

**強制yieldフック（`vmtest_vm_set_force_yield`）は埋まっている。** `components/quickjs-ng/quickjs-ng/quickjs-vm.c` が定義し `tools/vmtest/build.sh` がリンクする。止まる地点は§2の分類Aの7地点だけで、「止まる」は現状の捕捉不能な `interrupted` 例外のまま（L2c本体が置き換える）。埋める先をVM側の新設ファイルにしたのは、`quickjs.c` 自体に置くと `tools/vmtest/build.sh` の変更は不要になる一方、上流との差分が広がるため。

**流用してはいけないもの:** `vmrun.c` の `JS_SetInterruptHandler(...)` は `host_stopping`（セッション終了検知）専用。ここに相乗りすると終了と中断が同じ経路で混線する（台帳03が記録する「割り込みが捕捉できない例外として実装されている」現状との衝突そのもの。§4で正面から決める）。

### 1.3 セーフポイントが届かない区間（N1〜N5）

仕様§7は「選んだopcodeの入口または処理完了後に固定の中断確認を置く」方式だが、**バイトコードのディスパッチループに戻ってこない区間には、opcodeに確認を何個置いても届かない。** 仕様は「未対応経路は従来の同期呼び出しとして囲い、中断禁止を明示する」としており、以下がその一覧（G5の対象そのもの）。

| # | 区間 | 上限 | 事実 |
| --- | --- | --- | --- |
| N1 | 正規表現マッチ | 無し（入力とパターン次第） | `lre_check_timeout` は `interrupt_counter` を経由せず `rt->interrupt_handler` を無条件に直接呼ぶ。真を返すと `js_regexp_exec` が `JS_ThrowInterrupted` する箇所が2つ。opcode確認の完全な外側にある既存の唯一の別系統（台帳03、台帳04） |
| N2 | ネイティブ関数だけで完結する処理 | 無し | `js_call_c_function` 系の呼び出し経路に `js_poll_interrupts` が1つも無い。JSコールバックを介さない大きな `JSON.stringify` 等はここに入る |
| N3 | `for-in` の1ステップ | オブジェクトの `prop_count`／配列の `array_length` | `js_for_in_next` の内部ループが、非enumerableなプロパティと削除済みプロパティを中断確認なしに読み飛ばし続ける。上限は有限だが、1回の `OP_for_in_next` が全プロパティ数に比例して長くなりうる |
| N4 | パース | ソース長 | direct eval のパース段。実行段は通常の `JS_CallInternal` に戻るので既存機構に乗る。起動時パースと同種 |
| N5 | 関数プロローグ（`new_target` をローカルへ写すまで） | 固定長・短い | §5のD2の条件。ここで中断しないなら `new_target` を保存しなくてよい |

**届く経路として決着したもの:**

- **`for-of` はそのまま乗る。** `js_for_of_next` は内部でループせず `JS_IteratorNext` を1回呼んで返る。反復はバイトコードの後方分岐が回すので、毎イテレーションでディスパッチループに戻る。N3（for-in）と対照的に扱う。
- **`apply` は有界。** `js_function_apply`/`build_arg_list` に中断確認は無いが、`len > JS_MAX_LOCAL_VARS`（65535）を弾くので内部の作業量に定数上限がある。引数を組んだ後は `JS_Call` でバイトコード実行に戻る。N3より優先度は低い。

N1〜N4を機構で埋めない決定はD3（§15）。

---

## 2. メモリ表現: 実サイズ・整列・JSValue

### 2.1 実機の構造体サイズ（実測(host)、実機と同じコンパイル行でクロスコンパイルして`.rodata`から読み出した値）

| 型 | 実機 (ESP32-S3, 32bit) | 64bit ホスト | 備考 |
| --- | --- | --- | --- |
| `JSValue` | **8** | 16 | 実機は `JS_NAN_BOXING` 有効 |
| `JSObject` | **48** | 72 | |
| `JSShape` | **48** | 64 | |
| `JSShapeProperty` | 8 | — | |
| `JSProperty` | 8 | — | |
| `JSVarRef` | **32** | 48 | |
| `JSStackFrame` | **48** | — | |
| `JSString` | 28 | — | |
| `JSFunctionBytecode` | 92 | — | |
| `JSGCObjectHeader` | 16 | — | |
| 初期shape（4,2） | **80** | 96 | |

`JSValue`/`JSStackFrame`/`JSVarRef` の3値は実機の `VMPROBE STATIC` の報告値（[vm-L0-report.md](vm-L0-report.md) §2.1）と一致する。

確保サイズの分布（ホスト実測、コーパス36件全件）で正体が判明したクラスは実機換算で48・80・48・32になり、**すべて16の倍数**（未判明クラスは32・24・8バイトのホスト側の値で残る）。実機の確保サイズ分布そのものは未取得（恒久ハーネスにヒストグラムを足せば取れる、backlog.md参照）。

### 2.2 整列（D7）

**決定: 個々の確保に8バイト境界を強制しない。4バイトで足りる。ただしセグメントの先頭は16バイト境界に取る。**

- 実機コンパイラで `_Alignof(uint64_t)=8`・`_Alignof(void*)=4`・`sizeof(max_align_t)=16` だが、**Xtensa LX7に64bitのロード命令が無い**。`uint64_t` の読み出しは4バイト整列アクセス`l32i.n`2命令に展開される。よって4バイト境界に置いた `JSValue` をスカラーで触っても落ちない。
- **セグメント先頭を16バイト境界に取る理由はPIEの一括転送**（`ee.vld.128.ip`/`ee.vst.128.ip` が16バイト/命令、`JSValue` 2個分を1命令で動かせる）。L2は生存セグメントを移動しないのでここでは効かないが、**L4のコンパクションはコピー速度がそのまま停止時間になる**（仕様§9）。セグメント単位（数KBに1回）の要求なので個々の確保の余白は増えない。
- **この整列は守らないと静かに壊れる。** 128bitアクセスはアドレス下位4bitを0に強制し、**例外は上がらない**（[pie-simd.md](../perf/pie-simd.md) §1.3）。読み込みは非整列でも `EE.LD.128.USAR.IP` + `EE.SRC.Q` の組み合わせでコピー無しに読めるが、**書き込みは非整列ではできない**（`EE.VST.128.IP` はアドレス下位4bitを常に切り捨てる。名前に非整列が見える命令もデータ側をずらすだけで書き込み番地は変わらない）。したがって「整列を諦めてもPIEで埋められる」は成り立たず、16B境界のセグメント先頭がストアの使える唯一の場所になる。

**VMの中でPIEストアを使うために整列を揃えるか → 揃えない。** フレームを組むときの一括充填（`var_buf[i]=JS_UNDEFINED`、`sf->var_refs[i]=NULL`）が形の合う唯一の候補だが、実機の典型的なフレームはスロット数が少なく（推測、未計測）、`JS_VM_FRAME_ALIGN` を上げて揃えるとフレームごとの詰め物と切り上げでスタック使用量が1割前後増える。ヒープ側の一括コピー・クリアもアロケータ仕事全体の0.3%程度（推定）で、ビルド間で15%揺れる世界では主張できない差になる。**この機体で足りないのはサイクルよりRAM。** 再検討の条件はVが大きい関数ばかりのワークロードが主になったとき、あるいはL4でセグメント単位の一括転送を書くとき（こちらは既に揃っている）。

### 2.3 JSValueの表現（D9、既決）

**決定: L2では触らない。** 実機は既定でNaN boxingが有効で `sizeof(JSValue)=8`。NaN boxingはIEEE754のdoubleを64bitの中にそのまま格納しており（ビットパターンをオフセットするだけで確保は発生しない）、4バイトに切るとタグ付きポインタか小整数しか入らなくなり、非整数の数値をすべてヒープ箱詰めする必要が出る。払っている代価はXtensa側の命令数（読み出し1回が `l32i.n`×2）で、これは§2.2で測ったとおり。**値表現の変更は上流の値モデルそのものの変更であり、呼び出しとフレーム管理の話ではない。**

---

## 3. 中断を例外にしない（D1）

### 3.1 現在の「中断」が通っている道

`JS_ThrowInterrupted` は `InternalError: interrupted` を**uncatchableフラグ付きで投げる**。インタプリタの `exception:` ラベルはuncatchableなら catch/finally探索を丸ごと飛ばし、`ret_val=JS_EXCEPTION` としてフレームを失い、`done`/`done_generator` へ合流する。この道をyieldに使えない理由は4つ、いずれも構造的:

1. `done:` がローカルとオペランドスタックを全部解放する。再開する気ならここは通れない。
2. `ret_val=JS_EXCEPTION` でフレームが失われる。呼び出し元に返るのは値であって、中断状態ではない。
3. `try`/`catch`/`finally` が素通りする。仕様の「中断時にfinallyの実行やフレーム解放を開始しない」は満たすが、それは中断が終了と同じ扱いだからで、再開できないことの裏返し。台帳03が記録するとおり、`await` 再開中に起きるとそのasync関数のPromiseは永久にpendingで残る。
4. 中断の道が確保をする。`build_backtrace` はuncatchableかどうか判定する前に走る。メモリが苦しいときこそ止まりたいのに、止まる道が確保を要求する。

### 3.2 中断の原語はVMの中に既にある

`done_generator:` は `sf->cur_pc=pc; sf->cur_sp=sp;` を書いて**ローカルを解放せずに返る**。これはasync/generatorのsuspendが使っている道で、L2が必要とする「保存して戻る」そのもの。**保存と復帰の形は流用できるが、C再帰を消す部分（L2b）は流用できない。**

### 3.3 決定

**D1: 中断を3つに分ける。YIELDは返り値であって、投げられる値ではない。**

| 種別 | 意味 | 通る道 | finally | 確保 |
| --- | --- | --- | --- | --- |
| **YIELD** | 予算切れ・プリエンプト。同じジョブを後で続ける | `done_generator` 形（`cur_pc`/`cur_sp` を保存して返る）。`exception:` を通らない | 走らせない | **してはならない** |
| **TERMINATE** | セッション終了・ウォッチドッグ。もう続けない | 現行の uncatchable 例外のまま | 走らない（現状どおり） | 現状どおり |
| **WAIT** | `await` など言語上の中断 | 既存のPromise機構のまま。L2は触らない | — | — |

仕様§7の `VMRunStatus` に写すとYIELD=`VM_YIELDED`、TERMINATE=`VM_FAILED`、WAIT=`VM_WAITING`。

帰結:

- ウォッチドッグ述語（`pocketjs_guest_set_watchdog`）の返り値は「止めるか否か」の真偽値から「どの種別で止めるか」に広げる必要がある。ホストは唯一の `JS_SetInterruptHandler` 呼び出し口（`guest.c`）で述語だけ差し替えるので、変更はその型と読み手1箇所で済む。
- **YIELDは確保をしてはならない。** `build_backtrace` を通らないことが要件で、これは上流のOOM時use-after-free（[backlog.md](backlog.md) の独立の不具合 項目6）をYIELD経路については構造的に避けることにもなる。
- **YIELDでfinallyを走らせない**のは仕様どおりだが、TERMINATEとの違いは「後で走る」か「二度と走らない」か。TERMINATEがfinallyを飛ばす現状はL2では直さない。
- `await` 中断でPromiseがpendingのまま固まる問題（台帳03）はYIELDについては起きなくなる（例外にならないため）。**TERMINATEでは残る。**

YIELDがどの地点で許されるか、`cur_pc` を命令の先頭に戻すかデコード後の状態を保存するかは§4（D8）で決める。

---

## 4. セーフポイントと再開PCの契約（D8）

**原則: デコード済み状態の保存はしない。命令が完全に退役した地点にしか置かない。** セーフポイントに許されるのは「`cur_pc`=次に実行する命令の先頭」「`cur_sp`=その命令の副作用を全部反映した後のスタック」「半分消費したオペランドも解放済みなのに残っている参照も無い」という地点だけ。これでデコード済みの途中状態を保存する必要が無くなる。

### 4.1 分類A: 既存の7地点

`goto`/`goto16`/`goto8`/`if_true`/`if_false`/`if_true8`/`if_false8`。分岐先オフセットを`pc`に加算して**分岐先PCを確定した直後**に確認する。確認地点では`pc`は分岐先（または次命令）、`sp`はポップ済み、解放すべきオペランドは解放済み。**命令は完全に退役している。**

### 4.2 分類B: 呼び出し地点

呼び出しは「退役して止まる」ではなく「呼び先のフレームを積んで、呼び元は待つ」。

- **呼び元の再開PCは上流が既に書いている。** `sf->cur_pc = pc`（呼び出し直前）が呼び出し命令の次を指す。これは変更不要。
- **復帰後の後始末は`cur_pc`から復元できない。** 呼び出しが戻ってから捨てるスロット数（`call_argc`）は、呼び先の`sf->arg_count`とは別物（宣言側の個数で上書きされるため）で、`cur_pc`からは逆算できない。
  → **決定: 復帰の形（捨てるスロット数とtailかどうか）をフレームに明示的に持たせる**（D2の隙間、`ret_shape`）。
- **「callを飛ばさない」保証はPCではなくフレーム鎖が担う。** 呼び元の`cur_pc`は呼び出しの次を指すので「呼び出しは済んだ」と「これから呼ぶ」はPCだけでは区別できない。**セーフポイントを「呼び先フレームを積んだ後」にだけ置き、「積む前」には置かない**ことで、未実行のcallを飛ばさないことが構造的に保証される。

**分類C（置かない場所）**: §2のN1〜N5。**分類D（既に離脱経路を持つもの）**: `return`系と`await`/`yield`/`yield_star`系は`done`/`done_generator`を通ってフレームを離れる。§3.2のとおり新しい契約は作らず既存に合わせる。

### 4.3 A地点だけでは足りない（G5実測が動かした結論）

起案時は「ループは必ず後方分岐を通るのでA(既存7地点)だけで足りる」としていたが、G5（`vmrun --gaps`）の実測で誤りと分かった。**JSを走らせ続けているのに7地点を一度も通らない区間が実在する** — 代表例は比較関数付き`sort`（`n2_native_sort_callback`、実測10.3ms）: 比較関数は毎回呼ばれるが分岐を持たないため7地点を1つも通らない。呼び出しのプロローグは中断**確認**の地点だが**停止機会ではなかった**（`JS_CallInternal`入口の確認はA地点に含まれない）。**分類B（呼び出し地点）はC再帰の解消のためだけでなく、中断のためにも必要。**

### 4.4 まとめ

| 分類 | 地点 | `cur_pc`の意味 | 追加で保存するもの |
| --- | --- | --- | --- |
| A | `goto`×3、`if_*`×4 | 次に実行する命令 | 無し |
| B | 呼び出し（呼び先フレームを積んだ後） | 呼び出し命令の次 | 復帰の形（D2の隙間） |
| C | N1〜N5 | — | 置かない |
| D | `return`/`await`/`yield`系 | 既存の`done`/`done_generator`に従う | 無し |

L2cが新設するセーフポイントはA(既存)とB(新規、フレームpush直後のみ)の2種。B地点の具体的な挿入位置（通常呼び出しプロローグ末尾）はL2bの実装で確定した（§10.5）。

---

## 5. フレームが運んでいないもの（D2）

### 5.1 `JSStackFrame`の実レイアウト（実測、offsetof）

| フィールド | offset | サイズ |
| --- | --- | --- |
| `prev_frame` | 0 | 4 |
| *(隙間)* | 4-7 | 4 |
| `cur_func` | 8 | 8 |
| `arg_buf` | 16 | 4 |
| `var_buf` | 20 | 4 |
| `var_refs` | 24 | 4 |
| `cur_pc` | 28 | 4 |
| `var_ref_count` | 32 | 2 |
| `arg_count` | 34 | 2 |
| `is_strict_mode` | 36 | 1 |
| *(隙間)* | 37-39 | 3 |
| `cur_sp` | 40 | 4 |
| *(末尾隙間)* | 44-47 | 4 |
| 合計 | | **48**（`_Alignof`=8） |

**11バイトが隙間**（4バイト幅を2つ、1バイト幅を3つまで、構造体を太らせずに足せる）。`JSValue`（8バイト、8バイト境界）は隙間に入らず、足せば48→56になる。参考として `JSAsyncFunctionState` は64バイトで、`this_val`(8)+`argc`(4)+`throw_flag`(1)+詰め物(3)+`frame`(48) — **フレームを太らせるのではなく、包む構造体に持たせる**設計が上流に既にある。

### 5.2 何を保存し、何を保存しないか

- **`flags`（`JS_CALL_FLAG_*`）は保存不要。** 参照はすべてディスパッチループの入口より手前にあり、フレームの形を決めたら用済み。再開側は定数として渡し直せばよい。
- **`new_target`も保存不要 — ただし条件付き。** 参照する3つのopcode（`OP_special_object NEW_TARGET`、`OP_check_ctor`、`OP_init_ctor`）はいずれも関数バイトコードのプロローグにしか現れない。ユーザーの`super()`は`OP_scope_get_var new_target`経由でプロローグが既にローカルへ写した値を読むので、関数の途中で生の引数が要ることはない。**条件は「プロローグの中で中断しないこと」**（§2のN5）。
- **`this_obj`は保存が要る。** `JSAsyncFunctionState.this_val` として上流が既に「Cスタックに置けないので逃がしている」唯一のもの。
- **`caller_ctx`は保存が要るが費用はゼロ。** `JS_CallInternal`の呼び出し元realmで、callee側の`ctx`（`b->realm`から復元可）とは別物、かつ復元不能。**`JSContext*`は4バイトで、offset 4-7の隙間にそのまま入る。**

### 5.3 決定

**D2: フレームを太らせず、`JSAsyncFunctionState`に倣って包む。**

1. **中断フレームは`JSStackFrame`を内包する構造体に持たせ、`this_val`はそこに置く。** フレームを直接太らせると、L2bがC再帰を消さない残り3つの呼び出し規約（`js_call_c_function`系）にも代償がかかる（それぞれが1段につき1個`JSStackFrame`を素のC自動変数として持つため）。加えて`js_check_stack_overflow`の閾値計算はこの`sf_s`自体を計測の外に置いているので、太らせた分はスタックガードから見えないところで効いてしまう。
2. **`flags`と`new_target`は保存しない**（§5.2のとおり再構成する）。
3. **プロローグを中断禁止区間に加える**（N5）。
4. **`caller_ctx`はoffset 4-7の隙間に置く。** 全フレームが必要とし4バイトで、ちょうど空いている。
5. **残る隙間の使い道**: 37-39の3バイトは中断状態の目印（§6、L2cの`l2_flags`）へ。44-47の4バイトは復帰の形（§4.2、`ret_shape`）へ。**隙間は「全フレームが常に必要とするもの」にだけ使う** — 中断フレームだけが必要とするもの（`this_obj`等）は包む側へ置く。

**この結果、`JSStackFrame`は48バイトのまま。** `caller_ctx`と中断の目印が隙間に入り、`this_val`だけが包む側に出る。

L2bで追加確定した2点（D11・D12、§10.4）: `sf->arg_count`は再帰復帰後の後始末のため真の`argc`を残す方向に変更する。呼び出し元の退避`sp`はセグメントのブロック（`JSStackFrame`ではなく、push時に確保する側）に置く。

---

## 6. 中断フレームとGC（D4）

QuickJSは参照カウントが主、循環参照GCが従。循環GCは「各GCオブジェクトが申告した子への参照を辿ってカウントを引き、0になったものは輪の中からしか参照されていない」と判定するため、**GCが辿らない場所からの参照は減算されず対象は生き残る**（安全側）。実行中の関数のローカル（Cスタック上）をGCがwalkしなくても壊れないのはこのため。

`async_func_mark`はこの境界をそのまま実装している: `cur_sp`が非NULL（＝中断中）のときだけ`arg_buf`から`cur_sp`までをmarkする。**この1つの分岐が「実行中」と「中断中」を分ける唯一の目印。** 失敗の仕方は非対称: markしない→漏れる（回収されないが落ちない）。間違ってmarkする（実行中を古い`cur_sp`でwalkする）→早すぎる解放=use-after-free。

L2が作る「普通の関数の中断フレーム」はセグメントの中にいて、**GCオブジェクトの誰にも所有されていない**（今「中断できるフレーム」はasync/generatorだけで、それらは`JSAsyncFunctionState`に包まれ持ち主がGCオブジェクトなので辿れる）。

### 決定

**D4: 中断フレームは、GCが辿れるものから所有される。目印は明示的に持つ。**

1. **所有者を作る。** 中断フレームを、mark関数を持つGCオブジェクト（またはruntimeが持つ中断フレーム一覧で`gc_mark`から辿るもの）にぶら下げる。`async_func_mark`から辿られているのと同じ形。
2. **「実行中／中断中」の目印を`cur_sp`の再利用に頼らない。** L2のフレームは`cur_sp`が非NULLのまま実行中になりうるので、専用ビット（§5.3の隙間）を使う。
3. **フレームを汎用的に歩く仕組みを足すときは、`class_id`のガードを外さない。** ガード無しに`JSFunctionBytecode*`等を読むと未初期化のCスタックメモリを読む（台帳02）。

**GC閾値修正（[backlog.md](backlog.md) の独立の不具合 項目5）とは順序の依存がある。** 実機では循環GCの自動閾値（256KiB）がゲストのヒープ上限（160KiB）より大きく、`JS_SetGCThreshold`/`JS_RunGC`をどこからも呼ばないため循環GCは事実上一度も走っていない。だから§6.1の漏れは今は表面化しない。**閾値の修正はL2c（中断・再開の統合）より後に入れる。** どちらの順でも、L2cの関所に「GCを強制的に回した状態で中断・再開してもコーパスがバイト一致する」を入れる（完了条件#4の対象）。

---

## 7. フレームセグメント — L2a（D5〜D7、D42、D43）

**アロケータの選定（D5）**: 台帳06の3実装比較（tlsf/estalloc/naive）を入力に、L2aは専用のセグメント方式アロケータ（`quickjs-vmstack.h`）を新設する。estallocの`assert()`がNDEBUGで消える懸念は成立しない — estallocはファームに組み込んでおらず、実機ビルドは`-DNDEBUG`を渡していない（`CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=1`）。

**JS用セグメントとtaffyの共有／分離（D6、未決）**: 今日は同じ共有ヒープを使っている。分離するとCLAUDE.mdのtaffyノード段差（2,048/29,648/59,296B）に対する余裕の意味が変わるが、**L2aのセグメントが実在してからでないと実機の空き方が測れず、測ってからでないと決まらない**。

### 7.1 セグメントの形

16B境界のセグメント、内部は4B粒度のfirst-fit＋free時の即時結合。超過サイズ（標準セグメントに収まらない）は要求量ちょうどの専用セグメント（1ブロック、freeで即返却）。整列はD7（§2.2）のとおり。

### 7.2 サイズと成長（D42）

**決定: 鎖のn本目のセグメントは`min(JS_VM_SEG_FIRST × n, JS_VM_SEG_MAX)`。倍々ではなく線形。** `JS_VM_SEG_MAX=4096`（taffyの59,296B段とぶつかる大きさを避ける）、`JS_VM_SEG_FIRST=512`（実機の典型的なフレーム使用量、[vm-L2-results.md](vm-L2-results.md) §5.1が実測450B前後、が1本に収まる）。大きさは**鎖の上の位置**で決め、過去の確保回数では決めない（一度深く潜ったアプリが以後ずっと大きく取らないため）。その位置の大きさに入らないが`JS_VM_SEG_MAX`以下のフレームは専用セグメントにせず、入る大きさまで位置を進める。

セグメントは動かさない（`JSVarRef.pvalue`・`argv`・`sp`がフレームの中を指すため）。ヘッダは実機20B（`standard`ビットを`JSVMSeg`に足した分、16→20B）。

### 7.3 キャッシュの持ち方（D43）

線形成長を入れると、キャッシュ1本では「戻って空いた大きさ」と「再度潜るときに要る大きさ」が位置ごとに食い違い、確保・解放の嵐になる（[vm-L2-results.md](vm-L2-results.md) §5.2）。

**決定: キャッシュはターンの間は本数上限なく持ち、ターンの終わりに全部返す。** `JS_VM_SEG_CACHE_MAX=UINT32_MAX`で戻りセグメントをすべてキャッシュし、ジョブキューが空になった時点（論理的なドレインの終わり、例外終了時も含む）で`JS_VMStackTrim()`が全部ヒープに返す。呼ぶのは`guest.c`の`drain_jobs()`と`vmrun.c`の`run_turn()`。予算で切られたYIELDEDのドレインは同じターンなので返さない。**持つ量はそのターンの中で一度生きていた量を超えない**（キャッシュは鎖から外れた本だけ）。却下した案: 本数固定上限（深く潜ったアプリが止まっている間も抱え続ける）、バイト数固定上限4,096B（往復の一部が嵐として残る）。`vmrun --vm-seg-size`は従来どおりキャッシュ1本・固定サイズ（L2aの掃引の意味を保つため）。

D42+D43の実測結果は[vm-L2-results.md](vm-L2-results.md) §5にまとめてある。

---

## 8. セグメント予算とスタック超過判定（D10〜D12）

L2b（C再帰の解消）の後、`js_check_stack_overflow`が見ているCスタック残量は深さに応じて減らなくなる。**無限再帰を止めるものが無くなる。**

### D10: 無限再帰を止めるのは、フレーム数ではなくセグメントのバイト予算

**決定: VMセグメントスタックの総バイト数で切る。フレーム数では切らない。** フレーム1個の大きさは`sizeof(JSStackFrame)+alloca_size`で関数のローカル数とオペランドスタック深さにより桁で変わるため、フレーム数はメモリを束縛しない。この機体の制約は深さではなくメモリ（DRAM約334KiB、ゲスト上限160KiB）。

**エラー種別を2つに分ける**（既存の期待値が両方を固定している）: 予算を使い切った場合は`RangeError: Maximum call stack size exceeded`、ヒープがセグメントを渡せなかった場合は`InternalError: out of memory`。**予算はヒープが尽きるより先に当たらなければならない**（そうでないと2つが区別できない）。

**knobは既存の`JS_SetMaxStackSize`/`rt->stack_size`を読み替える**（単位・APIは変えず、対象がCスタックからVMセグメントに変わる）。**ガードはフラット化(L2b)より前に入れる**（後回しにすると最初のフラット化コミットで`deep_recursion.txt`の期待値が`InternalError`に変わり、仕様の「新たな失敗を期待値の書き換えだけで処理しない」と衝突するため）。

**セグメントスタックはバンプアロケータで、使用量（`top-base`の和）はO(1)かつ既に正確に持っている**ので、ビットマップのpopcountのような別の数え方は採らない（TLSFのビットマップはサイズクラスごとの1ビットで使用バイト数ではなく、estallocのような「サイズ別リストとビットマップ」方式のセグメント*内部*の詰め方を採るなら、その層では自然な数え方になりうるが、予算とは別の層）。

出荷値は host 7 MiB（`--profile host`）、実機`stack_limit=20*1024`（`app_session.c`）。予算の窓が実在すること・出荷値がその窓の中にあることの実測は[vm-L2-results.md](vm-L2-results.md) §2。

### D11: 呼び出し元へ戻った後の`argc`は、フレームに既にある場所へ真の値を置く

フラット化すると、呼び出し元のディスパッチループのCローカル`argc`/`argv`を復元する必要がある（`OP_rest`が本文中で読む）。`argv`は`sf->arg_buf`から取れるが、`sf->arg_count`は上流コードが宣言側の個数で上書きしてしまうため使えない。

**決定: 上書きをやめ、`sf->arg_count`に真の`argc`を残す。** `JSStackFrame.arg_count`はvendoredツリー全体で一度も読まれていなかったため、この変更は新しい記憶域を要らず、`JSStackFrame`は48バイトのまま。詰められた個数が要る箇所は`b->arg_count`を`cur_func`経由で引く。

**この決定は「フラットフレーム」についてだけ成り立つ。** 床（C から入ったフレーム、または generator/async の `JSAsyncFunctionState`）は自分の `arg_buf`/`arg_count` の意味が別で、床の持ち物（§10.3）として別途 C ローカルに退避する。

### D12: L2bがフレームごとに要るものは、セグメントのブロックに置く

呼び出し元の`sp`（子が走る間の退避先）は復元できない — 実行によって決まる量で、`prev_frame`からも`arg_buf`からも引けない。`JSStackFrame`の隙間はD2・D8で使い切っている。

**決定: セグメントのpushが確保するブロック（`sizeof(JSStackFrame)+alloca_size`）を広げ、そこに置く。`JSStackFrame`の定義は変えない。** §5.3-5の規則どおり — 隙間は「全フレームが常に必要とするもの」、それ以外は包む側へ。`sp`の退避が要るのはセグメントに積まれたフレームだけで、床の4つの呼び出し規約が持つ素のC自動変数は払わなくてよい。

**`cur_sp`を呼び出し元の`sp`の退避先に流用しない。** `async_func_mark`/`async_func_free`が`cur_sp != NULL`を「中断中」の証明に使っており、流用するとgeneratorの床が子を呼んでいる間にGCが回ったとき、実行中のフレームを中断中と誤認して子のオペランドスタックの途中までをmarkする（§6のUAF側）。D4が決めた専用ビット（offset 37-39）への差し替えはL2b/L2cが負う。

---

## 9. フラット呼び出し — L2b

### 9.1 対象と押し方

対象opcodeは`OP_call`/`call0`〜`3`/`call_method`/`tail_call`/`tail_call_method`で、呼び先が`JS_CLASS_BYTECODE_FUNCTION`かつ`func_kind==JS_FUNC_NORMAL`のとき（`js_vm_flat_callable`）。それ以外はすべて従来のC呼び出しのまま（§9.3）。

呼び出し: 中断ポーリング → D10の予算判定 → `[JSVMLink][JSStackFrame][slots][var_refs]`をpush → 呼び出し元の`sp`をlinkに、復帰の形を`ret_shape`に、`ctx`を`caller_ctx`に書く → ループのCローカルを呼び先のものに差し替えて通常呼び出しプロローグの末尾（B地点、§4.4）へ。**押せなかった場合（予算・ヒープ）は何も積まずに呼び出し元へthrow**（上流の例外分岐と同じ場所に落ちる）。

復帰: `done:`（または捕捉されなかった`exception:`）の後、フラットフレームなら`link`を読んでからブロックをpopし、`prev_frame`から呼び出し元のローカルを組み直して、例外なら`goto exception`、tailなら`goto done`、それ以外は上流と同じスロット整理をして`goto restart`。

### 9.2 床の判定（`func_kind`ではなくセグメントの生存範囲）

床（1回のC活性で最初に積んだフレーム）を`func_kind`で判定できない — モジュール本体は`JS_FUNC_ASYNC`で組まれるが通常呼び出し経由で呼ばれる（`js_inner_module_linking`がhoisting済み宣言の初期化のために通常呼び出しする）。フラットで積むフレームは`js_vm_flat_callable`が`JS_FUNC_NORMAL`を要求するので**フラットフレームが`done_generator:`に入ることは構造的に無く**、非NORMALがセグメントに積まれるのはこのモジュール本体（床）だけ。**床の判定は`l2_flags`の1ビット（`JS_SF_SEG`/`JS_SF_FLAT`）で行い、`done`/`done_generator`の鍵は上流のまま`b->func_kind`。**

**ネイティブ再入は新しいC活性=新しい床。** 組み込みがJSに戻るたびに`JS_CallInternal`の新しい活性が始まり、その最初のフレームが床になる。深さを数えるruntimeフィールドはL2bでは足さない — 床の入口の`js_check_stack_overflow(rt,0)`が「ネイティブ再入の深さ」をCスタック残量として直接測っている。

### 9.3 床の持ち物（4つ）

C活性ごとに4スロット（JSの深さには比例しない）: 入口引数の`argc`/`argv`/`this`/`new.target`（`floor_argc`/`floor_argv`/`floor_this`/`floor_new_target`）。フラットフレームには要らない（`argc`はD11どおり`arg_count`、`argv`は自分のlinkの`caller_sp-arg_count`、`this`は`ret_shape`がmethodならfuncスロットの下、`new.target`は常にundefined）。

**`argc`が4つ目として必要な理由**: generator/asyncの床のフレームは`JSAsyncFunctionState.frame`で、その`arg_count`は`async_func_init`が書く`arg_buf_len=max(宣言数,渡した数)`であり、再開時にホストが渡す実際の`argc`とは違う。デフォルト引数の初期化子でフラット呼び出しをした後に`OP_rest`が`argc`を読むケースで、これを退避しないとフラット経路だけ結果がずれる（`corpus/l2b_floor_argc.js`が固定）。**D11の「`argv`は`sf->arg_buf`」「`argc`は`sf->arg_count`」は、フラットフレームについてだけ成り立つ。**

### 9.4 費用と既定

フレームごとにlink 4B（ホスト8B）。`JSStackFrame`は48Bのまま — `caller_ctx`/`l2_flags`/`ret_shape`は隙間に収まる（実機コンパイラの`_Static_assert`で確認）。C活性ごとに3スロット（実装は4スロット、§9.3の訂正参照）。既定onにした理由と実測は[vm-L2-results.md](vm-L2-results.md) §3。

### 9.5 対象外（囲った経路・従来の同期呼び出しのまま）

- **`OP_call_constructor`。** legacy constructorが`js_create_from_ctor`で作った`obj`を呼び出しの外で保持する構造で、その置き場をL2bでは作らない。派生クラスも同じ入口なので同様。
- `OP_apply`/`OP_apply_eval`/`OP_eval`（間接・直接とも）/`OP_init_ctor`（`super()`の暗黙形）。
- 呼び先が非バイトコード（native/bound/Proxy/Promiseのresolve関数）と、generator/async/async generator（別クラス、通常呼び出しは§10で扱う）、モジュール本体（床として扱う、§9.2）。
- ネイティブからJSへの再入すべて（getter/setter/Proxy trap/`sort`の比較関数等）。台帳01のとおり列挙可能ではないため「新しいC活性=床」で機械的に扱う（§9.2）。

これらは仕様の「未対応経路は従来の同期呼び出しとして囲い、中断禁止を明示」に当たる。中断禁止は今のところ暗黙（L2c本体まで中断そのものが無い）で、L2c本体は床ごとに明示する。

---

## 10. async関数のフラット化（D31〜D38、D40、D41）

L2bの初版はJSから呼ばれたasync関数の最初の同期区間（awaitに達するまで）を止められなかった。依頼者の決定（D31）でL2bの対象を広げ、これを埋めた。**この節はL2c（中断・再開）とは独立で、先に実装・検証している。**

### 10.1 対象と判定（D32）

**決定: `js_vm_flat_callable`を「`JS_CLASS_BYTECODE_FUNCTION`かつNORMAL」または「`JS_CLASS_ASYNC_FUNCTION`」に広げ、`flat_call:`の先頭でクラスにより`flat_async_call:`へ分岐する。** 対象opcodeはL2bと同じ。モジュール本体（BYTECODE_FUNCTIONクラスだがNORMAL要求で弾かれる、§9.2）は引き続き弾かれる。`OP_apply`/`.call`/`Reflect.apply`/`OP_call_constructor`/bound/Proxy経由のasync呼び出しは§9.5の囲った経路のまま（C再帰、保持）。

### 10.2 押し方と戻り方（D33、D34）

**`flat_async_call:`（呼び出し元の状態で、確保はここで起きる）**: 中断ポーリング → `JSAsyncFunctionData`を`js_mallocz`（失敗なら呼び出し元へ`InternalError`、D38） → `JS_NewPromiseCapability` → `async_func_init` → 呼び出し元のfuncスロットへpromiseを書き込み → 呼び出し元の`sp`を`JSAsyncFunctionData.flat_caller_sp`に保存（D12のlinkに相当） → `l2_flags = JS_SF_FLAT`（`JS_SF_SEG`は立てない、ヒープ上のフレームのため） → ローカルを切り替えてB地点（§4.4）へ。

**フラットasyncフレーム**は`JSAsyncFunctionData.func_state.frame`に居り、最初のawait/return/例外で活性を抜けると以後は普通のasync床になる。**セグメントに無いのでD10の予算には乗らない（D38）。**

戻り方: 最初のawait/return/例外で`done_generator:`を通り、`async_flat_return:`から2段のsettle（core本体は上流の`js_async_function_resume`後半を切り出したもので生成者参照とpromiseに触らない。呼び出し元向けの包みがそこに生成者参照解放とpromise確定を足す）で呼び出し元へpromiseを渡す。

### 10.3 D38: 予算はセグメントのバイト数だけ — フラットasyncフレームは数えない

**決定: フラット化したasync関数のフレーム（ヒープ上）は、再帰の予算（D10）に数えない。** 検討した代案（ヒープ上のasyncフレームのバイト数も同じ予算に乗せる）は採らない。

**この限界を受け入れる:** 深いasync再帰はヒープが尽きた時点で`InternalError: out of memory`になり、asyncについてだけ「予算がヒープより先に当たる」（D10）は成り立たない。ヒープ満杯時の上流`build_backtrace`のuse-after-free（[backlog.md](backlog.md) の独立の不具合 項目6）にも到達しやすくなる。深いasync再帰の検査は`RangeError`ではなく、ヒープで止まることを固定する側で書く。

### 10.4 D40: 深いasync再帰の壊れ方を期待値に固定する

flatで深さ77（1段約1.9KB、ホスト`--profile device`）でヒープが尽き、エラーオブジェクトを作れず理由`null`、巻き戻し中に23段のawait登録が失敗して未処理の拒否が残りexit 2（D42/D43適用後は深さ82・未処理26、[vm-L2-results.md](vm-L2-results.md) §4.1）。仕様が予告していた「`InternalError`が`.catch`に届く」形とは違う。配置に敏感で深さや件数は動きうるので、比較は種類（`null`）と終了コードだけで縛る。**確保を伴わない事後判定（D39、失敗地点での回数・最初の失敗の要求サイズ・使用量の記録）と組み合わせて枯渇由来と判別する。予備ブロック（最初の失敗で解放するカナリア）は入れない**（実機の空きを常に削るため）。

### 10.5 D41: 上流の二重解放を根本修正

Stage Aの攻撃で見つかった二重解放（`JS_NewPromiseCapability`の2つ目のresolve関数の確保失敗で、解放済みの`resolving_funcs[0]`をもう一度解放）は、フラット経路だけの個別回避ではなく上流の`js_async_function_call`も含めて根本でパッチ修正する。上流への報告は送らず、報告書を`reports/upstream/`（`docs/`とは別）に保存する。

### 10.6 対象外（囲った経路のまま、D37）

generator/async generatorの呼び出しは分けて扱い、どちらも囲った経路のまま（C再帰）。実測結果と関所は[vm-L2-results.md](vm-L2-results.md) §4.1。

---

## 11. 中断・再開の機構 — L2c設計

**この節は設計であり、実装はまだ無い**（実装状況は文書冒頭の表、backlog.md #7）。「実測」と明記していない数字は計算値または推定。前提（既決、変えない）: D1（YIELDは`done_generator`形の返り値で確保しない）、D2（`JSStackFrame`48B）、D4（所有者と`l2_flags`の専用ビット）、D8（Aの7地点とB=push後）、D9（JSValue表現を触らない）、D10（予算は文字どおりセグメントのバイト数、D38によりフラットasyncフレームは対象外）、D11・D12、床の持ち物4つ（§9.3）、D31〜D41。

スイッチ: L2b拡張は既存の`CONFIG_POCKET_VM_FLATCALLS`の中。L2c本体は新設の`CONFIG_POCKET_VM_YIELD`（`depends on POCKET_VM_FLATCALLS`）で、offならA地点のslow pathは今の`JS_ThrowInterrupted`のまま。

### 11.1 用語

| 語 | 意味 |
| --- | --- |
| 活性 | `JS_CallInternal`のC呼び出し1回。床と、その上にフラット呼び出しで積まれたフレームの列 |
| 床 | 活性の最初のフレーム。SEG床（C入口）かgenerator系床（`JSAsyncFunctionState.frame`） |
| フラットSEGフレーム | `flat_call:`がセグメントに積んだ通常関数のフレーム。`l2_flags=SEG|FLAT` |
| フラットasyncフレーム | §10のヒープ上フレーム。`l2_flags=FLAT`（SEG無し）。セグメントに無いのでD10の予算に乗らない |
| 鎖 | 中断時に生きているフレーム列。topから`prev_frame`で床まで。SEGフレームとフラットasyncフレームが混ざる |
| 止まってよい床 | `l2_flags & JS_SF_MAY_YIELD`（D17r、§11.2）。フラット子（SEG/asyncとも）にはpush時に写す |
| 保持 | 要求ビットは立っているが受理できない地点で、ビットを消さずに続けること |
| 再開の持ち主 | 鎖を再開する側。SEG床はホスト、async系床は`JSAsyncFunctionData`/`JSAsyncGeneratorData`、保留ジョブ（D36）はruntimeが握る`JSJobEntry`と tail関数 |
| 保留ジョブ | 通常関数のhandlerが中断したとき、`JS_ExecutePendingJob`が解放せず`rt->vm_susp.job`に預けた`JSJobEntry`。論理的にはキューの先頭のまま |
| 囲い | 入口トークンを書く側が、呼び先から戻ったときに必ず0に戻す構造（D17r-3）。トークンは囲いの外に漏れない |

**依頼者の追加決定（D13〜D16）:**

| # | 決定 | 帰結 |
| --- | --- | --- |
| D13 | 止まってよい床は最小集合より広げる。async/generatorの本体も止める | generator/async床の上の中断をGCがどう辿るか（D21r）と、床の持ち物をyield時の確保なしに所有する問題を解く必要が生じる |
| D14 | 実機のYIELD要求は専用の要求ビット。他タスク／ISRがatomicに立て、`interrupt_counter`を0に寄せて次のポーリングでslow pathへ入れる。TERMINATEは従来の割り込みハンドラのまま | ホットパスの読み出しは増えず、要求は落ちない |
| D15 | 複数ターンにまたがる同期`frame()`の暴走は、タスクの累積時間で上限を切る | ターンごとの壁時計締切とは別に、`frame()`1回が中断をまたいで使った合計時間に上限を設けTERMINATE |
| D16 | 「キューにジョブが残っている」と「実行途中で中断している」を別のビットにする | 公平モードは前者のときだけpumpする |

### 11.2 D17r: 止まってよい床の決め方

**決定: `JS_SF_MAY_YIELD`を床のpush時に1回だけ計算し、フラット子に写す。床がMAY_YIELDになる条件は「入口トークン`rt->vm_entry_ok`が立っている」かつ「`rt->current_stack_frame==NULL`」の2つ。**

トークンを書けるのは再開の持ち主の**囲い**だけ: ホストの`JS_VMCall`/`JS_VMEval`、await復帰（`js_async_function_resolve_call`）、async generator再開、D36の`JS_VMCallJob`、そして`JS_CallInternal`のclass分岐がASYNC_FUNCTIONのとき（フラット化されない囲った経路のasync呼び出しで、内側の`JS_CallInternal`が床になる）。**書く位置はプロローグ・ポーリングの前**（後だとTERMINATEの早期returnでトークンが残る）。**囲いの不変条件: トークンを書いた側は、呼び先がどう戻っても戻った直後に0に戻す**（Cスタック検査の失敗・TERMINATE・確保失敗を含む）。

**モジュール本体は対象外にする。** `js_async_function_resume`と`js_async_function_call`自体はトークンを書かない（無条件に書くと、TLA無しのモジュールがA地点でyieldしたときホストの`JS_EvalFunction`経路が再開できず`TypeError: promise is pending`になる）。**モジュール本体の最初の同期区間は止まらない。TLA復帰後は普通のASYNC床として止まる**（他のasync関数と同じ扱い）。

**D31で止まるようになるもの**: (a) JSから呼ばれたasync関数の最初の同期区間、(b) 通常関数のPromise handler（D36がトークンを書く）。止まらないままのものは§11.9。

### 11.3 D18r: 再開を受け取るC呼び出し元と再開の持ち主

**決定: YIELDは`vm_yield:`で`done_generator`形に保存して返る。返り値は`JS_EXCEPTION`、`rt->current_exception`は未初期化のまま、`rt->vm_susp.top != NULL`が「中断中」の唯一の真偽。呼び出し元は`JS_IsException`の前に`js_vm_suspended(rt)`を見る。再開は`vm_resume:`の1ラベル。**

再開の持ち主は4種:

| 床の種類 | トークンを書く場所 | 中断が通って戻るC関数 | 再開の持ち主 |
| --- | --- | --- | --- |
| SEG床（ホスト） | `JS_VMCall`/`JS_VMEval`（実機の起動時evalは対象外、§11.7） | `JS_CallFree`→`__JS_EvalInternal`→ホスト | ホストの`JS_VMResume(ctx)` |
| SEG床（保留ジョブ、D36） | `JS_VMCallJob`（各ジョブ関数） | `JS_ExecutePendingJob`（ジョブは完了せず`rt->vm_susp.job`に預けて`2`を返す） | `JS_VMResume`が鎖を走らせ完了後にtail（§11.6）を呼ぶ |
| ASYNC床 | class分岐の書き直し、`js_async_function_resolve_call`（await復帰） | `async_func_resume`→…→`promise_reaction_job`→`JS_ExecutePendingJob`（ジョブは正常完了） | `JS_VMResume`が`js_async_function_resume(ctx,s)`を再度呼ぶ |
| ASYNC_GENERATOR床 | `js_async_generator_resume_next`の前後 | 同上 | `js_async_generator_resume_next(ctx,s)` |

`vm_yield:`は確保ゼロでO(鎖の深さ)。`sf->cur_pc`/`sf->cur_sp`を保存し、`l2_flags |= JS_SF_SUSPENDED`。**鎖の途中にヒープフレーム（generator系床、フラットasyncフレーム）が混ざる**ため、SEGでないフレームについてはその子の`caller_sp`を親の`cur_sp`へ書き込む（`async_func_mark`/`async_func_free`が「中断中」の証拠として読む値をここで作る）。`floor`（鎖の最下端）まで辿ったら`rt->vm_susp`一式を書き、`rt->current_stack_frame = floor->prev_frame`（§11.4）にしてから要求ビットを消しJS_EXCEPTIONを返す。

`vm_resume:`は1つで、SEG床は再開フラグでの`goto`、async系床は`l2_flags & JS_SF_SUSPENDED`で`vm_resume:`へ分岐する。topの組み立ては床の種類（フラットSEG子／ヒープフレーム／SEG床本体）で`argc`/`argv`/`this_obj`の取り出し方が変わる（§9.3の床の持ち物、フラットasyncフレームは`JSAsyncFunctionState`の`argc`/`this_val`を使う）。

**`JS_VMResume`の返り値契約:** 起点SEG（ホスト）は元の呼び出しと同じ（完了値・例外・再中断ならホストが所有し解放）。起点が保留ジョブなら`JS_VMResume`自身がtailの返り値を解放し、`UNDEFINED`/`EXCEPTION`だけ返す（ホストは解放しない）。起点ASYNC系は`UNDEFINED`。起点を知る口として`JS_VMSuspendedOrigin(rt)`を`quickjs-vm.h`に足す。

### 11.4 D19r: 中断中の`rt->current_stack_frame`と鎖の保護

**決定: yield後の`current_stack_frame`は`floor->prev_frame`。中断中にJSを走らせる`JS_CallInternal`の入口（バイトコード床のpush、generator/asyncの再開入口）を`JS_ThrowInternalError(ctx, "VM suspended")`で拒む。** D36で保留ジョブができると、`JS_ExecutePendingJob`が先頭ジョブを`list_del`してからhandlerが拒まれるとジョブが失われるので、**`JS_ExecutePendingJob`本体の直前でも同様に拒む（popせずに拒む）**。実機では`vm_sched_drain`を通らない呼び出し口（stop hookのdrain、§11.8）にも同じ後ろ盾が要る。

### 11.5 D36: 通常関数のPromise handlerを止める — ジョブの保留とtail

**決定: ジョブは再投入ではなく保留にする。** `JS_ExecutePendingJob`が既に`list_del`した状態のまま解放せず、runtimeが握る。

1. **口: `JS_VMCallJob(ctx, func, this, argc, argv, tail, aux)`。** トークンを書き、`JS_Call`、トークンを0に戻し（囲い）、戻ったときに「suspendedかつ`kind==SEG`」（yieldが床の種類で付けた`kind`）なら`kind=JOB_HELD`として`tail`と`aux`の所有をruntime側へ移し`JS_EXCEPTION`を返す。ジョブ関数はその直後に`kind==JOB_HELD`ならtailを走らせずに戻る。判別は`kind`の1条件のみ。
2. **各ジョブのtail**（handler呼び出し後の後始末を関数に切り出したもの）: `promise_reaction_job`はresolve/rejectの呼び分けとfree、`js_microtask_job`/`js_finrec_job`はhandlerの戻り値をそのまま返す、`js_promise_resolve_thenable_job`はpromise_hookとargsのfree。`js_dynamic_import_job`はhandlerではないのでトークンを書かない（保持）。await復帰のhandler（`JS_CLASS_ASYNC_FUNCTION_RESOLVE`）はclass分岐で消費され`kind`はASYNCのままなので、常に完了する（初版どおり）。
3. **`JS_ExecutePendingJob`の後**: suspendedかつ`kind==JOB_HELD`なら`argv`の解放とエントリの解放を通さず、`rt->vm_susp.job`に預けて`VM_JOB_HELD`(=2)を返す。
4. **`vm_sched_drain`**（`vm_sched.h`、firmwareとvmrun共有）: ループ先頭で中断中なら`VM_DRAIN_SUSPENDED`を返し、次のジョブに触らない。
5. **再開後のtail**: `JS_VMResume`は鎖が完了して結果を得たら、`kind==JOB_HELD`ならtailを呼び後始末をして`ok?UNDEFINED:EXCEPTION`を返す。ホストはこれを見て「その時点で1件完了」として扱う。
6. **FIFO（仕様§3-3）**: 保留中は他のジョブは実行されず（4と§11.4の後ろ盾）、ホストもJSを呼ばない（§11.8: pumpは`!suspended`のときだけ）。handler自身が積んだジョブ・tailが積む派生reactionは末尾に付く — 非中断実行と同じ順序。
7. **GC/破棄**: `susp.job->argv`はジョブが所有する参照でリストに無い間と同じくmarkされない（外部参照）。`susp.aux`は鎖のGC所有者（§11.6）がmarkする。破棄（§11.7）は鎖を畳んだ後に`argv`/`e`/`aux`を解放する。
8. **TERMINATE**: 鎖がuncatchableで畳まれると結果は`JS_EXCEPTION`になり、tail経由でホストは「ジョブが投げた」として扱う（今`interrupted`で死ぬジョブと同じ終わり方）。

費用: 中断しないときはトークンのstore2つと分岐1つがジョブ1件につき増える。`rt->vm_susp`に`job`(4B)・`tail`(4B)・`aux[2]`(16B)。

### 11.6 床の持ち物の所有（D20）とGC（D21r）

**D20: SEG床のspill**（`[argv[argc]][JSVMFloorSpillHdr][JSVMLink][JSStackFrame]…`）はyield時に`js_dup`するだけ。generator系床は`JSAsyncFunctionState`が既に所有、ASYNC床は参照カウント++、ASYNC_GENERATOR床はgeneratorオブジェクトをdup。保留ジョブの床はSEG床なので同じspillを使う。フラットasyncフレームは床になれる（活性を抜けた後）が、そのときは`JSAsyncFunctionData`が所有者でspillは要らない。

**D21r: 鎖のGCは所有者側`JS_MarkContext`に足す1行。** topから床までwalkし、`JS_SF_SEG`のフレームだけ`local_buf..sp`（topは`cur_sp`、それ以外は子の`caller_sp`）をmarkし、SEG床のspill5種をmarkする。**ヒープに居るフレーム（generator系床、フラットasyncフレーム）はwalkが触らない** — それぞれの持ち主（`JSAsyncFunctionData`/generator/async generator）が`cur_sp != NULL`のときだけmarkする既存の仕組みに乗る。§11.3の`vm_yield:`が鎖の全ヒープフレームの`cur_sp`を埋めるのはこのため。フラットSEG子の`arg_buf`はヒープ親のオペランドスロットをaliasするが、walkはSEGの`local_buf..sp`（aliasを含まない）だけを見るので二重にはならない。

### 11.7 TERMINATEと破棄（D25、D26r）

**D25: `JS_VMTerminate`は`vm_susp.throw=1`で`vm_resume:`から`goto exception`（uncatchable）。`JS_VMDiscard`は内側から畳む。** セッションが続く終了はTerminate、終わる破棄はDiscard。

**D26r: 鎖にヒープフレームと保留ジョブがある形での破棄。** Discard（`JS_FreeRuntime`直後、ジョブ解放ループの前）はtopから床へ歩き、SEGフレームは通常どおり畳む。**ヒープフレームは畳まない** — `l2_flags`からFLAT/SUSPENDED/MAY_YIELDを落とし`cur_sp`はyieldが置いた値のままにして持ち主に返す（フラットasyncフレームは`JSAsyncFunctionData`の生成者参照を解放。**予算の払い戻しは無い**、D38）。保留ジョブは`argv`/`e`/`aux`を解放。H14の計測口として`#info vm susp_bytes_max=`（セグメントのbytesだけ、D38）と別に`susp_async_frames=`（鎖の中のヒープフレーム数）を出す。

### 11.8 実機統合の設計（D14、D27、D28r、D24r）

**D27: 要求ビットと3値化。** `_Atomic uint8_t vm_yield_req`、`JS_VMRequestYield`（`interrupt_counter=0`）、slow pathの3値化、A地点7箇所の書き換え、保持（MAY_YIELDでない床のpopで`caller_ctx->interrupt_counter=1`）。**leaveターン（Back操作、0x2000）はtimerを張らず`JS_VMClearYield`で要求ビットを消す**（yieldの意味が無いターンのため）。N1（正規表現）はTERMINATE系統のまま。

**D28r: 累積時間の計測。** 起点JOB_HELDの`JS_VMResume`時間は起点JOB_ASYNCと同じく`drain_us`に足し、完了時に`drain_jobs`を1件足してから既存の`drain_jobs()`に委ねる（EMPTY判定・`report_rejections`を既存位置で通すため）。起点FRAMEは`frame_us`/`frame_runaway`。1つの保留ジョブが最大約30ターン居座りうる計算（`VM_TURN_BUDGET_US` 8ms × `VM_RUNAWAY_US` 250ms）。

**D24r: guestの3状態と3起点。** `suspended`/`jobs_pending`の2ビットと`origin`（FRAME/JOB_HELD/JOB_ASYNC）。`jobs_pending = JS_IsJobPending(rt) || origin==JOB_HELD`（保留ジョブはリストに無いが論理的には先頭）。FAIRの`run_pumps`は`jobs_pending && !suspended`。実機の起動時evalは`JS_Eval`のままにする（§11.9）。**Backターン・stop hookの扱い**: `pocket_app_reset`のstop hook呼び出し（`run_hook`とペンディングジョブのdrainループ）はleaveターンと同様に武装しない・中断中なら先に鎖を完了させる・後ろ盾（中断中なら`JS_VMTerminate`+`JS_VMResume`で畳んでから進む）の3点で保護する。

### 11.9 閉じられなかったもの（設計上の恒久的な制約）

- **同期generatorの本体は止められない**（D8を変えない限り。`.next()`の呼び手がCスタック上で同期的に待つため）。
- **async generator本体の最初の同期区間（`.next()`の下）は止まらず、`.next()`を通る再帰はC再帰のまま。** 止まるのはawait復帰のジョブからの再開だけ。
- **囲った経路からのasync呼び出し**（`OP_apply`/`.call`/`Reflect.apply`/bound/Proxy/`OP_call_constructor`）はC再帰のままで止まらない。ただしclass分岐の書き直しで、その内側のASYNC床自体はMAY_YIELDになる。
- **`js_async_flat_settle`の中のJS再入**（resolve関数の`then` getter、`js_promise_resolve`の`constructor`読み）は新しい床で、そこでの中断は保持。
- **実機の起動時evalは止めない。** `app_start_test`にはターン構造が無く、戻る先が無いまま同期ループで再開してもyieldの意味が無い。2秒の締切がTERMINATEで守る。
- **モジュール本体の同期区間（TLAまで）は止まらない**（§11.2）。TLA復帰後は普通のASYNC床。**TERMINATEでpromiseがpendingのまま残る**のは既知（§3.3）で変わらない。
- **D38の限界**（§10.3）: async関数の同期再帰の予算はヒープ枯渇で答える。対処は上流バグの修正（VMとは独立）で本設計の範囲外。

---

## 12. 末尾呼び出し最適化（TCO）

設計下書きのみ。[vm-tco-design.md](vm-tco-design.md)を参照。L2c本体の実装後に入れる前提（フレーム鎖・GC分担・B地点の位置がL2c本体で確定してから）。

---

## 13. 決定表（D1〜D43）

| # | 決定 | 状態 | 節 |
| --- | --- | --- | --- |
| D1 | 中断を YIELD/TERMINATE/WAIT の3つに分ける。YIELDは返り値であって投げられる値ではなく、確保をしてはならない | 決定済み | §3.3 |
| D2 | フレームを太らせず `JSAsyncFunctionState` に倣って包む。`JSStackFrame` は48Bのまま。`caller_ctx` は隙間(4-7)へ | 決定済み | §5.3 |
| D3 | N1〜N4は中断禁止区間として囲い、それ以上の機構を作らない | 決定済み | §1.3 |
| D4 | 中断フレームはGCが辿れる所有者を持つ。実行中／中断中の目印は`cur_sp`の再利用に頼らず専用ビットで持つ | 決定済み | §6 |
| D5 | セグメントアロケータは専用実装（台帳06比較が入力） | 決定済み | §7 |
| D6 | JS用セグメントとtaffyの共有／分離 | 未決（L2aのセグメントが実在してから測って決める） | §7 |
| D7 | 個々の確保に8バイト境界を強制しない。セグメント先頭は16バイト境界 | 決定済み | §2.2 |
| D8 | セーフポイントは命令が完全に退役した地点にしか置かない。呼び出し地点は復帰の形を明示的に持つ | 決定済み | §4 |
| D9 | JSValueの表現は触らない | 決定済み（L2範囲外） | §2.3 |
| D10 | 無限再帰を止めるのはセグメントの総バイト予算。フレーム数では切らない | 決定済み（着地: §9） | §8 |
| D11 | 呼び出し元復帰後の`argc`は`sf->arg_count`に真の値を残す（フラットフレームのみ成立） | 決定済み（訂正: §9.3） | §8 |
| D12 | L2bがフレームごとに要るものはセグメントのブロックに置く。`JSStackFrame`は変えない | 決定済み | §8 |
| D13 | 止まってよい床は最小集合より広げる。async/generatorの本体も止める | 決定済み | §11.1 |
| D14 | 実機のYIELD要求は専用の要求ビット | 決定済み（着地: §11.8） | §11.1, §11.8 |
| D15 | 複数ターンにまたがる同期`frame()`の暴走はタスクの累積時間で上限を切る | 決定済み（値は実機で計測して決める、backlog.md） | §11.1 |
| D16 | 「ジョブが残っている」と「中断している」を別ビットにする | 決定済み（§11.5-6, §11.8のD24rで実装形が確定） | §11.1 |
| D17r | 止まってよい床は`l2_flags`の1ビットで、活性の入口で決まる。トークンは囲いを持つものだけが書く | 決定済み | §11.2 |
| D18r | YIELDを受け取るC呼び出し元の集合（4種）と、返し方・再開の入口 | 決定済み | §11.3 |
| D19r | 中断中の`rt->current_stack_frame`と、鎖の上に何も積ませないこと | 決定済み | §11.4 |
| D20 | 床の持ち物4つの所有 | 決定済み | §11.6 |
| D21r | 鎖のGCと表明。非SEGフレームは持ち主がmark | 決定済み | §11.6 |
| D22r | 関所: vmrunの4つの受け口（起動時eval・本体eval・`frame()`・ジョブ）に同じ再開ループを入れ、「安全地点を通ったファイルは停止回数=再開回数」を正の証拠として要求する | 決定済み（着地: [vm-L2-results.md](vm-L2-results.md) §4.2） | §13（本行） |
| D23 | G5の「停止機会」の再定義（MAY_YIELDの活性で通ったA/B地点、ENTER/LEAVE） | 決定済み | §1.2 |
| D24r | guestの3層に第3状態（origin）とD16の2ビット。Backターン・stop hookを含める | 決定済み | §11.8 |
| D25 | TERMINATEは`vm_susp.throw=1`で`goto exception`。Discardは内側から畳む | 決定済み | §11.7 |
| D26r | 鎖にヒープフレーム・保留ジョブがある形での破棄 | 決定済み | §11.7 |
| D27 | 要求ビットとslow pathの3値化。leaveターンは武装せず`JS_VMClearYield` | 決定済み | §11.8 |
| D28r | JOB_HELD起点の累積時間・件数の数え方 | 決定済み | §11.8 |
| D29 | B地点は通常呼び出しプロローグの末尾。フラットasyncも同じラベルへ合流 | 決定済み | §4.4, §9.1 |
| D31 | L2bの範囲を広げ、JSから呼んだasync関数の同期区間とPromise handlerも止める対象にする | 決定済み | §10, §11.2 |
| D32 | `js_vm_flat_callable`をASYNC_FUNCTIONにも広げる対象と判定 | 決定済み | §10.1 |
| D33 | `flat_async_call:`の押し方 | 決定済み | §10.2 |
| D34 | `async_flat_return:`の戻り方（core+包みの2段settle） | 決定済み | §10.2 |
| D35 | （廃止。D38に置き換え） | 撤回 | §10.3 |
| D36 | 通常関数のPromise handlerを止める。ジョブは保留、tailで後始末 | 決定済み | §11.5 |
| D37 | generator/async generatorの呼び出しは囲った経路のまま | 決定済み | §10.6 |
| D38 | 予算はセグメントのバイト数だけ。フラットasyncフレームは数えない | 決定済み | §10.3 |
| D39 | ヒープ枯渇は確保を伴わない事後判定の記録だけを入れる。予備ブロックは入れない | 決定済み | §10.4 |
| D40 | 深いasync再帰の壊れ方を期待値に固定する | 決定済み | §10.4 |
| D41 | 上流の二重解放を根本修正する。報告は`reports/upstream/`に保存、上流へは未送信 | 決定済み | §10.5 |
| D42 | フレームセグメントを線形に伸ばす（`min(FIRST×n, MAX)`） | 決定済み | §7.2 |
| D43 | キャッシュはターンの間は全部持ち、ターンの終わりに返す | 決定済み | §7.3 |

---

## 関連文書

- 実測値・関所の通過結果: [vm-L2-results.md](vm-L2-results.md)
- 末尾呼び出し最適化（未着手の下書き）: [vm-tco-design.md](vm-tco-design.md)
- 未着手・未確認の作業項目: [backlog.md](backlog.md)
- quickjs.c の事実の根拠: [vm-ledger/](vm-ledger/) 01〜07
- 実機の基準値: [vm-L0-report.md](vm-L0-report.md)
- L1（ジョブ境界まで）: [vm-L1-design.md](vm-L1-design.md) / [vm-L1-report.md](vm-L1-report.md)
