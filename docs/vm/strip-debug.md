# ゲストの行番号表を落とす（`CONFIG_POCKET_VM_STRIP_DEBUG`、2026-09-30）

ブランチ `vm/strip-debug`（`vm/main` b692f0c から）。DERBY WATCH の首振りカメラ（段階 3、ブランチ `vm/pan-camera` の `docs/apps/derby-pan-memory.md`）が DRAM に収まらなかったので、「最後の手」として、ゲストの JS を行番号表（pc2line）なしでコンパイルする選択肢を入れて測った。**数値は種別を明記する**（実機の実測／host m32 の実測／推定）。

## 結論

- **Kconfig `CONFIG_POCKET_VM_STRIP_DEBUG`（既定 OFF）**。ON で、パーサが関数ごとの行番号表を作らない（`quickjs.c` の `compute_pc2line_info` を飛ばし、`JSFunctionBytecode` に表のブロックを持たない）。アプリの評価（`app_session.c`）もチャンク（`pocket_app_load.c`）も import も同じパーサを通るので、1 か所で全部に効く。ローカル変数名は残す（クロージャと TDZ のエラーの名前に使う）。
- **効果**: DERBY WATCH の評価の余裕が **29,969 → 32,709 B（+2.7 KB、実機）**、評価後のゲストが 116,408 → 113,256 B（−3.2 KB、実機の `heapprobe`）。host（m32、コンパイル済みの常駐）では DERBY −2.8 KB、BIG WAVE −2.1、MEGADEMO −1.9、stress −0.8、LCD CATCH −0.7 KB（表）。先の見積もり（DERBY −3.7〜−4.8 KB）は多すぎた（見積もりはデモのチャンクと `STRIP_SOURCE` のファイル名を含み、実機への 1.3 倍の換算もしていた）。
- **段階 3（首振り）は、これを足しても受け入れ条件に届かない**（実機）: 評価の余裕 15,242 → 19,009 B（条件 20 KB に −1.5 KB）、ゲストの最大 139,056 B（条件 140 KB を満たす）、**ターン内の空きの最小 5,016 B（条件 11,208 B に −6.2 KB）、毎レース `GO LOADSTALL`**。plan の場面入れ替えは、境目ではない（−6 KB）ので試していない（入れ替えで空くのは plan 3〜4 本 = 約 3 KB が上限、推定）。
- **失うもの**: 実行時エラーのスタックの位置が「関数の先頭の行」になる（`at f (rt.js:2:1)`、トップレベルは `<eval> (file:1:1)`）。**構文エラーの位置は正確なまま**（トークナイザが出すので表に依らない）。ファイル名は残る。
- **回帰（実機、ON の通常 image）**: `smoke_device.py --cycles 20` `SMOKE_OK 20`、`stress_app.py` `STRESS_APP_PASS`、`test_app_resume.py` `TEST_APP_RESUME_OK`、APPS の JS アプリ 10 行がすべて起動（`EVAL_ERROR`・`START_FAILED` なし）。

## 実装

| 場所 | 変更 |
| --- | --- |
| `components/quickjs-ng/quickjs-ng/quickjs.c` | `compute_pc2line_info()` の頭で `#ifdef CONFIG_POCKET_VM_STRIP_DEBUG return;`。`js_create_function()` で `b->pc2line_buf = NULL; b->pc2line_len = 0`（長さ 0 の `js_realloc` もブロックを 1 つ取るので呼ばない）。`find_line_num()` は表が無いと関数の先頭行を返す（既存の分岐）。 |
| `main/Kconfig.projbuild` | `POCKET_VM_STRIP_DEBUG`（既定 n）。設定は `quickjs-vmstack.h` が取り込む `sdkconfig.h` から届く |
| `sdkconfig.stripdebug.defaults` | ON の上書き。共有の `sdkconfig` を汚さないよう、`-D SDKCONFIG=<dir>/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.stripdebug.defaults"` で別のディレクトリに作る（`sdkconfig.vmprobe.defaults` と同じ作法） |

**既存の `CONFIG_POCKET_VM_STRIP_FN_SOURCE` との関係**: `quickjs.c` の `js_function_toString` の注記が名前を出しているが、**その Kconfig は存在しない**（`main/Kconfig.projbuild` に無い）。関数のソースを持たない変更は無条件で入っている（`fd->source` を設定する所が無い）。今回の選択肢はそれとは独立で、ソースは元から無く、行番号表だけを落とす。

## 測定

### host（m32、`tools/vmtest/strip_cost.py`。コンパイル済みの常駐、TLSF の課金）

| アプリ | ソース B | 常駐 B | ON B | 差 B |
| --- | ---: | ---: | ---: | ---: |
| DERBY WATCH（6 チャンク） | 32,256 | 55,212 | 52,384 | −2,828 |
| DERBY WATCH（段階 3 の版） | 41,343 | 65,992 | 62,464 | −3,528 |
| BIG WAVE | 22,372 | 45,652 | 43,520 | −2,132 |
| MEGADEMO | 28,175 | 43,976 | 42,068 | −1,908 |
| news zoom | 10,054 | 14,376 | 13,468 | −908 |
| stress | 8,287 | 15,064 | 14,252 | −812 |
| grid lab | 11,286 | 16,372 | 15,636 | −736 |
| LCD CATCH | 8,347 | 16,136 | 15,416 | −720 |
| imucal / pet / grid fold / kasane demo / player / companion / keytest / hello / deskclock | | | | −576 / −560 / −428 / −416 / −404 / −300 / −232 / −48 / −12 |

おおむね「ソース 1 KB あたり 90 B 前後」。

### 実機

| 項目 | OFF | ON | 差 |
| --- | ---: | ---: | ---: |
| DERBY の評価の余裕（`heapprobe` derby の二分探索） | 29,969 | 32,709 | +2,740 |
| 同、評価後のゲスト（`used`） | 116,408 | 113,256 | −3,152 |
| 段階 3 の DERBY の評価の余裕 | 15,242 | 19,009 | +3,767 |

アプリを起動して 4 秒後の `MEM js=`（1 回ずつ、起動直後の動きを含むので参考）: MEGADEMO 100,824 → 98,604、BIG WAVE 104,608 → 101,904、LCD CATCH 70,160 → 69,600、stress 60,444 → 59,512、pet 59,684 → 59,080、imucal 53,620 → 52,972、companion 50,968 → 50,680、hello 38,892 → 38,848。DERBY（117,524 → 111,804）と GRID LAB（91,812 → 100,372）は起動直後の登録・確保の途中で揺れる（二分探索の値を使う）。

### 段階 3（首振り）＋ ON（実機、診断 image、MID・HEAVY のデモ各 3 レース）

| 条件 | 基準（OFF、段階 3 なし） | 段階 3（OFF） | 段階 3（ON） |
| --- | ---: | ---: | ---: |
| `LOADSTALL` なし | 満たす | 毎レース | **毎レース**（レース開始時 plan 22〜24 本） |
| ターン内の空きの最小 ≥ 11,208 | 13,256 | 5,156 | **5,016** |
| 評価の余裕 ≥ 20 KB | 29,969 | 15,242 | **19,009** |
| ゲストの最大 ≤ 140 KB | 128,736 | 143,040 | **139,056** |
| fps ≥ 27（MID 横見 WIDE / 首振り） | 29.7 / — | 28.5 / 27.3〜27.8 | 28.5 / 27.5〜27.9（首振りの plan が載らない状態） |

ON で段階 3 のゲストは 3〜4 KB 減ったが、ターン内の最小は変わらない（plan が載らない本数が 1〜2 本減って、その分ネイティブを使うため）。**残る不足は 6 KB 前後**で、段階 3 のコード（ゲスト約 +10 KB）と plan 5 本（約 4.4 KB）が大きすぎる。

## 影響（ON のとき、位置が変わるもの）

| 場所 | ON での出力 |
| --- | --- |
| `app_session.c` の `EVAL_ERROR`（例外の文字列＋スタックの 1 行目） | 構文エラーは `at file:line:col` のまま。実行時エラーは `at <eval> (file:1:1)` や `at f (file:関数の先頭行:1)` |
| `pocket_app_load.c` の `APP_LOAD ... failed`（`describe()`）と import の失敗 | 同上（構文エラーは正確、実行時は関数の先頭行） |
| アプリ自身の `FRAMEFAIL` などのログ（例: DERBY の `log('FRAMEFAIL ' + e)`） | メッセージだけを出すものは変わらない。`e.stack` を出すものは関数の先頭行 |
| host の試験 `tools/test_app_load.c`（`appload_throw.js`）・`tools/test_app_import.c`（`imthrow`） | ON でビルドすると、投げた行を確かめる検査が落ちる（試した: `appload_throw.js:1:1`）。`appload_bad.js:3` などの構文エラーの検査は通る。今の試験のビルドは `sdkconfig.h` を持たないので OFF のまま |
| Playground（実行時にソースを受け取る） | 実行時エラーの行が関数の先頭になる。構文エラーは正確 |
| 文書: `docs/vm/eval-peak.md` §9.5「`STRIP_DEBUG` を使うと行番号が消える」、CLAUDE.md の DERBY の `FRAMEFAIL` の読み方 | ON のイメージでは関数の先頭行と読む |

## 運用案

1. **開発ビルドは OFF、出荷（または DRAM が足りないアプリを入れる）ビルドで ON**。今の測定では DERBY で +2.7 KB で、首振りを入れる決め手にはならない。他のアプリが評価の余裕で困ったときの 3〜4 % の余裕として持っておく。
2. ON でも、ファイル名と関数の先頭行は残る。実機のログで「どの関数か」は分かる（関数名も出る）。行が必要なときは OFF のイメージで再現する。
3. host の試験は OFF のまま（`sdkconfig.h` が無い）。ON を出荷に使うなら、`test_app_load.c`・`test_app_import.c` の「投げた行」の検査を、ON のときはファイル名だけにする分岐が要る。

## 確信の低い点

- 段階 3 の実機の値は、plan が全部は載らない（`LOADSTALL`）状態の値。全部載れば、ターン内の最小はさらに下がる。
- 起動 4 秒後の `MEM js=` は 1 回ずつで、揺れが大きい（DERBY・GRID LAB）。効果は `heapprobe` の二分探索と host の表で読む。
- 変数名（vardefs）や `filename` の atom は残した。落とすと数百 B 増えるかもしれないが、クロージャの名前解決とエラーの名前に使うので試していない。
