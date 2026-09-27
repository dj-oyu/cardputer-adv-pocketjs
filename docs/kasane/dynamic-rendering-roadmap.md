# Kasane 動的描画ロードマップ

状態: **D0–D3a 進行中**（2026-09-27）。[Kasane v1 の判定](roadmap.md)は変更しない。この文書は実装と実測の順序、各段で下す判断、参照する既存ノウハウを記録する。中間命令の形式、JS API、codec、固定FPSを先に仕様化しない。決まった契約は実装と試験に置き、採否の理由と結果をこの文書へ戻す。

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

各段の終了時に下の表を更新し、根拠となる commit、host試験、実機 image SHA・flags・workload・ログへリンクする。性能の変化は `prepare`、描画、LCD、decoderを分けて示す。取り下げた案と旧不合格も残し、後の成功で上書きしない。実装中に確定した小さな契約はコードと契約試験へ置き、この文書を命令セットの詳細設計書にはしない。

| 段階 | 状態 | 実装・実測から得た知見 / 次の判断 |
| --- | --- | --- |
| D0 | 進行中 | host 基準として `test_flower` の14種×60 pose、全画面と8行帯の一致・境界・`scene_mem` 再利用、`test_glass_rain` の300秒・1～11行帯・重なり、`test_garden` の背景＋雨を通した。host の FLOWER 共有ブロックは 16,188 B。`idf.py -B build_api build` は成功し、app 2,038,592 B、DIRAM 159,212 B（直近基準比 +176 B）。実機の描画時間、LCD、peak RAM、最大連続空き、音声は未測定で、host 実行秒数を実機性能に読み替えない。 |
| D1 | 進行中 | 雨の6滴を 192 B のフレーム値に capture し、シミュレーションを進めた後も同じ下地に全画面・逆順8行帯で同じ画素を再描画できることを `test_glass_rain` で確認。FLOWER のシーン描画もこの固定値を読むよう変更した。FLOWER 本体は依然として単一 `scene_mem` と可変状態を読むため、旧フレームの lease/repair 契約は未達。次に FLOWER の導出データの保持と更新の境界を調べる。 |
| D2 | host 試作・実機単独診断済み | 手続き型 surface に確定・候補の2スロット、世代 ticket、旧/新 damage、下地からの再合成、部分転送失敗後の旧フレーム修復を実装。複数面を APP 内で順に合成し、Kasane の backdrop 経路へ接続した。`test_procedural_surface`、`test_procedural_layers`、`test_procedural_present`、`test_procedural_pattern_matrix` は O0/O2 host で通過。波形・格子・画面外交差・色変更・消去の構造例で帯順序と damage 範囲を確認。`vm/main` (`6430ad6`) 取り込み後の ESP-IDF 6.0.1 / ESP32-S3 image は app 2,051,456 B、SHA-256 `e930a3098fb3786eaa417cd298f578e9a1a34dda00a7b2b4be7a9dfa6e7e400`。[実機の表示・負荷診断](procedural-device-probe.md)は60フレーム完走、2面確保、平均 `prepare` 641 µs、合成等 677 µs、LCD 2,924 µs、内部 RAM free 109,516 B（確保後）を確認。2面のフレームスロットだけで 40,984 B を使う。透明、音声・FLOWER と同時の負荷、LCD GRAM の読戻しは未検証。 |
| D2b | UI 画像ノード接続・実機試作済み | `kasane.procedural.resource()` が現在の手続きフレームを 240×135 の不透明な画像資源として公開し、`tx.image` の bounds と描画順序で通常 UI に重ねられる。転送失敗時は候補画素を UI ticket とともに保持し、成功時に確定する。ニュースセットの小窓から全画面への往復を実機で表示した。現状は単一資源、全面 invalidate、毎フレーム64,800 B転送であり、拡大時の合成費用と矩形 damage が次の課題。 |
| D3–D4 | JS/host 試作・実機単独診断済み | 有界反復・条件付き終了・計算色、VM 所有の検証済み命令コピー、明示的な native state copy-in/out を試した。JS 登録 API は実 QuickJS から15本の plan を登録し、`beginFrame`→`draw`→`commit` で48フレームを表示した。3D depth と下地読取りは未着手。 |
| D3a | host/compiler 試作・実機単独診断済み | 登録時の有界 CFG・依存解析、命令を所有する scalar plan、独立した VM 意味論テスト、型付き Q14 点列の scalar/PIE 候補と明示的な選択 policy を実装。Astra の関連機能レビューで寿命・再入・経路未接続・試験漏れを修正。実機で PIE 280ケースの scalar 同値、40点 plan の PIE 選択、8点からの速度優位を確認したため、型付き Q14 点列は8点以上で PIE を選ぶ。一般の float IR を自動的に PIE 化する段階にはまだ達していない。 |
| D5–D6 | 未着手 | D1/D2 の frame 寿命と RAM 予算を見て進む。 |

D3a の PIE 点列カーネルについて、[実行オブジェクトのストール解析](pie-stall-host-report.md)と[命令順の検証](proc-pie-stall-options.md)を host で実施した。元の 31 PIE 命令には QR のロード直後使用が 12 箇所あり、既存の `stalls.py` は符号付き QACC 命令の入力を見落として 4 箇所しか報告しなかった。検出器を修正し、同じ 31 命令のまま係数レジスタを交互に使う順へ変更した結果、静的予測は 0 箇所。元と変更後は命令シミュレータで 1,024 入力条件の出力が scalar と一致し、変更後の C 全体を ESP32-S3 向けにコンパイルした逆アセンブルでも順序を確認した。その後の実機診断で値一致と8/16/40点の時間を測り、型付き点列の PIE 選択閾値を8点にした。静的ストール予測だけを実測サイクル数とはみなさない。

登録時コンパイラの連続 workload として、疑似3D・glitch の [48フレーム host メガデモ](../../tools/kasane_contract/proc_megademo.h)を追加した。3場面×5レイヤーの IR plan を場面開始時に登録し、毎フレームは入力だけを更新する。門型ワイヤーフレーム、消失点へ向かう線、奥行き格子、正弦信号、色ずれした走査線を native `REPEAT` で描く。[契約試験](../../tools/kasane_contract/test_proc_megademo.c)は通常VM・plan・デバッグ経路の状態と全画素を比較し、O0/O2 host で 48フレーム・5,680 segment・登録時の融合箇所6件が通過した。[プレビュー生成](../../tools/kasane_contract/run_proc_megademo_preview.py)は RGB565 の描画結果から PPM と一覧 PNG を出す。

2026-09-27 に COM3 の ESP32-S3 rev0.2 / 8MB で optional メガデモ probe を実行した。image は 2,056,720 B、SHA-256 `928028763e5486021664779afdd41f28f2c0cc8b82fcc139f8a59ff863543d26`。48フレーム・5,680 segment を完走し、全48フレームの RGB565 hash と3場面末尾（15/31/47）の全135行×240画素キャプチャが host と一致した。[起動・回収](../../tools/kasane_contract/run_proc_megademo_device.py)と[照合](../../tools/kasane_contract/check_proc_megademo_device.py)を再実行できる。キャプチャなし45フレームの平均は VM 669 µs、帯描画 7,085 µs、LCD転送 7,242 µs、全体 15,076 µs（14,045–16,169 µs）。フレーム間の実測中央値は50 msで、末尾の33 ms `vTaskDelay` が描画後に加算されている。キャプチャ3フレームはシリアル出力待ちにより各約962 msで、通常描画費用に含めない。実行中の内部 RAM free は171,680 B、終了時は開始時の230,780 Bへ戻った。LCDへの転送成功と転送前画素は確認したが、GRAMの読戻しや肉眼でのパネル像、音声との同時負荷、深度バッファ、PIE点列カーネルの速度はこのprobeの判定外。元のアプリ領域3,145,728 Bを同セッションで復元し、書込みhash検証と `HOME_READY` を確認した。

同日に [JS メガデモ](../../apps/kasane/proc_megademo.js)を登録 API へ接続した。JS は15本の float IR plan と3本の Q14 点列 descriptor（各40点）を登録し、各フレームは5回の `draw` で native 反復と線分描画を実行する。型付き点列は登録時に係数と点を16-byte整列の所有領域へコピーし、実機では PIE plan を選択する。実 QuickJS の[ホスト試験](../../tools/kasane_contract/run_pocket_proc_qjs.py)は通常 backend の scalar fallback と fake PIE backend の両方で48フレーム全画素一致、転送失敗修復、reset、再入拒否を確認した。Kasane host suite も失敗0件。実機の直接 PIE 診断は280/280ケースで scalar と一致し、2048回の集計時間は8点で scalar/PIE=6,392/2,342 µs、16点で12,478/2,834 µs、40点で30,706/4,301 µs。これは点列変換 kernel 単体の時間で、JS や raster/LCD を含まない。

最終診断 image は ESP-IDF 6.0.1、`KASANE_PROC_DEVICE_PROBE=ON`、`KASANE_PROC_JS_DIAGNOSTIC=ON`、2,072,976 B、SHA-256 `7c0a1bdb25752baebfb5c19dcc82deec3da03f5a92f287e14d6c1aafca780789`。[実機起動・キャプチャ](../../tools/kasane_contract/run_proc_js_device.py)と[ホスト全画素照合](../../tools/kasane_contract/check_proc_js_device.py)で3場面の連続2フレームずつ（8/9、26/27、39/40）が一致し、診断ログは各表示フレームで `scalar=0`、PIE呼出しが1回ずつ増加、転送64,800 Bを記録した。COM3 の元アプリ領域3,145,728 Bは保存値（SHA-256 `6178382394f4970c6cd06ab3e49f87114eeb20cef32ae00a5cdcfd5a3fb8ba94`）に復元し、flash digest 照合と `HOME_READY` を確認した。キャプチャはLCD転送前の送信画素であり、GRAM読戻し・肉眼像・音声との同時負荷は未検証。次の実装判断は、float IR から安全に型付き kernel へ lowering できる形と、修復可能なフレーム寿命を複数描画面へ一般化する範囲である。

同日の UI 混在試作では、[ニュースセット JS](../../apps/kasane/proc_news_zoom.js) が既存の15 plan・3 Q14点列を登録し、`commit()` 後に `tx.image` の矩形を動かした。小窓 `[64,20,176,83]`、拡大途中、全画面、縮小途中の [COM3転送画素](proc-news-zoom-device.png) を取得した。実 QuickJS host 試験は scalar/fake PIE とも成功し、UI 構成の320フレーム mock 試験も通過。診断 image は 2,082,976 B、SHA-256 `1b4f044cc6f59716a0c0b54304c7f59b04d3c7bb641b2fe96816e644d3949217`。キャプチャ前の連続フレーム間隔の中央値は小窓47 ms、拡大中59 ms、全画面74 ms、縮小中64 ms。全画面は約13.5 fpsで、目標とする滑らかさにはまだ達していない。PIE点列は各フレーム1回動き、転送量は全フェーズ64,800 B。次は Kasane の画像合成費用を計測して縮め、資源の旧・新矩形に基づく damage と列単位の部分転送を通す。元アプリ領域3,145,728 Bは同セッションで全量復元・digest照合し、`HOME_READY` を確認した。この画像は LCD 送信直前の画素であり、パネル読戻しや複数独立 canvas は未検証。

D2 より先の探索として、D3 の小さな [host IR 実験](procedural-ir-experiment.md)を独立に走らせた。これは D1/D2 完了や JS 公開を意味しない。逐次命令、native 反復、単一 step のデバッグ、完成フレームの帯再描画を試し、容量と描画コストの判断材料を得る。
続けて格子・状態を持つ軌跡・複数 path・脱出時間型軌道を比較し、固定 `REPEAT` だけでは点ごとの条件付き終了を表せず、脱出軌道がフレーム全体を無効にする問題を再現した。色も即値だけでは反復回数を反映できない。D3/D4 で制御と描画属性の境界を決める際の判断材料とする。
追加の host probe では、フレーム間の物理状態、入力で変わる反復回数、step 中のプログラム所有権、失敗した候補による旧フレーム喪失、移動図形の旧位置 damage、交差図形の depth を調べた。詳細は実験記録に残した。D1/D2 の寿命・再合成契約が先であり、D3/D4 の opcode を今固定しない。
入力駆動の有界反復 `REPEAT_REG` と最内ループからの条件付き終了 `BREAK_IF_GT` を host 実験へ追加。旧 `c=2` 軌道の失敗を対照として残し、新経路では2反復目で終了・フレーム公開、`c=-1` は16回完走した。さらに VM 所有コピー、明示的な native state、計算した RGB565 色を追加。step、生成、ラスタの上限は維持した。色や制御の実験結果は製品 API 採用を意味しない。
次に host の手続き型 surface で確定・候補の2スロット、世代 ticket、旧/新 damage、下地からの再合成、部分転送失敗後の旧フレーム修復を試した。複数面の順序付き合成と Kasane core の backdrop 経路も host で試し、SYSTEM 命令の重なり、変更帯の表示、最初の転送失敗後の旧確定への修復を確認した。候補と core 提出が重なった場合は core 提出を旧確定面の上に先に表示する。現状は手続き型 damage の帯だけを core に渡し、列範囲の最適化は使わない。これらの保持量と転送量を実機許容と判定したものではない。

現時点の重要な未決事項は、フレーム用導出データを旧・候補ぶん保持するか小さな固定入力から再導出するか、端末上で編集した関数をどう中間命令へ変換するか、下地読取りをどの範囲まで認めるか、動画の実用解像度とフレーム保持数である。いずれも先に一般解を決めず、該当段階の workload と予算で選ぶ。
