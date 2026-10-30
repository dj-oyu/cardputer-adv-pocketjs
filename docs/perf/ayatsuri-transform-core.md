# ayatsuri 共通TRS核の縦切りとhost検証

基準はPR110の`1a712359658b160dcf29079dc8d7afeb6a014cb1`。検証候補は変更せず、`feat/ayatsuri-common-core`へ分離した。仕様は[共通核](../api/ayatsuri-transform-core.md)、authoring側は[JS API](../api/character-utility.md)。broker flowの再投入・単独flashは行っていない。

## 意図と実装済み境界

最初の題材は3体のroot/頭/手/尾等の親子配置。JSはmodel・handle・親子graph・dirty依存・時計とframe所有権を維持し、TRS数式だけを`ayatsuri_transform_evaluate`へ移した。JSのdevice用数式は削除し、JS bridgeと直接C利用が同じheader実装を使う。host専用の独立JS referenceは検算用である。

C APIはcaller所有のpose36B、親matrix48B（省略可）、出力48Bを同期借用し、失敗時は出力を変えない。JS bridgeは入力snapshotと成功時の反映を行うため、完全なcopy 0ではない。C scene/handle/allocator、材質/UV/法線/投影の共通核、HOME adapterは未実装。実機を待たず進められる最小境界として分けたもので、汎用framework全体の完成は宣言しない。

## 実行したhost simulation

| 試験 | 観測 |
| --- | --- |
| 直接C API | MinGW64 O0/O2、ASan/UBSan64 O0/O2、m32 SSE2 O0/O2で各2071 checks。失敗時の全出力/canary、親とoutのalias、入力不変、非一様/負scale、NaN/Inf/0scale、Float32丸め境界を検査 |
| 独立JS oracle | 408 vectors・4860成分を照合、全bit一致、maxULP=0、拒否3件も一致。有限な試験範囲の事実で、全入力や別libmでのbit保証ではない |
| 実QuickJS bridge | 型/長さ/immutable出力、重なるview、INVALID/RANGEでの全出力不変と復帰を検査 |
| 限定したalloc検査 | warm-up後、preallocated pose/outputへの固定入力1000 callsでguest allocator呼出し0。通常appのsetter・モデル切替・失敗時・native全体のalloc 0ではない |
| キャラapp ASan/UBSan64 | 3体×6mode×301=5418 frames、例外/提示失敗0、incrementalと全描画一致、範囲失敗6件からの復帰、realm解放後guest残量0 |
| キャラapp m32 | 163840B cap、強制frame毎GCなし、同5418 framesを通過。切替OOMで旧画像保持、上限復旧後の再生成、共通C親子overflow後のrender拒否・旧画像保持・再評価からの復帰を検査 |
| 画像回帰 | 基準の108枚のnative captureと、新ASan64・新m32の各108枚がbyte一致 |
| JS更新依存 | no-op/時間だけ/光だけでTRS再評価なし、部位だけ/親変更の伝播、同sceneへの再入拒否、evaluator例外後の描画拒否/復帰、複数instance分離 |

ログは`.cache/core-native-{asan,m32}.log`、408 vectorsは`.cache/ayatsuri-transform/`。再現コマンド:

```text
python tools/ayatsuri/run_transform.py
node tools/character/transform_oracle.cjs
node tools/kasane_contract/test_character_utility.cjs
CHARACTER_FULL_REFERENCE=1 python tools/kasane_contract/run_character_app_host.py
CHARACTER_HEAP_LIMIT=163840 CHARACTER_NO_EAGER_GC=1 CHARACTER_FULL_REFERENCE=1 python tools/kasane_contract/run_character_app_host.py --m32
```

m32 runnerは`-msse2 -mfpmath=sse`を追加して`FLT_EVAL_METHOD=0`とした。旧m32のx87余剰精度profileとは区別する。直接Cのx87診断も408件では差分0だったが、任意入力の精度同義性は保証せず、S3/64bit/SSE2のbinary64中間profileを契約とする。

## 資源の観測と限界

m32のQuickJS malloc_size最大は160916B、評価中TLSFモデルpeak93384B、fixture allocator peak142792B。load最大単一guest要求10912B、run中1571B。比較元の旧記録はmalloc_size最大162088Bだが、今回のbridge warm-upやcompiler profileも違うため、差を実機の速度/総空き改善と呼ばない。capとの差2924Bも一般の拡張予算ではない。

64bit側はmalloc_size最大226840B、評価peak120124B、allocator peak208928B。これらをm32やnative free/largestと足し引きしない。範囲失敗試験は通常のheap gate後にcapを解除してnative validatorへ到達させる別phaseで、160KiBで極端な入力を生成できる保証ではない。

同head CI・製品linkはPRの結果を正とする。実機のturn/render/send、入力応答、native free/最大連続領域、stack、質感は未確認。flash/broker問題で測れなかった項目をhost値で埋めず、次の独立した実機工程へ残す。
