# P1: インタプリタの熱い部分を IRAM に置く（設計・素案、2026-10-25）

`interpreter-stages.md` §9 の続き。P0（guard の要らない T0 の変更）で、DERBY の命令の数から見える取り分はおおむね取った（dispatch −4.9%、host）。残る大きな手は、1ターンに触るコードを flash の命令キャッシュから外すこと（P1）。この文書はその作り方・予算・測り方を決める。**数字は実測（実機）/ 実測（host・静的）/ 推定を必ず書き分ける。実機ではまだ何も測っていない。**

## 0. 結論（素案）

1. **`JS_CallInternal` を丸ごと IRAM に置くのは大きすぎる**（約 21〜25 KB、静的）。熱い部分だけを置くには、冷たいハンドラを手で別関数に出す（outline）しかない。Xtensa の GCC は関数の中の hot/cold 分割をしない（§2.1、実測）。
2. outline の後に IRAM に置く本体は **約 8〜10 KB（推定）**。前例（R5、helper 8 KiB を IRAM へ。実機で JS −15%、空き −8.7 KiB）と同じく、DRAM を 1:1 で払う。前例は「STRESS は 30 fps に届いている」としてユーザーが見送った。**DERBY の JS ターンは 14.5〜44 ms（実測、構成別）で、重い構成は 33 ms を超える。** 見送りの記録にある再開の条件（30 fps に届かないアプリ）に当たる。
3. 進め方: (a) outline は単独でも害が無い変更（flash のまま、命令キャッシュの行の密度が上がるだけ）として先に入れ、host の検査を通す。(b) IRAM 配置は Kconfig（既定 n）で切り替え、同じソースの A/B/A を実機で測る。(c) 入れるかは、JS ターンの短縮と空きの減少を並べて、ユーザーが決める（前例と同じ）。
4. ジャンプテーブル（`dispatch_table`、1 KiB、flash の `.rodata`）を DRAM に置くことも同じ切り替えに含め、別の Kconfig で単独の効果も測る。

## 1. 前提（既存の記録）

| 事実 | 区分 | 出典 |
| --- | --- | --- |
| JS のターンのサイクルの 70% が flash キャッシュのミス待ち（IPC 0.17） | 実測（実機） | `turn-cpi.md` |
| STRESS の 1ターンが触るコード 41.8 KiB（core 7.40、helpers 13.93、object/alloc 5.65、builtins 6.07、native 8.51） | 模型 | `turn-working-set.md` §2.2 |
| `JS_CallInternal` のうち 1ターンに何度も通る行は 約 6 KiB（core の床） | 模型 | `turn-working-set.md` §4 |
| helper 8 KiB（95 関数）を IRAM へ: STRESS の JS 7.00 → 5.93 ms（−15%）、`I_STALL_BUSY` 819K → 588K、空き −8,752 B | 実測（実機） | `r5-icache.md` §3.4 |
| 同じ 8 KiB は見送り（ユーザーの判断、2026-09-27）。再開の条件は 30 fps に届かないアプリ | 記録 | `r5-icache.md` §4 |
| icache のアラインメントだけで同じカーネルが 15% 動く | 実測（実機） | `CLAUDE.md` |

## 2. 調べたこと（2026-10-25）

### 2.1 GCC に hot/cold 分割を任せられるか — できない（実測、コンパイラ）

computed goto の小さなインタプリタ（熱い op と、`__attribute__((cold))` のラベルを付けた冷たい op）を、IDF v6.0.1 の `xtensa-esp-elf-gcc` 15.2.0（`-mdynconfig=xtensa_esp32s3.so -mlongcalls -ffunction-sections`）で `-O2` / `-O2 -freorder-blocks-and-partition` / `-Os -freorder-blocks-and-partition` に通した。どれも `.text.run` 1 つだけで、`.text.unlikely` は作られなかった。profile のフィードバック（`-fprofile-use`）があれば分けるかもしれないが、IRAM（0x4037_xxxx）と flash（0x42xx_xxxx）の間は Xtensa の分岐命令の届く範囲を超えるので、分けられても飛び先の作りに別の問題が出る（未確認）。**分割は手で関数を分ける。**

### 2.2 `JS_CallInternal` の命令ごとの大きさ（実測、静的）

PR #69 の image（`92c9822d`。`quickjs.c` は main `e0fe98a` と同一、本番のダンプ処理がまだ入った版）の ELF を逆アセンブルし、命令ごとに `addr2line -i` の一番外側の `quickjs.c` の行を、その行を含む `CASE(OP_x)` か関数内のラベルの領域に割り当てた（道具は scratch の `case_sizes.py`。リポジトリに入れるなら `tools/r5sim/` の隣）。`JS_CallInternal` は 25,498 B。DERBY（host opprof、`oval_w2_mid_w100`）の命令の使い方と突き合わせると:

| 区分 | バイト | 割合 |
| --- | ---: | ---: |
| DERBY で 1ターン 4 回以上使う命令（85 種）のハンドラ | 6,495 | 25% |
| まれに使う命令（15 種） | 746 | 3% |
| **DERBY で使わない命令** | **13,001** | **51%** |
| 入口・フレーム・呼び出し・戻り・例外などのラベル領域と prologue | 5,256 | 21% |

ラベル領域の大きいもの: prologue 910、`flat_async_call` 462、`done` 345、`frame_pushed` 340、`not_a_function` 309、`flat_call` 258、`restore_call_inputs` 247、`exception` 243、`vm_yield` 227、`has_call_argc` 200。このうち DERBY で熱いのは呼び出しと戻りの経路（`flat_call`・`frame_pushed`・`has_call_argc`・`call_inputs_ready`・`done`・prologue の一部）で、`flat_async_call`・`not_a_function`・`vm_yield`・`exception` は冷たい（推定。ターンの記録で確かめる）。

注意: 領域の割り当ては行の範囲で切っているので、共有するコード（`OP_xor` の 874 B は二項の論理演算の共通部分を含むと思われる）や、隣の領域へのはみ出しで数百 B は動く。ダンプ処理を消した今のブランチ（`53af3d2` 以降）では、各ハンドラがさらに小さい（`JS_CallInternal` 26,602 → 21,742 B、別ビルドの値）。

### 2.3 IRAM に置く本体の見積もり（推定）

熱いハンドラ 6.5 KB ＋ 熱いラベル領域 約 3.5 KB ＝ 約 10 KB（ダンプあり）。ダンプ無しの縮み（約 18%）を当てると **約 8〜9 KB**。DERBY 以外のアプリ（STRESS、MEGADEMO、BIG WAVE）の熱い命令は DERBY と重なりが大きいはずだが、未確認。足りない命令は flash の outline 関数で動く（遅くなるだけで、正しさは変わらない）。

## 3. 作り方

### 3.1 outline（単独でも入れられる変更）

冷たい `CASE` の本体を `static no_inline` の関数に移し、インタプリタには呼び出しだけを残す。

```c
CASE(OP_define_class): {
    sf->cur_pc = pc;
    if (unlikely(js_op_define_class(ctx, sf, &sp, &pc)))   /* flash */
        goto exception;
}
BREAK;
```

- **移せる命令の条件**: 本体が `ctx`・`sf`・`sp`・`pc`（とオペランド）だけを読み書きし、出口が `BREAK` か `goto exception` だけのもの。`var_buf`・`arg_buf`・`var_refs`・`b`・`this_obj` などの局所変数を使うものは、引数を足すか移さない。`goto` で別の領域へ飛ぶもの（`flat_call`、`done`、フレームの出入り、`park`/`yield` の検査点）は移さない。
- **移す順**: §2.2 の「DERBY で使わない命令」から大きい順。`with_*`（6）、`define_class*`、`define_method*`、private field 系、`eval`/`apply_eval`、`for_in`/`for_of`/iterator 系、`special_object`、super 系、`delete`/`define_var`/`check_define_var` など。13 KB のうち 8〜10 KB を出せれば、本体は 10 KB 台前半になる（推定）。
- **正しさの検査**: vmtest（asan・o2）、test262 の既定の範囲と、移した命令が出てくるディレクトリ（`with`、`class`、`for-in`/`for-of`、`eval-code`、private 系）の変更前後の項目ごとの一致（§9.2 と同じ方法）。例外のときの `cur_pc`（行番号）が変わらないこと。
- **outline 単独の効果**: flash のままでも、熱いハンドラが詰まって並ぶので、1ターンに触る行が少し減るかもしれない（模型で、`turnset.py` を新しい ELF に写して測る）。悪くなる要因は、移した命令の呼び出し 1 回分だけ（使う命令ではない）。

### 3.2 IRAM 配置（Kconfig で切り替え）

- `CONFIG_POCKET_VM_INTERP_IRAM`（既定 n）: `JS_CallInternal` を IRAM に置く linker fragment（`noflash`）。outline の後でなければ IRAM が足りない。
- `CONFIG_POCKET_VM_DISPATCH_DRAM`（既定 n）: `dispatch_table` を DRAM に置く。`quickjs.c` は host でも組むので、置き場所は IDF のときだけ定義されるマクロ（`JS_DISPATCH_TABLE_ATTR`）にする。1 KiB。
- 断片を差し替えたら `idf.py reconfigure`（`r5-icache.md` §4 の注意）。A/B は同じソースで Kconfig だけ変える。

## 4. 予算

| 項目 | 量 | 区分 |
| --- | --- | --- |
| IRAM 本体 | 約 8〜10 KB → 静的 DIRAM が同じだけ増え、空きヒープが同じだけ減る | 推定 |
| `dispatch_table` | 1 KiB | 静的 |
| ホーム画面の空き | 274 KiB 前後（Wi-Fi リンク後） | 実測（実機、`CLAUDE.md`） |
| DERBY の評価の余裕 / ゲスト最大 | 約 28.9 KB（線 20 KB）/ 134.1〜134.8 KB（140,000 B まで 約 5 KB） | 実測（実機） |

ゲストのヒープ上限（160 KiB）は別枠なので、評価の余裕とゲスト最大は直接は動かないはずだが、ネイティブの heap と最大連続ブロックが減る（R5 では 139,264 → 131,072 B）。**`memlog.py --port --check` の実測を受け入れ基準に入れる。**

## 5. 測り方（すべて実機、同一ソース、Kconfig だけを変える）

1. outline だけ（IRAM n）: 基準と outline 版の A/B/A。DERBY の固定台本（`derby_fixed_workload`）のフレーム時間と、評価の余裕・ゲスト最大。悪くなっていないことの確認。
2. IRAM y、DRAM n / IRAM n、DRAM y / 両方 y: それぞれ A/B/A。TURNPERF ビルドで `I_STALL_BUSY` / ターンと JS ターンの ms。
3. 同じターンを 2 回呼ぶプローブ（`interpreter-stages.md` §6.2(a)）: 1 回目と 2 回目の I_STALL の差で、ターン間の追い出し（描画・転送が触る量）を読む。IRAM 化の後に、残りの flash のコードが毎ターン取り直されているかが分かる。
4. `memlog.py --port --check`: 静的 DIRAM、ホームとアプリ実行中の空き、最大連続ブロック。

icache のアラインメントで 15% 動くので、それ未満の差は同じ image の中の比較（2 の Kconfig 違いは別 image なので、A/B/A の往復で揺れを見る）で判断する。

## 6. 決めること（ユーザー）

- IRAM 本体を入れるか: JS ターンの短縮（ms）と空きの減少（KB）を並べて判断する。前例は −1.07 ms 対 −8.7 KiB で見送り。
- outline だけを先に入れるか（IRAM を入れない場合も、作業集合の密度と、将来の IRAM 化の準備になる）。

## 7. リスク

1. outline で例外の位置（`cur_pc`）や参照の数を取り違える。命令ごとに vmtest と test262 の該当ディレクトリで確かめる。
2. 熱い命令の集合がアプリで違う。DERBY だけで選ぶと他のアプリが遅くなるかもしれない（outline された命令は呼び出し 1 回分遅いだけ）。STRESS・MEGADEMO・BIG WAVE で opprof を取り直してから集合を固定する。
3. IRAM に置いた本体から flash の helper を呼ぶのは今と同じ（`-mlongcalls`）。IRAM の中のコードが flash のリテラルを読むことは無い（リテラルは同じセクションに置かれる）が、ビルド後に `nm` と map で確かめる。
4. 測定の揺れ（アラインメント 15%）より小さい効果は主張しない。
