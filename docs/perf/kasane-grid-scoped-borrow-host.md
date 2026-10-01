# Grid scoped borrow: host 検証

2026-10-01 UTC。基点 `origin/vm/main` = `89a71ae72c721206dfbb63acfeffc74b38dde604`。cloud Linux host、vendored QuickJS、CのPIE modelを使う。実機の性能測定ではない。

## 変更した契約

- `run` / `measure` は入力mapとparamsのgetterを解決してから、現在のviewのoffset・長さ・ポインタを一度だけ取得する。後続getterによる変更を含む内容で実行する
- 通常の `Int16Array` / `ArrayBuffer` は呼び出し内だけ借用する。viewとbufferのJS参照はローカルに保持し、native実行中にJSへ再入しない
- 共有backing storeと奇数アドレスのnative外部storageは、呼び出し内だけの整列コピーにする。コピー領域をhandleへ保持しない。共有入力の取得中の同時書き換え・growthは未対応
- `JS_GetTypedArrayBufferCurrent` は既存helperの型・detach・OOB検査を使い、resizeで更新される `array.count` から現在のbyte長を読む。既存 `JS_GetTypedArrayBuffer` の動作は変更しない
- 16-byte整列への常時コピーは無くなるが、scalarの型整列、PIEの整列・読出しextent、添字範囲、カーネル合法性の検査は残す。配置が変われば選択経路も変わり得る
- `measure(handle, buffers, params, repeats = 8, strategy = 'AUTO')` は呼び出し元が明示した入力を使う。前回のrun、保持入力コピー、保持paramsへの依存は無い。scalar/PIE用scratch DESTを別々にゼロ初期化し、表示画像とACK状態を変更しない
- DEST依存のfoldは、入力が同じでも前回の出力状態を再現しない。両経路が利用できないfoldのmeasureは拒否する。通常のrunは従来どおり確定DESTから候補DESTへseedする
- `profile` は成功runの計数として独立したまま。`copyUs` は引数取得・入力capture・DEST seed（必要時は一時コピー）の時間。`totalUs` は入力cleanupと返り値生成を含めない

APIの利用方法と詳細な順序は [Grid JS 記法](../kasane/grid-js-notation.md)。

## 検証

既存のsymbolic fold suite（29例、scalar/PIE model/scan）と任意比率RGB565 resize suiteはPASS。grid profile-builderの8件、JS構文、Python構文、差分の空白検査もPASS。

```sh
python3 tools/kasane_contract/run_proc_grid_fold_qjs.py
python3 tools/kasane_contract/run_proc_grid_resize.py
python3 tools/kasane_contract/test_build_grid_profile.py
node --check apps/kasane/grid_lab.js
node --check tools/kasane_contract/test_pocket_grid_scoped_inputs.js
python3 -m py_compile tools/kasane_contract/run_pocket_grid_qjs.py
git diff --check
```

production adapterは以下のstrict-warningコンパイルにPASS。

```sh
mkdir -p .cache/grid-scoped
cc -std=gnu11 -O2 -Wall -Wextra -Werror \
  -DQUICKJS_NG_BUILD -D_GNU_SOURCE -DKSN_GRID_PIE_MODEL \
  -DKSN_GRID_APP_HOST_TEST -I components/quickjs-ng/quickjs-ng \
  -I main -I main/pocket -I main/ui/kasane -I tools/hostshim \
  -c main/pocket/pocket_grid.c -o .cache/grid-scoped/pocket_grid.o
```

追加したscoped-input suiteは、getter順序・変更・detach、tracking/fixed RABのgrow/shrink・offset、共有入力fallback、ephemeral viewのrootとGC、入力alias、非16-byte整列、奇数アドレスのexternal storage、8192要素の合計上限、metadata propertyのshadow、再入拒否、native reset後のhandle再検証を扱う。measureの明示入力・params、run前のmeasure、表示画像/ACK/profileの非変更、DEST依存scalarの表示保存も検査する。

input/params getterの失敗は従来のrunと同じ `INVALID_ARGUMENT` / `grid bind or execution failed` に包む。backend getterは従来どおり元の例外を返す。最終のerror wrapper調整後、次の2コマンドを再ビルド・再実行し、両方PASS。ASan/UBSanの診断は無かった。

```sh
python3 tools/kasane_contract/run_pocket_grid_qjs.py
ASAN_OPTIONS=detect_leaks=0 python3 tools/kasane_contract/run_pocket_grid_qjs.py --sanitize
```

通常検査のログは `.cache/kasane-pocket-grid-qjs/logs/scoped-standard.log`、sanitizerは `scoped-sanitized.log`（いずれも追跡外）。

## 対象外と注意点

- full Kasane contract suite、allocation failure / OOM試験、ESP-IDF firmware build、実機、FPS、実機RAM削減量は未実施
- ASan/UBSanはhostのC経路とPIE modelの検査であり、Xtensa命令や実機の同時実行を検証したものではない
- LSanはこのhostのptrace制約で使えない。外部bufferのfinalizer検査とLSanによる全体リーク検出は別である
- growable `SharedArrayBuffer` のhost検査は、maxByteLength分を確保するSAB allocator callbackを設定したruntimeで行う。現在のfirmware guestはこのcallbackを設定しておらず、hostの成功をfirmwareのgrowable SAB対応の証明にしない。この既存runtime制約は変更していない
- コピー削減はソース上の所有権変更として確認した。実機の速度・空きRAM改善は計測していない
