# Kasane のゲスト生存量と arena の分断を減らす

2026-09-17、実機（COM3）で測った。`kasane-guest-memory.md`（`kasane/mem-probe` の `0fedbed`。`git show 0fedbed:docs/kasane/kasane-guest-memory.md`）が挙げた改善のうち、関数ソースの保持（別作業）を除く 3 つを `kasane/remove-taffy`（`1668f7c`）の上に実装した。

数値は断りのない限り**実測(device)**。計測は `0fedbed` の `kasane-guest-memory.patch` を当てたビルド（コミットしない）で、imucal → companion → pet を 2 巡し、300 フレーム後に `JS_RunGC` した後の `malloc_size` を採った。GC の前後はすべての行で等しい（ゴミは 0 B のまま）。基準は `kasane/remove-taffy` に同じパッチを当てたもので、Taffy 削除後なので `0fedbed` の表とは絶対値が違う。

## 結論

| 案 | コミット | imucal | companion | pet | 確保回数 | 最大連続ブロック（f300） |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| 2a シーンコントローラを C へ | `dbbf655` | −5,086 | −5,082 | −5,060 | −55 | +4〜8 KiB（ゲストが小さくなった分） |
| 2b proto の遅延・共有、クラス名 1 つ | `dbbf655` | −1,170 | −1,092 | −1,208 | −23 | 変化なし |
| 3 シーン object が状態を持つ | `8b15a39` | −34 | −196 | −76 | −2〜−3 | 変化なし |
| 4 arena を 1 確保・評価前 | `530caee` | ±0 | ±0 | ±0 | ±0 | imucal +4,096 / companion +4,096 / pet −4,096 |
| **計** | | **−6,316** | **−6,366** | **−6,352** | **−80〜−81** | |

- **ゲストは 3 アプリとも 6.2 KiB 減った。** ほとんどが案 2a（5.0 KiB）。`kasane-guest-memory.md` が Kasane 移植の増分とした 13.5〜15.0 KiB の 4 割強にあたる。
- **案 3 は小さい。** Kasane 側でネイティブに束ねられる確保はほとんど残っていなかった（後述）。
- **案 4 はゲストを変えず、arena を分断の前に取るようにした。** arena は評価前の 131,072 B のブロックから切り出される（前は評価の途中、78〜98 KiB まで割れたブロックから 6 回に分けて）。300 フレーム後の最大連続ブロックは 2 アプリで +4 KiB、pet で −4 KiB。2 回ずつの計測で、改善と言えるのは「評価中に arena がブロックを割らなくなった」ことまで。
- **JS から見える API は変えていない。** 変わったのは `Object.getPrototypeOf` で見える proto の同一性（ticket・template・image は `Object.prototype` を直接継承）と、デバッグ出力のクラス名だけ。`createScene` の戻り値は同じ形（own の `invalidate`/`flush`、`length` 1、外して呼べる）。

## 表: アプリ別（f300、GC 後、B、2 回の平均）

| 欄 | imucal 基準 | imucal 最終 | companion 基準 | companion 最終 | pet 基準 | pet 最終 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| malloc_size | 123,128 | 116,812 | 111,038 | 104,672 | 123,848 | 117,496 |
| malloc_count | 1,753 | 1,673 | 1,552 | 1,471 | 1,723 | 1,642 |
| atom_count | 830 | 798 | 776 | 743 | 839 | 806 |
| atom_size | 37,567 | 36,231 | 35,819 | 34,449 | 37,905 | 36,535 |
| obj_count | 311 | 301 | 276 | 266 | 310 | 300 |
| shape_count | 132 | 130 | 127 | 125 | 128 | 126 |
| js_func_count | 20 | 16 | 13 | 9 | 16 | 12 |
| js_func_size | 10,023 | 7,697 | 7,089 | 4,763 | 10,207 | 7,881 |
| js_func_code_size | 2,743 | 2,280 | 2,532 | 2,069 | 3,174 | 2,711 |
| c_func_count | 155 | 152 | 159 | 156 | 171 | 168 |
| system free | 120,970 | 126,672 | 132,290 | 138,048 | 120,082 | 125,828 |
| system largest | 69,632 | 81,920 | 83,968 | 94,208 | 73,728 | 73,728 |

「最終」は案 4 まで入れた計測ビルド。案ごとの途中値は各コミットのメッセージにある。

### arena（`ensure_state` / `pocket_kasane_prepare` の前後）

| ビルド | 確保の時点 | nativeBytes | free の減少 | largest 前 → 後 |
| --- | --- | ---: | ---: | --- |
| 基準 | 評価中、最初の Kasane 呼出し（6 回の calloc） | 9,908 | 9,976 | imucal 81,920 → 73,728、companion 98,304/86,016 → 94,208/86,016、pet 81,920/77,824 → 同じ |
| 案 4 | 評価の直前（1 回の calloc） | 9,916 | 10,244 | 3 アプリとも 131,072 → 122,880 |

nativeBytes の +8 B は tail を 8 バイト境界に揃えた分。free の減少は 268 B 増えた（1 ブロックにした分のアロケータ側の丸め。内訳は未確認）。

## 案ごとの方式

### 案 2a: シーンコントローラを C へ

`createScene` はこれまで呼ばれるたびに `apps/kasane/create_scene.js` を `JS_Eval` し、4 つの閉包（`replace`・`update`・`invalidate`・`flush`）と、その bytecode・ソース複写・var_ref・局所名のアトムがアプリの寿命のあいだ残っていた。これを `pocket_kasane.c` の `scene_flush_turn` に文ごとに写した。

- 意味論は JS 版のまま: pending 中は `SUBMITTED` なら false、`PRESENTED` で候補 refs を昇格、`DISCARDED` なら dirty にして REPLACE だったなら rebuild、それ以外は `Error('scene lost its submission')`。例外は candidate を捨てて dirty（REPLACE なら rebuild）を立て、`code === 'BUSY'` だけを飲み込む。`!error` の偽値は投げ直す。再入は `Error('scene flush is not reentrant')`。build が null・関数・プリミティブを返せば TypeError。thenable の拒否は `run_build` が同じ経路で行う。
- JS 版と違う点: `view.poll`/`view.replace` をプロパティとして引かずに直接呼ぶ（`pocket.kasane.replace` を差し替えても controller は使わない）、scene 経路では ticket を作らない、中断不能な例外（watchdog）はそのまま通す（JS でも catch/finally は走らない）。
- `create_scene.js` は埋め込みをやめ、node テスト（`test_pet.cjs`・`test_companion.cjs`）が使う参照モデルとして残した。`tools/test_kasane_hello.c` に JS 版の文を固定するテストを足し、**旧 JS 版と新 C 版の両方で通ること**を確かめた（実測(host)）。
- 副産物: 同じ割込み故障スイープで、JS 版は 14 回中 1 回（index 13）が「戻り値は例外でないのに例外が残り、提出も残る」状態になった。C 版は 39〜42 回すべて通る。

### 案 2b: proto の遅延・共有、クラス名 1 つ

- instance と animation の proto は最初のラッパ生成時に作る（使わないアプリは proto・shape・`place`/`stop`/`finish` のアトムを持たない）。
- メソッドの無い ticket・template・image は空 object を proto に持たず `Object.prototype` を直接継承する。
- 9 クラスのクラス名を `"Kasane"` 1 つにした。QuickJS はクラス名をアトムとして持つが、使うのはデバッグ出力と `JS_GetOpaque2` の型エラーだけ（このファイルは使わない）。
- 見つけた不具合: `JS_InstantiateFunctionListItem` は autoinit プロパティの追加失敗を返り値に出さず、例外だけを残す。遅延 proto の故障スイープ（index 59）で表に出たので、`set_proto` は `JS_HasException` も見る。

### 案 3: 確保をまとめる

`createScene` の戻り値そのものを scene クラスの object にして状態を持たせ、別の hidden holder（object・プロパティ配列・shape）を無くした。束ねられたのはこれだけだった。

ホストで companion の生存確保を呼出し元ごとに数えた（実測(host)、64 bit、スクラッチの計測でコミットしない）。Kasane のネイティブ側に残る確保は、メソッド名のアトム（proto と名前空間で約 30）、DrawRef ラッパ 1 個につき object とプロパティ配列の 2 回（companion で 8 個）、実体化した C 関数 1 個につき 2 回、petImage のメタデータ（アトムとプロパティ）で、どれも QuickJS が 1 回ずつ確保するもの。まとめるには VM 側の変更（object とプロパティの同居など）が要り、この作業の範囲外とした。残りの確保の大半はアプリ自身のコード（関数・文字列・object）。

### 案 4: arena を 1 確保にし、評価前に取る

- `ksn_runtime.c` は制御状態・コマンド帯 2 本・テキスト帯 2 本を 1 つの `runtime_block` にし、`ksn_runtime_app_attach_tail()` で APP 側の `kasane_state` を同じブロックの後ろに置く。予算は `KSN_RUNTIME_BASE_BUDGET`（`KSN_CORE_STORAGE_BYTES` + 1 KiB）と tail 3 KiB。従来の「各ブロック 3 KiB 以内」はこの 2 つの予算に置き換わった。
- SYSTEM が先に runtime を持っているときは tail だけ別確保にする。その確保の失敗は SYSTEM に触らない。tail は detach で runtime が解放する（`pocket_kasane_reset` はもう free しない）。
- `app_session.c` は、overlay でなく、ソースかプレリュードに `kasane` の文字列を含むときだけ、評価の直前に `pocket_kasane_prepare()` を呼ぶ。失敗は黙って見送り、最初の Kasane 呼出しが従来どおり確保して OUT_OF_MEMORY を報告する。Kasane を使わないセッションは 0 B のまま。

## 検証

- Kasane host スイート（実測(host)、ASan/UBSan）: `tools/kasane_contract/run.sh`、`build_kasane_test.sh` の `test_pocket_kasane`・`test_kasane_hello`・`test_kasane_imucal`、`build_lessons_test.sh`（`test_lessons`）、node の `test_pet.cjs`・`test_companion.cjs`。各段で全件 PASS。差分描画と帯ごとの修復・原子性・故障スイープは `test_pocket_kasane` が画素比較で見ている。
- 実機: 各段で `smoke_device.py --cycles 20`（SMOKE_OK 20）と `memlog.py --check`（MEMLOG_OK、DIRAM +0）。
- 画素: 基準ビルドと出荷ビルドで 3 アプリの起動 1.5 秒後を `board_capture` で取って比べた。companion と pet は 32,400 画素すべて一致。imucal は 184 画素が違い、すべて行 53〜89・列 38〜168（IMU の生値を出す 3 行のテキスト）。`capture_home.py` はホームの背景設定を書き換えるので使っていない。
- 出荷状態（計測パッチなし）を `build_mr_ship` でビルドして焼き、`esptool verify-flash 0x10000` が一致。
