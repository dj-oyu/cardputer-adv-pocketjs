# 末尾呼び出しでフレームを使い回す（TCO）— 設計下書き

対象: [vm-L2-design.md](vm-L2-design.md) の L2b（フラット呼び出し）の後に独立した段として入れる案。
**この文書全体が下書きであり、決定ではない。** 実装前に L2c（[vm-L2-design.md](vm-L2-design.md) §10）の本体が要る（§4「決定すること」参照）。今回コードに入れたのは足場（§3）だけで、挙動は変えていない。未着手・未確認の項目は [backlog.md](backlog.md) にもまとめてある。

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
- **観測できる変化。** `Error.stack` から畳んだ呼び出し元が消える。仕様上 strict の末尾呼び出し（PTC）では許される。**sloppy も含めるか**は未決（コンパイラは区別していない）。コーパスの期待値に末尾位置の backtrace を固定したものがあるかは未確認。
- **既存の期待値への影響。** 末尾再帰で予算（`RangeError`）に当たることを固定したコーパスや `budget_probe.sh` の項目があれば変わる。未確認。
- **Test262 の見込み（未測定）。** strict の通常・メソッド呼び出しの2本は通る見込み。`try`/`catch`/`finally` の3本は、コンパイラがそれらの位置で `OP_tail_call` を出しているかを確かめてから。sloppy の4本は sloppy を含めるかの決定次第。
- **検査の形。** 末尾再帰の深さを2通りに変えても VM スタックの使用バイトが一定であること（G1 と同じ「深さに比例しない」判定）を関所にする。
- **切り替え。** `CONFIG_POCKET_VM_TCO`（既定 n）で入れ、関所が通ってから既定を決める。

## 5. 足場（コードに入れたもの、挙動不変）

- `CASE(OP_tail_call)` を `CASE(OP_call)` の**上**に、`CASE(OP_tail_call_method)` を `CASE(OP_call_method)` の上に置き直した。`DIRECT_DISPATCH`（gcc の計算型 goto、ファームもホストも）では `CASE(op)` がそのまま `case_OP_xxx:` ラベルなので、**末尾呼び出し専用のジャンプ位置がすでにある**。いまは空で、下の通常呼び出しの本体へフォールスルーする（`opcode` は `OP_tail_call` のまま読めるので `JS_RET_TAIL` と `goto done` は従来どおり）。
- §2 の分岐はこの位置に足す。**通常の `OP_call` の経路には比較が1つも増えない。** 足すコードは `pc` を進めない（引数の数は `get_u16(pc)` で読む）ことで、フォールスルー先の解読と両立させる。
- 新しい命令の enum は足していない。命令は既にあり、番号をずらすとバイトコードの読み込みと食い違う。
