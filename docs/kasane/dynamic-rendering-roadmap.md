# Kasane 動的描画ロードマップ

状態: **D0–D6 の初回実装・代表workloadの採否を記録済み**（2026-09-28）。採用範囲と保留条件は末尾の「D0–D6 初回採否」を参照。[Kasane v1 の判定](roadmap.md)は変更しない。この文書は実装と実測の順序、各段で下す判断、参照する既存ノウハウを記録する。中間命令の形式、JS API、codec、固定FPSを先に仕様化しない。決まった契約は実装と試験に置き、採否の理由と結果をこの文書へ戻す。

## 到達したいこと

- JS はプログラムと入力をまとめて渡し、関数アートの反復・座標計算・描画を native 側で実行する。JS→native 呼出しを点・画素単位で発生させない。
- 2D、奥行きを使う描画、動画フレームを APP 内の複数の描画面として重ねる。Kasane core の APP/SYSTEM を描画面の数だけ増やさない。
- 各描画面の完成フレームは、LCD 転送失敗後も同じ帯を再生成できる。動画の producer、物理状態の更新、JS の実行は LCD の帯描画から切り離す。
- Cardputer ADV の PSRAM なし・240×135 という条件で、RAM、最大連続空き、描画時間、音声・入力との共存を実機で判断する。全画面 RGB565 は 64,800 B、8行 strip は 3,840 B であり、論理2 bank は画素2面の予算を意味しない。

## 既存の足場

| 参照 | この計画へ持ち込む知見 |
| --- | --- |
| [Kasane の境界と契約](architecture.md)、[判断台帳](decisions.md)、[検証](verification.md) | owner、ticket、dirty帯、APP/SYSTEM、途中転送失敗時の旧論理基準と全面修復。既存の画像 provider は不変な source span を読み、動的フレームの pin/release はまだ持たない。 |
| [FLOWER のシーン](../scenes/flower.md)、[`flower.c`](../../main/scene/flower.c)、[`garden.c`](../../main/scene/garden.c) | `prepare` で最大56部品と係数を導出し、行描画で森→深度付きの花→元行を読む雨を順に合成する。全画面バッファなしで動く厳しい参照workload。 |
| [FLOWER の行不変量](../perf/flower-row-invariants.md)、[shade の内訳](../perf/flower-shade.md)、[PIE の実測](../perf/pie-simd.md)、[命令キャッシュの調査](../perf/ray-stall-census.md) | 計算を frame/row/span/pixel の適切な頻度へ移す効果、棄却数と実時間の違い、native/PIE の局所最適化、命令配置による計測の揺れ。汎用IRの実行速度を推測で保証しない。 |
| [ESP32-S3 hardware MCP](https://github.com/dj-oyu/esp32s3-hw-mcp) の PIE命令・パイプライン資料と[実機例題](https://github.com/dj-oyu/esp32s3-hw-mcp/blob/main/examples/README.md) | TRM 由来の命令の意味・use/def 段、アセンブラとの不一致、実機で C/Python 参照と照合した頂点変換・画素演算・物理演算を D3a/D4 の kernel 候補と検証の参照にする。兄弟プロジェクトから取り込んだ cost model はこの文書の実測とは独立の再測定ではない。 |
| [scene_mem](../../main/scene/scene_mem.h)、[ハードウェア制約](../platform/hardware-constraints.md) | 現在の背景用領域は「同時に一背景」の再利用が前提。複数描画面、旧フレーム保持、動画との同時使用には別の保持量と一時領域の予算が要る。 |
| [Kasane の部分転送](../perf/kasane-text-damage.md)、[画像変換の調査](../perf/kasane-image-transform-recon.md) | dirty列・帯の利益と、変換中の除算など画素単位コストの測り方。draw CPU と LCD 転送量を別々に判定する。 |

## 実行順序と判定

各段はまず代表workloadと比較条件を固定し、hostで画素・寿命、必要な段だけ実機で時間・RAM・音声を確認する。数値ゲートを新設する場合は測定前に条件と全runの扱いを記す。D3とD4はD2後に独立して進められる。

| 段階 | 作るもの・確かめること | 次へ進む判断 |
| --- | --- | --- |
| **D0 基準** | FLOWER の14種・代表ショット・遷移・雨、Kasane overlay、音声を含む比較workloadを選ぶ。`prepare` / 行描画 / LCD、peak RAM・最大連続空き、描画した帯と画素を分けて記録する。 | 後続で何を維持し、何を速度・容量の交換として認めるかを事前に決める。計測用 image と製品 image の結果を混ぜない。 |
| **D1 フレーム境界** | 既存 FLOWER を参照に、論理状態の進行、フレーム用データの導出、帯描画を区別する。時刻・乱数位相・虫・雨・カメラを固定したフレームで、全画面、8行分割、帯の順序変更、再描画が同じ画素になるか確認する。既存の最適化済み kernel は保持する。 | 旧確定・候補・描画中の寿命と scratch の実測予算が成立すること。現在の単一 owner `scene_mem` を複数描画面へ無条件に流用しない。 |
| **D2 APP 内の描画面** | 上限付きの描画面を順番に合成し、描画面ごとの更新、旧・新 bounds の damage、下地を読む pass、透明・不透明の遮蔽を扱う。変更面を明示的に公開し、未表示更新は合流する。 | 半透明・移動・消去・下地読取り・通知・LCD途中失敗で画素と repair が一致すること。描画面追加の RAM と時間を確認する。 |
| **D3 JS からの手続き型描画** | JS がプログラムを登録し、毎フレームは型付き入力を一括更新する。native の上限付き反復と path/Bezier/graph/図形のまとまった操作を試す。逐次的な描画順は保つが、点ごとの JS 呼出しや APP の80命令への点列展開をしない。 | JS 呼出し回数、プログラム検証費、生成量、描画時間を実測する。中間命令の範囲はこの段の結果から固定し、熱い処理は native kernel を選べるようにする。 |
| **D4 関数画素・奥行き** | 画素関数、行の scratch、限定した下地参照、深度付き描画を段階的に試す。独立画素の式と、FLOWER のような行・span・深度依存の kernel を同じ描画面契約へ接続する。 | 全画面の逐次画素処理を無条件に汎用IRで解釈しない。反復回数×画素数、行ごとの最大時間、精度・近似による画素差を測り、native kernel・明示的な低解像度・画像化のどれを採るか決める。 |
| **D5 動画フレーム** | まずローカルの再読出し可能な小さなフレーム列で、世代付き frame handle、pin/release、表示待ちの最新優先、枯渇時の skip/BUSY を確認する。その後、入力ストリームと音声時刻に合わせた選択・遅延フレームの破棄を試す。 | decoder と network/SD I/O を帯描画へ持ち込まず、旧フレームの repair と APP 終了が安全であること。圧縮形式、解像度、保持面数は RAM と時間の実測後に選ぶ。 |
| **D6 共存と採否** | FLOWER＋JS描画面＋動画または音声、通知、画面切替、低heap、部分転送失敗を代表条件で組み合わせる。 | 画素、lease/teardown、入力応答、音声障害、描画・転送の分布、peak RAM/最大連続空きから機能ごとに採用・縮小・保留を決める。全機能を一括で合格扱いしない。 |

### D3a 登録時コンパイラ（サブタスク）

JS から渡された手続き型プログラムを登録時に一度解析し、逐次実行する部分と PIE にまとめられる部分を持つ native 実行 plan に変換する。毎フレームの入力変更では plan を再利用する。IR は型、反復、状態への依存、描画の副作用順を残し、PIE 固有のレジスタ番号や命令を JS に要求しない。

既存コンパイラの [SelectionDAG](https://llvm.org/docs/CodeGenerator.html#selectiondag-instruction-selection-process)、[loop vectorizer](https://llvm.org/docs/Vectorizers.html)、[VPlan](https://llvm.org/docs/VectorizationPlan.html) を参考に、上限付き CFG → def-use と live-in/out → 保守的なループ依存判定 → DAG 上の kernel 選択 → 費用付き plan の範囲だけ採用する。LLVM 本体や汎用 JIT は初期段階で載せない。まずは検証済みの小さなプログラムと少数の事前コンパイル済み融合 kernel を対象にし、適用できない部分は scalar VM へ戻す。float と固定小数の結果、途中の非有限値、最終 state、logical step 上限、描画順、debug step を壊す最適化は採用しない。

最初の比較は同じ点列変換を (A) scalar VM、(B) 登録時に融合した scalar kernel、(C) PIE kernel で実行する。点数・値域・端数を変え、出力と失敗条件の一致、登録費用、フレーム時間、plan/scratch/実行 RAM を測る。PIE の呼出しや pack/unpack を含めて利益が出るサイズを判断する。詳細な命令セットと最適化手順はこの比較から絞る。

PIE kernel の選択とスケジューリングでは上記 hardware MCP の `get_instruction`、`instruction_pipeline`、`analyze_sequence` と実測例を参照する。これは登録時コンパイラにMCPを組み込む指定ではなく、kernel の実装・検証時に根拠を確認する手順である。登録時 plan には候補 kernel の型・値域・整列・scratch・tail・実測費用を記述する。`analyze_sequence` は命令依存の一次近似であり、実機時間の代用にしない。

128bit書込みの16バイト整列、飽和・丸め、命令依存のストールを契約と費用の両方で扱う。[ex07 の8頂点変換](https://github.com/dj-oyu/esp32s3-hw-mcp/blob/main/examples/README.md)を独立点列の参考にし、描画IRのfloat値を暗黙にQ8へ変えない。実機asm・C参照・独立したhost参照の照合に、処理要素数とtailの検証も加える。[メディア/3Dメモ](https://github.com/dj-oyu/esp32s3-hw-mcp/blob/main/notes/08-media-3d-perf.md)の135×240を64,800画素とした計算は誤り（32,400画素、RGB565で64,800 B）なので、そこからの画素当たり予算を転記しない。

レビューの区切りは関連機能が一緒に検証可能になった時点とする。第1区切りは有界な依存解析と現行VMの意味を固定する比較基盤、第2区切りは融合 scalar plan と同値判定、第3区切りは型付き PIE kernel の選択・端数処理・実機費用をまとめて Astra がレビューし、指摘を改善してから次へ進む。独立して作れる解析・比較基盤は並行して進める。

## 実装で蓄積する記録

下の表は初期段階の判断を残す経過記録であり、現在の採否は末尾に置く。根拠となる commit、host試験、実機 image SHA・flags・workload・ログへリンクする。性能の変化は `prepare`、描画、LCD、decoderを分けて示す。取り下げた案と旧不合格も残し、後の成功で上書きしない。実装中に確定した小さな契約はコードと契約試験へ置き、この文書を命令セットの詳細設計書にはしない。

| 段階 | 状態 | 実装・実測から得た知見 / 次の判断 |
| --- | --- | --- |
| D0 | 進行中 | host 基準として `test_flower` の14種×60 pose、全画面と8行帯の一致・境界・`scene_mem` 再利用、`test_glass_rain` の300秒・1～11行帯・重なり、`test_garden` の背景＋雨を通した。host の FLOWER 共有ブロックは 16,188 B。`idf.py -B build_api build` は成功し、app 2,038,592 B、DIRAM 159,212 B（直近基準比 +176 B）。実機の描画時間、LCD、peak RAM、最大連続空き、音声は未測定で、host 実行秒数を実機性能に読み替えない。 |
| D1 | 進行中 | 雨の6滴を 192 B のフレーム値に capture し、シミュレーションを進めた後も同じ下地に全画面・逆順8行帯で同じ画素を再描画できることを `test_glass_rain` で確認。FLOWER のシーン描画もこの固定値を読むよう変更した。FLOWER 本体は依然として単一 `scene_mem` と可変状態を読むため、旧フレームの lease/repair 契約は未達。次に FLOWER の導出データの保持と更新の境界を調べる。 |
| D2 | host 試作・実機単独診断済み | 手続き型 surface に確定・候補の2スロット、世代 ticket、旧/新 damage、下地からの再合成、部分転送失敗後の旧フレーム修復を実装。複数面を APP 内で順に合成し、Kasane の backdrop 経路へ接続した。`test_procedural_surface`、`test_procedural_layers`、`test_procedural_present`、`test_procedural_pattern_matrix` は O0/O2 host で通過。波形・格子・画面外交差・色変更・消去の構造例で帯順序と damage 範囲を確認。`vm/main` (`6430ad6`) 取り込み後の ESP-IDF 6.0.1 / ESP32-S3 image は app 2,051,456 B、SHA-256 `e930a3098fb3786eaa417cd298f578e9a1a34dda00a7b2b4be7a9dfa6e7e400`。[実機の表示・負荷診断](procedural-device-probe.md)は60フレーム完走、2面確保、平均 `prepare` 641 µs、合成等 677 µs、LCD 2,924 µs、内部 RAM free 109,516 B（確保後）を確認。2面のフレームスロットだけで 40,984 B を使う。透明、音声・FLOWER と同時の負荷、LCD GRAM の読戻しは未検証。 |
| D2b | UI 画像ノード接続・実機試作済み | `kasane.procedural.resource()` が現在の手続きフレームを 240×135 の不透明な画像資源として公開し、`tx.image` の bounds と描画順序で通常 UI に重ねられる。転送失敗時は候補画素を UI ticket とともに保持し、成功時に確定する。ニュースセットの小窓から全画面への往復を実機で表示した。現状は単一資源、全面 invalidate、毎フレーム64,800 B転送であり、拡大時の合成費用と矩形 damage が次の課題。 |
| D3–D4 | JS/host 試作・実機単独診断済み | 有界反復・条件付き終了・計算色、VM 所有の検証済み命令コピー、明示的な native state copy-in/out を試した。JS 登録 API は実 QuickJS から15本の plan を登録し、`beginFrame`→`draw`→`commit` で48フレームを表示した。3D depth と下地読取りは未着手。 |
| D3a | host/compiler 試作・実機単独診断済み | 登録時の有界 CFG・依存解析、命令を所有する scalar plan、独立した VM 意味論テスト、型付き Q14 点列の scalar/PIE 候補と明示的な選択 policy を実装。Astra の関連機能レビューで寿命・再入・経路未接続・試験漏れを修正。実機で PIE 280ケースの scalar 同値、40点 plan の PIE 選択、8点からの速度優位を確認したため、型付き Q14 点列は8点以上で PIE を選ぶ。一般の float IR を自動的に PIE 化する段階にはまだ達していない。 |
| D3a→D4 型付きメモリ IR | host・実機単独診断済み | [矩形ループ・affine添字・型付きload・積和の試作](../../main/ui/kasane/ksn_proc_grid.h)を既存float VMから独立させた。`LOAD/MUL/ADD/MIN` と複数のbuffer IDで縮小、畳み込み、prefix、IIR、2D DPを表す。前画素依存、出力衝突、実メモリaliasは独立性を不成立とするが、scalar 実行では既出力を読める。範囲外は実行拒否、QACC 40bit超過は積和PIE候補から除外する。[積和PIE kernel](../../main/ui/kasane/ksn_proc_grid_pie.c)は初期の固定係数形で実機 scalar と一致し、8出力から速度優位を確認した。行ごとのprefix和と固定係数IIRは8行をPIEレーンに割り当てるhost試作を追加したが、実機速度は未測定。[JS fold試作](../../apps/kasane/grid_fold.js)は実QuickJSから型付きIRとPIE候補まで到達したが、本体の公開APIには未接続。 |
| D5 | 通常アプリ試作・実機確認済み | 3-slot poolを画像資源へ結線し、JSの`Uint16Array`入力とPTS/音声位置選択を公開。64×48のVIDEO LABを実機で継続表示した。decoder、外部I/O、音声との同期実測は残る。 |
| D6 | 複合host試験済み | 動画・深度線・半透明SYSTEM UIの重畳、移動・消去、途中転送失敗とrepairを同時に検証。FLOWERと音声を含む実機複合負荷・低heap判定は残る。 |

このメモリIRの `safe` はbindされた添字のメモリ範囲を示し、任意の算術が必ず完走する意味ではない。scalar経路はint64の加算・乗算を実行時にchecked演算し、失敗した時点で停止する。PIE経路は独立したint16積和、符号付き32bitの初期値、8出力単位、整列した出力、QACC 40bit合法の形だけを受け付ける。初期値は既存Q14点列と同じ分解で全精度をQACCへ加える。対応しない形はscalarへ戻す。選択policyは固定係数と配列係数で個別であり、未較正の閾値0ではPIEを選ばない。次は境界処理、失敗時の書込み可視性とdebug stepをまとめて設計する。

JS側の `fold` は、描画ごとにcallbackを呼ぶ形ではなく、登録時にsymbolicな出力座標・tap・累積値を渡して式IRを1回構築する。[試作builder](../../apps/kasane/grid_fold.js)は `g.add`/`g.mul`/`g.min`/`g.load`/`g.constant` を用い、通常のJS演算をsymbolic式と誤認しない。一般のfoldは左から順に実行でき、独立出力の積和はPIE候補として検出する。callback中の副作用を描画時に再実行しない。既出力読取りによるprefix、IIR、2D DPも試すが、任意の依存グラフを自動並列化する段階ではない。

2026-09-27 の COM3 / ESP32-S3 rev0.2 診断では、image 2,089,328 B（SHA-256 `e29d004809e50afb0e07c53af813a11d0252a7b4db8e37ac7fc78a9ce0eeac86`）を一時書込みした。[USB診断](../../main/ui/kasane/ksn_proc_compiler_device_probe.c)は 16×12 出力・2×2 tap の全192画素を独立したC参照、scalar IR、8出力レーンモデルで照合し、範囲外の拒否、実メモリaliasでの独立性棄却、planの命令所有を確認した。`validation_work=26`、登録15 µs、開始時bind初回71 µs、64回のbind合計706 µs、scalar実行64回66,434 µs、8レーンモデル64回65,706 µs。モデルは実PIE命令を使わず、速度差も小さいため、これをPIEの性能とはみなさない。診断用work領域は4,768 B、全診断後の内部heapは開始前後とも168,156 B、診断中の最小freeは104,384 B。元のアプリ領域3,145,728 B（退避SHA-256 `dc87df6b245f0ccea8a2695590dac6b1105192c49a04042e689fa9fede8f19c3`）を同セッションで復元し、flash digest一致と `HOME_READY` を確認した。

続く実PIE診断では、8レーンの入力をCで gather して `ee.vmulas.s16.qacc` で積算し、`ee.srcmb.s16.qacc` で出力した。[命令シミュレータ試験](../../tools/pie/test_proc_grid_pie.py)168ケース、host scalar/dispatch 112ケース、ESP32-S3の重み・shift・tap・端数72ケースで値が一致した。最後の診断 image は2,091,760 B（SHA-256 `20202af2090d4f7cc57ef54e185f5f0a9ccd88f0e132bbc8ab2e40acdbbe32df`）を一時書込みした。16×12出力・2×2 tapの64回合計は scalar 66,436 µs、PIE 2,393 µs。出力数8/9/16/40/192の各1024回では scalar/PIE がそれぞれ 46,037/2,347、51,777/4,208、91,861/3,821、229,558/8,582、1,062,099/37,151 µs。実行時間にはgatherとtailを含み、登録・bind・JS・描画合成・LCD転送は含まない。この形について選択閾値8出力を診断policyに設定した。実行オブジェクトの `block8` はPIEロード・積算・shift/storeを含み、tap内ロード直後使用の静的予測は1箇所。元のアプリ領域3,145,728 Bを同セッションで復元し、flash digest一致と `HOME_READY` を確認した。

次に[登録時の積和正規化](../../main/ui/kasane/ksn_proc_grid.c)を追加した。固定レジスタ番号と `LOAD`/`CONST` の定義順を外し、`LOAD×CONST`、`LOAD×LOAD`、直接加算、両定数を同じdescriptorへ変換する。descriptorは使用した元命令のslotを保持するので、開始時に確定したaffine添字を実行時に再利用できる。全命令が結果へ寄与することを要求し、捨てた算術の失敗を消す変換は認めない。メモリ範囲、出力独立性、40bit積算限界は既存のbind時検証を通す。PIE実行は定数をq1に保持する経路と係数も毎tapロードする経路に分け、選択閾値も個別にする。host scalar/dispatch 376ケース（O0/O2、PIEモデル有無）、PIE命令シミュレータ232ケースが一致した。最初の実機版は108ケースで全画素一致したが、汎用値取得をレーン内で行い、固定係数192出力のPIE時間が1024回で37,151→238,165 µsへ退行した。実行カーネルを分け、32bit初期値を追加した修正版はESP32-S3向けビルドと逆アセンブルまで確認し、実機再計測を待つ。最初の実機版では配列係数192出力がscalar/PIE=1,137,295/400,565 µs（各1024回）で、配列係数の自動選択閾値は未設定のままとする。

その後のhost-only改善では、bind済みaffine添字 `base + sx*x + sy*y + stx*tx + sty*ty` の行・tap基底を8レーンで共有した。固定係数の `sx=1` は整列した8要素を直接ロード、`sx=2` は整列した16要素を `EE.VUNZIP.16` で分離し、2×2の隣接tapなら1組のロードから両tapを元の順でQACCへ積算する。直接ロードが余分なセルを読む境界ではscratch gatherへ戻す。可変係数も行・tap基底を共有し、連続したsource/weightだけを直接ロードする。host PIEモデルで376設定のscalar・旧gather・新経路が一致し、経路カウンタはdirect 1、単一stride2 977、隣接tap pair 112、gather 8,082（境界・負stride例を含む）。命令シミュレータ677件、ASan/UBSan、ESP32-S3向けコンパイルと`VUNZIP`の逆アセンブルを確認した。**この段階では実機時間は未測定**で、従来の選択閾値は変えない。

別にTRM §1.8.140–145の符号付きQACC融合命令 `EE.VMULAS.S16.QACC.LD.IP`、`.LD.XP`、`.LDBC.INCP`、`.LD.IP.QUP` をPIE命令シミュレータと静的ストール解析へ追加した。ロード前の乗算入力、16B/2B整列、アドレス増分、QUPの旧QRとSAR_BYTE、QRのdef/use段を34件のhostユニット試験で確認した。これらの融合命令は**まだ実際のgrid backendから発行しない**。直接ロード化で入力の並びを整えた後、命令シミュレータと同一バイナリA/Bで採否を判断する。

[JSの演算例](../../apps/kasane/grid_fold_examples.js)は従来の縮小・畳み込み・32tap FIR・Q14点列変換・行列積・逐次foldに、8行prefix和、各セルで飽和する固定係数IIR、2D経路数、min-plus畳み込み、2D最短路を加えた。[実QuickJS host試験](../../tools/kasane_contract/run_proc_grid_fold_qjs.py)は13例を登録時生成のIRから実行し、独立したJS参照値、scalar IR、選択されたPIE hostモデルを全出力で照合した。7例は積和PIE、prefix和とIIRの2例は依存行PIE、残る4例はscalarで一致した。依存行PIEの合法性は、前出力の添字が現在出力の1つ左であること、行pitchで行が独立すること、別入力が出力と重ならないことをbind時に検証する。prefixは `EE.VADDS.S16`、係数付きIIRはQACCの積和と出力時の飽和を使う。命令モデルは357ケース、host O0/O2 の scalar/dispatch は376ケース通過した。ESP32-S3向けに grid 本体・PIE backend・診断結線のコンパイルは通ったが、この依存行kernelの実機同値・速度・選択閾値は未測定なので、自動選択は明示的な `min_scan_rows` が8以上の時だけに限定する。prefixの静的stall解析は4命令中1箇所のload直後使用を示し、gather/scatterを含む全費用は表さない。

D3a の PIE 点列カーネルについて、[実行オブジェクトのストール解析](pie-stall-host-report.md)と[命令順の検証](proc-pie-stall-options.md)を host で実施した。元の 31 PIE 命令には QR のロード直後使用が 12 箇所あり、既存の `stalls.py` は符号付き QACC 命令の入力を見落として 4 箇所しか報告しなかった。検出器を修正し、同じ 31 命令のまま係数レジスタを交互に使う順へ変更した結果、静的予測は 0 箇所。元と変更後は命令シミュレータで 1,024 入力条件の出力が scalar と一致し、変更後の C 全体を ESP32-S3 向けにコンパイルした逆アセンブルでも順序を確認した。その後の実機診断で値一致と8/16/40点の時間を測り、型付き点列の PIE 選択閾値を8点にした。静的ストール予測だけを実測サイクル数とはみなさない。

登録時コンパイラの連続 workload として、疑似3D・glitch の [48フレーム host メガデモ](../../tools/kasane_contract/proc_megademo.h)を追加した。3場面×5レイヤーの IR plan を場面開始時に登録し、毎フレームは入力だけを更新する。門型ワイヤーフレーム、消失点へ向かう線、奥行き格子、正弦信号、色ずれした走査線を native `REPEAT` で描く。[契約試験](../../tools/kasane_contract/test_proc_megademo.c)は通常VM・plan・デバッグ経路の状態と全画素を比較し、O0/O2 host で 48フレーム・5,680 segment・登録時の融合箇所6件が通過した。[プレビュー生成](../../tools/kasane_contract/run_proc_megademo_preview.py)は RGB565 の描画結果から PPM と一覧 PNG を出す。

同じ5レイヤーの最前面へ、前後の菱形を4本の稜線で結ぶ揺れるワイヤーフレーム結晶を追加した。奥面のずれは既存 `draw` 入力の空き2スロットで渡し、描画命令は登録済み plan に保つ。[ニュースセット内の表示](../../apps/kasane/proc_news_zoom.js)にも同じ定義を反映し、15 program と48フレーム×5入力が全画面版と一致することを確認。3配色・48フレームの host プレビューで中央の輪郭と奥行き変化を確認した。更新後は6,352 segment、通常VM・plan・デバッグ経路の全画素一致、JS/C の15 plan・48フレーム一致、実 QuickJS adapter の48フレーム一致を確認。先の5,680 segmentと実機 hash は変更前の記録であり、この結晶を含む版の実機表示・速度は未確認。

2026-09-27 に COM3 の ESP32-S3 rev0.2 / 8MB で optional メガデモ probe を実行した。image は 2,056,720 B、SHA-256 `928028763e5486021664779afdd41f28f2c0cc8b82fcc139f8a59ff863543d26`。48フレーム・5,680 segment を完走し、全48フレームの RGB565 hash と3場面末尾（15/31/47）の全135行×240画素キャプチャが host と一致した。[起動・回収](../../tools/kasane_contract/run_proc_megademo_device.py)と[照合](../../tools/kasane_contract/check_proc_megademo_device.py)を再実行できる。キャプチャなし45フレームの平均は VM 669 µs、帯描画 7,085 µs、LCD転送 7,242 µs、全体 15,076 µs（14,045–16,169 µs）。フレーム間の実測中央値は50 msで、末尾の33 ms `vTaskDelay` が描画後に加算されている。キャプチャ3フレームはシリアル出力待ちにより各約962 msで、通常描画費用に含めない。実行中の内部 RAM free は171,680 B、終了時は開始時の230,780 Bへ戻った。LCDへの転送成功と転送前画素は確認したが、GRAMの読戻しや肉眼でのパネル像、音声との同時負荷、深度バッファ、PIE点列カーネルの速度はこのprobeの判定外。元のアプリ領域3,145,728 Bを同セッションで復元し、書込みhash検証と `HOME_READY` を確認した。

同日に [JS メガデモ](../../apps/kasane/proc_megademo.js)を登録 API へ接続した。JS は15本の float IR plan と3本の Q14 点列 descriptor（各40点）を登録し、各フレームは5回の `draw` で native 反復と線分描画を実行する。型付き点列は登録時に係数と点を16-byte整列の所有領域へコピーし、実機では PIE plan を選択する。実 QuickJS の[ホスト試験](../../tools/kasane_contract/run_pocket_proc_qjs.py)は通常 backend の scalar fallback と fake PIE backend の両方で48フレーム全画素一致、転送失敗修復、reset、再入拒否を確認した。Kasane host suite も失敗0件。実機の直接 PIE 診断は280/280ケースで scalar と一致し、2048回の集計時間は8点で scalar/PIE=6,392/2,342 µs、16点で12,478/2,834 µs、40点で30,706/4,301 µs。これは点列変換 kernel 単体の時間で、JS や raster/LCD を含まない。

最終診断 image は ESP-IDF 6.0.1、`KASANE_PROC_DEVICE_PROBE=ON`、`KASANE_PROC_JS_DIAGNOSTIC=ON`、2,072,976 B、SHA-256 `7c0a1bdb25752baebfb5c19dcc82deec3da03f5a92f287e14d6c1aafca780789`。[実機起動・キャプチャ](../../tools/kasane_contract/run_proc_js_device.py)と[ホスト全画素照合](../../tools/kasane_contract/check_proc_js_device.py)で3場面の連続2フレームずつ（8/9、26/27、39/40）が一致し、診断ログは各表示フレームで `scalar=0`、PIE呼出しが1回ずつ増加、転送64,800 Bを記録した。COM3 の元アプリ領域3,145,728 Bは保存値（SHA-256 `6178382394f4970c6cd06ab3e49f87114eeb20cef32ae00a5cdcfd5a3fb8ba94`）に復元し、flash digest 照合と `HOME_READY` を確認した。キャプチャはLCD転送前の送信画素であり、GRAM読戻し・肉眼像・音声との同時負荷は未検証。次の実装判断は、float IR から安全に型付き kernel へ lowering できる形と、修復可能なフレーム寿命を複数描画面へ一般化する範囲である。

同日の UI 混在試作では、[ニュースセット JS](../../apps/kasane/proc_news_zoom.js) が既存の15 plan・3 Q14点列を登録し、`commit()` 後に `tx.image` の矩形を動かした。小窓 `[64,20,176,83]`、拡大途中、全画面、縮小途中の [COM3転送画素](proc-news-zoom-device.png) を取得した。実 QuickJS host 試験は scalar/fake PIE とも成功し、UI 構成の320フレーム mock 試験も通過。診断 image は 2,082,976 B、SHA-256 `1b4f044cc6f59716a0c0b54304c7f59b04d3c7bb641b2fe96816e644d3949217`。キャプチャ前の連続フレーム間隔の中央値は小窓47 ms、拡大中59 ms、全画面74 ms、縮小中64 ms。全画面は約13.5 fpsで、目標とする滑らかさにはまだ達していない。PIE点列は各フレーム1回動き、転送量は全フェーズ64,800 B。次は Kasane の画像合成費用を計測して縮め、資源の旧・新矩形に基づく damage と列単位の部分転送を通す。元アプリ領域3,145,728 Bは同セッションで全量復元・digest照合し、`HOME_READY` を確認した。この画像は LCD 送信直前の画素であり、パネル読戻しや複数独立 canvas は未検証。

その後、画像資源の再公開に旧・新矩形の damage を割り当て、未表示の更新と転送失敗後の修復を host で確認した。小窓の通常転送は 64,800→16,128 B。1:1 `STRETCH` は座標写像を省き32画素ずつ読み、手続き型画像 provider は8行をまとめて生成・保持する。不透明を明示保証した画像が損傷列と帯全体を覆う場合だけ、その画像より前の APP 描画を省く。SYSTEM と後続 APP は残す。旧経路との全画素比較は identity 変換・8行の任意順読出し・16種類の遮蔽条件で一致し、frame=138 の COM3 送信前画素も変更前と全135行一致した。8行キャッシュの静的 RAM 増分は3,360 B。画像以外や不透明保証のない画像では遮蔽を適用しない。

最終診断 image は 2,084,576 B、SHA-256 `355c0229a6e36859a0873c5b3d6f3b918bd78a9ecb6a86d7c024afcd9da92558`。同じニュースデモのキャプチャ前連続フレーム間隔の中央値は小窓33 ms、拡大中35.5 ms、全画面33 ms、縮小中48.5 ms。全画面の `KASANE_PAINT` 30フレーム窓では帯描画16.28–16.79 ms、そのうち画像12.65–12.67 ms。全画面の LCD 転送は依然64,800 Bで、縮小区間には最大61 msのフレームもある。これらは診断ログを[集計スクリプト](../../tools/kasane_contract/report_proc_news_device.py)で読んだ値であり、固定30 fpsや他のシーン・音声同時実行の保証ではない。COM3の元アプリ領域を全量復元・digest照合し、`HOME_READY` を確認した。次はズーム中の拡大縮小写像と複数 canvas の保持量・合成順を workload で判断する。

縮小中の改善候補は、1:1 `STRETCH` だけが座標写像を省けること、画像が損傷帯を覆わなくなると背後の APP を再描画すること、8行単位の source cache が縮小時には未参照行も生成し得ること。この3点はコード上の候補であり、費用の内訳は縮小フレーム単位でまだ分離していない。縮小区間の転送量中央値は61,440 Bで全画面64,800 Bより小さい一方、フレーム間隔は33→48.5 msへ増えるため、LCD転送量だけでは遅さを説明できない。

結晶追加後の COM3 再確認では、`KASANE_PROC_DEVICE_PROBE=ON` と `KASANE_PROC_JS_DIAGNOSTIC=ON` の image 2,099,360 B（SHA-256 `7018c5e38ae7846f4202875114736cbc969d6d9255c02bc6bfa771be2c4e119c`）を一時書込みし、書込み hash を照合した。2×2 固定係数縮小 kernel は192出力×1024回で scalar 1,045,724 µs、PIE 167,566 µs（6.24倍）。可変係数は scalar 1,114,317 µs、PIE 287,605 µs（3.87倍）。144件のPIE境界例を含め `GRID PASS` を確認した。固定係数 PIE は直前の汎用 kernel 238,165 µs より短いが、初期の専用 kernel 37,151 µs より約4.5倍遅い。この kernel は現状の UI `STRETCH` に接続されておらず、ニュース画面の縮小速度を示す値ではない。

ニュースセットの新規実機ログ3回では、160フレームの各フェーズで PIE点列を1回/フレーム実行し、転送は小窓16,128 B、全画面64,800 B。フレーム間隔中央値は小窓33 ms、拡大36–36.5 ms、全画面34 ms、縮小全44フレーム35.5–36 msだった。ただし前回の48.5 msは縮小前半のframe 116–139だけを測っていた。同じ24フレームに揃えると今回は49.5/50/49.5 ms（最大60 ms）で、前回48.5 msから改善していない。縮小後半のframe 140–159は33–34 msまで下がるため、全44フレームの中央値だけで高速化を判定しない。[送信直前の画素5段階](proc-news-zoom-crystal-device.png)では小窓、拡大途中、全画面、縮小途中、復帰後のモニター枠と前面ニューステロップの重なりは連続している。縮小は `stretch_sample` の画素中心1点サンプリングであり、細線にはエイリアシングが残る。パネルのGRAM読戻しと肉眼像は今回も未確認。試験後は開始時のアプリ領域3,145,728 Bを復元し、書込み hash と `HOME_READY` を確認した。

速度内訳を読むと、全画面の30フレーム窓は帯描画16.58 ms（画像12.98 ms、合成1.55 ms）、縮小を含む窓は帯描画32.68 ms（画像17.15 ms、合成13.38 ms）。LCD送信は7.34→6.12 msで減る。画像が縮むと旧矩形と新矩形のdamageに新画像が帯全体で重ならず、`app_start[band]` の不透明画像遮蔽が成立しないため、背後のニュースセットのグラデーションや枠を再合成する。画像側も1:1専用経路を外れ、16画素spanの座標写像と8行source cacheを通る。PIEの2×2 kernelは別系統で、逆アセンブルでは固定係数の192出力・4tapに対して `input_address` を768回呼び、各呼出しで4回の整数乗算を行って8レーン入力を集めている。PIEの積算よりCPU側のgatherが支配的と推測されるが、個別の費用は未計測。次の改善では UI の露出下地の再合成量と、kernel の登録済みstrideを使った増分アドレス計算を別々に測る。

縮小前半の実機改善を1機能ずつ測った。同じニュースデモのframe 116–139、各段階を独立に2回測定した連続フレーム間隔の中央値は、従来49.5/50/49.5 ms（3回）→不透明画像の外側だけ下地を描く部分遮蔽39/38.5 ms→線描画をBresenhamの誤差状態ごと行帯入口へ飛ばす処理36/36 ms→STRETCH画像の参照座標をコマンド単位で再利用35/35 ms。PIE実行は全段階で1回/フレーム、転送量の段階別分布も同じ。部分遮蔽は最大4矩形に分け、画像より後のAPPとSYSTEMは従来順で合成する。行帯ジャンプは元の線の位相を保ち、約105万件の帯比較とメガデモ48フレームで全画素一致。座標表は元画像の幅・高さが最大256という制約から横240 B＋縦135 Bで、hostの132設定・アニメーション120フレームで従来span増分経路と画素・画像取得回数が一致した。実機のDIRAM増分は座標表を含め400 B。縮小前半は33 msの周期をまだ超えるフレームがあり、35 ms中央値は固定30 fpsの保証ではない。

計測にはアプリ領域だけを一時書込みし、フラッシュ全量退避は行っていない。最後に改善を含む通常版image（2,079,856 B、SHA-256 `f706656c869a89526a91c63aab80e16ceaa71cd891635a5f63308f89b1cabf1a`）をアプリ領域へ書込み、書込みhashを照合した。通常版のAPPSからMEGADEMOを2回起動し、それぞれ120フレーム以上描画、Backで終了して再起動できることを確認した。

続けて画像の費用を分解した。縮小を含む30フレーム窓では `image` が約14.6–14.8 ms/フレームで、手続き画像の8行帯生成は約2.1–2.2 ms、providerの全span処理は約3.0–3.1 msだった。下地などの `blend` は約2–3 msに下がっており、以前の約13.4 msの再合成は現在の主因ではない。画像の短いspanごとにコアで命令とリソースを再解決する処理を、描画中の命令ごとに一度だけ解決する読取経路へ変えた。hostのSTRETCH 132設定・388画面hashと不透明画像遮蔽試験は画素一致。COM3の2回のニュースデモでは縮小前半 frame 116–139 の中央値が35→34/34 ms、後半 frame 140–159 が33/33 ms。全画面と小窓も33 msで、フレーム間隔は約33 msの基礎周期に近づいた。これはニュースデモ単独の値であり、すべての描画条件で30 fpsを保証するものではない。

登録画像が全画素不透明と宣言し、命令のopacityも255なら、画像のRGB565値を直接投影する経路も追加した。透過・グループ・損傷修復・アニメーションを含むhost画像試験は通過。診断imageの独立した2回のCOM3実行では縮小前半の中央値が33.5/33 ms、縮小後半33/33.5 ms、全画面33/33 msだった。縮小を含む30フレーム窓の画像処理は約11.4–12.1 ms/フレームで、最初の約14.6–14.8 msから減った。フレーム間隔の最大値は43–45 msであり、短い遅延は残る。診断後、通常版image 2,080,304 B（SHA-256 `dc559892dfe6c19d2527b9bea942e28e76656121d07e85b46e8941548a0673e9`）をアプリ領域だけに書込み、書込みhashを照合した。APPSのMEGADEMOを2回起動し、終了・再起動を確認した。フラッシュ全量退避は行っていない。

疑似3D・glitch の [JSメガデモ](../../apps/kasane/proc_megademo.js) は診断起動に加えて、通常ファームの APPS 末尾 `MEGADEMO`（`local.megademo`）から起動できるようにした。48フレームで循環し、Backで終了、再選択で新しいセッションを作る。COM3へアプリ領域だけをインストールした通常 image は 2,073,360 B、SHA-256 `090da6ad00e21edf624589924426d9437d64ed00b1632845caa4bb288a369397`。書込み後にflash digestを照合し、[メニュー起動試験](../../tools/kasane_megademo_menu_device.py)で2回の `APP_ID local.megademo`、それぞれ120フレーム以上の描画、Back後の `APP_STOPPED` と再起動を確認した。元のアプリ領域のバックアップは `.cache/kasane-megademo-device-20260927/app-before.bin` に保持し、今回は通常アプリとして使えるよう新imageを本体に残した。

2026-09-28、MEGADEMO本体にニュースセットと手続き画像リソースを統合した。初期表示は従来どおり全画面で、Enterを押すたびに44フレームの矩形補間で `[64,20,176,83]` の小窓と往復する。Enter長押しは1回と数え、画像を最後に描くため全画面ではUIが隠れる。縮小はKasaneの画像 `STRETCH` 経路であり、試作中の2×2 PIE畳み込みとは未接続。実QuickJS hostでは48フレームのRGB565一致と全画面→小窓→全画面の往復を確認した。COM3へ通常 image 2,083,152 B（SHA-256 `1aa0d7885f9628f6cee1775db72f49e132adca84bd3da8f279425df217dcb7e8`）をアプリ領域だけ書込み、書込みhashを照合した。実機の転送画素 `(0,0)` は全画面 `0x080c`→小窓 `0x00a4`→全画面 `0x000d` となり、BackでHOMEへ戻った。フラッシュ退避は行っていない。

D2 より先の探索として、D3 の小さな [host IR 実験](procedural-ir-experiment.md)を独立に走らせた。これは D1/D2 完了や JS 公開を意味しない。逐次命令、native 反復、単一 step のデバッグ、完成フレームの帯再描画を試し、容量と描画コストの判断材料を得る。
続けて格子・状態を持つ軌跡・複数 path・脱出時間型軌道を比較し、固定 `REPEAT` だけでは点ごとの条件付き終了を表せず、脱出軌道がフレーム全体を無効にする問題を再現した。色も即値だけでは反復回数を反映できない。D3/D4 で制御と描画属性の境界を決める際の判断材料とする。
追加の host probe では、フレーム間の物理状態、入力で変わる反復回数、step 中のプログラム所有権、失敗した候補による旧フレーム喪失、移動図形の旧位置 damage、交差図形の depth を調べた。詳細は実験記録に残した。D1/D2 の寿命・再合成契約が先であり、D3/D4 の opcode を今固定しない。
入力駆動の有界反復 `REPEAT_REG` と最内ループからの条件付き終了 `BREAK_IF_GT` を host 実験へ追加。旧 `c=2` 軌道の失敗を対照として残し、新経路では2反復目で終了・フレーム公開、`c=-1` は16回完走した。さらに VM 所有コピー、明示的な native state、計算した RGB565 色を追加。step、生成、ラスタの上限は維持した。色や制御の実験結果は製品 API 採用を意味しない。
次に host の手続き型 surface で確定・候補の2スロット、世代 ticket、旧/新 damage、下地からの再合成、部分転送失敗後の旧フレーム修復を試した。複数面の順序付き合成と Kasane core の backdrop 経路も host で試し、SYSTEM 命令の重なり、変更帯の表示、最初の転送失敗後の旧確定への修復を確認した。候補と core 提出が重なった場合は core 提出を旧確定面の上に先に表示する。現状は手続き型 damage の帯だけを core に渡し、列範囲の最適化は使わない。これらの保持量と転送量を実機許容と判定したものではない。

現時点の重要な未決事項は、フレーム用導出データを旧・候補ぶん保持するか小さな固定入力から再導出するか、端末上で編集した関数をどう中間命令へ変換するか、下地読取りをどの範囲まで認めるか、動画の実用解像度とフレーム保持数である。いずれも先に一般解を決めず、該当段階の workload と予算で選ぶ。

## D3a grid のhost更新（2026-09-28）

affine添字の横strideをbind時に `broadcast / contiguous / interleaved2 / gather` へ分類する。係数が全レーンで同じ場合は `EE.VLDBC.16`、隣接tapが交互配置なら `EE.VUNZIP.16` を候補にし、整列と余分なvector読取り範囲はブロックごとに守る。2tap共有loadは静的テンプレートで5命令＋1 QRストール、分離したstride2 loadは合計8命令＋2ストールと解析され、両方を含めた比較で共有loadを選ぶ。C側のaccess説明は候補・選択・費用を返すが、この費用はCのgather、メモリ待ち、tail、割込みを含まない。融合MAC/loadのシミュレータ対応は維持し、実grid backendからはまだ発行しない。

JS foldはcallbackが返す式を不変DAGとして扱い、最後の結果だけをaccへ書く。分岐したacc参照、共有部分式、16命令のレジスタ再利用、別builderの値の拒否を追加した。名前付きaffine `view` はbufferと添字を束ねる糖衣で、最終的なメモリ合法性は引き続きnativeのbindで判定する。hostのscalar/PIEモデル400設定（ASan/UBSan）、PIE命令モデル997件、QuickJSの16例、PIEなしフォールバック、ESP32-S3向けクロスコンパイルと `VLDBC`/`VUNZIP` 逆アセンブルを確認した。実機時間は未測定なので自動選択閾値は変えない。今後の `plan.explain()` をJSへ公開するには、nativeのbind後access説明とfallback理由を接続する必要がある。

続いて [QuickJS→IR→選択PIE命令のhost検証](../../tools/kasane_contract/run_proc_grid_fold_pie_asm.py) を追加した。実QuickJSの19例からC backendが実際に選んだ各8レーン経路と入力をhost traceに記録し、対応するインラインアセンブリをPIEシミュレータで再生する。全92ベクトルブロックがCモデルおよび独立したJS参照値と一致した。通過した経路は2tap共有load 48、単一stride2 1、入力broadcast 1、係数broadcast 272、レーン別係数load 1、通常tap 4、依存行scan 28。静的解析は再生した1,584命令にQR interlock 407箇所を予測した。この数は選択命令列の診断であり、CPU gather・実メモリ待ち・割込みを含む実行時間ではない。実機速度と融合MAC命令の本体発行は引き続き未検証。

COM3実機では[同一バイナリの3経路計測](../../tools/kasane_contract/run_proc_grid_measure_device.ps1)を2回行い、scalar、scratch gather PIE、新しいaffine/broadcast PIEの全画素一致と `GRID PASS` を確認した。各1024呼出しの2回平均は、192出力×2×2固定係数で1,045,624 / 78,888 / 85,360 µs、同じ形の可変係数で1,114,237 / 123,223 / 124,494 µs、192出力×3×3係数broadcastで2,132,574 / 251,730 / 181,307 µs（順にscalar / gather PIE / 新PIE）。新経路は固定係数で8.2%、可変係数で1.0%遅く、係数broadcastでは28.0%速かった。8–40出力の固定係数でも新経路はgatherより遅い。静的なQRストールと命令数だけでは全体の選択を決められず、Cの整列判定・添字計算・gatherと実機の命令配置を含めて見直す。したがって自動選択閾値は変更せず、2tap共有loadを速度改善とみなさない。診断後はアプリ領域へMEGADEMO入り通常版（SHA-256 `dc559892dfe6c19d2527b9bea942e28e76656121d07e85b46e8941548a0673e9`）を書き戻してflash digest照合と `HOME_READY`、MEGADEMOの2回起動・終了・再起動を確認した。フラッシュ退避は行っていない。

次に固定係数2×2の各行で8レーン×2tapをscratchへ揃え、1tap目の積和と2tap目のロードを `EE.VMULAS.S16.QACC.LD.IP` に融合した。登録・bindの合法性判定はそのまま使い、隣接tapかつ横stride=2のときだけ自動選択する。実QuickJS→IR→C backend→PIE命令シミュレータの19例・92ブロックは全画素一致し、強制affineの1,584命令/407予測ストールに対し強制融合は1,488命令/455予測ストールだった。COM3の同一バイナリ4経路を2回測定し、固定係数2×2・192出力・各1024呼出しではscalar 1,045,721、gather 78,948、affine 85,637、融合 60,497 µs（2回平均）で全画素一致した。融合はgatherより23.4%速く、8/9/16/40出力でも速かった。可変係数2×2は融合対象外で従来gatherを維持し、3×3係数broadcastはaffine経路を維持する。診断後はフラッシュ退避をせず通常版を書き戻し、verify-flash と `HOME_READY` を確認した。この測定はgrid kernelの実行のみで、JS登録、UI再合成、LCD転送を含まない。

[grid image adapter](../../main/ui/kasane/ksn_proc_grid_image.c)を追加し、型付きgridの連続したint16出力を追加コピーなしの不透明RGB565 `ksn_image_port`としてKasaneへ渡せるようにした。bind時に出力の行pitchと座標を検証し、run成功前の画像公開と範囲外spanを拒否する。[host結合試験](../../tools/kasane_contract/test_grid_image_projection.c)はgrid生成→画像登録→2倍拡大→通常の矩形UI重ね合わせ→PATCHによる移動と等倍表示を通した。現状のadapterは借用バッファなので、公開後から表示完了まで所有者が出力を書き換えない契約が必要。JSフレームAPIへ接続する際に二重バッファとpresent ACKに合わせた寿命管理を作る。通常アプリではまだgrid JS APIがなく、このadapterは診断ビルド内のみ結線される。

## D3a grid の候補自動選択（2026-09-28）

PIEのgather・affine・融合MAC/loadをbindしたexecutionごとの候補にし、合法性判定と速度選択を分けた。合法な候補だけを[実機probe](../../main/ui/kasane/ksn_proc_compiler_device_probe.c)で同じバイナリから交互に1024回ずつ測定する。[生成器](../../tools/kasane_contract/build_grid_profile.py)は独立した3回以上のログ、実行中ELFとフラッシュしたimageの一致、CPU・IDF・最適化設定・測定ケースの一致を要求する。候補ごとに全runでgatherを上回り、中央値でも5%以上速い場合だけ[選択表](../../main/ui/kasane/ksn_proc_grid_profile.h)へ採用する。表が候補を合法化することはなく、未知の形状・環境はgatherへ戻る。これは登録済みテンプレートの選択であり、実行時に任意のPIE機械語を生成するJITではない。

COM3 / ESP32-S3 rev0.2、240 MHz、IDF 6.0.1、SIZE最適化で同一バイナリ3回の全画素一致を確認した。固定係数2×2・16×12出力の中央値はgather 80,043 µs、融合 61,554 µs（23.1%短縮）、3×3係数broadcastはgather約252 ms、affine約182 ms（27.9%短縮）。可変係数2×2の5形状はgatherが選ばれた。生成表でAUTOが実機の固定ケースをPROFILE/FUSEDとして選ぶことを再確認し、診断後はフラッシュ退避を行わず通常版をverify-flashと`HOME_READY`まで確認して復元した。hostではPIEモデル有無の各400ケース、実測キーのPROFILE命中と未知形状のFALLBACK、JS→IR→PIE命令再生の19例・92ブロック、生成器の反例テスト5件を通した。計測はgrid kernelのみで、JS登録、UI再合成、LCD転送を含まない。未計測形状でのgather fallbackの損失と閾値境界は今後のholdout測定対象。

続いて同じ[JS fold前段](../../apps/kasane/grid_fold.js)から[2×2縮小式](../../apps/kasane/grid_fold_device_probe.js)を実機QuickJSで実行し、生成されたIRをCへ読み取って登録・bind・AUTO選択・PIE実行までつないだ。診断用JS文字列は[生成器](../../tools/kasane_contract/make_grid_fold_device_assets.py)が元のJSから作り、host検査で同期を確認する。最終ビルドを同一バイナリで3回測った中央値はQuickJS runtime作成5,540 µs、前段読込み31,787 µs、式からIRへのJS生成14,453 µs、IR読取り254 µs、native登録29 µs、bind＋選択44 µs。1024回のwarm実行は強制gather 80,052 µs、AUTO融合 62,094 µsで22.4%短時間だった。AUTOは3回ともkey=`0a123287ac2f3390`でPROFILE/FUSEDを選び、独立した2×2平均参照、scalar、gather、AUTOの全192画素が一致した。QuickJS解放後の内部heapは各回とも開始値へ戻った。これは診断専用の単一JS式であり、通常のJSアプリ公開API、UI再合成、LCD転送の測定には進んでいない。診断後は通常版をverify-flashと`HOME_READY`まで確認して復元し、フラッシュ退避は行っていない。

通常版MEGADEMOをCOM3のAPPSから2回起動し、[計測スクリプト](../../tools/kasane_contract/measure_megademo_menu_device.py)で各回とも全画面→Enter→小窓→Enter→全画面を測った。各 `KASANE_PAINT` は30描画フレーム平均で、起動直後と遷移直後の窓を除いた中央値は全画面でJS turn 3.73 ms、帯描画7.94 ms、LCD転送7.34 ms（8窓）、小窓で3.72 / 7.11 / 2.89 ms（6窓）、復帰後の全画面で3.73 / 7.90 / 7.34 ms（6窓）。転送量は全画面64,800 B、小窓16,128 B。Enter直後の最初の2窓は、縮小時の帯描画8.42–15.96 ms、拡大時11.14–13.08 msで、各窓に44フレーム遷移の一部と安定フレームが混ざる。連続する30フレーム窓の時刻差を30で割った中央値は33.35 ms（32窓、32.70–33.63 ms）で、単フレームの最大遅延はこのログからは分からない。すべて `prof=0` の通常ビルドで、画像・合成の詳細サイクルは採っていない。Back後の `APP_STOPPED` と `HOME_READY` を2回確認し、フラッシュ書込み・退避は行っていない。MEGADEMOの縮小は現在もKasane `STRETCH` で、grid foldのPIE経路を呼ばないため、先の22.4%短縮を通常アプリの改善としては扱えない。

## D3b grid PIE を通常アプリへ公開（2026-09-28）

[通常アプリ用 adapter](../../main/pocket/pocket_grid.c)を追加し、`gridFold.fold(...)` で作った型付きIRを `pocket.kasane.grid.register(program)` で登録、`run(handle, {0: Int16Array, ...}, params)` で実行、`resource(handle)` でKasane画像として `tx.image` に渡せるようにした。`explain(handle)` は選択したbackend・strategy・reason・profile keyを返す。アプリ固有の命令分岐は設けず、全アプリが同じ登録・合法性検査・AUTO選択・PIE実行を通る。JS前段は初回使用時だけ読み込む。planはセッション内で最大6件、出力は最大4096画素、入力合計は8192個のint16要素まで。入力は16-byte整列したnative領域へコピーし、出力は2世代を保持して表示ACK後に切り替える。転送失敗時は未確定世代を保持し、次の表示で修復する。

通常アプリの入力整列に合わせた固定係数2×2・16×12出力の追加probeを同一診断バイナリで3回実施した。各1024回の中央値はgather 80,073 µs、融合 61,590 µsで23.1%短い。profile key=`51df87624d6af998` を[選択表](../../main/ui/kasane/ksn_proc_grid_profile.h)へ追加した。選択表はbindで合法と判定された候補の順位だけを決める。[生成器のテスト](../../tools/kasane_contract/test_build_grid_profile.py)は、整列条件の不一致を拒否する例を含め6件通過した。[QuickJS結合試験](../../tools/kasane_contract/run_pocket_grid_qjs.py)は前段から画像画素と表示ACK・失敗修復まで通過した。

[GRID LAB](../../apps/kasane/grid_lab.js)を通常APPSメニューへ追加し、JSが32×24の入力を更新して2×2 foldで16×12へ縮約し、通常のKasane UIと画像を合成することをCOM3で確認した。2回の起動で各120フレーム以上進み、どちらも `backend=PIE strategy=FUSED reason=PROFILE`、終了後に `HOME_READY` を確認した。安定30フレーム窓の代表値はJS turn 12.83 ms、帯描画8.95 ms、LCD転送5.26 ms。これはアプリ全体の値であり、23.1%はkernel単体の比較である。通常版image（2,105,632 B、SHA-256 `7be2a5709db6986788818be7b82fcf9dfa61141fd0c95e0378f8e390b4f3f00d`）をアプリ領域に書込み、flash digest照合と `HOME_READY` を確認した。フラッシュ退避は行っていない。既存のMEGADEMOも2回起動・終了・再起動できたが、その縮小は引き続き `STRETCH` を使う。MEGADEMOのgrid化を主張するには、アプリ固有分岐を作らずに一般の縮小表現からこのAPIへloweringする設計と、その実測が別途必要になる。

## D3c 任意倍率のRGB565縮小実験（2026-09-28）

[登録型resizer](../../main/ui/kasane/ksn_proc_grid_resize.c)をgridの共通演算として追加した。アプリは `pocket.kasane.grid.registerResize({sourceWidth,sourceHeight,width,height})` で倍率を固定し、既存の `run(handle,{0:Int16Array})` と `resource(handle)` で毎フレーム異なるRGB565入力をKasane画像へ投影する。登録時に画素中心の座標と7-bit小数のバイリニア係数を準備し、実行時は8画素単位のRGB各成分をPIEの積和・QACCで計算する。端では参照座標を固定し、等倍はコピーする。リサイズは独立した登録済み演算であり、現行のaffine添字だけを持つfold IRから自動生成されるわけではない。`explain()` は `BILINEAR/EXPERIMENT` と返し、未計測形状の速度優位は保証しない。

[host試験](../../tools/kasane_contract/run_proc_grid_resize.py)は15:7、縦横異倍率、2:1、等倍、拡大、1画素入力でscalar・PIEモデルの全画素一致と独立した浮動小数点参照値との誤差上限を確認した。[QuickJS結合試験](../../tools/kasane_contract/run_pocket_grid_qjs.py)は登録、入力コピー、画像資源、表示ACK、転送失敗後の保持を通した。実機の診断バイナリを同一構成で3回実行し、60×30入力からの256回実行では、28×14（15:7）の中央値がscalar 182,919 µs / PIE 125,737 µs（31.3%短縮）、37×23が395,172 / 269,975 µs（31.7%）、30×15（2:1）が208,926 / 147,676 µs（29.3%）。各runで全画素一致と `GRID_RESIZE PASS` を確認した。別の小出力試験では8×1、8×4、16×8、16×16でもPIEがscalarより短時間だった。これらはkernelだけの値である。

[GRID LAB](../../apps/kasane/grid_lab.js)は60×30の動く入力を3倍率へ順番に縮小し、Enterでも切り替えられる。通常版COM3で2回起動し、各回300フレーム以上、3モードとも `PIE/BILINEAR`、終了と再起動を確認した。定常30フレーム窓のJS turnは15:7で約30.55 ms、異倍率で31.11 ms、2:1で30.64 ms。JSで毎フレーム1800画素を生成する費用を含むため、kernel単体の速度比とは区別する。[15:7のLCD送信前画像](grid-resize-15to7-device.png)の縮小後392画素は、hostのRGB565参照値と全点一致した。通常版imageは2,108,640 B、SHA-256 `f688482b3cb9e3316fab3582ee144afb82c3c6a773394907bdc75547328914fd`。診断後に通常版をアプリ領域へ書き戻し、flash digest照合と `HOME_READY` を確認した。フラッシュ退避は行っていない。

現在の上限は入力8192要素・出力4096画素・各軸256画素・セッション内4 planであり、MEGADEMOの240×135→112×63はまだ対象外。任意倍率の座標は表現できるが、全面画像を帯ごとに供給する入力・出力寿命と大きな出力の分割が必要になる。またバイリニア補間は強い縮小の低域フィルタではないため、細線のエイリアシング改善は面積フィルタなどで別途評価する。

## D3d 画像資源からの帯単位リサイズ（2026-09-28）

`registerResizeSource({source,width,height})` を共通のgrid APIとして追加した。登録済みの不透明・単一フレームKasane画像資源から必要な2行を読み、出力画像の要求されたspanをPIE QACCで計算する。元画像・縮小画像の全面バッファを作らず、行キャッシュと位相表だけを保持する。入力元の画像資源が更新されると依存する縮小画像も無効化し、表示失敗時には元の候補をそのまま読める。型付き配列版の8192/4096画素制限は維持し、資源版では各軸256画素まで扱う。hostのQuickJS結合試験は240×135→112×63について分割spanと行全体の全画素一致、画像更新後のキャッシュ破棄、通常のgrid実行との同居を確認した。新しいKasane coreの資源記述子参照は同一レイヤー内の登録済み画像に限る。

[MEGADEMO](../../apps/kasane/proc_megademo.js)はズーム中に可変矩形のSTRETCHを使い、112×63の固定小窓に到着した時だけ資源版バイリニアへ切り替える。専用のnative描画分岐は作っていない。host JS試験は全画面→小窓→全画面の可視画像切替を確認。COM3の通常ファームで2回起動し、両方で `FIXED_PIE` と `DYNAMIC_STRETCH` の切替、描画継続、HOMEへの復帰を確認した。30フレーム窓の定常中央値は固定小窓のJS turn 3.52 ms、帯描画13.16 ms、LCD転送2.91 ms（6窓）。従来のSTRETCH小窓は帯描画7.11 msで、今回のバイリニアは約85%遅い。全画面へ戻った後は帯描画7.94 ms。PIEを使うこと自体は速度上の採用理由にならない。補間画質と性能の比較、短いspanでのPIE起動費用、元画像行の読出し量、面積フィルタへの拡張を次に評価する。フラッシュ退避は行っていない。

初回実機版では `tx.image({visible:false})` が無視され、隠すつもりの固定小窓を全画面上に重ねていた。drawの生成時指定には `visible` がなく、直後に `monitorImage.setVisible(tx,false)` を発行するよう修正した。hostのKasane描画試験は、同一REPLACE内で追加・非表示にした画像について読出し0回、中央画素は下の全画面画像、資源更新の損傷領域なしを確認し、その後の表示切替も検証した。修正版をCOM3へ書込み、MEGADEMOの起動・縮小・復帰・終了を確認。起動直後の定常全画面帯描画は7.97 ms（修正前19.17 ms）、固定小窓は13.13 ms、復帰後全画面は7.92 msだった。修正後の値を以降の基準とする。

Astraレビューでは、先に二つの画像命令を登録して可視性を切り替える構成は現行APIの範囲で妥当と判断された。資源版は縮小済み画素のキャッシュではなく、描画時に元画像2行から毎回計算する遅延評価である。240×135→112×63では1出力画素が各軸約2.14入力画素を覆うため、2×2のバイリニアは厳密な縮小用低域フィルタにならず、細線のちらつきが残りうる。今後は元画像の行生成、span読出し、RGB565成分分解、PIE積和を分けて計測し、最近傍・バイリニア・面積フィルタを選べる一般の画像sampling policyを検討する。レビューで発見された、元画像2行の後者の読出しに失敗した時に古い行キャッシュが有効のまま残る問題と、patch失敗前にJS側の表示状態を更新する問題は修正し、hostで失敗からの読直しを確認した。

RGB565直接コピーをPIE縮小へ部分的に取り込んだ。8出力レーンの各4参照画素がすべて同色なら、そのブロックの補間・成分分解・PIEを省き元のRGB565語をコピーする。hostでは通常・同色・疎な輪郭の画素一致を確認。COM3で同じMEGADEMOを2回測ると、固定小窓の定常帯描画中央値は13.13→12.03 ms、全画面は7.97 msのままだった。さらに8レーン中の非同色が1～2画素だけなら、その画素だけscalar補間し残りを直接コピーする経路を追加した。2回の再計測では固定小窓が10.37 ms、全画面7.97 ms。最初のバイリニア版比で約21%、同色コピーだけの版比で約14%短い。参照4画素の取得はPIEにも必要で、追加の色比較と分岐はこの疎な線画では元が取れた。ただし従来の最近傍STRETCH小窓7.11 msには届かない。`PIE`と表示する資源版もブロック単位ではコピー・少数scalar・PIEの混合である。描画時間はUI合成と元画像行生成を含むため、kernel単体のPIE寄与は未分離。画素の意味はバイリニアから変えていない。

同色判定を各参照画素の `!=` から3つのXORとORへ変更し、8レーンの差をORしてからブロック全体を1回だけ判定した。非同色ブロックだけ各レーンの差を数える。hostの画素一致後、COM3の同じ2回のMEGADEMOでは固定小窓中央値10.28 ms（前版10.37 ms）、全画面7.96 msだった。0.09 ms差は各窓のばらつきと重なるため、この実機全体計測だけで高速化とは断定しない。XOR/OR、疎な輪郭の計数は残るので、比較命令を1本に畳むことと計算コスト0は異なる。

PIEの4tapを二組のQRへ先読みする案も比較した。[静的ストール解析](../../tools/pie/stalls.py)は15命令の積和列でload直後のQR依存ストールを0と見積もったが、同じ実機で15:7の256回はPIE 125,732→130,326 µsへ増え、scalarも182,900→192,214 µsへ増えた。C側のレジスタ割当て・コード配置を含む実機全体では改善しなかったため、この命令並びは採用しなかった。ストール数だけで選ばず、backend全体の再計測を優先する。

4係数ベクトルをQRに保持してRGBの3回で共有する案と、`t=fx*fy` から残るバイリニア係数を加減算で作る案を診断ビルドでA/B比較した。240×135→112×63、密な入力、32画素span、32フレーム相当の2回中央値は旧係数＋旧PIE 300,344 µs、旧係数＋QR共有 288,871 µs、1乗算係数＋旧PIE 295,607 µs、両方 285,067 µsで、画素は全点一致した。1行を32画素spanに分ける費用は行全体の呼出しとの差が約1%だった。一方、最初のQR共有実装はRGB全チャネルを一度に作業配列へ展開し、通常MEGADEMOの固定小窓を10.28→13.70 msへ悪化させた。1チャネルの作業配列を再利用する形に直しても、通常版の固定小窓はQR共有のみ10.45 ms、両案10.55 ms、候補を外した版10.44 msで、元の10.28 msを上回る改善は確認できなかった。通常経路は旧係数・旧PIEを維持し、候補は診断ビルドだけで比較可能にしている。

診断版MEGADEMOの固定小窓では、元画像2行の`read_span`が安定フレーム当たり約2.5 ms、縮小カーネルが約5.5–5.8 msだった。30フレーム当たりの8画素ブロックは代表窓で同色5,887、疎な1–2画素8,921、密なPIE 11,652。計測のサイクル読取りと診断ビルドの配置が全体時間を変えるため、この内訳を通常版の絶対時間へそのまま足し引きしない。次は密ブロックだけの命令削減より、縮小カーネルの内容別選択と元画像行生成の削減を検討する。最後の通常版はCOM3へバックアップなしで書込み・flash照合し、MEGADEMOの2回起動と全画面↔小窓、HOME復帰を確認した。

## 残作業の棚卸し（2026-09-28）

上の初期状態表は各試作時点の記録であり、後続のD3a〜D3dの段落が優先される。完了判定に必要な作業を次の順に進める。

| 段階 | 未完の判定 |
| --- | --- |
| D0 | 通常ファームのFLOWER＋overlayと、同一診断imageでのFLOWER＋トーン交互測定は下記。SD/decoder音声とoverlayを同時にした分布、同期LCD転送、通常imageでのpeak内部RAM・最大連続空きは残る。過去のhost秒数と古いimageの値を混ぜない。 |
| D1 | 旧・候補FLOWER frameと雨を実画面のpresent/repairへ接続。背景切替、全14種の固定shotのhost/実機hash一致、真の内部heap断片化による候補確保失敗からの旧確定再描画・次候補復帰を確認。音声・JS動画・通知を同時にした枯渇時の保持量と描画時間は別判定。単一`scene_mem`を複数面で共有しない。 |
| D2 | 動画＋透明深度線＋半透明SYSTEM通知の移動・消去、部分転送失敗、16 KiB heap予約とAPP終了は複合実機probeで確認。複数のJS手続き面とFLOWERを同時に入れた画素比較、真のOOM、画像資源の全寿命と矩形damageの量的整理は残る。 |
| D3/D3a | cubic Bézierのnative逐次分割と[複数曲線の逐次合成・動的パラメータ](d3-path-float-decision.md)はhostでscalar VM/登録済みplan同値。float→Q14の暗黙変換は半画素の丸めを変えるため棄却し、現行の描画float IRはscalar、独立した型付きQ14点列だけPIEを選ぶ。prefix/IIR PIEの実機費用は下記6形状で測定済み。一般pathの適応分割・塗り・stroke joinとfloat専用PIEの費用は未判定。depthはD4で扱う。 |
| D3d/D4 | 固定小窓のhost画質/元画像アクセス比較と明示的なnearest/bilinear選択を追加。内容からの自動選択、元画像行生成の削減、nearestの実機時間は残る。奥行き線と限定pixel-function画像はhost/実機試作済み。JS公開と安全なフレームlease、最大負荷の実機費用は残る。 |
| D5 | hostと実機でのローカルフレーム列、通常アプリのJS公開、音声プレーヤー位置からの選択まで試作済み。外部入力のdecoder/I/O、音声並行時の実機同期と欠落率、APP終了・中止の組合せを判定する。 |
| D6 | 動画・深度線・半透明UIの複合hostと、動画＋深度線＋SYSTEM通知＋16 KiB heap予約＋部分転送失敗＋APP終了の複合実機probeは済んだ。FLOWER・JS面・音声を同時にした通常アプリの画素、時間、入力応答、真の低heapを組み合わせ、機能ごとに採用・縮小・保留を判定する。 |

D1の最初の切出しとして、[FLOWER frame lease](../../main/scene/flower.h)はprepare済み部品・seed・GardenFrame・カメラ・grain・fadeを実際の部品数だけ保持する。描画時の深度行はlease内を書き換えずstack scratchを使い、従来の高速な行kernelをそのまま呼ぶ。`test_flower_frame.c`は14種について別フレームのprepareと`scene_mem_release()`の後に旧フレームを全画面・逆順8行帯・再描画で画素一致させ、通常の現フレーム状態も戻ることを確認した。O0/O2 hostと既存`test_flower.c`、ESP32-S3通常ELFのビルドは通過。hostでの二つのleaseは15,419 B＋11,099 B。これはまだ画面ループには接続しておらず、実機のRAMと時間の採否判定は残る。

D0の一部として、現行の通常ファームをCOM3で読取り専用計測した。FLOWER mode=3、HOME、8秒、2秒集計5窓ではfps 29.4〜30.3、`prep` 0.58〜0.60 ms、`kernel` 14.67〜15.62 ms、`draw` 16.85〜17.80 ms、`send` 0.46 ms。`async=1` の`send`は非同期表示呼出し時間でありLCD実転送時間ではない。音声はHOME復帰時のSFXが1回鳴っただけで、音声並行負荷・overlay・peak RAM/最大連続空きはこの計測で判定していない。ログは`.cache/flower_d0_baseline.log`（ignored）で、製品imageのhashを記録していないため後続の性能ゲートの正式基準にはまだ使わない。

D5の最初のhost試作として[3-slot RGB565 frame pool](../../main/ui/kasane/ksn_video_frames.h)を追加した。最大4096画素/slotの呼出し側バッファに、世代付き書込みhandle、明示的なpin/release、PTS以下の最新フレーム選択、古い到着フレームの不採用、枯渇時BUSYを持つ。画像portの`frame`番号はslotを選び、表示中のslotはpinで上書きから守る。`test_video_frames.c`は最新優先、旧handle拒否、pin、BUSY、遅延フレーム破棄を通した。`test_video_image_present.c`はKasane coreの画像ノードへ結線し、部分転送失敗後に同じ候補を再試行できること、候補を明示破棄した場合に旧フレームの画素でrepairすることをO0/O2 hostで確認した。通常ESP32-S3 ELFにも組み込みビルド済み。decoder、JS API、音声同期、実機の表示・RAM/時間はまだない。

D1のFLOWER frame leaseを同一診断image内でA/Bした。最初の2秒後にscene clockを固定し、2秒窓を2窓ずつ旧描画とlease描画で交互にした。旧側の定常`kernel`は16.97 ms、lease側は17.06〜17.07 ms、`draw`は19.12〜19.13 ms対19.27〜19.28 ms。各leaseは9,887 Bで2つ保持し、捕捉は43.5〜45.0 µs/フレーム、内部heapの観測最小値188,544 B、最大連続空き102,400 Bだった（後者2値はgrid診断解放後の窓であり製品RAM基準ではない）。ログは`.cache/flower-frame-ab-pinned-20260928/flower-frame.log`。診断imageは実験後に通常版へ復元し、flash verifyと`HOME_READY`を確認した。

この結果から、通常のFLOWER描画でも2つのleaseと対応する雨フレームを保持する経路を接続した。描画中は候補を読む。Kasane overlayの転送に失敗したら候補とscene clockを保持して再試行し、成功後に確定側を切り替える。背景切替時は両leaseを解放する。ESP32-S3通常imageのビルド後、COM3のアプリ領域に書いてflash verifyと`HOME_READY`を確認した。image SHA-256は`9cc77ae4e8cdf4a278f254db3089bfa2c684be582f6ac4741678481485531866`。FLOWER単独12秒計測では2秒窓のfps29.4〜30.5、`prep`0.77〜0.78 ms、`kernel`18.00〜19.80 ms、`send`0.40 ms。sceneが進行するため固定シーンA/Bの速度差と混ぜない。`send`は非同期呼出しでLCDの実転送時間ではなく、音声並行と低heapでのRAM下限の製品image実測はまだ必要。ログは`.cache/flower-frame-shipping-20260928/flower.log`。

同じ通常imageで、FLOWER上のdeskclock/music overlayを起動・終了して両方の135行のLCD送出画素を捕捉した。2秒窓のfpsは29.7〜30.0、overlay作成後の空き内部heapはdeskclock 139,324 B、music 128,276 B、6秒の動作で停止・panicなし。設定は元のmusicへ復元した。診断imageではmusicの静的help表示に対して3帯送出後の失敗を注入し、17帯repairの32,400画素が失敗前と完全一致した。さらにhelpを開かずFLOWERが見えるmusic画面で失敗を注入し、先に送った24行5,760画素とrepairで再送した同じ行が完全一致した（表示画素185色、静的helpとの差5,054画素）。後者はフレーム候補が部分転送中に変わらないことの実機検証である。診断後は通常imageへ復元・flash verify・`HOME_READY`を確認。記録は`.cache/flower-frame-shipping-20260928/overlay`、`.cache/flower-frame-repair-20260928/pixels`、`.cache/flower-frame-repair-dynamic2-20260928/pixels`。いずれの画素捕捉もSPI送出前でありLCD GRAM読戻しではない。

捕捉用の追加確保に失敗したときは、描画面を確定済みFLOWERと雨へ戻し、転送失敗時はそのまま再試行する経路を加えた。初回フレームに確定値がまだない場合は、同じlive値を再試行まで固定する。この低heap分岐の実機fault injectionは未実施。更新した通常image SHA-256は`49d4bdde08778753e32da24b6e39f99314eaa90107ac81fe74563d16d0ba995f`で、COM3へアプリ領域だけを書き込み、flash verifyと`HOME_READY`を確認した。退避は行っていない。

D5のローカルフレーム列も実機診断へ進めた。3×64×64 RGB565のpoolをKasane画像ノードへ登録し、色を変えるPATCH、2帯目の転送失敗からの同候補再試行、候補破棄から確定フレームへのrepair、30フレームの連続表示を通した。poolとcoreを確保中の内部heap空きは116,764 B、最大連続空き73,728 B。30フレームの表示時間は全幅転送で平均5,369 µs・合計1,036,800 B、矩形転送で平均3,794 µs・合計345,600 B（1フレームあたり11,520 B）だった。LCDへ同期送出した診断値で、decoder、JS、音声clock、入力I/Oを含まない。診断imageは各回の後に通常imageへ復元・flash verify・`HOME_READY`確認。ログは`.cache/video-stream-device-20260928/grid-measure-1.log`、`.cache/video-stream-rect-device-20260928/grid-measure-1.log`。

D2の2面手続き描画にも実機で2帯目の転送失敗を注入し、確定フレームの17帯・64,800 Bをrepairした。実機ログは`.cache/proc-layers-repair-device-20260928/grid-measure-1.log`。この診断はUI通知や半透明surfaceと同時ではなく、送出前バッファの画素比較もhostの契約試験に依存する。診断後は通常imageへの復元と照合を確認した。

D3のpath候補として、4つの制御点をregister 0..7から読み、1命令で最大64分割をnative生成する`KSN_PROC_CUBIC`を追加した。実行では線分順序を保ち、penを終点に置く。登録時解析は全8registerの読取り、penと描画の副作用、座標・線分数・ラスタ上限で失敗しうることを記録するので、既存の純粋算術fusionへ誤って混ぜない。1デバッグstepで曲線全体を生成することは意図的な境界で、各線分はフレーム上限を消費する。host O0/O2は逆順8行帯との画素一致、動的制御点、範囲外と上限を通し、実QuickJSの登録→描画→commitで制御点による画素変化を確認した。既存メガデモ48フレームの画素一致とPIE模擬経路も通過。実機では32分割のVMと登録済みplanの生成結果が一致し、単発のprepare 30 µs、VM run 81 µs、plan run 54 µsだった。単発値から速度優位は判定しない。実機ログは`.cache/proc-cubic-device-20260928/grid-measure-1.log`。診断後は通常imageへ復元・flash verify・`HOME_READY`確認。一般pathの曲線品質・適応分割やPIE化はこの試作からまだ決めない。

D4の限定した深度描画として、[depth line画像資源](../../main/ui/kasane/ksn_depth_lines.h)を試した。最大256線分・合計8,192ラスタstepのフレームをcallerが所有し、読み出し時は1行240画素の深度 scratch で小さいzを前に描く。未描画画素はalpha=0なので、Kasaneの通常UI画像ノードとして下地を透かす。等深度は先の線分を優先する。フレーム交換後の画像資源invalidateは表示者の責任。host O0/O2で交差線の前後関係、線分順序、透明部分の下地、フレーム交換後のdamageと上限を確認した。実機では64×64の透明画像をUIに表示し、変更時の矩形再描画は4,187 µs・11,520 B、部分転送失敗後のrepairは64,800 Bで完了。確保中の内部heap空きは132,700 B、最大連続空き86,016 B。ログは`.cache/depth-image-device-20260928/grid-measure-1.log`で、診断後に通常imageへ復元・flash verify・`HOME_READY`確認。これはwireframe用のnative kernelであり、任意の画素関数や一般3DシェーダをIRへ採用する根拠にはしない。

D5の通常アプリ入力として[`pocket_video.c`](../../main/pocket/pocket_video.c)を追加し、`pocket.kasane.video.open(width,height)`で単一の小さなRGB565画像資源を作る。`push(Uint16Array,pts_us)`は全画素を1呼出しで3-slot poolへコピーし、枯渇時は`false`でフレームを飛ばす。`select(clock_us)`は指定時刻以下の最新フレームをpinして画像を無効化し、`selectAudio()`は同じ選択を現行プレーヤーの`position_ms`から行う。Kasaneの表示ACKまでは候補・旧確定フレームを保持し、I/O失敗時は候補を読み直す。JS→pool→画像providerのQuickJS host試験はPTS、BUSY、型・サイズ拒否、音声時計、転送失敗時の再読出しを通した。音声時刻はミリ秒粒度で、音声タスクと同時に動かした測定ではない。外部decoder、SD/network I/O、任意の動画codecは未接続。

[VIDEO LAB](../../apps/kasane/video_lab.js)を通常APPSへ追加し、64×48の動画資源とKasaneの文字・背景を合成した。最初のJS画素単位生成は30フレーム窓で`turn_ms`約60.5 ms、描画約5.4 ms、送出約4.8 msだった。`Uint16Array.fill`による全面色と行内8画素範囲の更新へ変えると、2回の起動で各60フレーム以上進み、`turn_ms`1.75〜1.80 ms、描画5.39〜5.43 ms、送出4.81〜4.90 msになった。1フレームの転送量は表示矩形の13帯で29,952 B。JSで画素ごとの生成を繰り返す設計はこの解像度でも描画より重い。ログは`.cache/video-lab-device-20260928/video-lab.log`と`video-lab-optimized.log`。通常image 2,118,768 B、SHA-256 `19fb3c248067ca33e11f7bcceaca6fc832f24bd634bf6dae92ed421cc9fb0679`をCOM3のアプリ領域へ書き、flash digestと`HOME_READY`を確認。フラッシュ退避は行っていない。

D6のhost複合試験では、動画画像の上へ透明な深度線画像と半透明SYSTEM矩形を重ね、旧矩形からの移動・消去、動画フレーム変更中のLCD 2帯目I/O失敗と同候補の再送、明示破棄からの旧確定修復をO0/O2で確認した。[試験](../../tools/kasane_contract/test_video_image_present.c)はAPP/SYSTEMの順序と透明部分の下地も検査する。FLOWER・音声・通知を同じ実機時刻で動かし、低heapと転送失敗も同時に加える判定は残る。

D0の現行通常image（上記SHA）でもFLOWER＋deskclock/music overlayを同一セッションで計測した。deskclock稼働中の2秒窓はfps29.8/30.1、`prep`0.72 ms、`kernel`17.46/17.09 ms、`send`0.46 ms。music稼働中はfps29.4/29.7、`prep`0.70 ms、`kernel`17.61/16.68 ms、`send`0.48 ms。overlay確保後の内部heap空きはdeskclock 139,608 B、music 128,652 B。送出前画素は両overlayとも135行捕捉した。`--playback`は既存のSD許可がなく`PLAYBACK_SKIP new folder grant required`となり、音声同時負荷は判定できなかった。`send`は非同期の呼出し時間でLCD実転送時間ではなく、最大連続空きもこの通常imageログにはない。設定は元のmusicへ戻した。ログは`.cache/d0-current-overlay-20260928`。

D5/D6のAstraチェックポイントレビューで、動画資源の既定の`sourceWidth/Height`が64×64に固定されていたこと、登録失敗した`video.open()`がpoolを残すこと、音声の巻き戻しが旧PTSを越えるまで動画を止めることを発見した。画像の既定寸法は登録済みportから取得し、登録失敗時は新規poolを破棄する。3-slot poolにはPTS epochを追加し、`resetTimeline()`で旧確定画像をpinしたまま新しい時間軸へ移る。`selectAudio()`はプレーヤーIDの変更・再生位置の後退で同じ切替を行う。既に`push()`した新epoch最初のフレームは自動切替時に破棄されるため、シークを指示したアプリは次フレームを再投入するか、先に`resetTimeline()`を呼ぶ。host試験は登録失敗から異なる寸法の再open、逆方向シーク、旧確定フレーム保持を追加。実機のVIDEO LABから明示的なsource寸法を外し、2回の起動で各60フレーム以上を確認した。`turn_ms`は1.74〜1.78 ms、描画5.40〜5.44 ms。レビュー後の通常image SHA-256は`3d83c549f6295a23b18e55f46c7ef7c1c7989debd53381e37ffc8e35984388a3`で、COM3のアプリ領域だけを書き換え、flash digestと`HOME_READY`を照合した。JS adapterと本物のKasaneを同じhostテスト内でつなぐ検証、APP teardown中のI/O失敗とSYSTEM保持は残る。

D3aの依存行PIEに[診断専用probe](../../main/ui/kasane/ksn_grid_scan_cost_device_probe.c)を追加し、prefix/IIR各3形状を同一実機binaryでscalar VMとPIEの順序を交互に変えながら各512回実行した。各runで全出力一致し、3runすべて`GRID_SCAN PASS`。中央値はprefix 8×8が78,467→9,269 µs、32×16が623,482→71,898 µs、64×17が1,316,285→171,757 µs。IIRは89,934→9,485、715,332→73,673、1,511,484→175,186 µs。6形状の短縮は87.0〜89.7%。これは同じseed列と有界入力で繰り返したkernelの累積時間で、JS、画像合成、LCD、登録費用は含まない。一般の依存行式への自動適用閾値はこの6例だけで広げない。診断image SHA-256は`7f7b1ed5e33c5aac22bbaac09a1d57a5b91c1625094340be1710a8176c0e4948`。ログは`.cache/grid-scan-cost-20260928/grid-measure-1..3.log`。試験後は上記通常imageへ復元・flash verify・`HOME_READY`確認。退避なし。

D1の候補frame確保失敗分岐を診断フラグで実画面へ注入した。最初の確定ACK後、120 FLOWER frameごとに候補を破棄して再確保を省くと、12秒の観測で2回とも`candidate=0 committed=1 fallback=1`を記録し、描画は停止しなかった。これは低heapそのものを再現した試験ではなく、確保失敗後に確定frameへ戻る制御分岐の確認である。ログは`.cache/flower-alloc-fault-20260928/flower.log`。診断imageの後、通常imageへアプリ領域だけを復元してflash verifyと`HOME_READY`を確認した。退避なし。

D3dの固定小窓240×135→112×63を[host sampling比較](../perf/kasane-grid-sampling-host.md)で調べた。16画素span分割では描画器の最近傍は504 span/14,490画素、現行bilinearは126行/30,240画素。面積平均を基準にしたRGB 8bit相当の平均絶対誤差は、1画素線が最近傍6.633/bilinear5.055、勾配0.620/0.747、チェッカー126.933/33.573。内容によって画質と行生成費の優劣が変わるので、単一のエッジ量で自動選択する根拠はない。`registerResizeSource`へ汎用の`sampling: 'nearest'|'bilinear'`を追加し、既定はbilinearのままとした。nearestは描画器と同じ画素中心の整数写像を使い、任意の出力spanから必要な元画像の連続spanだけを読む。実QuickJS→画像portのhost試験は異なる帯境界で全画素一致し、16画素span×63行で441 provider呼出し/14,616画素、元画像更新のinvalidate、I/O失敗後の再読出しを確認した。描画器の504回と異なるのは画像portが32画素scratchの制限を持たないため。`explain`はこの経路を`NEAREST`/`scalar`と報告する。PIE実機時間や動画の時間的画質はまだ測っていない。

D4の[限定pixel-function画像](../../main/ui/kasane/ksn_pixel_function.h)を通常ビルドへ追加した。最大24命令、8 register、8入力、1フレーム60万pixel-instructionの直線プログラムをbind時に検証・コピーし、`x/y`、入力、RGB565の明示的な下地スナップショットから色とalphaを作る。下地はlive合成帯ではなくcaller所有の不変画像で、表示中のbindは禁止する契約である。1行分の色/alpha scratchをcacheし、帯の再読出しと順序変更を許す。O0/O2 hostで不正な参照・上限拒否、bind後の元命令変更からの隔離、下地参照、透明行、Kasane画像ノードでのinvalidate後の画素更新を確認した。COM3の診断imageでは64×64の関数画像をKasaneへ表示し、更新時の2帯目転送失敗から同じ候補を再送した。`PIXEL_IMAGE PASS`、repairの64,800 Bは11,264 µs、観測時の内部heap空き109,756 B・最大連続空き65,536 B。ログは`.cache/pixel-function-device-20260928/grid-measure-1.log`。診断後は通常imageへ復元・flash verify・`HOME_READY`確認。通常image SHA-256は`e355f19009bbc0b327f9e8293742fe1d502a352d753ce2001a8db4096f274a5f`。この段階はC画像portの試作で、JSへの公開、最大命令数での実機行時間、候補/確定leaseによる安全な更新、任意の画素プログラムのPIE化は未検証。

D0の音声並行を、SD許可を必要としない診断トーンで一部確認した。同じ診断imageのFLOWERで2秒窓ごとに440 Hz・1.9秒のトーンを交互に要求し、観測中の5要求すべてで正のIDを得た。4つの音声あり窓を含むウォームアップ後の8窓では、音声要求あり/なしの両方でfps29.4〜30.2、`prep`0.78〜0.80 ms、`kernel`18.11〜20.29 ms、内部heap最小空き177,872 B、最大連続空き81,920 B。scene内容が進行するためこれらの幅をトーン費用と解釈せず、音声タスクが動く間も30fps近辺で止まらないことを確認した。最初の窓のheap最小値129,632 Bは起動時の一時使用が混じるため定常値から分ける。ログは`.cache/flower-audio-20260928/flower.log`。音量設定や物理出音は測っておらず、SD/decoderの音声負荷もこの試験に含まない。診断後は通常imageへ復元・flash verify・`HOME_READY`確認。退避なし。

D3dのAPI追加後の通常image SHA-256は`66d00a686c4eea373d3682f25ae3667ed00abb27268d2f4a4d0c651717a28e04`。COM3へアプリ領域のみを書いてflash verifyと`HOME_READY`を確認し、通常MEGADEMOの全画面→固定小窓→全画面、VIDEO LABの2回の60フレーム進行と終了が通過した。MEGADEMOの固定小窓の`render`中央値は10.44 msだった（新しいnearest指定は未使用で、従来のbilinear経路）。ログは`.cache/d3d-normal-20260928`。D0トーン診断の後もこの通常imageを復元・照合した。

診断フラグを追加した後に通常imageを再ビルドし直した最終SHA-256は`9b61fb5857eaeef49429ce2ebd999cf851ed98d83fd1eaa514831bb722271dc8`。COM3のアプリ領域だけを更新し、flash digest一致と`HOME_READY`を確認した。診断フラグは通常imageではOFF。ログは`.cache/d0-final-normal-20260928`。

D2/D6の複合実機probeでは、動画30フレーム計測の後、同じKasane coreで動画64×64、透明深度線16×16、半透明SYSTEM通知を重ねた。16 KiBの内部heapを予約した状態で深度線の画素、通知下の動画画素、2帯目LCD失敗からの同候補再送、通知移動後の旧bounds復元、APP layer reset後のSYSTEM通知保持を確認した。`COMBINED PASS`時の内部heap空き64,784 B、最大連続空き31,744 B。初回probeは`PATCH`でノードを追加しようとして`KSN_INVALID`になった。Kasane coreの契約どおり`REPLACE`で動画と深度ノードを組み直し、以後の動画frame値だけ`PATCH`へ変更して通過した。ログは`.cache/combined-device-replace-20260928/grid-measure-1.log`。これは16 KiBの人工予約であり、allocation失敗やdecoder/音声負荷を伴う真の低heapではない。diagnostic後は通常imageへ復元・flash verify・`HOME_READY`確認。退避なし。

チェックポイントの敵対的レビュー後、pixel-function画像が登録済み寸法と異なるframeへbindできる問題を修正した。Kasaneの画像portは登録時に寸法を保持するため、再bind時は同じ幅・高さだけを許す。host O0/O2で拒否と旧画素の保持を確認した。D0音声診断のrunnerも、`available=1`と正のtone request IDを全窓の合格条件へ追加した。D2/D6複合probeでは送出画素が実際に標本位置を通ったことと、通知半透明合成値、通知移動後の旧画素、APP破棄後のSYSTEM画素を正確なRGB565値で検査し直した。COM3で`COMBINED PASS`、`VIDEO PASS stage=12`、`ALL PASS`。ログは`.cache/combined-device-hardened-20260928/grid-measure-1.log`。診断後は通常image SHA-256 `9b61fb5857eaeef49429ce2ebd999cf851ed98d83fd1eaa514831bb722271dc8`へ復元・flash verify・`HOME_READY`確認。寸法guardは通常imageでは参照されない診断用関数なので、通常imageを再ビルドしてもこのhashは変わらなかった。

D1の通常アプリ背景切替は、保存されていたFLOWERからLEVEL WAVE→FLOWER→OCEAN + STARS→FLOWER→LEVEL WAVE→FLOWERを実行した。各FLOWER復帰で`PERF mode=3`が再開し、設定は最初のFLOWERへ戻った。runnerは失敗時も元の選択への復帰を試みる。ログは`.cache/flower-switch-20260928/switch.log`。全14種のFLOWER speciesを個別に固定した画素照合や真の確保失敗試験とは区別する。

D5の[QuickJS動画試験](../../tools/kasane_contract/test_pocket_video_qjs.c)を実Kasane coreとrendererまで接続した。4×2のJS投入画像をAPP画像資源として登録し、SYSTEM通知と合成する。最初の転送を2帯目で失敗させるとAPP resetは`KSN_BUSY`を返し、同じ候補を再送後に動画画素と通知画素が一致した。次のJS選択で画像invalidateによる更新を確認し、最終APP reset後は動画だけ消えてSYSTEM通知が残った。従来のPTS、音声時刻、型・枯渇のQuickJS検査も通過した。この試験はhostのstub glueで登録・invalidateをKasane coreへ接続したもので、実機の音声clockとdecoderを同時には実行していない。

### 2026-09-28 の段階判定

| 対象 | 判断 | 根拠と次に必要な検証 |
| --- | --- | --- |
| FLOWER の候補/確定frame lease | 通常経路に採用 | 通常アプリの背景切替、2帯目失敗からの同画素repair、14種の固定shotと真のheap断片化による候補確保失敗・旧確定再描画・次候補復帰を通過。複合負荷中の同時枯渇は別判定。 |
| APP 内のnative描画面と登録時IR/PIE | 限定したkernelを採用 | JSメガデモ、2面合成、深度線、prefix/IIR、積和と点列はhostと実機で同値。一般path、float画素IR、任意式のPIE化まで拡張する判断は保留。 |
| 固定小窓の縮小 | bilinearを既定、nearestを明示指定として採用 | 実機4区間でnearestの描画時間は約半分。実MEGADEMOのhost画質proxyでは空間・時間の誤差が増えたため、自動選択しない。LCD実画素の動画評価は残る。 |
| 小型RGB565動画frame列 | 通常APIとして採用 | `VIDEO LAB`と実機複合probe、JS→Kasane host試験で画像寿命と修復を確認。codec/外部stream、SD、音声同期の同時実機試験は別に必要。 |
| pixel-function画像 | 小面積C画像portの試験採用、JS公開は保留 | 候補/確定leaseはhost確認。全画面18命令は実機で103.7ms、全画面underlay2枚も内部RAM不足。backendと保持方法の再設計が必要。 |
| 全機能同時共存 | 診断トーンまで実機確認 | FLOWER＋JS動画小窓＋半透明APP UI＋SYSTEM通知＋人工16 KiB予約＋LCD失敗/修復を、受理された診断トーンの再生中に確認。実音声decoder・外部動画stream・深度線まで同時に加えたRAMと長時間の入力応答は未測定。 |

D6の追加診断は既存のホームKasaneオーバーレイを使い、FLOWERのnative帯をbackdrop loaderから読み、その上でJSが32×8 RGB565動画を`video.push/select`し、半透明APP矩形と文字を重ねた。診断フラグ`KASANE_D6_VIDEO_OVERLAY_PROBE`は通常版ではOFFで、元のデスククロックを一時的にこのJSへ差し替えるだけである。最初のビルドはフラグを別の診断オプションの内側に置いたため通常デスククロックが走り、次は`replace`の必須背景指定が欠けて起動を拒否された。両方を修正した後の実機9秒観測では26回の`D6_VIDEO_FRAME`報告がtick 260まで進み、BUSY・停止・panicなし。FLOWERの2秒窓はfps29.3〜29.9、オーバーレイ起動前後の内部heap空きは177,996→126,544 B、5秒の健康判定`worst=3339us`。これはFLOWER描画と小型動画JS turnの共存確認で、固定30fps保証や音声負荷の値ではない。[LCD送出前の240×135画素](d6-video-flower-device.png)では花の背景と右上の動画小窓・半透明領域が同時に見える。ログは`.cache/d6-overlay-video-bg-20260928/serial.log`。診断image SHA-256は`f8beac91074a8985dca6a58591f84a27010d4489cd1e5d156aaad5ff4ce622a00`。試験後は元のMUSIC設定を復元し、通常image SHA-256 `9b61fb5857eaeef49429ce2ebd999cf851ed98d83fd1eaa514831bb722271dc8`をアプリ領域へ戻してflash digest照合と`HOME_READY`を確認した。退避なし。

同じD6動画overlayへ既存のP0診断を組み合わせ、SYSTEM通知、16 KiBの内部heap予約、LCD失敗注入を同時に与えた。P0のUSBキー予約と手続きprobeの`J`が設定操作・通知操作と衝突したため、D6診断だけで使う別キーを追加してから実行した。9秒の基準窓で動画tick 260まで進み、FLOWERは29.5〜29.8 fps。予約直後の内部heap空き96,232 B、最大連続空き55,296 B。通知の`POST result=0`、`COMPOSITED 1`、転送失敗`y=72 after=3`、17帯64,800 Bの`REPAIR_OK`、通知消去の`COMPOSITED 0`、予約解放を順に確認した。[LCD送出前の通知付き画素](d6-video-flower-notice-device.png)にはFLOWER背景、右上の動画小窓とSYSTEM通知が同時に写る。ログは`.cache/d6-overlay-combined-keys-20260928/serial.log`。診断image SHA-256は`9066da9abf839972b8a1e7c9e83bc15df8528aa99c4aa446e92f68ca86318a3a`。診断中に音声decoderや深度線を動かしておらず、LCD GRAM読戻しも行っていない。元のMUSIC設定を復元し、再ビルドした通常image SHA-256 `45a4acf0e1e75135b0e86bf971f78a725417df0fe6479a08df144b676b3996ca`へアプリ領域だけを戻し、flash digestと`HOME_READY`を確認した。退避なし。

最後に同じ複合診断へFLOWERの440 Hz診断トーンを足し、転送失敗の注入を次の正の`tone_request`直後に揃えた。通知と16 KiB予約は先に有効化し、ログ上はトーン要求15,533 ms、LCD失敗15,558 ms、修復15,600 ms。要求する1.9秒のトーン再生区間内で、動画はJSから更新され続け、SYSTEM通知も合成されていた。基準9秒窓は音声あり2窓、動画報告tick 260、FLOWER fps29.6〜29.8。予約後の内部heap空き96,232 B、最大連続空き55,296 B、修復17帯64,800 B。ログは`.cache/d6-overlay-audio-overlap-20260928/serial.log`。これはトーン要求の受理と並行動作の確認であり、物理出音、codec/SD負荷、音画同期誤差は測っていない。診断終了時には通知と予約を解除し、元のMUSIC設定と通常ファームを復元してflash digestと`HOME_READY`を確認した。通常ファームの最終ビルドSHA-256は`f656040d077100bb9b0779cd0a75ab379f2471b94704bea7bca4459b3fca71e9`、診断専用フラグはOFFである。

この最終通常imageでAPPSからMEGADEMOを起動し、全画面→固定小窓→全画面が通った。小窓の`render`中央値は10.44 ms（3窓）。VIDEO LABも2回起動して各60フレーム以上進み、終了後に再起動できた。ログは`.cache/d6-final-normal-20260928/megademo.log`と`video-lab.log`。縮小のnearestはこの通常appでは指定されていない。

次の採否に必要な未実測は、外部動画stream/decoderと音声clockの同期誤差、複合負荷中のFLOWER確保枯渇、縮小動画のLCD実画素評価、pixel-functionの実Kasane合成内の小面積閾値と保持方式である。実SD MP3 decoderとJS動画・FLOWERの同時負荷、およびFLOWER単独の真の候補確保失敗と全14種の実機固定shotは後述の診断で測定した。一般pathや任意画素式のPIE化は、ここまでの限定kernelの結果だけで汎用APIに広げない。

D6のチェックポイントレビューで、最初の複合診断runnerは通知・予約・修復の応答だけで合格でき、複合負荷中の動画進行を再検査していないことが分かった。また画素capture中の非画素ログを捨て、トーン区間との重なりを合格条件にしていなかった。runnerを修正し、capture中の診断ログを保持して停止・BUSY・panicを拒否、複合区間のFLOWER `PERF mode=3` と修復後の新しい動画tickを要求、正のトーン要求IDと実機タイムスタンプで1.9秒以内の失敗・修復を照合する。復元後の`HOME_READY`確認も未追跡cacheファイルから[追跡対象のhelper](../../tools/kasane_contract/check_home.py)へ移した。

修正したrunnerのCOM3再測定では、9秒基準窓で動画26報告・tick 260、FLOWER約29.5〜29.6 fps。16 KiB予約・SYSTEM通知・LCD失敗中に受理されたトーン要求の25 ms後に失敗、67 ms後に修復し、修復後の動画tick 330とFLOWERの新しい2秒性能窓（30.0 fps）を確認した。通知付きの135行画素captureも完走し、`D6_COMBINED_PASS`。ログは`.cache/d6-overlay-finalgate-20260928/serial.log`。終了後に元のMUSIC設定、通常ファームのアプリ領域を戻し、flash verifyと`HOME_READY`を確認した。フラッシュ退避なし。ここでも実音声decoder、SD、外部動画stream、物理出音、LCD GRAM読戻しは含めていない。

D4のpixel-function C画像portへ2スロットの候補/確定poolを追加した。stageは命令と必要なunderlay画素を別スロットへコピーし、表示中の確定slotを上書きしない。転送失敗後は候補を保持して同じ画素を再読出し、候補破棄後は旧確定のrepair通知まで再stageを拒否する。hostの実Kasane rendererで2帯目失敗/再試行、入力underlay変更後の固定画素、旧ACK拒否を確認した。240×135×18命令（583,200 pixel-instruction）のhost参考時間は64回平均1.656/1.688 ms/frame、pool本体1,928 Bと全画面underlay 2枚で129,600 B。後者はこの端末の内部RAMへ安易に置ける量ではないため、実機のRAM配置と最大命令時間を測るまでは診断APIに留め、JSへの公開を保留する。

D1の既存lease試験から全14種の固定RGB565ショットを出力し、[一覧](flower-14-species-host.png)として目視確認した。14枚のhashはすべて異なり、花と茎が画面内に描かれている。これはhostで各speciesを直接prepareした結果で、実機の回転・fade・低heap時の見え方まで保証するものではない。再生成は`test_flower_frame.exe <出力ディレクトリ>`と[結合スクリプト](../../tools/kasane_contract/render_flower_species.py)を使う。

D3dの通常MEGADEMO固定112×63小窓を、同一診断imageでbilinear/nearestを交互に4区間測定した。各区間はtick 46へ位相を揃えた480フレームで、内部の30フレーム窓を14個採用。`render`中央値はbilinear 10.975/10.980 msに対しnearest 5.635/5.635 ms（48.7%短い）。JS turn約3.69 ms、LCD `send`中央値2.90 ms、転送16,128 Bは同程度。実MEGADEMOの48フレームをhostで面積平均を基準に比較すると、空間RGB 8bit平均絶対誤差はbilinear 17.978/nearest 33.352、時間差の誤差は17.820/33.062、基準の変化が小さい画素で30階調超の跳ねは458/1,783件。したがって既定はbilinear、nearestは速度重視の明示指定とする。実機時間は[再実行runner](../../tools/kasane_contract/measure_resize_sampling_device.py)、画質proxyは[host runner](../../tools/kasane_contract/run_megademo_temporal_sampling.py)で再現できる。ログは`.cache/kasane-d3d-sampling`。診断用JSは元のバイト列へ戻し、通常image SHA-256 `f656040d077100bb9b0779cd0a75ab379f2471b94704bea7bca4459b3fca71e9`をアプリ領域へ復元してflash verifyと`HOME_READY`を確認した。LCD実画素の動画撮影・主観的画質評価はまだ行っていない。

D0の実SD音声を通常ファームのMUSIC overlayから再生した。このプレイヤーのフォルダ許可はセッション単位なので、既に承認されていたSD rootの`music`フォルダを新セッションで再選択し、`sd:/KAKATO/KARA OK 2nd Edition/01 KAKATORO.mp3`を開いた。LCD captureを伴う初回試験ではデコード250 packets/156,735 frames、fault 0で、FLOWERと再生画面の前後画素を135行ずつ取得した。ただしcapture中のfps窓は15〜16台へ下がるため、再測定では再生中10秒間にcaptureを行わなかった。MP3DECは387 packets/242,689 frames、fault 0。再生開始を含む最初のFLOWER 2秒窓24.3 fpsの後、4窓は30.0/29.9/29.8/29.7 fps、`kernel`15.29/15.07/14.57/14.91 ms。終了後にoverlay設定を元のMUSICへ戻した。ログは`.cache/d0-sd-playback-granted-20260928/serial.log`と`.cache/d0-sd-playback-steady-20260928/serial.log`。これは実MP3 decoderとFLOWERの並行確認であり、物理スピーカーの音量や音画同期誤差の測定ではない。

D4の最大構成をCOM3の診断probeで測ると、240×135×18命令の行生成は8回の平均103,733 µs、最短103,722 µsであり、33.3 msの1フレーム予算を大きく超えた。hostの約1.7 msを実機の見積りに使えない。また内部heapの試験開始時空き119,708 B、最大連続73,728 Bでは64,800 Bのunderlayを2枚確保できず、1枚後の空き52,244 B・最大連続31,744 Bで`underlay_alloc`失敗となった。プログラム本体と小型poolの試験、既存表示/PIE/video/depth試験は通ったが、この最大構成を`ALL PASS`にはしない。ログは`.cache/d4-pixel-work-20260928/grid-measure-1.log`。診断後は通常imageをアプリ領域へ戻し、flash verifyと`HOME_READY`を確認。現状の直線pixel interpreterは小面積の関数画像に限定し、全画面の任意関数と2枚の全画面underlayはJS公開せず、backendと保持方法を再設計する。

D4チェックポイントのAstraレビューは、候補ACKだけではKasaneの保持シーンから旧nodeが消えた保証にならないことを指摘した。旧frame番号を参照するnodeが残ると、そのslotを再利用した後に別世代の画素を読む恐れがある。現行poolは同一資源の全nodeをまとめて次世代へ置き換える用途だけに制限し、一般APIではシーン参照を追跡したleaseが必要。60万pixel-instructionの制限も「ソースを1回生成する上限」であり、損傷修復や再読出しを含む1表示frameの実行上限ではない。実装方針はJSの純粋な式を型付きDAGへ取り込み、定数・frame・行の不変式を外へ出し、残りを命令ごとにspan全体へ実行する。任意JS callbackはproducer側で有界に画素化し、完了画像をatomicに公開する。underlayは必要領域だけを不変versionとして共有し、保持シーン・pending・repairの全参照が消えるまで再利用しない。

このレビューを受け、`finish`の成功ACKには呼出し側が旧世代を参照する全nodeの退役を確認した真偽値を必須にした。hostでは同一資源を参照する2 nodeのうち1 nodeだけを更新した場合のACK拒否、旧nodeの再描画、2 node更新後のACKを確認した。全画面試験の時間表示は33.3 msを超えたら明確に`FAIL`とし、2帯目での転送失敗を実Kasane rendererで注入した。

さらに命令を画素ごとに切り替える代わりに、命令ごとに1行の全画素へ適用する`ksn_pixel_span_eval`を試作した。hostで180種類の検証済みプログラム×30の順不同span読取りが従来経路と一致。実機の240×135×18命令（583,200 pixel-instruction）は従来平均103,734 µsに対しspan平均23,374 µs（8回、最短23,369 µs）。従来経路の小面積例は64×64×6命令で5,127 µs、112×63×8命令で11,090 µs。span単独で33.3 msを下回っても合成・LCD・JSを含む表示全体の30 fps保証ではなく、8 register×240画素＝3,840 Bのstack scratchは短いspanでも消費する。通常rendererのstack/call経路で未測定なので通常描画には接続しない。命令の構造的な不変式除去も残る。内部RAMの全画面underlay 2枚は今回も2枚目で失敗し、PSRAM capabilityの空きは0 Bだった。`PIXEL_WORK`/`ALL`は引き続き不合格とする。ログは`.cache/d4-pixel-span-device-20260928/grid-measure-1.log`。診断後に通常imageをアプリ領域へ戻してflash digest一致と`HOME_READY`を確認し、退避は行っていない。

D6では、FLOWERと32×8のJS動画overlayを動かしたまま、既に許可済みのSD `music`フォルダを新しいセッションで再選択し、実MP3 `sd:/KAKATO/KARA OK 2nd Edition/01 KAKATORO.mp3`を10秒再生した。動画はtick 10→300へ30回進行し、SD picker操作を含む最初のFLOWER性能窓は23.8 fps、安定した後の4窓は29.7/29.8/29.9/29.7 fpsだった。MP3 decoder終了統計は393 packets、246,387 frames、fault 0、decode平均4,154 µs・最悪5,434 µs。LCD captureは行わず、性能窓への観測負荷を避けた。初回SD mountは`0x108`の失敗ログを出した後に再試行で許可・再生へ進んだため、mountの頑健性は別途見る。ログは`.cache/d6-sd-mp3-video-20260928/serial.log`。元のoverlay設定と通常imageを戻し、flash digest一致と`HOME_READY`を確認。フラッシュ退避なし。この診断は物理スピーカー出音、動画PTSと音声clockのずれ、外部動画decoder/stream、LCD GRAM読戻しを測っていない。

D4の次段では、112×63×8命令の可変色・alpha画像を実KasaneのAPP画像ノードへ接続し、移動する半透明矩形とともに120フレーム表示した。従来の画素ごとの解釈と命令ごとのspan実行で、LCD送出順・矩形座標・フレーム境界を含む画素digestが一致。1フレーム平均は従来18,364 µs、span 10,081 µs、評価時間だけでは11,617→3,378 µs。LCD送出は約2.53 ms、digest検証の約0.675 msは別計測。2帯目転送失敗、同じ候補の再試行、別候補の部分送出後の破棄と旧確定フレーム修復、さらに修復後の新候補公開を両経路で通した。送出済み第1帯の画素は破棄候補0x009cから旧確定0x00a0へ復元し、半透明overlay内の標本と障害区間digestも一致した。ログは`.cache/d4-span-scene-device-20260928/grid-measure-1.log`、診断image SHA-256 `1bbca3d1dc1f3cb9b1f9dd93042381471b59d7d512abdad783de85ffc8ee346f`。終了後は通常imageを復元しflash digest一致と`HOME_READY`を確認。これは小窓・単一画像ノードの採用判断を支えるが、全画面・複数保持node・下地2枚・任意JS式の性能を保証しない。

D5には外部ファイル入力の小さな試作として[KSV1ファイルstream](d5-file-stream.md)を追加した。workerはSDの持続read leaseを1本使い、1回に最大8レコードのヘッダだけを走査して期限内の最後のRGB565画素だけを読み、owner turnが既存3-slot poolへ最大1フレームをコピーする。Astraレビューで見つかった短読取り時の混合画素publish、HOME待機中の遅いACK回収漏れ、lease失効後の`reading`残留を修正した。hostの境界/PTS/短読取り/候補repairと実QuickJS API試験、通常ファームのESP-IDFビルドが通った。診断JSが一度に31,456 Bを生成した初回実機runは`RUNAWAY`制限で停止したため、1レコードずつ実タイマーで分離して再試験した。修正版は2回とも31,456 BのSDファイルを作り、各54フレームを選択（PTS 0→1,966,647 µs）、EOF、worker ACK（8/9 µs）、ファイル削除、2回目の再作成まで通過。workerのstack freeは2,020/1,980 B。ログは`.cache/d5-sd-stream-yield-device-20260928/serial.log`。診断image SHA-256 `6ffba162b99d957a8010bf1863d9b4bda7c7aac5bded2566b5b7fbddee1a6bee`、通常image SHA-256 `170479264c237572472c0dc2153a345bcd15aae15a68ba4ca36647cdc46f0d23`へ復元しflash digest一致と`HOME_READY`を確認。フラッシュ退避なし。短読取りのworker競合、200 ms超ACK、外部revoke、LCD失敗との同時発生、実MP3との共存は実機未注入である。

D6の次の実機probeは専用`KASANE_D6_SD_AV_STREAM_PROBE`でFLOWER背面の小さいoverlayへKSV1を表示し、同じSDから既知のMP3を再生する。初回runではoverlay内で300フレームを生成し、50フレーム後に`OVERLAY_STOPPED OVER BUDGET worst=180307us`となった。通常imageは復元・verify済み。修正版は全画面VIDEO LAB helperでKSV1を作成し、`commit()`でhandleを完全closeしてから、小さいoverlayで持続video leaseとMP3の再open readerを始める。GRID LAB helperは実験前と終了時に予約ファイルの全バイトまたは一致する途中prefixを検証して削除し、不一致の衝突は保持して中止する。`streamPoll()`のclockは`player.status().positionMs * 1000`を毎frame渡し、native SELECTログのPTS/clock差で音声位置への追従を測る。ファイル生成中とMP3+KSV併走中のFLOWER FPS・内部heap窓は別々に集計する。hostでは書込み粒度、衝突拒否、音声clock選択、完全・部分ファイルのexact-content cleanupを確認した。実機結果は下記。これは表示frameの選択時刻を音声位置と照合する試験であり、物理出音の位相やcodec由来の音画同期は測らない。

この更新後の通常imageでは、VIDEO LABを2回起動して`VIDEO_LAB FRAME 60`とKasane描画を確認した。MEGADEMOも通常全画面→ニュースセットの縮小小窓→全画面を1往復し、各区間の画面更新を確認。ログは`.cache/d5-normal-video-lab-20260928.log`と`.cache/d5-normal-megademo-20260928.log`。診断差し替えフラグは通常imageではOFFで、MEGADEMOと通常VIDEO LABは引き続き起動できる。

D6 SD動画＋MP3の修正版をCOM3で実行した。事前cleanupが予約ファイル不在を確認し、通常アプリがKSV1の300フレーム・157,216 Bをcommitしてwriterを閉じた。その後FLOWER上でSD動画readerとSD MP3 decoderを同時に開き、音声の再生位置をclockとして296フレームを選択した。最終PTSは9,966,567 µs、音声位置10,021 ms、選択時の最大遅れ68,680 µs。再生開始をまたぐ集計窓は25.2 fps、定常4窓は29.7/29.9/29.7/29.8 fps、内部heap最小空き55,940 B、MP3は407 packets・255,165 frames・fault 0・underrun 0。再生後はstreamStop ACK、予約ファイルの157,216 B全バイト照合と削除が成功し、元のMUSIC設定と通常image SHA-256 `170479264c237572472c0dc2153a345bcd15aae15a68ba4ca36647cdc46f0d23`を復元、flash verifyと`HOME_READY`を確認した。ログは`.cache/d6-sd-av-stream-device-20260928b/`。物理出音・LCD GRAMの位相は未測定である。

Astraの再レビューで、性能集計窓の前後がsetup/EOFを含むことと、無条件の事前cleanupが予約名の衝突を削除し得ることが分かった。通常runnerは事前cleanupを廃止し、既存ファイルはstageで拒否する。過去の実験ファイルの回復は別操作で内容を照合して行う。性能判定には開始2秒後からstream終端までに閉じる窓だけを入れ、27 fps以上、PTS遅れ100 ms以下、underrun 0、280フレーム以上、最終PTS 9.9秒以上を要求する。改訂runnerのCOM3再実行では、定常4窓の最低29.7 fps、290フレーム選択、最終PTS9,966,567 µs、音声位置10,026 ms、最大遅れ69,400 µs、MP3 decode 406 packets・fault 0で合格した。ログは`.cache/d6-sd-av-stream-device-20260928c/`。

D2の追加host確認では、手続き面が既に計算していた旧・新の列範囲をKasane coreの矩形無効化へ渡した。1点の追加と、表示中の1点を画面外へ消す場合、各8行帯のLCD転送量は帯全幅3,840 Bから16列256 Bへ減った。2点の離れたowner矩形は同じ帯内で合流して1,024 B、部分転送失敗後の旧確定面への全面repairは64,800 Bで変わらない。O0/O2 hostでは2面の重ね描き、SYSTEMの移動、候補の再試行、画面外更新ゼロ転送も通過。APP終了時の画像資源は7種の終了状態と各100回の再登録で古いhandleがSTALE、SYSTEM資源が有効であり続けることを確認した。これはD2の転送量とcore資源の寿命の検証であり、複数独立JS手続き面を通常アプリへ公開したことやFLOWERとの同時画素比較、実機での矩形転送量の実測ではない。

D2の通常JS adapterにも[最大2枚の独立手続き面](d2-multi-procedural-surface.md)を加えた。`createSurface()`のセッション世代IDを`beginFrame(color,id)`と`resource(id)`へ渡す。2面の候補・確定frameは別々、作業scratchと8行画像cacheは共有し、同時pendingは1面に制限する。実QuickJSとKasane core/rendererを結んだhost O0/O2とfake PIE経路では、両面を異なる画像ノードへ重ね、片面の1点移動で256 Bだけを転送し、もう片面の画素を保った。8行帯の転送失敗後は候補を保持して64,800 B全面repairし、APP終了後の両画像資源はSTALE。1:1以外の画像変換ではsource矩形最適化を使わず、元の表示boundsを全てdamageとする。通常deviceのheapとFLOWER並行表示は未測定。

AstraのD2レビューで、backdropのI/O失敗後に`pending=false`となっても旧確定frameのrepairが未完了の間は次の`beginFrame`が通ってしまうこと、FLOWER overlayの専用present経路がproc/gridへACKを返さないことを発見した。全surfaceの`repair_required`をbegin gateへ含め、overlayは画像モードprocへだけ成功/失敗を返し、描かれない手続きbackdropのcommitを拒否する。hostではbackdrop失敗→再begin拒否→旧frame repair→新begin成功と、2画像面の更新→overlay経路のI/O失敗→同じ候補の再送→次更新を実QuickJSとKasane rendererで確認した。COM3のFLOWER overlay実負荷とheapは未測定。

D1の残っていた全14種と真の候補確保失敗を、専用`KASANE_FLOWER_D1_PROBE` imageでCOM3へ実施した。hostの既存paired lease試験を維持し、各種ごとに`scene_mem`を解放した独立固定shotのRGB565 hashを追加してO0/O2で照合した。実機でも14種すべてがhost hashと一致し、同一leaseをlive中・別種prepare後・`scene_mem`解放後・再描画で各行照合、`SUMMARY passes=14 failures=0`。初回診断ではPIEの非整列16-byte storeが診断用帯バッファを壊して偽の画素差を出した。製品のshared stripと同じ16-byte明示整列へ修正し、再prepareでGardenFrameのmote状態が進む表示保持中はprepare済みshotを再利用した。実機hashの対象はLCD送出前の描画値で、GRAM読戻しではない。

同じ実機runで、最初の確定ACK後に余剰候補slotを破棄し、64 KiBの内部heap空き下限を守りながら4 KiBブロックで断片化した。候補が実際に要求する9,723 Bに対し最大連続空き8,192 B・空き総量79,732 Bとなり、通常の`flower_frame_capture`が失敗した（`candidate=0 committed=1 fallback=1`）。旧確定FLOWER＋雨の8行再描画hashは圧迫前後とも`db9d6ef7`、予約解除後の次候補はACK成功した。これはfault injectionではなくallocatorによる1回の確保失敗と復帰だが、音声・JS動画・通知を同時に走らせた枯渇耐性やLCD GRAMの修復読戻しではない。最終診断コードの再実行ログは`.cache/flower-d1-device-20260928g/flower.log`、診断image SHA-256 `0dbe9f0c0c0d14634ce80e55ed4c90185131d357affc4cff05f694314679c412`。試験後は通常image SHA-256 `4211e76d352e69c1bcbcbc3b3ee6dc5c3c7ea2b278137e1aad8cc60f16f0842a`をアプリ領域へ復元し、flash verifyと`HOME_READY`を確認した。フラッシュ退避なし。

## D0–D6 初回採否（2026-09-28）

| 段階 | 採用する範囲 | 次段へ保留する範囲 |
| --- | --- | --- |
| D0 | FLOWER 14種と雨の固定shot、帯順序・画素・RAM・LCDの比較基準。実SD MP3 decoderとFLOWERの安定4窓29.7–30.0 fpsを基準負荷に含める。 | 物理スピーカーの出音と音量、LCD GRAM読戻し。 |
| D1 | 単一FLOWER ownerの確定/candidate/描画lease。14種でhostと実機の送出前画素が一致し、実allocator失敗時も旧FLOWER＋雨を保ち、解放後に次候補へ進む。 | `scene_mem`を複数の独立FLOWER面へ共有する一般化。 |
| D2 | 通常JSの最大2手続き画像面、面ごとの候補/確定、全体で同時pending1面、矩形damage、APP終了時の資源失効。FLOWER backdrop上の2面とLCD失敗後の再送も短い実機gateで確認。 | FLOWER＋2手続き面の長時間性能、音声・低heapの同時負荷、2面を超えるRAM予算。 |
| D3/D3a | 有界な逐次VMと登録済みplan、動的なBezier/path、明示Q14独立点列のPIE、合法な型付き積和/依存行kernel。floatの描画命令は意味を保つscalar。 | float→Q14の暗黙変換、任意pathのPIE化、適応分割・塗り・stroke join、任意式の実行時JIT。 |
| D4 | 下地なし・単一資源・最大112×63×8命令の関数画素を通常JS画像ノードへ公開。span backendと転送失敗後の同一候補再送。深度線は独立のhost/診断経路。 | 全画面の任意画素式、2枚の全画面underlay、複数独立pixel lease、任意JS callback、depthとpixelの一体API。 |
| D5 | 3-slot動画画像資源と、順方向・非圧縮の小型KSV1 SD stream。workerでI/Oし、owner turnで音声clockに対する最新frameを選ぶ。 | 圧縮codec、network stream、seek、外部revoke・200 ms超ACK・短読取り競合・LCD失敗の実機同時注入。 |
| D6 | FLOWER上のSD KSV1＋SD MP3の10秒代表負荷を合格。通知＋低heap＋LCD修復は別の複合gateで合格。 | これら全負荷を一度に重ねた性能保証、物理音画同期、LCD GRAM読戻し、任意解像度・bitrateへの性能外挿。 |

D2通常APPのCOM3診断は[2面API契約](d2-multi-procedural-surface.md)の`KASANE_D2_MULTI_SURFACE_PROBE`で2回実行した。各runは64表示、うち57回が256 Bの矩形転送。二面確保後の内部heap最小空き95,480/95,484 B、最大連続空き51,200 B、UI task stack最小余裕23,708 B。診断image SHA-256 `2fd0b9309b2b9c8df3ba4e063643c976898659386fc78f7ac07257f1d25f6025`、ログ`.cache/d2-multi-surface-device-20260928a/serial.log`。Astraが見つけたoverlay ACK漏れと旧backdrop repair中の再開始を修正し、画像面の成功/失敗/再送、backdrop修復待ちを実QuickJS＋Kasane rendererのO0/O2・native/fake PIEで確認した。Astra再レビューは両指摘の解消を確認。この通常APP診断はFLOWER overlayとI/O失敗を同時注入していない。

次に`KASANE_D2_OVERLAY_PROC_PROBE`のFLOWER背景で、2つの手続き画像面を8 turnずつ2回更新した。両runとも最初の候補pending中に4帯目を一度失敗させ、失敗時`pending=1`、64,800 B再送後`pending=0`、さらに面1→面0→面1の更新を確認した。`flower_mode=1`を各送出で検査。ログは`.cache/d2-overlay-proc-device-20260928c/serial.log`、診断image SHA-256 `f3a4a3ddebe8675ce2940a9084f87179fcdec85b663b068f70a02db9ae45d1e6`。初回runnerは起動時の正常なoverlay停止ログを故障扱いし、2回目はJSの半透明背景がoverlay契約に違反して起動拒否された。観測区間の限定と不透明背景を修正した最終runが上記結果である。短いgateなので長時間fpsや低heap・音声同時負荷へ外挿しない。試験後は通常imageのflash verifyと`HOME_READY`を確認し、退避はしていない。

D4通常JSのCOM3診断は[限定pixel API](d4-pixel-small-window.md)の`KASANE_D4_PIXEL_APP_PROBE`で2回実行した。最大112×63×8命令の同一資源を2画像ノードへ表示し、最初の2帯目送出を1回失敗させて再送。成功候補の表示は初回runで16.3–20.5 ms、UI task stack最小余裕23,708 B。診断image SHA-256 `ca0aa51e046560123fb2f8e3d57cc6f3c55ca0f4e4ca7e0f0755dc144a8f9d9b`、ログ`.cache/d4-pixel-app-device-20260928a/serial.log`。全画面18命令はspan化しても23.4 msの評価に加えて合成・LCDが必要で、2枚の全画面underlay確保も失敗するため、限定APIの上限を広げない。

各診断の終了時にその時点の通常imageをアプリ領域へ復元し、flash verifyと`HOME_READY`を確認した。フラッシュ退避なし。FLOWER overlay診断フラグも加えた最終通常imageはSHA-256 `6d94ab7fae63f196bcbfe3d23aeb3a3a1315814b0d63ab2693256bf9013e0d55`、app 2,128,640 Bでビルド成功。これをアプリ領域へ書いてflash verifyと`HOME_READY`を再確認した。最終image上で通常VIDEO LABを2回、通常MEGADEMOの全画面→縮小小窓→全画面往復を2回実行して通過した。ログは`.cache/d2d4-overlay-final-video-lab-20260928.log`と`.cache/d2d4-overlay-final-megademo-20260928.log`。

## D3a 登録時の積和認識を拡張（2026-09-29）

[grid登録時解析](../../main/ui/kasane/ksn_proc_grid.c)の積和候補認識を、命令数と順序が固定された照合から、最大16命令のレジスタ定義ごとの値追跡へ変更した。レジスタ再利用、独立した定義の並べ替え、同一loadの再利用、`ADD 0`/`MUL 1`、int16に収まる定数式を正規化する。int16を超える定数の積は途中値を狭めず、2つの定数をPIEの積として保持する。最終結果は累積値への1回の加算と1つの積項に限り、すべての命令が結果に寄与する場合だけPIE候補にする。途中の検査付き演算を消しうる死んだ命令、非ゼロの加算項、複数の積和項はscalarへ戻す。実行時の独立性・alias・値域・QACC上限の検証は従来どおり別段で行う。

追加したJS例を含む22式は実QuickJSから登録し、PIE命令シミュレータで通常・融合経路それぞれ94ベクトルブロックの出力一致を確認した。CのPIE模擬・非PIE経路は400ケース、通常アプリのgrid adapterも通過し、Xtensa向け通常imageはビルド成功。今回の認識拡張の実機時間は未測定で、候補範囲を広げたこと自体を速度改善とはみなさない。次は複数項の累積と依存行kernelの共通表現、登録費用と実測費用に基づく選択を検討する。float描画IRからQ14への暗黙変換は行わない。

実機で遊べる確認用に、既存の[GRID LAB](../../apps/kasane/grid_lab.js)へ4番目の`FOLD ART`モードを追加した。Enterまたは120フレームごとの自動切替で、従来の3縮小モードから進める。48×28の動くRGB565模様をJSの`fold`式`acc+(load+0)*(1+2)`から登録し、nativeループの結果を通常Kasane画像として表示する。COM3へ通常image 2,138,192 B（SHA-256 `94fafd24229abfd7695f440884f5e67fb2cb77b114b09c221603c7d0b146b9cb`）をアプリ領域だけ書込み、書込時hash照合後、GRID LABを2回起動した。両runで既存3モードと新モードに到達し、新モードは`backend=PIE strategy=GATHER reason=FALLBACK`、終了後は`HOME_READY`。ログは`.cache/grid-lab-d3a-20260929/serial.log`。これは選択と表示の確認であり、scalarとの実機時間比較や画面の全画素読戻しではない。

続いて同じloadを共有する`load*7+load*(-4)`を登録時に`load*3`へ縮約した。この段階では係数の和がsigned16を超える形をscalarへ戻していた。2つの積の途中値はsigned64の範囲に収まり、bind時の既存のQACC上限・独立性検査も通す。CのPIE模擬/非PIE経路で一致し、実QuickJSの24例とPIE命令シミュレータの通常/融合各95ベクトルブロックが通過した。GRID LABの`FOLD ART`もこの式へ更新し、通常image 2,138,416 B（SHA-256 `0aff44fe166407ca378385960fb5f539d8d5905d3190da2c3b6a051322dd8c42`）をアプリ領域だけ書込み、書込時hash照合と2回のGRID LAB全4モード・`HOME_READY`を確認した。新モードは両runで`PIE/GATHER/FALLBACK`。ログは`.cache/grid-lab-d3a-factored-20260929/serial.log`。実機のscalarとの速度比較はまだ行っていない。

この速度比較のため、通常のgrid adapterに`measure(handle, repeats)`を追加した。直前に`run`へ渡したコピー済み入力を使い、表示中の候補/確定画像とは別の整列scratchでscalarとAUTO PIEを交互に測る。bindとJS入力生成は計時から除外し、両経路の全出力一致を各回で要求する。GRID LABのFOLD ARTは8回合計と1回平均をログ/画面へ表示する。COM3の2回の独立起動でscalar合計28,110 / 28,122 µs、PIE合計1,883 / 1,879 µs、全画素一致だった。1回あたり約3.514 ms対0.235 ms、約14.95倍である。PIEは両runで`GATHER/FALLBACK`を選び、4モードを表示後`HOME_READY`へ戻った。image 2,140,416 B（SHA-256 `476590105f4a26297a86d2ae71a277f1c13a5222bb5b11378c8efa1e6e0276fc`）をアプリ領域だけ更新し、書込時hash照合済み。ログは`.cache/grid-lab-d3a-measure-20260929/serial.log`。この比は特定の48×28・1tap式のnative kernel実行だけであり、同じ式を旧コンパイラで動かす場合のscalarとの差に相当する。登録費用、毎フレームのJS入力生成、画像合成、LCD転送を含むアプリ全体の倍率ではない。

## D3a 2項の積和へ拡張（2026-09-29）

1つの積へ縮約できない`termA+termB`を登録時planに最大2項の順序付き積として保持し、同じPIE QACCへ順に積和する経路を追加した。各項はint16 load/constantの積または直接値で、既存の独立性・QACC・alias・整列・出力検証を通る。出力に依存するloadはこの独立PIE候補から除外し、従来の依存行scan認識へ残す。2項は未知の実機費用を既存1項の選択表へ混ぜず、GATHER候補だけで開始した。1項のprofile keyと既存経路は変えない。係数の和がint16を超える共有load、非ゼロoffset、異なる位置の2 loadも新経路に入る。2項へ縮約できない3項以上と任意の依存式はまだscalar。

JS前段の25例を実QuickJSから実行し、PIE命令シミュレータの通常/融合各98ベクトルブロックでCモデル・独立JS期待値と一致した。CのPIE模擬/非PIE経路各400ケース、通常アプリadapter試験を通した。GRID LABのFOLD ARTは元の画素×2と左右反転位置の画素を1tapで混ぜる48×28の動く絵へ更新し、既存の`measure`でnative scalar/PIEの全画素を毎回照合する。初期の2項PIEはCOM3の2起動で8回合計scalar 26,959 / 26,966 µs、PIE 8,364 / 8,384 µs。定数係数を毎回8レーン分組み立てる代わりに`EE.VLDBC.16`へ渡すと、別の2起動でPIE 7,191 / 7,189 µsとなり、初期経路より約14.2%短縮した。静的QR予測はこの差を示さず、C側の係数準備費用を含む実機測定が必要だった。

最終image 2,141,616 B（SHA-256 `1ac5ad7e020d4d16bdc112b9f09a8d0821ef2669dcd162c6a7f7c81dfa2ac541`）をアプリ領域だけ更新し、書込時hash照合後、GRID LABを2回起動した。8回合計はscalar 26,969 / 26,968 µs、PIE 7,187 / 7,185 µsで全画素一致。1回あたり約3.371 ms対0.898 ms、約3.75倍。4モードを表示し、`PIE/GATHER/FALLBACK`と終了後`HOME_READY`を確認した。ログは`.cache/grid-lab-d3a-dual-final-20260929/serial.log`。これは登録済みkernelだけの費用であり、1項版と式・表示内容も異なるため、両者の速度差を2項追加の純粋なオーバーヘッドとは断定しない。次は2項のloadごとのcontiguous/reverse-stride判定と実機選択表、依存行kernelの共通表現を検討する。

## 次のタスク（2026-09-29）

1. **D3a: 2項のアクセス経路と費用選択（完了）。** 各loadの連続・逆順・broadcast・gatherを登録時に判定し、合法な2項PIE候補を比較した。GRID LABのFOLD ARTに加え、寸法・stride・係数を変えたJS例でscalarとの全画素一致を確認。同じ通常imageで独立した実機runを繰り返し、5%以上速い形状だけをAFFINEへ切り替えた。結果は下記。
2. **D3a: 登録時解析と依存行kernelの共通化（完了）。** 命令ごとの値バージョン、live-in/out、依存集合、検査付き演算、絶対値上界を共通グラフに記録し、MAC正規化・依存行の認識・QACC前段の上界証明で共有する。レジスタ再利用、alias、overflow、死んだ検査付き命令をhostで反例として通し、実機で登録費用とRAMを測った。2項を超える式と別種の依存行反復は、合法性と費用の両方を示せるまではscalarに残す。
3. **JS APIと診断（完了）。** `grid.registration(handle)`は登録時の式形状、解析時間、planと内部heapの大きさを返す。登録失敗はIRの理由と命令番号を例外に含め、`grid.explain(handle)`は実行後のkernel、PIE候補とscalar選択理由を返す。`gridFold.index({base,x,y,tapX,tapY})`と`view({buffer,...})`の座標引数をオブジェクトに統一し、動的係数も名前付きオブジェクトで記す。[JS記法](grid-js-notation.md)にnative反復とscalar fallbackの契約をまとめた。float式からQ14への暗黙変換は導入しない。
4. **表示全体の採否。** 登録、JS入力生成、kernel、Kasane合成、LCD送出、heap、フレーム停滞を別々に測り、全画面とUI内の小窓でscalar/PIEの体感上の差と余裕を確認する。D2のFLOWER＋2面、D5のSD動画＋音声との複合負荷は、単独のkernel倍率から外挿せず別のgateで判定する。

## D3a 2項アクセス経路の実機選択（2026-09-29）

2項の各loadについて、bind時に出力レーンのstrideを連続・2間隔・broadcast・逆順・その他へ分類する。GATHERに加えてAFFINE候補を登録し、整列した連続8セルだけを直接PIEへ渡す。逆順と整列・余剰セル条件を満たさないブロックはscratchへ集める。係数loadがbroadcastなら1セルをPIEで広げる。QACCへの加算順、bind時のalias・範囲・QACC証明、scalar tailは維持した。profileのない2項式は引き続きGATHERを自動選択する。

JSの独立期待値を持つ連続2 load・レーン別係数・broadcast係数を追加して28例とし、実QuickJS→IR→PIE命令シミュレータで通常・融合それぞれ101ベクトルブロックが一致した。CのPIE模擬/非PIE各400ケースと通常アプリadapterも通過。GRID LABには既存の3縮小＋FOLD ARTに加え、FOLD PAIRとFOLD WEIGHTをEnterで選べるようにした。2項測定APIはGATHER/AFFINEを強制でき、表示候補とは別scratchで各回scalarとの全画素一致を確認する。grid slotとKasane画像資源の枠を両方6へ揃えた。

同一通常imageで3回独立起動した[計測記録](../../tools/kasane_contract/profiles/grid_dual_20260929.json)から[選択表生成器](../../tools/kasane_contract/build_grid_measure_profile.py)で[2項profile](../../main/ui/kasane/ksn_proc_grid_dual_profile.inc)を生成する。8回合計の中央値は、鏡像48×28がGATHER 2,852 µs、AFFINE 2,749 µs（3.6%短縮）、連続2 load 48×20が2,045→1,808 µs（11.6%）、動的係数40×20が2,339→2,167 µs（7.4%）。既存生成器と同じ5%の採用余裕を適用し、鏡像はGATHER、残り2形状だけをAFFINEにした。計測記録はバイナリSHA-256、COM3、3つの独立run ID、形状と両候補の全画素一致済み時間を持つ。全runで同じバイナリ・形状・候補集合でなければ生成を拒否する。

最終image 2,145,136 B（SHA-256 `b8f605d97046f97f3d8392c976b4f8b6339175ea6773733dd4f298b5d23d355b`）をアプリ領域だけ書き、書込時hashを検証。独立2起動で鏡像`PIE/GATHER/PROFILE`、残り2形状`PIE/AFFINE/PROFILE`、全画素一致、6モード表示、終了後`HOME_READY`を確認した。最終ログは`.cache/grid-lab-d3a-profile-gated-final-20260929.log`。フラッシュ退避なし。Astraの敵対的レビューは、初稿の鏡像3.7%行が5%規則を破ることと生成経路の欠如を指摘し、上記の生成式とGATHER選択へ修正した。

比較用に試した64×24の連続2 loadと40×24の動的係数は、JS模様の計算を事前化しても30フレーム窓の平均が約36.0/32.9 msだった。表示サイズを48×20/40×20へ調整した最終版では、同窓の平均24.49/27.60 ms、最大26.50/29.20 ms。鏡像48×28は平均26.03 ms。これはアプリ実行中の周期ログで、個々のフレーム最悪値、音声や他overlayとの同時負荷、画面画素の読戻しを保証しない。起動時の内部heap free標本は79,132/79,356 B、最大連続31,744 Bであり、実行中の低水位ではない。以前の2項GATHER約7.19 ms/8回から今回約2.85 ms/8回への短縮には、PIE命令の変更だけでなくCのレーン収集を1ブロック単位へまとめた効果も含む。次は登録時の共通依存表現と、JS・合成・LCDの費用分解を扱う。

## D3a 登録時の共通値グラフ（2026-09-29）

登録時に各レジスタ書込みを不変の命令値として記録し、入力元の命令番号、累積依存集合、live-in/out、検査付き演算とDEST読取りの印、全入力に対して保証できる絶対値上界を持たせた。MAC正規化と依存行の認識は同じ値グラフを読み、bind時のQACC判定はそこで証明した上界を使う。命令の物理的な並びやレジスタ番号を式の同一性とみなさず、最終値が全命令を含まない場合は高速経路へ降ろさない。

依存行は、前画素のDEST loadと独立したsource loadを、`source + coefficient * previous` のどちらの加算順でも認識する。sourceを先に定義し、previousとsourceのレジスタを途中で再利用する6命令をhostでscalar/scan PIE比較した。別buffer IDからのDEST alias、死んだ検査付きADD、前画素でないDEST indexは候補外になる。3段の自己乗算で上界が`int64`を超える例も候補外とし、scalar実行時のoverflowを確認した。JSの`iirReordered`を含む29例は実QuickJS→IR→PIEモデルで一致し、命令シミュレータの通常・融合それぞれ113ベクトルブロックが一致。CのPIE模擬と非PIE各400ケース、scalar/レーンモデル、通常アプリQuickJS adapterも通った。初回の通常ファームビルドは成功し、アプリサイズは2,145,456 B、flash予算残り1,000,272 B。Astraのチェックポイントレビューでも合法性の穴は見つからなかった。その時点では新しいscan形状の実機速度と登録時のRAM・時間を測っていなかった。

## D3a 登録費用とJS診断（2026-09-29）

既存のGRID LABの3つのfold planを使ってCOM3で3回独立起動した。登録時にJS programの読取り、`ksn_grid_prepare`、出力2面の確保を個別に時計測した。フィールド配置の修正前は値グラフ392 B・plan 1,240 Bで、各planの確保直後に内部heapが1,284 B減った。詰め物を減らした後は値グラフ264 B・plan 1,112 Bとなり、同じ確保段階の減少は1,156 B。追加前のplan 848 Bはcommit `622582e`のヘッダを同じhost ABIで計測した値なので、新解析のplan本体の純増は264 B。allocatorの上乗せ44 Bは旧版実機で測った値ではないため、その差を旧版heapの実測とは扱わない。

圧縮後の同一バイナリで9登録（3形状×3起動）の中央値は、JS program読取り786 µs、native prepare 97 µs、登録全体936 µs。prepareの最長標本は190 µsで、その要因は未分離。3番目のfold登録直後の内部heap空きは60,128〜60,256 B、最大連続空き31,744 B。これらは起動中の一点の標本であり、低水位ではない。出力2面とallocator費用も登録全体のheap差に含まれる。ログは`.cache/grid-lab-registration-20260929.log`（圧縮前）と`.cache/grid-lab-registration-compact-20260929.log`（圧縮後）。

`grid.registration(handle)`にIR命令数、MAC項数、検査付き演算数、未証明の中間値数、plan/解析領域のbytes、各時間・内部heap標本を追加した。登録失敗の例外には`body[n]`と検証理由を含める。実行後の`grid.explain(handle)`は`kernel`、`candidateMask`、`scanCandidate`、`scalarReason`を返す。host QuickJSで不正dst registerと一般形MIN式、7画素幅、QACC範囲超過の各scalar理由を検証。通常ファーム最終imageは2,149,072 B、SHA-256 `5f7d17c94bbfa0d1e7da69aae8f76206bf59e6ede56667f3c29e03bb7a605747`。アプリ領域だけを書き、hash照合後にGRID LABの6モード、PIE候補理由、全画素比較、終了後`HOME_READY`を確認した。最終ログは`.cache/grid-lab-registration-diagnostics-final-20260929.log`。フラッシュ退避なし。新しい並び替えscan式そのものの実機速度は未測定。

`gridFold.index`を1個の名前付きオブジェクトへ統一した。5個の位置引数と動的係数の配列形式は拒否する。GRID LAB、29個のQuickJS→IR例、通常アプリadapterのテスト、実機診断用JSを移し、生成C assetも同期した。Astraのレビューでは`view`が`base`を無視する不一致を発見したため、`view({buffer,base,x,y,tapX,tapY})`へ統一し、非ゼロbaseの直接loadと同じIRになるテストを追加した。hostの29例は独立期待値との比較とPIE/scalar経路を通過。

最終通常image 2,150,240 B、SHA-256 `9cc5cd85d91db97cbd5626bd13773dd6bb94364158255a250f7e1d548a5af449`をCOM3のアプリ領域だけに書き、書込時hashを検証した。既存GRID LABの最終起動で6モード、全画素一致、PIE経路と`HOME_READY`が通過。鏡像foldの8回合計はscalar 26,962 µs、PIE 2,857 µs。最終ログは`.cache/grid-lab-object-index-final-20260929.log`。レビュー前の2回独立起動ログは`.cache/grid-lab-named-index-20260929.log`。フラッシュ退避なし。次は表示全体の費用分解へ進む。
