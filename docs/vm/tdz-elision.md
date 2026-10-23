# TDZ 検査をコンパイル時に消す（interpreter-stages §4.3）

2026-10-23、ブランチ `worktree-agent-a07f1203eefcaae28`（`main` e0fe98a から）。**host だけで測った。実機（COM3）では測っていない。** 数値の種類は「計数（host）」「実測（host）」「コードから読んだ値」「推定」を書き分ける。

## 結論

- ブロックの `let`/`const` の読み書きと、ブロックに入るたびの TDZ への戻しは、DERBY の 1 フレームで 2,323 命令（18.8%、計数（host）、楕円 WIDE 2 MID w=−100、449 フレームの平均）。
- コンパイル時の前向きのデータフロー解析（`tdz_elide_checks`、`quickjs.c`）で、この 2,323 命令が 11 命令に減る。フレームの命令数は 12,375 から 11,506 に減る（−7.0%、計数（host））。
- 確保は増えない。各ラベルの状態は `LabelSlot.addr` に置くからだ。この欄は `resolve_labels` が書き込むまで −1 で、−1 は「全部初期化済み」という楽観的な初期値にもなる。バイトコードは短くなる。評価のピークは 129,240 B から 126,608 B に下がる（−2,632 B、`run_derby.py --m32` の TLSF 模型、実測（host））。
- 仕様の ReferenceError は保たれる。新しいコーパス `tdz_elision.js`（node の出力と一致）、test262 の標準部分集合（退行 0）、TDZ に関わる 17 ディレクトリ（10,315 ファイル）で確かめた。17 ディレクトリの結果の集合は、変更前のエンジンと 1 行も違わない。

## 1. どこで出るか（コードから）

| 命令 | 出す場所 |
| --- | --- |
| `get_loc_check` | `resolve_scope_var`。字句変数の読みはすべてこれになる（`quickjs.c` 36638-36639、参照の最適化 36578-36579） |
| `put_loc_check` | 同じ場所。初期化でない代入（36631-36632） |
| `put_loc_check_init` | `this` の初期化だけ（36626-36627）。ほかの初期化は素の `put_loc`（36629） |
| `set_loc_uninitialized` | `resolve_variables` の `OP_enter_scope`。スコープの字句変数ごとに 1 個（38091-38096、元のコメント「XXX: should check if variable can be used before initialization」）。派生コンストラクタの `this` には `resolve_labels` の前置き部で出す（38524） |
| `get_var_ref_check` / `put_var_ref_check(_init)` | クロージャ側の同じもの（36850-36901） |

変更前は、消す処理がどこにも無かった。`resolve_labels` の覗き穴最適化（`inc_loc`・`add_loc`・`set_loc`・`get_loc8` など）は `get_loc` / `put_loc` だけを見るので、`let` の `i++` にはどれも効かなかった。`skip_dead_code` が消すのは到達しない命令だけ。

## 2. DERBY のどの書き方から来るか（計数（host））

関数ごとの TDZ 命令（1 フレーム）: `pose` 740、`step` 486、`pan` 253、`at` 252、`rng` のクロージャ 144、`pj` 144、`frame` 133。

- **関数本体の `const`/`let`**: `pj` の `const a, b, Z`、`pose` の `let i`・`const e`・`let x, z, c, d`。本体スコープも `enter_scope` を通るので、呼び出しのたびに変数ごとに `set_loc_uninitialized` が 1 回走る。
- **ループ本体の `const`**: `step` の `for` の中の `const h, x, top, gap, early` / `let v` / `let g` / `const a, up` / `const y`。8 頭 × 周回ごとに戻す。`pose` の `if (e[1]) { const r, C, S … }` も同じ。
- **`for (let i = 0, x, s; …)`**: `at()`。ループ変数を戻すのは 1 回だけだが、読みはすべて検査つき。

## 3. 規則と、正しいと言える理由

対象は関数の中だけ。トップレベルは 1 回しか走らないので外した。追う変数は局所 0〜31 で、32 番以降は従来どおり。

- **状態**: 変数ごとに「必ず初期化済み」の 1 bit。関数の入口では全部 0。
- **gen**: `get_loc_check` / `put_loc_check` / `put_loc_check_init` / `put_loc` / `set_loc`。検査は、通れば初期化済みで、通らなければ例外で経路を出る。初期化は `put_loc` で出る。JS_UNINITIALIZED をスタックに載せて書き戻す命令は無い。
- **kill**: `set_loc_uninitialized` だけ。このフレームの枠に JS_UNINITIALIZED を書くのはこの命令だけで、クロージャが書けるのは初期化済みの値だけ。
- **合流**: ラベルでは、前向きと後ろ向き（ループ）の辺を AND する。`with_*` の分岐も辺にする。catch の行き先は 0（try の中のどこからでも来る）。`gosub` の後も 0。無条件の飛び・return・throw の後は、次のラベルまで到達しない。
- **不動点**: 楽観的な初期値（全部 1）から AND で減らし、ラベルが変わらなくなるまで回す（上限 8 回。超えたら何も変えない）。最大不動点なので、入口から読みまでのどの経路にも「最後の kill より後に gen がある」。
- **書き換え**: bit が立っている検査を、同じ長さの `get_loc` / `put_loc` に置き換える。位置は動かないので、ラベルの表には触らない。
- **kill を落とす条件**: 変数が `JS_VAR_NORMAL` の字句変数である。どのクロージャにも eval にも捕捉されていない（子の関数は親より先にコンパイルされるので、`is_captured` はこの時点で確定している）。この関数の中のアクセスが、すべて証明済みの検査か書き込みだけである。証明済みの読みの直前に最後に書いたのは、kill があってもなくても同じ gen なので、印を見るものがいない。違いは、前の周回の値が解放される時点が、ブロックに入るときから次の書き込みのときに移ることだけ。

捨てた考え: 「同じブロックの直線部分だけ」の規則。`if` の後や `while` の後の読みが取れず、DERBY の `pose` の最後の `return` も `step` の `continue` の後も残る。

## 4. 結果

### 4.1 DERBY の命令（計数（host）、`tools/vmtest/opprof/run_derby_prof.py`、楕円 WIDE 2 MID w=−100、449 フレーム）

| | 変更前 | 変更後 |
| --- | ---: | ---: |
| 1 フレームの命令 | 12,375.2 | 11,505.9（−7.0%） |
| `get_loc_check` | 1,729.8 | 0 |
| `put_loc_check` | 175.2 | 0 |
| `set_loc_uninitialized` | 417.6 | 11.0（`course` 9 ほか） |
| `dup` + `drop` | 771.8 | 465.2（`inc_loc`・`set_loc` が効くようになった） |

- 消えた読みは、`get_loc8` と `get_loc0`〜`get_loc3`（1〜2 バイト）になった。
- この worktree の HEAD は `ser()` がネイティブなので、derby-wide2-profile.md の 38,246 命令とは場面の中身が違う。
- 5 構成とも、レースの結果（着順・タイム）は変更前と同じ。

### 4.2 評価のメモリ（実測（host））

| 測り方 | 変更前 | 変更後 |
| --- | ---: | ---: |
| `run_derby.py --m32`: 評価中のピーク | 129,240 | 126,608 |
| 　評価後 | 114,396 | 112,176 |
| 　GC 後の生存ピーク | 141,796 | 137,780 |
| `compile_peak.py` のチャンクごとのピーク（watch / prog / view / scene / pan / play） | 20,376 / 53,300 / 49,732 / 43,296 / 11,140 / 29,484 | 20,376 / 53,256 / 48,172 / 43,104 / 10,384 / 28,748 |

- ピークはどのチャンクでも上がらない。新しい状態は `LabelSlot.addr`（既存の欄）と C のスタックの数語だけだからだ。下がるのは、出力のバイトコードが短くなる分。
- コードサイズの増加は、x86-64 `-Os` の `tdz_walk` で 641 B（実測（host））。Xtensa では未測定（推定で同程度）。

### 4.3 検査

- `tools/vmtest/run.sh`: asan・o2・`--force-yield` で 81/81（新しい `tdz_elision` を含む）。期待値は node の出力。変更前のエンジンでも同じ出力になる。
- `tools/vmtest/tdz_elision.sh`: 31 関数で、残る検査と kill の数を固定した。
- test262: 標準部分集合は asan と o2 で 7,501 pass / 194 fail、退行 0。block-scope・let・const・switch・for・for-in・labeled・class・with・block・while・do-while・break・continue・assignment・destructuring は、変更前後で 20,012 pass / 74 fail、結果の集合は同一。
- `run_derby.py --m32`: 全段 `DERBY_HOST PASS`。

## 5. 残るリスクと未測定

- **実機で測っていない。** 命令数は 7% 減るが、ターンの時間は命令キャッシュに支配される（turn-cpi.md）。短い命令形と `inc_loc` が効く分、命令の作業集合は小さくなるはずだが、ms は推定もしていない。
- 値の解放の時点が変わる。kill を落とした変数では、前の周回の値が次の書き込みまで生き残る。GC から見えるのはこの点だけで、FinalizationRegistry の時点が動きうる。
- 追うのは局所 0〜31 だけ。大きな関数の 32 番以降は従来のまま（正しさには影響しない）。
- catch の行き先と `gosub` の後を「何も知らない」としている。保守的で正しいが、try の後の読みは残る。DERBY の `frame` の try は、catch が投げ直すので影響しない。
- 解析の前提は二つある。「素の `get_loc` で字句変数の未初期化値をスタックに載せて、別の字句変数へ書く命令列を、コンパイラが出さない」と、「`set_loc_uninitialized` 以外に枠へ JS_UNINITIALIZED を書く命令が無い」。どちらもコードから読んだ（`get_loc_or_ref` の私的名は読むだけ）。上流を取り込むときは、この 2 点を見直す。
