# 組み込みアプリの plan を flash に置き、名前で登録する（q33）

2026-10-01、ブランチ `vm/flash-plan`（`vm/main` 806b10b から）。[js-to-ir.md](js-to-ir.md) §4 B の設計を具体化し、native（`pocket_proc.c`・`ksn_proc_plan.*`・`ksn_procedural.*`）と表の生成器を実装して host で検証した。**firmware への表の組み込み（CMake・`make_app_chunks.py`）と DERBY の移行はしていない**（q34 の実機測定の後の別作業、§8）。`apps/` は変えていない。実機は使っていない。

数値の区別: **host 実測**（WSL。m32 は実機と同じ 4 B ポインタ・8 B JSValue、確保は TLSF の長さで課金）、**ビルド確認**（ESP-IDF v6.0.1 の Xtensa `-Os` オブジェクトの `size`/`nm`）、**推定**（大きさの式 × host で測った本数など）。

## 結論

- **API**: `pocket.kasane.procedural.register('derby.crowd', args, points?)`。第 1 引数が文字列なら組み込みの表を引き、配列なら今までの経路（バイト単位で不変）。`args` は plan 関数が宣言した引数の数ちょうど（無ければ `undefined`/`null`/`[]`）。
- **native の常駐**: 1 本 **40 + 4 × 引数 B**（Xtensa、`_Static_assert` で固定、ビルド確認）。命令は表（flash）に残り、`draw()` のたびに VM の自前の写し（`vm->owned_code`、今もここへ写している）へ写してから引数を当てて検証する。DERBY の同時 25 本（host 実測、3 段階とも）で **9,936 → 1,244 B（−8.7 KB、推定）**。
- **ゲスト**: `derby_prog.js` を id 形（`T` の値が `'derby.rail'` の文字列、復号器なし）にすると、評価後・評価のピークとも **−3,108 B**（host m32 実測）。`T` ごと無くす形（`load()` が `'derby.' + n` で名前を作る）なら評価後 **−5,676 B**・ピーク **−4,204 B**（同）。依頼時の見込み −5.5 KB は後者の評価後に相当する。
- **出力**: DERBY の 13 本（捕獲した入力と摂動、12,971 ベクトル）と点列の `nil` で、配列登録と名前登録の VM の状態・全線分（端点・色・順序）・ラスタ step が全件一致（ASan/UBSan、スカラーと偽 PIE の 2 通り）。名前登録も融合の経路を通る（参照経路への fallback 0 回を数えて確認）。
- **flash の費用**: DERBY の表 6,203 B（`.rodata` 6,048 B ＋名前 155 B、ビルド確認）、コード +2.4 KB 前後（下の §7）。静的 DRAM は +8 B（表のポインタと本数。スロットは 12 B のまま）。

## 1. JS の API

```js
const h = H.register('derby.stands', [tiers, rows, dots, arcs]);  // 組み込み
const p = H.register('derby.nil', [], {kind: 'affineQ14Points', ...}); // 点列つき
const q = H.register(rows, points);                                  // 今まで（不変）
```

| 呼び方 | 結果 |
| --- | --- |
| 第 1 引数が文字列 | 表を引く。引数は 1〜3 個（名前、引数、点列）。4 個以上は `INVALID_ARGUMENT` |
| 第 1 引数がそれ以外 | 今の配列の経路。コードは点列の読みを関数に括り出しただけで、判定・順序・確保は同じ |

誤りはすべて `PocketError`、`outcome: not_applied`。**種類の順は配列の経路と同じ**（呼び方の形 → スロット上限 → 値 → program）:

| 条件 | code | message |
| --- | --- | --- |
| 表に無い名前（表が無い、`''`、`'rail'`、末尾の空白・NUL を含む） | INVALID_ARGUMENT | `unknown built-in plan` |
| 表の項目が壊れている（生成器は出さない。§4） | INVALID_ARGUMENT | `invalid built-in plan` |
| `args` が配列でも `undefined`/`null` でもない | INVALID_ARGUMENT | `expected an array of plan arguments` |
| `args.length` が宣言した引数の数と違う | INVALID_ARGUMENT | `plan argument count mismatch` |
| 32 本が使用中（**確保の前、`args` を読む前**に判定） | LIMIT_EXCEEDED | `procedural plan limit` |
| plan の確保に失敗 | OUT_OF_MEMORY | `plan allocation failed` |
| 引数が有限の数でない（使わない引数も） | INVALID_ARGUMENT | `invalid plan argument` |
| 引数がその欄に入らない（回数 0..255 の整数、色 0..65535 の整数、値は float で有限） | INVALID_ARGUMENT | `invalid instruction entry`（配列の経路と同じ文言） |
| 引数を当てた program を解析が拒む（例: `REPEAT $0` に 0） | INVALID_ARGUMENT | `invalid procedural program`（同） |
| 点列 | 配列の経路と同じ（`point allocation failed` / `invalid typed point batch`） | |

- **名前の名前空間**: `アプリのディレクトリ名.plan 名`（`apps/derby` の `@plan crowd` → `derby.crowd`）。生成器が表全体で重複を拒む（ビルド時）。**実行中のアプリが自分の名前空間しか使えない、という強制はしない**: plan は描くだけの関数で capability を持たず、他のアプリの plan を描けても害が無い。強制するには `pocket_proc.c` が実行中のアプリの id を知る必要があり、その配線に見合う利益が無い。
- **配列の経路より厳しい点（意図的）**: 使わない引数も有限の数でなければならない。配列の経路では `prog()` が使わない引数を読まないので、文字列でも通っていた。名前の経路は引数を float で保つので、数でないものは受け取れない。
- **feature-test は足さない**: 組み込みの名前は firmware と同じビルドで作ったアプリだけが使う（§2）。SD やエディタのプログラムは配列の経路のまま。`capabilities` に出すのは、SD のアプリが組み込み plan を使いたくなったときに決める（CLAUDE.md の「実装した面だけ true」の規則に合わせ、実装した今なら出せる）。

## 2. ビルド時の表（生成器は実装、firmware への組み込みは未実装）

**生成器** [`tools/kasane_ir/emit_rom_plans.mjs`](../../tools/kasane_ir/emit_rom_plans.mjs)（実装済み）:

```
node tools/kasane_ir/emit_rom_plans.mjs OUT.c derby=apps/derby/derby_prog.js [app=FILE ...] [--symbol NAME] [--json OUT.json]
```

- 各 `@plan` 関数を `plan_js.mjs` → `kir.mjs` でコンパイルし（`lower_plans.mjs` と同じ道）、**行は出荷しているアプリが今登録している行そのもの**から作る: 詰めた文字列（`pack.mjs` の nibble 形）を、アプリが出荷している復号器（`DECODER_NIBBLE`）に引数すべて 0 で通す。引数の欄は表では 0 のまま、patch（`{pc, 欄, 引数番号}`、pc 順）に記録する。同じ文字列を `kir.mjs` の `assemble()` に試験の引数で通した行と `Object.is` で比べ（`-0` と `+0` も区別）、違えば止まる。
- float は **正確な 16 進の浮動小数**（`0x1.200000p+8f`）で書く。10 進で書くと C コンパイラと JS の丸めが違いうる（JS は 10 進 → double → float の 2 回丸め）。
- 出力: `const ksn_proc_rom_entry ksn_proc_rom_plans[]` と `const unsigned ksn_proc_rom_plans_count`。項目は `{名前, 命令, patch, 命令数, patch 数, 宣言した引数の数}`。1..64 命令、引数 8 個以下、`$n` が宣言の範囲内、を生成時に検査。
- DERBY: 14 本、480 命令、patch 19 個（stands 2、crowd 4、runner 13）。

**firmware への組み込み（設計、未実装）**:

1. **アプリの選択**: `@planDecoder` の印に `rom` を付けたアプリ（`/** @planDecoder rom */`）だけを flash 形にする。`make_app_chunks.py` がそれを見て、そのアプリの `@plan` チャンクを `APP_CHUNK_ROM`（アプリ id と元ファイルの組）に入れ、`APP_CHUNK_LOWER` の変換を `lower_plans.mjs --ids APP --file`（`--file` に `--ids` を通す小改修）にする。印の無いアプリは今の詰めた形のまま。1 つのアプリが両方の形を持つことは configure で拒む。
2. **CMake**: `add_custom_command(OUTPUT build/generated/ksn_proc_rom_plans.c COMMAND node emit_rom_plans.mjs OUT app=src ... DEPENDS 元ファイル emit_rom_plans.mjs plan_js.mjs kir.mjs pack.mjs)`、`target_sources` に追加。`APP_CHUNK_ROM` が空なら `make_app_chunks.py` が空の表（`count 0`）を Python で直接書く（**Node は今と同じく `@plan` があるときだけ要る**）。
3. **登録**: `app_session.c` がゲストの生成前に 1 度 `pocket_proc_rom_plans(ksn_proc_rom_plans, ksn_proc_rom_plans_count)` を呼ぶ（`reset()` は表を保つ。呼ばなければ全部の名前が未知）。extern の宣言を `ksn_proc_plan.h` に足す。
4. **host**: `tools/games/run_derby.py` の `lower()` が同じ `--ids` の変換を通し、`test_derby_host.c` が表をリンクして設定する。その oracle（`js_cap_reg` は行の配列を読む）は、名前と引数から表の行を組み立てる形に直す。

**版の管理**: アプリの JS と表は**同じビルドが同じ `@plan` 関数から作る**ので、組み込みアプリでは食い違いが起きない。ninja は両方を同じ元ファイルに依存させるので、片方だけ古くなることもない。食い違いうるのは firmware の外から入るプログラム（SD・エディタ・保存したプログラム）だけで、そのときは未知の名前（`INVALID_ARGUMENT`）か引数の数の不一致で止まる。plan の中身だけが変わり引数の数が同じ場合は検出できない。必要になったら、名前に内容のハッシュを付ける（`'derby.crowd#5f3a1c'`、名前の比較は完全一致なので native の変更は要らない。ゲストに 1 本 7 B 増える）。

## 3. 引数（`$n`）の扱い: begin で当てる

今は JS の `prog()` が `$n` を置き換えた行を `register()` に渡している。flash の命令は const なので、2 案を比べた。

| | (i) 引数を持つ命令だけ RAM に写す | **(ii) begin で引数を当てる（採用）** |
| --- | --- | --- |
| RAM | 写した命令 12 B ＋ pc（runner は patch 13 個がすべて別の命令 → 約 170 B） | 引数 4 B × 宣言数（runner 12 B、crowd 32 B） |
| begin | flash の命令と RAM の命令を合わせて VM へ | flash の命令を VM へ写し、patch を当てる（最大 13 回の欄の書き込み） |
| 検証 | 同じ | 同じ（当てた後の VM の写しを検証する） |
| 追加コード | 合成の処理 | `ksn_proc_apply_binding()`（30 行） |

(ii) にした。RAM が小さく、「VM が自分の写しを検証してから実行する」という今の不変条件がそのまま残る。begin は今も毎回 plan から `vm->owned_code` へ写しているので、写す元が DRAM から flash に変わるだけ。

- `ksn_proc_begin_bound(vm, program, binding, input, frame)`（`ksn_procedural.h`）: 写す → 当てる → `valid_program()`。欄ごとの検査（回数は 0..255 の整数、色は 0..65535 の整数、値は有限）は `apply_binding` 自身も持つ（登録時に検査済みでも、壊れた表や plan から VM を守る）。`binding` が NULL なら INVALID（プレースホルダの 0 のまま走らせない）。
- 既存の `ksn_proc_begin_state()` は同じ内部関数を `binding` なしで呼ぶだけで、挙動は同じ（コンパイル後の 13 本の出力 12,971 ベクトルが、元の VM と新しい VM でバイト単位で同一、§5）。

## 4. native の型

```c
typedef struct { uint8_t pc,field,param; } ksn_proc_patch;          /* ksn_procedural.h */
typedef struct {                                                      /* ksn_proc_plan.h: flash */
    const char *name; const ksn_proc_inst *code; const ksn_proc_patch *patch;
    uint8_t count,patches,params;
} ksn_proc_rom_entry;
typedef struct {                                                      /* heap: 40 + 4 * params B */
    KsnProcAffineQ14 points_coeff; KsnProcPointsPolicy points_policy;
    uint32_t fused[KSN_PROC_FUSED_WORDS];
    const ksn_proc_rom_entry *rom;
    uint8_t params,fused_count; bool valid,points_registered;
    float args[];
} ksn_proc_rom_plan;
```

- `_Static_assert(sizeof(void *)!=4 || (sizeof==40 && offsetof(args)==40))`: 実機と m32 host で 40 B を固定（Xtensa のコンパイルと m32 の DERBY のビルドで通った）。64 bit の host はヘッダが 48 B に伸びるので、`ksn_proc_rom_plan_bytes(p)` は `max(sizeof, offsetof(args) + 4p)`。
- 固定長・sized の plan とは別の型にした（sized の plan にポインタを足すと、配列で登録する全 plan が 4 B ずつ太る）。`pocket_proc.c` のスロットは `uint32_t handle:31, rom:1; void *plan; proc_points *points;` で、種類の 1 bit を handle の語に入れた（handle は `INT32_MAX` 以下、`register` が保証）。**スロットは 12 B のまま**（`.bss` の `slots` 384 B、ビルド確認）。
- `ksn_proc_rom_entry_valid()`: 1..64 命令、引数 8 個以下、patch が pc 順で pc・引数・欄が範囲内。`register` は名前を引いた直後にこれを呼ぶ（壊れた項目は確保の前に拒む）。

## 5. 解析・融合の印・上限の検査: 登録時に行う

- **採用: 登録時に解析する**。`ksn_proc_rom_plan_prepare()` が表の命令をスタック（768 B、配列の経路の `register` が行を読むのと同じ大きさ）へ写し、引数を当て、**配列の経路と同じ `mark_fused()`** で解析して融合の印を付ける。受理・拒否と融合の印が配列の経路と同じになる（試験で印を全登録で比較）。
- **ビルド時に済ませる案を採らなかった理由**: 受理が引数で決まる（`REPEAT $0` の 0 は拒否、回数は解析の上限に効く）ので、解析を引数なしで済ませることはできない。融合の印だけを表に持たせても（1 本 8 B の flash）、登録時の検証は残る。登録時の解析の費用は今の `prepare` と同じで、名前の経路で増えるものは無い。
- **draw 時の守り**（`run_core` の `memcmp` の代わり）: `rom_matches()` が VM の写しを、表の命令と「この登録の引数」で比べる（patch の欄は引数と比べてから表の値に戻し、残りをバイト比較）。違えば参照経路（`ksn_proc_run`）へ。試験で、同じ登録なら融合の経路（fallback 0 回）、同じ項目の別の引数の登録や別のバイトの VM なら参照経路へ落ちて参照と同じ結果になることを確かめた。
- **上限**（1..64 命令、レジスタ 16、ループの深さ、step 10,000、線分・ラスタ、32 本）はすべて今の場所のまま（`valid_program`・VM・`draw_impl`）。名前の経路で新しく増えた上限は、引数 8 個と表の項目の形だけ。

## 6. 見積もり（DERBY WATCH）

`python3 tools/kasane_ir/flash_plan_estimate.py`（WSL、`bash -lc`）。`apps/` は変えず、一時コピーで走らせる。

### 6.1 native（本数は host 実測、大きさは実機の式で計算 = 推定）

host のゲーム（m32、LIGHT・MID・HEAVY）の `load()`/`drop()` に記録を足したコピーで、生きている plan の集合を追った。

| 段階 | 同時の最大 | 今（sized、40 + 12n） | flash 形（40 + 4p） | 差 |
| --- | ---: | ---: | ---: | ---: |
| LIGHT / MID / HEAVY | 25 本（712 命令） | 9,544 B（TLSF の丸めとヘッダ込み **9,936 B**） | 1,144 B（**1,244 B**） | **−8,400 B（−8,692 B）** |

- 3 段階とも同じ集合（crowd と stands の段は引数だけが違う）。
- 今の値は plan-sized-alloc.md の実機実測 9,696 B（crowd が 40 命令だった頃、686 命令）と、crowd の +24 命令 × 12 B を足して整合する。
- 段階 3（首振り、30 本）は今のツリーに無いので測っていない。js-to-ir.md の本数（30 本・916 命令、crowd が 40 命令だった頃）で同じ式を当てると、要求の合計で sized 約 12.2 KB → flash 約 1.4 KB（推定、TLSF の丸めの前）。

### 6.2 ゲスト（host m32 実測、評価だけ）

| `derby_prog.js` | 大きさ | 評価後 | 差 | 評価のピーク | 差 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 出荷形（詰めた文字列 14 本と復号器） | 6,344 B | 106,524 | — | 120,936 | — |
| id 形（`T` の値が `'derby.rail'`、復号器なし） | 1,908 B | 103,416 | **−3,108** | 117,828 | **−3,108** |
| `T` なし（`load()` が名前を作る形、上限） | 1,584 B | 100,848 | **−5,676** | 116,732 | **−4,204** |

- `derby_view.js` は変えていない（`load()` が `prog()` を呼ぶまま。評価中には呼ばれない）。
- `T` の 14 個の短い文字列を消すだけで評価後が −2.6 KB も動くのは、atom 表・shape 表の段階的な伸長の閾値をまたぐためと見られる（js-to-ir.md §2 の注記と同じ、±1.5 KB 程度の揺れ）。id 形と `T` なしの間の差は、この揺れを含む。
- 実機では host の約 1.3 倍になった前例（derby-pan-memory.md）で換算すると −4〜7 KB（推定）。
- 登録 1 本の一時的なピーク（今は詰めた形で 6.2〜7.0 KB、js-to-ir.md §2.1）は、引数の配列（8 個以下）だけになる（測っていない）。

### 6.3 登録と描画の時間（推定、実機で測る）

- **登録**: 今の実機の登録は 1 本約 1.1〜1.2 ms で、行の配列の読みが大半（plan-sized-alloc.md §7）。名前の経路は、表の名前の比較（DERBY で最大 14 回の `strcmp`）、数 8 個以下の読み、解析（今と同じ）だけ。ゲスト側の `prog()` の復号も無くなる。**native の登録は解析の時間まで縮む**と見込む（解析の時間は未測定: `KASANE_MEGADEMO_TRACE` の `prep_us`）。
- **描画**: begin が写す 12n B の元が DRAM から flash（キャッシュ経由）に、`rom_matches` の比較の相手も flash になる。DERBY の 1 フレームで約 712 命令 × 12 B × 2 ≈ 17 KB の読み。表（6 KB）が data cache に載っていれば数十 µs 以下、載らなければキャッシュの行の取り直しが加わる（推定）。

## 7. 変更したファイルと大きさ

| ファイル | 内容 |
| --- | --- |
| `main/ui/kasane/ksn_procedural.h/.c` | `ksn_proc_patch`・`ksn_proc_binding`・`ksn_proc_apply_binding()`・`ksn_proc_begin_bound()`。`ksn_proc_begin_state()` は同じ内部関数の束縛なしの呼び出しになった |
| `main/ui/kasane/ksn_proc_plan.h/.c` | `ksn_proc_rom_entry`・`ksn_proc_rom_plan` と prepare/begin/run、`rom_matches()`。`run_core()` のループを `run_loop()` に括り出した |
| `main/ui/kasane/ksn_proc_plan_points.c` | 点列の登録と実行の rom 版 |
| `main/pocket/pocket_proc.h/.c` | `register(name, args, points)`、`pocket_proc_rom_plans()`、スロットの種類の bit、点列の読みの括り出し（`attach_points()`、配列の経路も同じ関数を通る） |
| `tools/kasane_ir/emit_rom_plans.mjs` | 表の生成器（新規） |
| `tools/kasane_ir/flash_plan_estimate.py` | §6 の見積もり（新規） |
| `tools/kasane_contract/test_pocket_proc_rom_qjs.c`、`run_pocket_proc_rom_qjs.py` | §8 の試験（新規）。`run.sh` に追加 |

**大きさ（ビルド確認、Xtensa `-Os`）**: `pocket_proc.c` の `.text`+`.literal`+`.rodata` が 7,702 → 8,873 B（+1,171、実際のビルドの `.obj`）、`ksn_proc_plan.c` の `.text` 1,032 → 1,734（+702）、`ksn_procedural.c` 2,472 → 2,820（+348）、`ksn_proc_plan_points.c` 320 → 472（+152）（後の 3 つは単独のコンパイル）。計 **約 +2.4 KB の flash**。IRAM には置いていない（リンカ断片に無い）。静的 DRAM は `rom_table`・`rom_count` の **+8 B**。DERBY の表を組み込めば flash がさらに **6,203 B**。`idf.py -B build_flashplan build` は通る（表は未リンク）。

## 8. 検証（host）

`python3 tools/kasane_contract/run_pocket_proc_rom_qjs.py [--mutate]`（WSL、`bash -lc`。捕獲が無ければ最初に約 3 分で作る）。実際の QuickJS と、`pocket_proc.c` を取り込んだ試験（アダプタの VM・scratch・スロットを直接読む）。ASan/UBSan、スカラーと偽 PIE の 2 通り。

1. **等価性**: DERBY の捕獲した入力（LIGHT・MID・HEAVY の台本）と摂動（check_equivalence.py と同じ `cases.json`）で、plan ごと・引数の組ごとに、**出荷しているアプリの詰めた文字列を出荷している復号器で行にして配列で登録したもの**と、**名前で登録したもの**を同じ入力で描き、JS の結果・VM の status・step・pc・レジスタ・ペン・scratch と candidate の全線分・ラスタ step を比べた。**40 登録・12,971 ベクトル（描けた 11,924、同じ形で失敗 1,047）で全件一致**。登録ごとに、名前の plan の確保がちょうど `ksn_proc_rom_plan_bytes(引数)` の 1 回であること、両者の融合の印が同じことも確認。runner の引数は捕獲に無い（`load()` が馬ごとに作る）ので、毛色と勝負服の色を与えた。
2. **融合の経路**: 等価性の全描画で参照経路への fallback が 0 回（`--wrap=ksn_proc_run` で数えた）。同じ項目を別の引数で登録した plan で走らせた VM、1 バイト違う行から begin した VM は参照経路に落ち、結果は参照と同じ。NULL の束縛は INVALID。
3. **引数の表**: stands・crowd・runner の各引数の位置に 16 通りの値（0、−0、1.5、−1、255、256、65535、65536、±1e40 など）を入れ、**配列の経路と名前の経路の結果（成否と code）が 240 通りすべてで同じ**、両方が描けた 178 通りは出力も同じ。
4. **誤り**: 未知の名前 7 通り、引数の数（多い・少ない・無い・配列でない）、数でない引数（使わない位置も）、4 個目の引数、getter からの再入（BUSY を経て INVALID）と throw する getter（plan は残らない）、点列の不正、点列だけの確保失敗（plan は解放）、表が無いとき、解除済みの handle（CLOSED）。
5. **壊れた表**: 0 命令、65 命令、引数 9 個、命令なし、patch なし、patch の pc・引数・欄の範囲外、pc 順でない patch の 9 項目が、どれも**確保の前に**拒まれる（`calloc` 0 回）。正しい項目の `REPEAT $1` に 0 と 1.5 は拒否、3 は描く。
6. **32 本**: 名前と配列を交互に 32 本、33 本目は `args` の getter を一度も呼ばずに LIMIT_EXCEEDED、確保失敗の下でも LIMIT_EXCEEDED、1 本空ければ OUT_OF_MEMORY。両方の形を 32 スロットに 600 回登録・解除して描く churn。最後に LeakSanitizer が解放漏れと二重解放を見る。
7. **変異**（`--mutate`、それぞれ単独で外して ASan で止まることを確認）: 引数の数の検査 → heap-buffer-overflow、表の項目の命令数の検査 → スタックの溢れ（ASan の unknown-crash）、plan の確保を 4 B 少なく → heap-buffer-overflow。
8. **既存の経路は不変**: `run.sh`（`GAMES_M32=0`）が全部通る（EXIT 0。WSL の git が worktree を読めないので、MEGADEMO の baseline を `.cache/kasane_megademo_app/proc_megademo_baseline.js` に置いた。MEGADEMO・LCD CATCH・BIG WAVE・DERBY の台本と画素ハッシュ、`run_pocket_proc_qjs.py` の 48 フレーム、`run_pocket_proc_limits_qjs.py` の既存の契約）。さらに、DERBY のコンパイル後の 13 本を `tools/kasane_ir/run_ir.c` で元の VM（806b10b）と新しい VM で走らせ、12,971 ベクトルの出力がバイト単位で同一。

**既存の失敗（この変更と無関係）**: `tools/kasane_ir/check_equivalence.py` は、q27（806b10b）で crowd が市松に描く 64 命令になって以来、凍結した手書き IR（40 命令）との比較で crowd だけ不一致になり `EQUIVALENCE FAIL` で止まる（元の VM でも同じ結果）。そのため、その後ろの JS の関数との比較（`check_js.mjs`）も走っていない。手書き IR 側の crowd を q27 の形にするか、crowd を比較から外すかは別作業。

## 9. 実機で測る手順（q34 の後の別作業への提案）

1. §2 の組み込み（`@planDecoder rom`、CMake、`app_session.c` の 1 行、host の DERBY の oracle）と DERBY の移行（`load()` が `H.register(T[n], args)`、引数は宣言の数に切る: stands 4、crowd 8、runner 3、他 0）を入れる。host で `run_derby.py` と `--m32` の全画素ハッシュが今と全桁一致することを先に見る。
2. 診断 image（`-DKASANE_MEGADEMO_TRACE=ON -DKASANE_BGCOST_TRACE=ON`）で `bgcost_device.py run --seconds 200`: `PLANSZ rom=` の `took`（1 本 44〜76 B の見込み）、25 本の合計、レースの空きの最小・ターン内の最小 `mn`・最大の連続 `lg` を、同じ日に焼いた今の image と比べる。
3. heapprobe image の `derby` 変種で評価の余裕と評価後の `used`（ゲスト側、§6.2 の実機での値）。
4. 時間: `reg_us`・`prep_us`（登録）、`draw_us` と帯・JS の中央値（描画。flash から写す分）、レースと写真判定の fps。命令キャッシュで同じカーネルが 15% 動くので、描画時間の小さな差は同一バイナリでの比較が要る（例えば同じ image で配列と名前を切り替える診断の分岐）。
5. 段階 3（首振り）を載せたビルドで、js-to-ir.md の受け入れ条件（ターン内の最小 ≥ 11,208 B）を満たすか。

工数（推定）: §2 の組み込みと host の DERBY の oracle 0.5〜1 日、DERBY の移行 0.5 日、実機測定 0.5 日。

## 確信の低い点

- native の削減は、host で測った生きている plan の集合に実機の大きさの式と TLSF の丸めを当てた**推定**。実機の空き・`mn`・`lg` では測っていない。
- ゲストの削減は host m32 の評価だけ。表の伸長の閾値による揺れ（±1.5 KB 程度）があり、id 形と `T` なしの差はこれを含む。実機の値は 1.3 倍という 1 点の比からの換算。
- 描画時間への影響（flash からの読み）は見積もりだけ。data cache の容量と DERBY の他のデータとの競合は見ていない。
- 等価性は捕獲した入力とその摂動、引数の表で見た。すべての入力での同値の証明ではない（どちらの経路も同じ行を同じ VM に渡す構造であることが根拠）。
- 名前の経路は firmware に表がまだリンクされていないので、実機では一度も走っていない。
