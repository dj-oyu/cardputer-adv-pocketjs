# Kasane 移植でゲストが増えた分の生存量と内訳

2026-09-17、実機（COM3）で測った。起点は Kasane 版に移したアプリで `MEM js=` が +14KiB 増えたこと（imucal 108,932→122,784、companion 102,972→117,916、pet 115,412→129,868）。**増えた分は未回収のゴミか、生きているデータか**、生きているなら**どの欄が増えたか**を、1回の計測ビルドで決める。

数値はすべて**実測(device)**。例外は「算出」と書いたもの。計測パッチは [kasane-guest-memory.patch](kasane-guest-memory.patch)（リポジトリのルートで `git apply`。コミットはしない前提の計測専用コード）。

## 結論

1. **ゴミは 0 B。** 4ビルド×3アプリ×3時点×2回＝72 組すべてで、`JS_RunGC` の前後の `malloc_size` が 1 バイトも違わない。QuickJS は参照カウントで非循環のゴミをその場で解放するので、GC が拾うのは循環だけで、その循環が無い。`js=` の読み値は**生存量そのもの**で、「GC 閾値までゴミが溜まっているだけ」という仮説は外れ。
2. **生存差（旧→Kasane、300 フレーム後、`malloc_size`）は imucal +13,870 B / companion +15,162 B / pet +15,376 B。** 起動直後の `MEM js=` の差（+13.5 / +14.6 / +14.1 KiB）とほぼ同じで、実行中に増えも減りもしない。
3. 内訳の上位は **アトム +2.7〜3.2 KiB**、**オブジェクト＋shape＋プロパティ +3.8〜4.4 KiB**、**関数（ソース複写 +2.1〜3.1 KiB を含む。bytecode と pc2line を除く）+3.2〜4.4 KiB**、**アロケータのヘッダ +2.3〜2.7 KiB**（確保が約 210 回増えた分）、bytecode +0.7〜1.3 KiB。
4. **関数ソースの保持をやめると Kasane 版で 4.8〜7.1 KiB、旧版で 2.5〜4.6 KiB 減る。** 壊れるのは `Function.prototype.toString` だけ（vmtest o2 コーパスで 75 件中 `special_calls` の 1 件、差分は toString の出力行のみ）。単一の手段としては最大だが、Kasane 差分（13.5〜15.0 KiB）の半分に届かない。
5. **ネイティブ arena はゲスト外に 9,908 B**（`nativeBytes`、実測 free の減少 9,976 B）。確保前後で最大連続ブロックは、すでに 32 KiB 近くまで割れている時は −1,024 B、まだ大きな塊がある時は **−4〜10 KiB** 削られる。

## 方法

### ビルド

| 名前 | ソース | 関数ソース保持 | ビルドディレクトリ |
| --- | --- | --- | --- |
| old_on | `kasane/app-ports`（44b9910）の `apps/` だけを `vm/main`（445f348）の版に差し替え | 有 | `build_mp_old_on` |
| old_off | 同上 | 無 | `build_mp_old_off` |
| kas_on | `kasane/app-ports`（44b9910） | 有 | `build_mp_kas_on` |
| kas_off | 同上 | 無 | `build_mp_kas_off` |

旧 UI 版は worktree を別に切らず、**同じツリーで `apps/` だけを差し替えた**。`git diff vm/main kasane/app-ports` は `apps/` と `tools/` 以外に差が無いので、これは「vm/main に同じ計測パッチを当てたビルド」と同一のファームになる（VM 基盤・ネイティブ面・計測コードはすべて共通）。4つとも `-D SDKCONFIG=<dir>/sdkconfig` で sdkconfig を分離し、off は `sdkconfig.memprobe-nosrc.defaults`（`CONFIG_POCKET_MEMPROBE_NO_SOURCE=y`）を重ねた。

### 計測点

- `MEMPROBE <phase> <pre|post>`: `JSMemoryUsage` の全欄と、システムの free / largest。`pre` を出してから `JS_RunGC` を1回走らせて `post` を出す。GC は計測のためだけ。
  - `first`: 最初のフレームを present した次のターンの冒頭（Kasane は `pocket_kasane_end_turn()` と present の後、旧 UI は初回 present の後。ゲストのコードはスタックに無い）
  - `f300`: 300 フレーム目を越えた次のターンの冒頭
  - `stop`: `app_stop()` の冒頭（各面の reset より前）
- `MEMPROBE arena before|after`: `ensure_state()` の calloc と `ksn_runtime_app_attach()` の前後で free / largest、後に `nativeBytes`（`stats.nativeBytes` と同じ式）。
- 関数ソース保持の切替: `quickjs.c` で関数定義（通常・アロー・クラス）が `fd->source` に `js_strndup` する3箇所を、`CONFIG_POCKET_MEMPROBE_NO_SOURCE` で飛ばす（`source=NULL`、`source_len=0`）。

操作はメニューの起動と終了だけ（アプリの中でキーは押さない）。各ビルドで imucal → companion → pet を2巡。`first`/`f300`/`stop` はすべて2回そろった。起動失敗・リセットは 0 回。

## 結果

### GC の前後（`malloc_size`、2回の平均、B）

pre と post はすべての行で等しいので1列にまとめる。`spread` は2回の差の最大。

| ビルド | アプリ | 起動時 `MEM js=` | first | f300 | stop | spread |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| old_on | imucal | 108,948 | 113,378 | 116,058 | 116,068 | ≤32 |
| old_on | companion | 103,000 | 103,080 | 103,080 | 103,080 | 0 |
| old_on | pet | 115,440 | 115,520 | 115,520 | 115,520 | 0 |
| kas_on | imucal | 122,796 | 122,796 | 129,928 | 129,938 | ≤60 |
| kas_on | companion | 117,914 | 117,914 | 118,242 | 118,242 | ≤68 |
| kas_on | pet | 129,886 | 129,886 | 130,896 | 130,896 | ≤20 |
| old_off | imucal | 104,218 | 108,606 | 111,308 | 111,312 | ≤40 |
| old_off | companion | 100,432 | 100,512 | 100,512 | 100,512 | 0 |
| old_off | pet | 111,324 | 111,404 | 111,404 | 111,404 | 0 |
| kas_off | imucal | 115,678 | 115,678 | 122,810 | 122,814 | ≤36 |
| kas_off | companion | 112,984 | 112,984 | 113,312 | 113,312 | ≤16 |
| kas_off | pet | 122,574 | 122,574 | 123,594 | 123,594 | ≤60 |

**ゴミの読み値差は 0 B**（旧・Kasane とも pre−post が 0）。imucal は起動から f300 までに旧版で 6.9 KiB、Kasane 版で 7.0 KiB 伸びるが、GC で減らないので生存データの増加（Kasane 版では first→f300 でアトム +46、C 関数 +12、配列 +13 が実体化する）。

### 生存差の欄別内訳（kas_on − old_on、f300、GC 後、B）

`memory_used_size` は QuickJS が各欄を足した値、`malloc_size` との差は確保ごとのヘッダと丸め。

| 欄 | imucal | companion | pet |
| --- | ---: | ---: | ---: |
| **malloc_size** | **+13,870** | **+15,162** | **+15,376** |
| memory_used_size | +11,123 | +12,650 | +13,061 |
| アロケータのヘッダ（差） | +2,747 | +2,512 | +2,315 |
| （malloc_count） | +212 | +212 | +205 |
| atom_size（atom_count） | +2,746（+71） | +3,225（+86） | +3,126（+84） |
| obj_size（obj_count） | +1,968（+41） | +2,064（+43） | +2,112（+44） |
| shape_size（shape_count） | +1,072（+9） | +1,496（+14） | +1,200（+10） |
| prop_size | +864 | +976 | +984 |
| js_func_size（js_func_count） | +3,289（+7） | +3,377（+6） | +4,532（+6） |
| 　うちソース複写（on−off 差の差） | +2,178 | +2,175 | +3,202 |
| 　うちソース以外（vardefs・cpool・closure_var・構造体） | +1,111 | +1,202 | +1,330 |
| js_func_code_size | +742 | +1,297 | +1,102 |
| js_func_pc2line_size | +82 | +94 | +161 |
| str_size | 0 | −167 | −180 |
| c_func_count | +6 | +12 | +12 |
| array_count（fast_array_elements） | +11（+45） | +1（+4） | +1（+3） |
| 残り（配列要素・var_ref など欄に出ない分） | +360 | +288 | +24 |

主因の順は3アプリで同じ: **名前空間とシーン（アトム＋オブジェクト＋shape＋プロパティ）で 6.5〜7.6 KiB、関数（bytecode と pc2line を含む）で 4.0〜5.7 KiB（うちソース 2.1〜3.1 KiB）、確保回数の増加で 2.3〜2.7 KiB**。

### 関数ソース保持の総量（on − off、f300、B）

| アプリ | 旧 `malloc_size` | 旧 js_func_size | Kasane `malloc_size` | Kasane js_func_size | Kasane の free 増 |
| --- | ---: | ---: | ---: | ---: | ---: |
| imucal | 4,750 | 4,458 | 7,118 | 6,636 | 7,028 |
| companion | 2,568 | 2,458 | 4,930 | 4,633 | 4,890 |
| pet | 4,116 | 3,866 | 7,302 | 7,068 | 7,250 |

保持をやめると、Kasane 版 3アプリで **4.8〜7.1 KiB** ゲストが減り、同じだけシステムの free が戻る。旧 UI 版では 2.5〜4.6 KiB。ソース以外の欄（アトム・オブジェクト・bytecode）は on/off で 1 B も動かないので、切替が消したのは複写だけ。

互換性（実測(host)）: `tools/vmtest` の o2 コーパスを `VMTEST_CFLAGS=-DCONFIG_POCKET_MEMPROBE_NO_SOURCE=1` で走らせ、**75 件中 74 件合格、`special_calls` 1 件が不一致**。差分は `toString function named(a) { return a; }` が `function named() {\n    [native code]\n}` になる1行だけ（`js_function_toString` のフォールバック）。保持ありの o2 は 75 件合格。

### ネイティブ arena（`ensure_state()` の前後）

`nativeBytes=9,908`、`state=1,068`（全実行で同じ）。free の減少は全実行で 9,976 B（calloc 6回分のヘッダを含む）。どれもゲストの評価中、トップレベルの最初の Kasane 呼び出しで確保される（imucal は `kasane.replace`、companion と pet は `kasane.petImage`）。

| ビルド | アプリ | largest 前 | largest 後 | 差 |
| --- | --- | ---: | ---: | ---: |
| kas_on | imucal | 32,768 | 31,744 | −1,024 |
| kas_on | companion | 51,200 | 47,104 / 45,056 | −4,096 / −6,144 |
| kas_on | pet | 32,768 | 31,744 | −1,024 |
| kas_off | imucal | 47,104 | 36,864 | **−10,240** |
| kas_off | companion | 61,440 / 55,296 | 53,248 / 51,200 | −8,192 / −4,096 |
| kas_off | pet | 45,056 | 45,056 / 43,008 | 0 / −2,048 |

大きな塊が残っているときは、arena の 6 回の calloc が最大ブロックを 4〜10 KiB 削る。すでに 32 KiB 付近まで割れているときは −1 KiB で済むが、それはゲストが先に割っているから。

システム側の結果（f300、GC 後、kas_on − old_on）: free imucal −5,630 / companion −15,964 / pet −17,080 B、largest imucal 31,744→31,744 / companion 49,152→32,768 / pet 40,960→31,744 B。imucal の free 差がゲスト差より小さい理由は未確認。旧 UI 版だけが持つ rgb565 renderer/target を Kasane 版が初回 present で捨てる（`present_frame()`）ことが一因と推定するが、companion と pet で同じ効果が見えない理由は説明できていない。

## 仮説との照合

| 仮説 | 判定 | 実測 |
| --- | --- | --- |
| `js=` は未回収ゴミを含み、生存差は GC 後に小さくなる | **外れ** | GC 前後の差は 72 組すべて 0 B。参照カウントが非循環のゴミを即時解放しており、循環ゴミも無い |
| 生存差は 7.5〜10 KiB（推定） | **外れ（過小）** | 13.5〜15.0 KiB（`memory_used_size` でも 10.9〜12.8 KiB）。ヘッダ 2.3〜2.7 KiB と、アトムの大きさ（2.7〜3.2 KiB）を見込んでいなかった |
| create_scene.js 内側4関数のソース複写 1,626 B（算出） | 方向は当たり | Kasane で増えたソース複写は 2,175〜3,202 B。create_scene.js とアプリ側の関数の変化の合計で、ファイル別には分けていない |
| bytecode / pc2line | 当たり（小） | code +742〜1,297 B、pc2line +82〜161 B |
| 閉包と var_ref | 当たり（小） | 欄に出ない残りは +24〜360 B |
| 名前空間の proto / shape / prop | 当たり | obj＋shape＋prop で +3,904〜4,536 B |
| アトム | 当たり（最大級） | +2,746〜3,225 B（+71〜86 個） |
| AUTOINIT で実体化した C 関数 | 当たり（小） | c_func +6〜12 個 |
| ゲスト外 8,192 B＋runtime＋state 1,072 B、6回の calloc | 当たり | nativeBytes 9,908 B、state 1,068 B、free −9,976 B |
| ソース保持をやめれば Kasane 差分より大きく減る | **外れ** | 4.8〜7.1 KiB。Kasane 差分（13.5〜15.0 KiB）の半分に届かない。ただし単一の手段としては最大 |

## 次に効く改善（効く順）

1. **関数ソースを保持しない**（全アプリで 2.5〜7.1 KiB、実測）。失うのは `Function.prototype.toString` の本文だけ。計測用の切替をそのまま出荷の Kconfig にできる。
2. **Kasane の名前空間とシーンが作るアトム・オブジェクト・shape を減らす**（合計 6.5〜7.6 KiB）。プロパティ名の数（アトム +71〜86 個）と名前空間オブジェクトの数が効いている。遅延構築の粒度を細かくするか、名前を束ねる。見込みは推定。
3. **確保回数を減らす**（ヘッダ 2.3〜2.7 KiB、確保 +205〜212 回）。小さなオブジェクトを配列や1つのレコードにまとめる。見込みは推定。
4. **arena の 6 回の calloc を 1 回にし、ゲストの評価より前に取る**。量は変わらない（9.9 KiB）が、大きな塊を 4〜10 KiB 削る分断を避けられる。
5. GC を早める・閾値を変える: **効果なし**（ゴミが 0 B）。

## 再現

```powershell
git apply docs/kasane/kasane-guest-memory.patch
idf.py -B build_mp_kas_on -D SDKCONFIG=build_mp_kas_on/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults" build
idf.py -B build_mp_kas_off -D SDKCONFIG=build_mp_kas_off/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.memprobe-nosrc.defaults" build
# 旧版: git checkout vm/main -- apps のあと build_mp_old_on / build_mp_old_off を同じ形で。終わったら git checkout HEAD -- apps
python tools/kasane_memprobe_run.py --port COM3 --label kas_on --runs 2 --out memprobe.log
python tools/kasane_memprobe_parse.py memprobe.log
```
