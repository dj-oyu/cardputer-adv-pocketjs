# CHARACTER LAB 固定入力capture — HOST SIMULATION

この手順の画像は実QuickJSとnative Kasane rendererによる**シミュレーション画像**であり、実機LCDのcaptureではない。[a30b2a2の実機smoke](character-device-smoke.md)とも区別する。実機の正式APIはcaptureとapp_sessionを同時利用できず、STOPでguestを破棄するため、今回の実機app画像は未取得。

## 実装の境界

app所有の`AyatsuriLab.control`を診断時だけ明示ロードする。通常のapp時計/キー/描画先/提示経路は保持し、utilityへtimer/capture/入力依存を持ち込まない。[controller契約](../../apps/character/README.md)はcommit前失敗とcommit後view失敗を区別する。`lastSubmittedTick`はnative候補へのcommit成功であり、LCD提示ACKではない。

host fixtureはframeを評価してからnativeのpresentを完了させ、次のframeへ進む。待ち時間やwall clockからsample位置を推測しない。既存5418frameの回帰経路と、診断をロードするcapture経路は別である。

## 再現コマンド

Linux/WSLで既存のhost toolchainとNodeを利用する。m32は既存sysroot/multilibが必要。

```sh
# 基準/身体のみ/光のみ/カメラのみ × 3体、計12枚
python3 tools/kasane_contract/run_character_app_host.py --capture-dir .cache/character-seek-asan
# 同じ4mode × 3体の固定sample列。逆順/重複も入力順を保つ
python3 tools/kasane_contract/run_character_app_host.py --capture-dir .cache/character-seek-sequence --capture-ticks '90:80:-2,0'
# 通常再生の資源門。診断chunkの追加費用とは別に検査する
CHARACTER_HEAP_LIMIT=163840 CHARACTER_NO_EAGER_GC=1 CHARACTER_FULL_REFERENCE=1 python3 tools/kasane_contract/run_character_app_host.py --m32
```

既定12枚はHUMAN/FOX/CLOUDそれぞれSTILL tick0、BODY tick82、LIGHT tick90、CAMERA tick90。呼吸OFFで、BODYは固定光/カメラ、LIGHTは光だけ、CAMERAは視点だけ変える。tickは`tick*1000/30`の論理時刻。TEXTURE/EXPRESSIONはこのcapture planの対象外で、通常の6mode回帰では検査する。

`--capture-ticks`はカンマ区切り整数か包含範囲`start:stop[:step]`。0..1000000000、modeあたり最大1000 samples。範囲の確保前に上限を検査する。PNG/PPMの名前は`HOST_SIMULATION_...`、PNG metadataとmanifestにもdevice_capture=falseを記録する。raw画素は比較のため文字を焼き込まない。

manifestはmodel/mode/tick/呼吸、画像hash、source SHA256、git HEAD/dirty/diff hash、host binary hash/build flags、検査成否を保持する。HEADだけで未commitのsourceを示したと扱わない。失敗runは成功画像として受け入れず、再実行時に同じplan名の古い画像を除去して取り違えを防ぐ。

## 回帰で守ること

- 同一入力の再描画と逆順seek後の再評価が全画素一致する。
- pause中もframe評価/再描画でき、stepは次tickを描く。resume時の提出tickとcursorを区別する。
- 評価/描画/commitの注入失敗で旧panelと時計/stepを保ち、正常再試行で復帰する。対象hookが本当に呼ばれたことも検査する。
- 各captureの差分描画を全invalidate描画と比較し、終了後guest残量を検査する。

native fixture初回にはpresentを挟まず複数frameを提出する試験不備がありBUSYになった。fixture側の評価→present順を直し、appの所有権契約は緩めていない。またBODY tick82/83は傾きが対称でTRS更新が省略され得るため、評価失敗注入は変形の異なる90→91を使う。これは描画モデルの変更ではない。

診断controllerを通常appへ常駐させた初稿はm32/163840Bの最初のframeで`null`例外となった。既存strip-debugだけでも通らず、無条件常駐を採用せず診断chunkを明示ロードする方式へ変更した。上限を増やして通常再生の合格へ読み替えない。最終のhost結果・メモリ値は実行ログと対応させて追記する。

## 2026-10-30 ローカル最終条件の観測

明示ロードへ分離した後、strip-debugなしでは通常5418frameは例外0だが、意図的OOM後の人モデル再生成に失敗した。同じnative binaryで`chunks.txt`の既存`option strip-debug`だけを切り替え、通常5418frame・全描画一致・OOM再生成・6種範囲失敗復帰・解放後0が通過したため、この設定を採用した。部品/試験/上限は減らしていない。debug位置情報は保持せず、明示的なエラーメッセージと検査stageは維持する。

| 条件 | 観測 |
| --- | --- |
| 通常m32、cap163840、強制GCなし、最終opt-in+strip-debug | compile peak93372 B、QuickJS malloc_size最大157896 B、fixture allocator peak142220 B、単一要求load10912/run1571 B。切替失敗前141224/失敗後95092/再生成157944 B、PASS |
| opt-inだがstrip-debugなし（同binary比較） | 通常malloc_size最大160372 B、切替再生成FAIL。成功結果へ混ぜない |
| 診断m32、capなし、12capture | app評価後89100 B、診断ロード後94428 B、fixture allocator peak147892 B、実行中最大単一要求1571 B。終了直前147056 B、解放後0。診断中の最大malloc_sizeは未集計 |
| 診断ASan/UBSan64、capなし、12capture | app評価後119664 B、診断ロード後126464 B、fixture allocator peak215848 B、実行中最大単一要求2103 B。解放後0 |
| 画像 | 上記64bit/32bitの12枚のRGB hashが全一致。同一入力・逆seek・停止再描画・step/resume・評価/render/commit失敗注入と復帰、差分/全描画一致を確認 |
| 連番 | ASan/UBSan64でtick80..92、4mode×3体=156枚を順序通り出力しPASS。連番も実機の動画ではない |

ログは`.cache/character-seek-m32-optin-strip.log`、`.cache/character-capture-{final,m32,sequence}.log`、画像/manifestは同名directory。画像captureはwarm fixtureの既存binaryを再利用しておりmanifestへ`reuse_existing_binary=true`とbinary/source hashを記録した。再現コマンドは通常どおりbuildから実行できる。これらはローカルのhost観測で、同head CI/製品リンク/実機は別工程。診断時の160KiB保証や実機peak/largest/速度改善は主張しない。

画質の受入はこの画像だけで完了しない。髪/肌/布、狐の固有模様と陰影、雲の柔らかな縁/透過、連続frameのちらつき/モアレ、物理LCDは[実機記録の次工程](character-device-smoke.md)へ残す。速度改善や実機予算は主張しない。
