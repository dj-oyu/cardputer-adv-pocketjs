# Math の呼び出しを native フレームなしで済ませる（2026-10-23）

**状態: 試作（`js_call_math_fast`、`quickjs.c`）。host の検査は通過。実機の速度は未測定。** 数字の区分は、実測（host または実機で測った値）、推定、コード（ソースや逆アセンブルで確かめた事実）の3つ。逆アセンブルは、`quickjs.c` を Xtensa の GCC 15.2 で `-Os -mlongcalls -ffunction-sections` を付けて単体でコンパイルしたもの。ファームの ELF ではない。

## 1. 今の経路（コード）

DERBY は `const M = Math, sin = M.sin, flo = M.floor, rnd = M.round, mx = M.max, mn = M.min`（`apps/derby/derby_watch.js:6`）で、すでに別名を使っている。そのため、呼び出し方は2通りある。

- `sin(x)`: `get_var sin` → `call1`
- `M.cos(x)`: `get_var M` → `get_field2 cos` → `call_method`

`Math.sin(x)` と直接書いた場合だけ、`get_var Math` が `global_var_obj` で外れ、`global_obj` で当たる（ハッシュの検索が2回、`JS_GetGlobalVar`、`quickjs.c:13200`）。DERBY では `Math.exp` の1箇所しかない。

`sin(x)` を、引数が倍精度の場合に、`-Os` で実際に呼ばれる関数の順に並べると次のとおり。インライン化されたものは除いてある。

| 段 | 呼ばれる関数 | 一般性のためにあるか |
|---|---|---|
| `get_var` | `get_u32`、`find_own_property`、`js_dup` | |
| `call1` | `js_vm_flat_callable`（isra）。偽を返し、C の再帰に入る | |
| 再帰 | `JS_CallInternal` の入口（`entry a1, 0x130`）、`js_poll_interrupts`、クラスの表から間接呼び出し | ○ |
| | `js_call_c_function`（586 B）: スタックの検査、`JSStackFrame` の push、realm の切り替え、cproto の switch | ○ |
| | `JS_ToFloat64` → `js_dup` → `JS_ToFloat64Free` | ○ |
| | `js_math_sin` → `sin`（libm。倍精度はソフト浮動小数点） | |
| | `js_number`（`__fixdfsi` を含む） | |
| | `js_vm_pop_frame`、戻り際の `js_vm_leave_frameless` | ○ |
| 後始末 | `JS_FreeValue` → `JS_FreeValueRT` を（関数＋引数）回 | |

C の関数は、重複を除いて約18種類、呼び出しは約22回（コード）。このうち ○ の8種類は、「呼び先が任意の native かもしれない」ためだけにある。

## 2. DERBY の Math（host 実測、現 HEAD）

`tools/vmtest/opprof/run_derby_prof.py --only oval_w2_mid_w100`（楕円・WIDE 2・MID、449フレームの平均）で測った。

| 関数 | 回/フレーム | 呼び方 | 主な呼び出し元 |
|---|---:|---|---|
| sin | 59.8 | 別名 `sin()` | `rin`（view:216）39.9、`pose` 11.9、`at` 8.0 |
| imul | 32.0 | `M.imul` | `rng`（watch:18） |
| min | 32.0 | 別名 `mn()` | `at` 16.0、play 9.0、`step` 6.0 |
| max | 17.0 | 別名 `mx()` | `at` 9.0、play 3.0 |
| cos | 11.9 | `M.cos` | `pose` |
| pow | 8.0 | `M.pow` | `at` |
| round | 2.0 | 別名 `rnd()` | |
| sqrt | 1.0 | `M.sqrt` | `shot` |
| **計** | **163.5** | 別名 110.8、`M.x` 52.9 | |

[derby-wide2-profile.md](../apps/derby-wide2-profile.md) の 466 回は、`ser()` を native へ移す前の値。移した後は 163.5 回になった。1フレームの命令も 38,246 から 12,375 に減っている。

ESP32-S3 の FPU は単精度だけなので、imul 以外はすべて倍精度のソフト浮動小数点で計算している。min / max の比較（`__unorddf2`・`__ltdf2`・`__eqdf2`）と、結果を整数に戻す `js_number` の `__fixdfsi` も同じ。

## 3. 案の比較

| 案 | 消える関数 | `JS_CallInternal` の増分 | ゲストのヒープ / DRAM | 正しさの危険 |
|---|---|---|---|---|
| (a) アプリの別名 | `get_field2` の検索だけ（`M.x` の 52.9 回） | 0 | 1つ数十 B（推定） | なし。DERBY はすでに半分を適用済み |
| (b) `get_var` / `get_field2` の IC | ハッシュの検索。ただし探査の手間は `find_own_property` と同程度 | 命令の処理ごとに検査と更新が入る | 箇所ごとに持つと数 KB〜（推定）。共有の表なら約0.5〜1 KB の DRAM | shape の無効化、chunk の読み込み、遅延の組み込み |
| (c) 呼び出し箇所の特殊化 | (d) と同じ分と `get_var` / `get_field2` | 新しい命令と書き換え | 書き換えに場所が要る（ポインタを持つなら32bit限定） | 書き換えのタイミング、TDZ、`Math` / `M` の差し替えの監視、ガード |
| **(d) 呼び出し命令の native 速い経路** | ○ の8種類 | **+68 B**（コード） | **0 / 0**（flash +399 B） | 引数が数値で、呼び先が純粋な組み込みのときだけ通る（§4） |

- (a) で節約できる額は、実機のベンチで `M.sin` 13.1 µs と別名の `sin` 12.1 µs の差、1.0 µs/回（実測、[derby-pan-camera-cost.md](../apps/derby-pan-camera-cost.md)）。52.9 回で約0.05 ms/フレーム（推定）。
- (c) で `get_var` を消す部分は、Math に限らない §4.2（get_var をスロットにする）と同じ仕組みになる。const のグローバルは初期化後に変わらないので、ガードが要らない。そのため Math 専用にせず、§4.2 で扱う方がよい。
- (b) は Math のための案としては割に合わない。

## 4. 採った案: (d) `js_call_math_fast`

`OP_call*` と `OP_call_method` で、`js_vm_flat_callable` が偽を返した後（呼び先が native のとき）に、1回だけ呼ぶ。速い経路に入る条件は次の2つ。

- 呼び先が `JS_CLASS_C_FUNCTION` で、次のどれかであること。
  - cproto が `f_f` / `f_f_f`（ctx を受け取らない純粋な double 関数）。
  - 関数ポインタが `js_math_min_max` / `js_math_imul`。
- 引数がすべて int か float64 で、個数が length 以上であること。

当てはまれば、libm を直接呼んで `js_number` で包む。imul で引数が int 同士なら、その場で計算する。当てはまらなければ偽を返し、今までの汎用の経路を通る。後始末（関数と引数の `JS_FreeValue`）は汎用の経路と共通。

**ガードは、呼び出しの時点の呼び先の値そのもの**。だから、無効化しなければならない状態を持たない。`Math` を差し替えても、`Math.sin` を置き換えても、別名や getter や bind や Proxy を使っても、呼び出し命令に届くのは「その時の関数値」で、それが上の組み込みでなければ汎用の経路になる。

native のフレームを省いても見分けがつかない理由:

- 引数がすでに数値なので、valueOf は走らない。変換による確保も、例外も起きない。
- `f_f` は ctx を受け取らないので、例外を投げられず、JS にも戻れず、スタックも見られない。min / max / imul は、数値の引数なら ctx を変換にしか使わず、その変換は失敗しない。
- そのため、ここから例外は出ない。バックトレースに native のフレームが無いことは観測できない。BigInt や Symbol や valueOf が投げる場合は汎用の経路を通り、`at sin (native)` は今までどおり出る（コーパスで確認）。

違いとして残るのは次の2つだけ。

- 割り込みカウンタの減り方。1回あたり1減っていたのが減らなくなる。ループの safepoint は今までどおり poll する。
- opprof の native 呼び出しのフック。`patch_quickjs.py` で速い経路にも入れた。DERBY の計数は 163.5 回で、変更前と一致した。

## 5. 検証（host）

- `tools/vmtest/corpus/math_fast_call.js` を新しく足した。期待値は、**速い経路の無いエンジン**（HEAD の `quickjs.c` の写し）で書いた。値（−0、NaN、±Infinity、int の境界）、呼び方（別名・`call_method`・局所の別名・他のオブジェクトのプロパティ・末尾呼び出し）、引数の個数（不足・余分。余分な引数は変換されない）、変換の順序と副作用、投げる変換とバックトレース、`Math.sin` の置き換えと復元、`globalThis.Math` の差し替え、getter、bind、Proxy、`Reflect.apply`、`.call`、`new`、同じ箇所で呼び先が替わるループ、を確かめる。
- 負の対照: 速い経路の結果を壊した写しでは、`v` `mm` `int-result` `forms` `arity`（max 5引数）`before` `restored` `getter` `loop` の行だけが変わった。`conv` `coerce` 投げる例 `replaced` `reassigned` `bound` `proxy` の行は変わらなかった。つまり、速い経路は数値の場合に通り、それ以外では通っていない。
- コーパスの全81件が asan、`--force-yield`、`--budget-jobs 3` で通過した。
- DERBY の host ハーネス（`run_derby.py`、既定の全構成）は exit 0 で、FAIL は無かった（オッズの較正、500 seed のコーナー、リプレイの一致を含む）。opprof の楕円 WIDE 2 は、ゴールの順位と時刻が変更前とビット単位で一致した。
- 時間（host、x86 の `-m32 -O2`、wall、5回）: DERBY の呼び方を5種類まとめたループを300万回回した。最小値で 1,563 → 1,316 ms（−16%）。x86 は浮動小数点がハードウェアでキャッシュも大きいので、実機の比率の根拠にはならない。

## 6. 結果と残り

- コード（Xtensa `-Os` の再現コンパイル）: `JS_CallInternal` 27,172 → 27,240 B（+68）。`js_call_math_fast` は 308 B。text の合計は +399 B（flash）。DRAM とゲストのヒープは増えない。
- 推定: 1回あたり C の呼び出しが8回減り、`JS_CallInternal` への再入（300 B のウィンドウフレームと、その入口のキャッシュ行）が無くなる。呼び出しの深さも1〜2段浅くなるので、ウィンドウのあふれも減る見込み。実機の差は、[derby-pan-camera-cost.md](../apps/derby-pan-camera-cost.md) §1 の `sqrt` / `sin` / `M.sin` のベンチで測る（未実施）。命令キャッシュの配置だけで15%動くので、`CONFIG_POCKET_VM_TURNPERF` で同じ配置どうしを比べること。
- 次の一手（未実施）: 引数が数値なら `JS_FreeValue` を呼ばない。関数の参照を減らす処理をインライン化する。§4.4 の「`JS_FreeValue` のインライン化」と同じ話。
- `mx.apply(null, xs)`（`OP_apply`、5回/フレーム）は対象外。
