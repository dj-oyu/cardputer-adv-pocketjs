# 末尾呼び出しでフレームを使い回す（TCO）— 実験実装

対象: [vm-L2-design.md](vm-L2-design.md) の L2b（フラット呼び出し）の後に独立した段として入れる案。
**既定nの実験実装を開始（2026-09-16）。** §1〜§3には当初の方式案を残し、現行実装との差は§6に記す。既定動作は変更しない。未完了の検証・拡張は [backlog.md](backlog.md) にもまとめてある。

`components/quickjs-ng/quickjs-ng/quickjs.c` 内で `OP_tail_call` を扱う箇所のコメントがこの文書を "sec.14 draft" として参照している。

## 1. 前提となる事実（コードで確認済み）

- **コンパイラは末尾呼び出しを既に見つけている。** `OP_call` / `OP_call_method` の直後がそのまま `OP_return` なら `OP_tail_call` / `OP_tail_call_method` に置き換える（quickjs.c、`resolve_labels` 内。命令は `quickjs-opcode.h`）。strict / sloppy の区別はしない。`try`/`finally` の中の `return` は直後に後始末の命令（`nip_catch`/`gosub`）が入るので置き換わらない。async 関数の `return` は `OP_return_async` なので対象外。
- **実行時はフレームを使い回していない。** 上流は「普通に呼んで、戻ったら `goto done`」。L2b（[vm-L2-design.md](vm-L2-design.md) §10）もそれを `JS_RET_TAIL` で再現しているだけで、末尾呼び出しでも毎回フレーム1つ分をセグメントに積む。末尾で自己再帰する関数は D10（[vm-L2-design.md](vm-L2-design.md) §9）の予算（実機 20 KiB）で `RangeError` になる。
- **Test262:** `tail-call-optimization` の10本のうち9本が `RangeError` で失敗する（`language/expressions/call/tco-*` 6本、`language/statements/try/tco-*` 3本）。`built-ins/Proxy/revocable/tco-fn-realm.js` だけ通る。現在の194件の失敗に含まれる。

## 2. 方式

`OP_tail_call` の呼び出し先が `js_vm_flat_callable`（通常のバイトコード関数）で、**呼び出し元がフラットなフレーム**（L2bの `JS_SF_SEG|JS_SF_FLAT`）のとき、呼び出し元を先に畳んで同じ番地に呼び出し先を積む。

1. 関数・`this`・引数を退避する（引数は呼び出し元のオペランドスタック、つまり**呼び出し元のブロックの中**にある）。
2. 呼び出し元の後始末: `close_var_refs`、退避した値以外のローカルとオペランドの解放（通常の `done:` と同じ）。
3. 呼び出し元のリンクから `caller_sp`、フレームから `ret_shape`・`prev_frame`・`l2_flags` を読み、ブロックを pop。
4. 呼び出し先のブロックを push（セグメントは LIFO なので同じ番地から積み直す）。3 で読んだ値を引き継ぐ — 呼び出し先は**呼び出し元の呼び出し元へ**直接戻る。
5. 退避した引数を呼び出し先の引数領域へ書き戻し、通常呼び出しのプロローグ末尾（L2bの `body_entry:`）へ合流。

D10 の予算は使用量が一度減ってから増えるだけなので、自己再帰の深さに上限が効かなくなる（それが目的）。無限の末尾再帰は予算ではなく、L2c の割り込み機構（D14/D15）で止まる。

## 3. 使い回さない場合（今の呼び出しに落とす）

| 条件 | 理由 |
| --- | --- |
| 呼び出し先がネイティブ / bound / Proxy / generator / async | フラットに呼べない（`js_vm_flat_callable`）。async は L2b のヒープフレーム経路 |
| 呼び出し元が床（C から入ったフレーム） | 床は引数・`this`・`new.target`・`argc` を C 側に退避している（[vm-L2-design.md](vm-L2-design.md) §10）。床から1段フラットに入った後は使い回せる |
| 引数が多い（閾値は未定） | 退避先を C のローカル配列に固定長で取るため |
| 呼び出し先のブロックが今のセグメントの残りに収まらない | 別のセグメントへまたぐと、退避と積み直しの番地が同じにならない。同じ関数の自己再帰は大きさが毎回同じなので、2回目以降は必ず収まる |

## 4. 決めること・閉じていないこと

- **L2c との順序。** L2c の本体は「鎖の上のフレーム」を前提に、止まってよい床の印、フラット async を含む鎖の `caller_sp` の埋め方、GC の分担、B地点の位置を決めている（[vm-L2-design.md](vm-L2-design.md) §12）。フレームが入れ替わるとこれらに影響するので、**L2c の本体実装の後**に入れる前提。先に入れるなら L2c 側の設計の見直しが要る。
- **観測できる変化。** `Error.stack` から畳んだ呼び出し元が消える。移植の既定挙動維持のため、実装時はstrictな呼び出し元だけを対象とし、sloppyには適用しない（2026-09-16方針）。呼び出し先のstrictnessではなく、現在の`JSFunctionBytecode`のstrictビットで判定する。コンパイラの末尾命令検出はsloppyにも行われるため、命令だけでは判定できない。実験スイッチは既定nを維持し、既定有効化は別の互換性判断とする。コーパス全体のbacktrace期待値監査は残る。
- **既存の期待値への影響。** `try_finally.js`のsloppyな`s1() { return s2(); }`はスタック文字列を丸ごと固定しており、sloppyへのTCO適用は既存契約を壊す。`budget_probe.sh`が使う`seg_oom_boundary.js`の`pileOnce`もsloppyな末尾再帰で、480B予算の陰性対照が現行のフレーム積み上げに依存する。したがって両方とも無変更で維持する。`deep_recursion{,_device}.js`の無限`dive()`は`return dive()`ではなく、`dive2()`は戻り値に加算があるため、明示的な末尾呼び出しではない。これは該当ファイルのソース監査であり、最適化実装後の全コーパス実行を代替しない。
- **Test262 の見込み（未測定）。** 通常・メソッド呼び出しの2本に加え、`tco-non-eval-{function-dynamic,function,global,with}.js`の4本も再帰する`f`自体は`"use strict"`を持つ。`flags: [noStrict]`はテスト全体の実行モードであり、sloppyな末尾呼び出しを要求するテストではない（固定Test262 revisionのソース監査、2026-09-16）。従来の「sloppyの4本」という分類は誤り。`try`/`catch`/`finally`の3本は、コンパイラが該当位置で末尾命令を出すかの確認を引き続き要する。
- **検査の形。** 末尾再帰の深さを2通りに変えても VM スタックの使用バイトが一定であること（G1 と同じ「深さに比例しない」判定）を関所にする。
- **切り替え。** `CONFIG_POCKET_VM_TCO`（既定 n）で入れ、関所が通ってから既定を決める。

## 5. 足場（コードに入れたもの、挙動不変）

- `CASE(OP_tail_call)` を `CASE(OP_call)` の**上**に、`CASE(OP_tail_call_method)` を `CASE(OP_call_method)` の上に置き直した。`DIRECT_DISPATCH`（gcc の計算型 goto、ファームもホストも）では `CASE(op)` がそのまま `case_OP_xxx:` ラベルなので、**末尾呼び出し専用のジャンプ位置がすでにある**。いまは空で、下の通常呼び出しの本体へフォールスルーする（`opcode` は `OP_tail_call` のまま読めるので `JS_RET_TAIL` と `goto done` は従来どおり）。
- §2 の分岐はこの位置に足す。**通常の `OP_call` の経路には比較が1つも増えない。** 足すコードは `pc` を進めない（引数の数は `get_u16(pc)` で読む）ことで、フォールスルー先の解読と両立させる。
- 新しい命令の enum は足していない。命令は既にあり、番号をずらすとバイトコードの読み込みと食い違う。

## 6. 第1実装（2026-09-16、実験スイッチ）

`CONFIG_POCKET_VM_TCO`はYIELDに依存し既定n。hostは`build.sh o2-tco` / `asan-tco`。strictなflat SEG呼び出し元の`OP_tail_call`/`OP_tail_call_method`だけが対象。通常bytecode関数へ、実引数8個以下で、置換後のフレームが現在のセグメントと使用量予算に収まる場合に再利用する。native/bound/Proxy/async/generator、床、大きな引数列や容量不足は通常経路に戻す。

当初案のpop/pushではなく、同じブロックをその場でresizeする。これによりsegment先頭のpopによる返却・再確保を避け、適格性検査と割り込みpollの後には失敗しうる確保を行わない。関数・this・引数を小さいC一時配列へdupし、旧var refsを閉じ、旧local/operandを解放してから、所有スロット`[func,this,args...,vars...,stack...]`を構築する。補助関数をnoinlineにし、一時配列を通常のVM活性全体へ広げない。`JS_SF_TAIL`でresume時のargv/this復元を区別する。親link・ret_shape・caller_ctxはそのままなので、引数個数・method形が変わっても元の親のoperand片付けを変更しない。cur_funcは所有slotへの借用で、既存のGC/Discardスロット走査に含まれる。中間状態には中断点を置かず、完全置換後にB地点を通す。

hostの初期検証: 10万回の相互再帰、method/rest/this、匿名クロージャcallee、例外、default引数が完走。`tco_probe.py`は100回/10万回/100回の毎中断GCでlive最大値の一致を要求し、実測はすべて512B。毎中断GCは1,603停止/再開一致、ASan/UBSan報告なし。既存66コーパスはo2通常とASan強制yieldで全件成功。既定OFFも再ビルドして66/66成功。寿命検査にはstrictなtail鎖＋捕捉変数の形を追加し、8起点×5モードで900ケース成功（新形は22境界×5、明示的returnへ修正後のsusp最大576B）。関連Test262 293ファイルは554成功・5失敗・後退0（新規成功4件: call-args/member-args/catch-finally/finally）。未対応の非組込みeval呼び出し4件、catch末尾1件はRangeErrorのまま。これは全TCO対応完了でも速度改善の証明でもない。専用故障注入、全Test262 subsetの再検証は残る。

深さ検査を追加した際、`return n?tail(n-1):42`は最適化されず、1万段を指定するとhostで283再開後にRangeErrorとなった。条件式中の末尾位置認識は未対応であり、単にテストを成功扱いしない。明示的な`if(!n)return 42;return tail(n-1)`では100段/1万段とも352B、203/20,003再開で完走。既存寿命検査の追加shapeも条件式だったため、明示的returnに修正し、実際に置換した状態のGC・終了・破棄を900ケース再検証した（変更前の1,368Bから576B）。コンパイラの条件式対応は今後の工程に残す。

## 7. 実機の第1関所（2026-09-16）

独立`build_vm_tco_selftest`と専用sdkconfigでYIELD/SELFTEST/TCOを有効化。ELFに`js_vm_tail_reuse`と`check_tail_depth`が含まれることをnmで確認してからCOM3で実行した。app2,159,504B、DIRAM123,356B、flash1568256B（診断コード込み）。image SHA256 `873ebacacf107925b0ee9b9c12c757ab10c1bd211f52f6b612c019c8c54decbc`。

- 明示的末尾再帰100段/1万段: SEGフレーム容量の中断時最大値は両方184B。203/20,003回の再開、1000再開ごとのGC、最終値42を各2周確認。これはJSフレーム容量で、Cスタック消費やsegment確保総量ではない。
- 8起点×5モード900ケースを各2周成功。TCO捕捉変数ケースの中断SEG容量最大は300B（host576B）。guest16ケース、実timer3モード、並行要求10回、watchdog9999再開も成功。100µs timer完走は1,371/1,379再開。
- 両周ともfree269272→269272B、largest159744B。所要55,463,866/55,462,320µsには検査の待機・GCを含み、性能比較には使わない。通常アプリのsmoke3周・故障回復6種も成功。
- 現行ソースのhost o2-tcoでTest262既存subset全4,099ファイルを再実行: 7,505成功・190失敗・0 skip・baselineからの後退0。新規成功4件は§6と一致。

実験設定は既定へ反映しない。未対応の条件式/eval/catch経路や対象外形の故障検査は引き続き必要。

検証後は元Kasane appを復元し、書き込みhash一致、smoke3周・故障回復6種・`HOME_READY`を確認。変更したのはapp領域のみで、NVS・storage・パーティションは書き換えていない。

## 8. 条件式・catch・非組込みeval（2026-09-16、host検証）

`bytecode_dump.c`で実行せずpass2/final命令を確認した。条件式はcall→goto→return、catchはcall→nip_catch→returnと、catch本体を囲む合成の再送出ハンドラだった。

TCO有効かつstrictのコンパイル時だけ、callからsource_loc/label/goto/close_loc/nip_catchを越えてreturnへ至るかを調べる。探索は64命令まで（循環や長い鎖でコンパイル負荷を増やさず、超過時は通常call）。gosubや値を変える命令は越えない。参照カウントや他の分岐先を壊さないため、継続側のコード/labelは残す。

nip_catchを越えられることと、実行中のハンドラを捨てられることは別。置換前に現フレームのcatch offsetを検査し、対象先がclose_loc列とthrowだけの透明な再送出ハンドラの場合だけ除去を許す。通常catch、finally、iterator（offset 0）は再利用せず、既存call/例外経路へ戻す。非組込みevalは既存のbuiltin同一性検査の後、直後がreturnの場合だけ同じ置換処理へ入る。組込みの直接evalとscope_idxは変更しない。新しい命令番号は追加していない。

host実測: 関連Test262 293ファイルは559成功・失敗0・後退0（初期実装から5件改善）。既存subset全4,099ファイルは7,510成功・185失敗・0 skip・後退0、元baselineから9件新規成功。ASan/UBSan強制yieldのコーパスは新しい`tco_guards`を含む67/67成功、寿命900ケース成功。`tco_paths.js`の条件式・catch・論理式・nullish・コンマ式は10万段を完走し、100段の毎中断GCでも値/副作用が一致。条件式/catchの2本だけの測定では407停止/再開・susp最大400B（host）。`tco_guards`は通常catchの最寄り捕捉、finally21回、捕捉したcatch変数、native/bound/Proxy、9引数、直接eval、iterator closeを出力比較し、既定OFFでも同じ期待値が成功した。

64命令上限追加後も深い経路と毎中断GCを再検証済み。この拡張版の実機検証は次工程で、§7の実機結果は拡張前のバイナリに限る。大きいフレーム/多数引数/低メモリ時の対照検査と既定値判断も残る。

## 9. 拡張版の実機・確保不要経路（2026-09-16）

§8の次工程を実施した。独立buildのimage SHA256は`eb25e11233a6e4ee8af35042a4cb1ac452e08a8ed2db9b9ef719188af6d2792b`。app2,160,768B、DIRAM123,356B、flash1,568,924B（診断コード込み）。既定設定は変更していない。

- 実機COM3で明示return・条件式・透明catch・非組込みevalの4形を各100段/1万段で2周検査。中断時SEGフレーム容量最大はそれぞれ184/184/200/224Bで、深さによる増加なし。各203/20,003再開、1000再開ごとのGC、最終値42が一致。host ASan版では352/352/384/432B。
- 再利用を10回行った後、native callbackでヒープ上限を1Bに下げ、以後1万回の自己末尾再帰を完走。host ASanと実機2周ともOOM canaryは0。これは適格な同一segment置換が追加確保不要である検査であり、通常callへのfallback中の確保失敗を網羅するものではない。
- 寿命900ケース×2周、guest16ケース、timer3モード、並行要求10回、watchdog9999再開が成功。timer完走は1,210/1,206再開。両周free269272→269272B、largest159744B。所要58,495,272/58,497,663µsは検査の待機・GC込みで、速度比較には使わない。通常起動3周・故障回復6種も成功。
- `tco_guards`に9引数の深い再帰、および600個の捕捉localを持つ大きいフレームを追加。通常callにfallbackした再帰が予算のRangeErrorで止まり、捕捉値179700と後続呼び出しの回復を確認。host profileの既定OFFとASan TCO強制yieldで同じ期待値に一致（後者58,518停止/再開）。これはhostの64MiB heap/7MiB stack profileであり、実機20KiB予算の測定ではない。拡張後のhost寿命900ケースも成功。

既定値はnを維持する。strictだけでもError.stackと再帰の予算境界が観測可能に変わるため、移植の既定挙動維持という条件では自動的な有効化を採用しない。実験スイッチとしての検証を続ける。残るTCO専用の故障検査は、再利用不能な大きいcalleeへfallbackした際の追加確保失敗と、その後の所有権・回復である。

実機は検証後に元Kasane appへ復元し、書き込みhash一致、通常起動3周・故障回復6種を再確認した。app領域だけを更新し、NVS・storage・パーティションは変更していない。

## 10. fallbackの確保失敗と回復（2026-09-16、host）

`tco_oom.c` / `tco_oom.sh`で§9に残した専用故障検査を追加した。Cからのfloorの次にstrictな小さいentryへ入り、捕捉したオブジェクトをクロージャとして外部へ残す。entry内のnative callbackで現在の使用量+2,048Bへヒープ上限を下げ、その直後の末尾callで600個の捕捉localを持つ大きいcalleeを呼ぶ。このcalleeは同一segmentに入らず、通常callへのfallbackが必要になる。小さいエラーとbacktraceを作る余裕を残し、大きいフレームの確保だけを拒否する。

現行ソースで再ビルドした`asan-tco` / `asan-yield`（TCO無効対照）/ `o2-tco`の全3変種について、通常実行と強制yield・毎中断GCの2モード、計6ケース成功。全ケースの最初の拒否は14,583B、OOM canaryは1回、例外はInternalError。callee本体へ未到達で、上限を戻してGCした後も捕捉値17を保持し、同じ大きいcalleeの再実行は179700、後続の小さい関数は42を返した。強制yield時は2停止/再開。ASan/UBSan報告なし。ホストのサイズであり実機の確保量ではない。既知のじわじわ型OOM/backtrace不具合を解消したという主張でもない。

これでTCO実験実装の予定していた関所（深さ非比例、所有権/中断/GC/破棄、対象外fallback、確保禁止・確保失敗、既存コーパス/Test262、実機）を通過した。既定は§9の互換性判断によりnで確定する。全エンジンの完了ではなく、セグメントサイズ・断片化・L2c実機統合などはbacklogに残る。速度改善の数値も主張しない。
