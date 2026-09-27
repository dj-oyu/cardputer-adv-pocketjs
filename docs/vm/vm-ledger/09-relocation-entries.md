# L3 台帳: セグメントを指す入口の再監査 (09-relocation-entries)

対象: `components/quickjs-ng/quickjs-ng/quickjs.c` と `quickjs-vmstack.h`、`vm/l3a-refs` の
基点 `953c627`（tag `vm-L2`）。行番号はこの commit で読んだ時点のもの。

[02-frame-pointers.md](02-frame-pointers.md) は **L2 実装前**の監査で、フレームが C スタック
（`alloca`）にあった頃の地図を持つ。この台帳はその続きではなく**やり直し**で、問いが変わって
いる。02 の問いは「フレームはどこにあるか」だった。09 の問いは

> **セグメントの payload を別の番地へ写したとき、書き換えないと古い番地を指したままになる
> フィールドはどれか。**

L2a/L2b でフレームは `JSVMSeg` の payload に移り、L2c で「実行が静止する瞬間」が定義された。
L3 が動かすのはこの payload であり、**動かす対象は3種類しかない** — セグメント payload、
その中の1ブロック、ブロック内の1スロット。それ以外（`JSAsyncFunctionData` の中のフレーム、
ネイティブフレームの C 自動変数、バイトコード本体）は L3 の移動対象ではない。ただし
**移動対象を指している**なら、この台帳に載る。

## 0. 何が動くのか — ブロックの実体

`js_vm_stack_push` が返す1ブロックの中身（`quickjs.c:19300-19364`、`quickjs-vmstack.h:655-659`）:

```text
 ← 低位                                                              高位 →
[JSVMLink][JSStackFrame][arg_buf: arg_allocated_size][var_buf: var_count]
                                     [stack_buf: stack_size][var_refs: var_ref_count]
```

- `JSVMLink` は `CONFIG_POCKET_VM_FLATCALLS` のときだけ在り、`JS_VM_FRAME_PREFIX` バイト前置される。
- `sf` はブロック先頭ではなく `JSVMLink` の直後。`(JSVMLink *)sf - 1` で link に戻る
  （`quickjs.c:2968` が実際にそうしている）。
- `arg_buf` は `arg_allocated_size == 0` のとき**ブロック外**（呼び出し元の `argv` を直接指す、
  `quickjs.c:19332`）。これは移動対象ではないが、呼び出し元がセグメント内なら **その argv は
  別のブロックの中**にある。§3-4 の理由。

セグメントそのものは `JSVMSeg`（`quickjs-vmstack.h:197-217`）で、ヘッダは payload と同じ
`js_malloc_rt` ブロックの先頭に置かれている。**ヘッダごと動かすのか payload だけ動かすのかは
設計の決定事項**（[vm-L3-design.md](../vm-L3-design.md) D44）で、台帳はどちらでも成り立つ形で
書く: 以下の「指す先」はすべて payload 内の番地である。

## 1. 根 — ここから全部に届く

| # | 入口 | 位置 | 指す先 | 備考 |
| --- | --- | --- | --- | --- |
| E1 | `JSRuntime.current_stack_frame` | `quickjs.c:338` | 実行中の最上位 `JSStackFrame` | 走っている間だけ。中断中は `floor->prev_frame` に差し替わる（`22305`） |
| E2 | `JSRuntime.vm_susp.top` | `quickjs.c:345` | 中断中の鎖の最上位フレーム | **中断中の唯一の根。** `top != NULL` が駐機状態の述語 |
| E3 | `JSRuntime.vm_susp.floor` | `quickjs.c:346` | 中断中の鎖の床フレーム | SEG 床ならセグメント内、async 床なら `JSAsyncFunctionData` 内（§4） |
| E4 | `JSVMStack.cur` | `quickjs-vmstack.h:220` | 最上位セグメント | 鎖は `JSVMSeg.prev` で下へ |
| E5 | `JSVMStack.cache` | `quickjs-vmstack.h:221` | 空セグメントの LIFO | **フレームを含まない。** 移動対象だが参照の書き換えは不要 |

E1〜E3 は `JSStackFrame *` を直に持つ3本で、**L3 の移動が始まる前にこの3本のどれが有効かを
決めておく必要がある**。E1 と E2 は排他ではない: 中断中でも `current_stack_frame` は床の下
（ネイティブフレーム、セグメント外）を指しうる。

## 2. フレーム鎖 — 1フレームあたり5本

`JSStackFrame`（`quickjs.c:426-463`）のうち、**セグメントを指しうる**のは次の5本。

| # | フィールド | 指す先 | 移動時に必要な補正 |
| --- | --- | --- | --- |
| E6 | `prev_frame` | 下のフレーム | 下のフレームが在るセグメントの差分。**フレームごとに違いうる** |
| E7 | `arg_buf` | 自ブロック内、または呼び出し元ブロック内（§0） | 指した先が在るセグメントの差分 |
| E8 | `var_buf` | 自ブロック内 | 自ブロックの差分 |
| E9 | `var_refs` | 自ブロック内 | 自ブロックの差分 |
| E10 | `cur_sp` | 自ブロック内（`stack_buf` 上） | 自ブロックの差分。中断中の SEG 最上位フレームでのみ非 NULL |

**`cur_pc` はバイトコード本体（`b->byte_code_buf`）を指すので対象外**（`quickjs.c:19369`）。
`cur_func` は `JSValue` でヒープのオブジェクトを指すので対象外。`caller_ctx` も対象外。

E6 が「フレームごとに違いうる」のがこの段の核心で、**根の1個の加算値では表せない**
（仕様 §8「断片ごとに異なる移動量を、根元の一つの加算値で表現しない」）。深さ 31 の再帰は
位置ごとに大きさの違うセグメントを 3〜4 本跨ぐ（`quickjs-vmstack.h:148-152`）ので、鎖の
途中で差分が変わる。

## 3. フラット呼び出しが足した2本

| # | 入口 | 位置 | 指す先 | 備考 |
| --- | --- | --- | --- | --- |
| E11 | `JSVMLink.caller_sp` | `quickjs-vmstack.h:657` | **呼び出し元**ブロックの `stack_buf` 上 | ブロックごとに1本。床のものは未使用だが在る |
| E12 | `JSAsyncFunctionData.flat_caller_sp` | `quickjs.c:1013` | 呼び出し元ブロックの `stack_buf` 上 | **保持側はセグメント外**（GC オブジェクト）。`JS_SF_FLAT` の間だけ有効 |

E12 が示すのは、**セグメント外の構造体がセグメント内を指す**経路が実在することで、
「セグメントの中だけ歩けば済む」という素朴な前提はここで崩れる。E12 に到達する道は2本ある:

1. フレーム鎖を歩き、非 SEG フレームに出会ったら `container_of(sf, JSAsyncFunctionData,
   func_state.frame)` で容器へ戻る（`quickjs.c:2969-2970` が GC マークでやっている）。
2. GC のオブジェクト一覧を全走査する。

**1 を採る**（2 は中断中に全ヒープを歩くことになり、L4 の停止時間の議論を先取りしてしまう）。
根拠は `js_vm_mark_suspended` が既に 1 で足りている実績（§6）。

## 4. 開いた var_ref — 唯一の「外から中への」多数入口

`JSVarRef.pvalue`（`quickjs.c:511`）は、**開いている**（`is_detached == false`）とき
`&sf->arg_buf[i]` または `&sf->var_buf[i]` を指す（`get_var_ref`、`quickjs.c:18256-18284`）。
閉じたものは自分自身の `value` を指すので対象外（`18305`、`18561`）。

`JSVarRef` は GC オブジェクトでセグメントの外に在る。にもかかわらず**全数に到達できる**のは、
開いた var_ref が必ず生成元フレームの表に登録されているからである:

```c
var_ref->stack_frame = sf;              // quickjs.c:18282   E14
sf->var_refs[var_ref_idx] = var_ref;    // quickjs.c:18283
```

この双方向の登録は `close_var_refs`（`18583-18594`）と `JS_FreeValueRT` の var_ref 経路
（`7202-7203` の `assert(sf->var_refs[var_ref->var_ref_idx] == var_ref)`）が維持している。

| # | 入口 | 指す先 | 到達方法 |
| --- | --- | --- | --- |
| E13 | `JSVarRef.pvalue`（開いているもののみ） | `arg_buf[i]` / `var_buf[i]` のスロット | フレームの `var_refs[0..var_ref_count)` を歩く |
| E14 | `JSVarRef.stack_frame`（開いているもののみ） | 生成元 `JSStackFrame` | 同上 |

**`close_var_refs` が L3 の var_ref 走査の雛形である。** 同じ2行のループで、閉じる代わりに
2本のポインタへ差分を足す。`sf->var_ref_count` が上限で、`NULL` 要素は飛ばす。

ここで**閉じた var_ref を触ってはならない**: `pvalue == &var_ref->value` は自己参照で、
差分を足したら壊れる。判定は `is_detached` を読む（`quickjs.c:8057` が
`pvalue == &var_refs[i]->value` で同じ判定を別の形でやっている。**どちらを使うかは実装の
決定**で、`is_detached` のほうが意図が読める）。

## 5. 対象外だが紛らわしいもの

- **`JSObject.u.array.u.var_refs[]`**（`quickjs.c:10476`、`11368`）: クロージャが持つ var_ref
  の**配列**。要素は `JSVarRef *` で、var_ref 自体はセグメント外。E13 経由で既に補正される
  ので、ここを歩く必要はない。**二重補正の危険がある**ので、歩かないことを決めておく。
- **`JSVMSeg.base` / `end` / `top`**: セグメント自身の記述。移動の結果として書き換わるが、
  「補正される入口」ではなく「移動の定義」。
- **`JSVMStack.used` / stats**: バイト数であって番地ではない。移動で変わらない。
- **ネイティブフレーム**（`quickjs.c:6948`、`18638`、`19090` などの `JSStackFrame sf_s;`）:
  C の自動変数。セグメントに無い。ただし `sf_s.prev_frame` はセグメント内を指しうる（E6 と
  同じ補正が要るが、**C スタック上の構造体を歩くことになる**）。§7。

## 6. 既にある走査 — `js_vm_mark_suspended`

`quickjs.c:2945-2978`。GC が中断中の鎖を根として辿るために L2c が書いたもので、
**L3 が要る走査の形をすでに持っている**:

```c
for (sf = rt->vm_susp.top; ; child = sf, sf = sf->prev_frame) {
    if (sf->l2_flags & JS_SF_SEG) {
        JSValue *end = child
            ? ((child->l2_flags & JS_SF_SEG)
                ? (((JSVMLink *)child) - 1)->caller_sp          // E11
                : container_of(child, JSAsyncFunctionData,
                               func_state.frame)->flat_caller_sp) // E12
            : sf->cur_sp;                                        // E10
        for (p = (JSValue *)(sf + 1); p < end; p++) ...
    }
    if (sf == floor) break;
}
```

読み取れる事実は4つ:

1. **`top` から `floor` までの片方向の歩きで鎖は尽きる。** 終端は `floor` で、`NULL` ではない。
2. **各フレームの「生きているスロットの範囲」が決まる。** 上端は子フレームの `caller_sp`
   （E11/E12）か、最上位なら `cur_sp`（E10）。
3. **`JS_SF_SEG` ビットが「このフレームはセグメント内か」の判定である。** 型でも番地でもなく
   フラグ。L3 の走査も同じ判定を使う（番地でセグメントを引く必要はない）。
4. **非 SEG フレーム（async/generator）は鎖の途中に混ざる。** 飛ばすのではなく、E12 のために
   容器へ戻る必要がある。

**差**: マーク走査は `JSValue` のスロットを読むだけで、E6〜E9・E11〜E14 のポインタを一本も
書き換えない。L3 の走査はスロットを読まず、ポインタだけを書く。**走る鎖は同じで、触る
フィールドが排他**である。

## 7. 走っている間は移動できない

E1 が有効な状態（`JS_CallInternal` が実行中）では、C の自動変数がブロック内を指している。
`quickjs.c:19300-19370` の `local_buf`・`arg_buf`・`var_buf`・`stack_buf`・`sp`、および
`var_refs`（クロージャ側）である。これらは**この台帳の外**にある — 型で列挙できず、
レジスタに割り付けられていることもあり、仕様 §8 が禁じる「C レジスタ・スタックの任意の数値を
ポインタと推測して補正する方式」以外に触る手段が無い。

したがって **L3 の移動は `rt->vm_susp.top != NULL` の間に限る**（設計 D45）。中断時に
`JS_CallInternal` の活性は呼び出し元へ戻り切っており、C スタックにはブロックを指す値が残って
いない。これは L2c が作った性質で、**L3 がそれを前提にできるのは L2 の成果そのもの**である
（仕様 §8 の「L2 の成果のうち L3 が前提にできるのは…安全地点でスタックが静止する瞬間が
定義されていること」）。

`ネイティブ呼び出し中の移動を全面禁止する`（仕様 §8）という初期実装の制限は、この形では
自動的に満たされる: ネイティブ関数の中には中断点が無いので、`vm_susp.top` は立たない。

## 8. 入口の総数

| 区分 | 入口 | 数 |
| --- | --- | --- |
| 根 | E1〜E5 | 5 |
| フレームあたり | E6〜E10 | 5 × フレーム数 |
| ブロックあたり | E11 | 1 × SEG フレーム数 |
| 非 SEG フレームあたり | E12 | 1 × 混在する async フレーム数 |
| 開いた var_ref あたり | E13、E14 | 2 × 開いている var_ref 数 |

**14 種類。** `quickjs.c` での出現行数（`current_stack_frame` 39、`prev_frame` 22、
`arg_buf` 29、`var_buf` 14、`var_refs` 14、`cur_sp` 27、`pvalue` 39、`stack_frame` 4、
`caller_sp` 17）は 200 行を超えるが、**そのほとんどは読み出しで、移動時に書き換える必要が
あるのは上の 14 種だけ**である。読み出し側は移動後に改めて読むので影響を受けない —
移動が「読み出しが起きない瞬間」に限られている（§7）ことの帰結。

## 9. この台帳が保証しないこと

- **網羅性の証明ではない。** 14 種は `quickjs.c` と `quickjs-vmstack.h` を読んで挙げたもので、
  機械的な全走査ではない。抜けを捕まえるのは実装側の毒化（移動後に旧 payload を毒で埋め、
  古い番地への touch を ASan と実機の両方で検出する）であって、この文書ではない。
- **ゲスト側とファームは監査済みで、入口は無い。** `grep -rn
  "JSStackFrame\|current_stack_frame\|JSVarRef" components/pocketjs_guest main/ tools/vmtest/`
  の一致は 5 件すべて散文（`main/Kconfig.projbuild` の説明 4 件、`tools/vmtest/vmrun.c:524`
  のコメント 1 件）で、**フレームやセグメントを保持するコードは quickjs.c と
  quickjs-vmstack.h の外に 1 箇所も無い**。E1〜E14 が VM の中で閉じているという意味で、
  これは L3 にとって大きい — 補正の範囲がコンポーネント 1 つに収まる。
- 行番号は `953c627` のもの。`quickjs.c` は 66,955 行あり、上流の取り込みで動く。
