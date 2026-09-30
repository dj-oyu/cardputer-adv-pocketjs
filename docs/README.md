# docs 索引

このディレクトリの入口。**いま動いている主線は3本**（VM の高速化、PIE による描画の高速化、デザインシステム Kasane）で、それぞれにディレクトリがある。

- **仕様**: いま守る契約。コードが従う。
- **設計**: 実装済み・実装中の方針と判断。決定と未決を書く。
- **記録**: 実測・調査の結果。測った時点の値と出所（日付・commit・実測/推定の別）を持つ。
- **backlog**: 開いている作業だけ。終わった項目は消す。各ディレクトリに1つ。

新しい文書は該当ディレクトリに置き、この表に1行足す。ファイル名は動かさない（本文や C のコメントが名前と節番号で参照している）。節番号を変えたら `git grep -n "<ファイル名>"` で参照元を直す。

## 主線1: VM の高速化 — [`vm/`](vm/)

QuickJS を FreeRTOS 上で中断・再開できる実行基盤に作り替える（L0〜L5）。

| 文書 | 種別 | 中身 |
| --- | --- | --- |
| [quickjs-freertos-vm-spec.md](vm/quickjs-freertos-vm-spec.md) | 仕様 | L0〜L5 の全体仕様。各段の現状と、実装で確定した値 |
| [vm-branching.md](vm/vm-branching.md) | 仕様 | `vm/*` ブランチとワークツリーの運用 |
| [vm-L1-design.md](vm/vm-L1-design.md) | 設計 | L1: ジョブ境界の実行制御と起床 |
| [vm-L2-design.md](vm/vm-L2-design.md) | 設計 | L2: 移動しない VM スタックと中断・再開。決定表 D1〜D43、整列（§2.2） |
| [vm-L3-design.md](vm/vm-L3-design.md) | 設計 | L3: 相対参照と移動可能スタック。決定表 D44〜D55、移動は中断中のみ（§2）、差分表（§3） |
| [vm-tco-design.md](vm/vm-tco-design.md) | 設計・検証記録 | strict末尾呼び出し最適化の実験実装。host/device検証済み、互換性維持のため既定n |
| [task-allocation-facade.md](vm/task-allocation-facade.md) | 設計 | FreeRTOS タスクの配置ファサード（`standalone/fp_ticket`、未接続） |
| [vm-L0-report.md](vm/vm-L0-report.md) | 記録 | L0 の実機計測（ターン内訳、ヒープ、ジョブ単価） |
| [vm-L1-report.md](vm/vm-L1-report.md) | 記録 | L1 の実測。時計読み出しのコスト（§8.7）、コア移行（§8.8）、スケジューラ定数の調律（§10） |
| [vm-L2-results.md](vm/vm-L2-results.md) | 記録 | L2 の実測と関所の結果（段ごと、host/device の別つき） |
| [vm-L3-results.md](vm/vm-L3-results.md) | 記録 | L3a の実測。コーパス74件バイト一致、枝ごとの踏まれ方、毒の負の対照3種、Test262 7,036ファイル、移動の単価。**実機のサイズは `--gc-sections` で落ちるので測れない**（§2） |
| [oom-parse-safety.md](vm/oom-parse-safety.md) | 記録 | 確保失敗時のコンパイル経路（VM の段とは独立）。全点掃引で見つけたメモリ安全性の穴（closures 71 点・generators 133 点 → 0）と、変数の捕捉が黙ってグローバル読みになる誤コンパイル。`DynBuf` のエラーが余りへの書き込みに効いていなかった根本原因。上流2系統とも未修正 |
| [r3-small-block-cache.md](vm/r3-small-block-cache.md) | 設計 | R3: ゲストのアロケータの下に小ブロックの再利用リストを置く設計。確保の大きさの分布（上位 6 つで 92%）とキャッシュ方式ごとの命中率のシミュレーション、置き場所・上限の外に出る量・手放す時点・検査の仕方 |
| [allocator-cost.md](vm/allocator-cost.md) | 記録 | ゲストのアロケータの値段を実機で測った（`CONFIG_POCKET_VM_ALLOCPROBE`、計測後に削除）: malloc→free 1 組 約 1,800 サイクル、STRESS の JS のターンの 20.6%。経路は `heap_caps` の走査・クリティカルセクション・flash 上の読み戻し |
| [app-suspend-design.md](vm/app-suspend-design.md) | 設計 | アプリの常駐中断: Back でゲストを壊さずに止めて残し、同じアプリを開くと続きから。状態遷移、各面の中断・再開の扱い、API（`suspend`/`resume` フック、`stop("evict")`）、退去の条件、メモリの予算、段と検証。決めること 4 つ |
| [r5-icache.md](vm/r5-icache.md) | 記録 | R5: STRESS のターンを QEMU で記録し、命令キャッシュの模型に通した（`tools/r5sim/`）。触るのは 45.5 KiB、衝突は 15%。flash 内の並べ替えは効かない。IRAM 8 KiB は実機で JS −15%・空き −8.7 KiB で、見送り |
| [turn-cpi.md](vm/turn-cpi.md) | 記録 | R4: JS のターンを Xtensa の性能カウンタで測った（`CONFIG_POCKET_VM_TURNPERF`）。IPC 0.17、サイクルの 70% が flash キャッシュのミス待ち。flash を QIO にして 1 フレームの JS 10.78 → 7.25 ms（同一配置） |
| [gc-cap-backoff.md](vm/gc-cap-backoff.md) | 記録 | 上限の手前 1/32 の GC の天井が、生存量が天井を超えるとオブジェクトごとに GC を走らせていた（STRESS LV3 で JS 時間の 70%）。天井が起こした GC の後はヒープが伸びるまで待つ。GC 1,085 → 109 回、確保失敗の時点は同一。道具は `tools/vmtest/prof/` |
| [builtin-floor-plan.md](vm/builtin-floor-plan.md) | 設計・計画 | F 系列（VM の段とは独立）: ゲストの起動床（実機レイアウトで js=64,420 B、うち組み込みの名前 21.7 KB と何も作っていない索引）を flash へ。F1 ROM atom・F2 遅延索引で 28,684 B（−55%、計算）。捨てた案 4 つ、`atom_array` 33 箇所の台帳、関所と負の対照。道具は `tools/vmtest/floor/` |
| [spread-eval-oom.md](vm/spread-eval-oom.md) | 記録 | 文字列のスプレッド・for-of が実機でだけ止まらなかった: `js_string_iterator_next` の `(int *)&idx` が Xtensa（`uint32_t`=`unsigned long`）の strict aliasing で消えていた。修正 `f937388`、同種の6箇所、再現アプリ `POCKET_HEAPPROBE` と変種の表 |
| [aliasing-types.md](vm/aliasing-types.md) | 記録 | spread-eval-oom の同種6箇所と libunicode の正規化配列を、宣言の型を合わせて修正（機械語は修正前と同一＝今のビルドでは誤コンパイルなし）。`-Wno-incompatible-pointer-types` を quickjs-libc.c 以外から外した |
| [eval-peak.md](vm/eval-peak.md) | 記録・比較 | 評価のピークの構成（pass-1 バイトコード 46%、JSFunctionDef 15%…、host の確保タグ）と下げる手段の実測: 分割（B 案）+18.5〜25.9 KB、事前コンパイル +26 KB・49 ms、パーサの余り返し host −11 KB、ES モジュール、遅延 import。推奨の順序と決めること。§7 製品化した `pocket.app.load`（チャンクの表・書き方・実機で 3 分割 +18.2 KB）、§8 静的 import への引き継ぎ |
| [vm-ledger/](vm/vm-ledger/) | 記録 | QuickJS 内部の台帳 01〜09（呼び出し経路、フレームへの生ポインタ、ジョブと割り込み、opcode チェックポイント、メモリ確保、アロケータ比較、セグメント検査、スラブと最大空きブロック、**09: L2 後のセグメントを指す入口の再監査**） |
| [backlog.md](vm/backlog.md) | backlog | L2 の未完了条件、L1 の範囲外として残った決定、VM とは独立の不具合（GC 閾値、確保ヘッダ 12B など） |

## 主線2: PIE と描画の高速化 — [`perf/`](perf/)

ESP32-S3 の PIE（SIMD）と、このコアでのスカラーコードの最適化。PIE 命令の一次情報（TRM 抽出、実機で確かめた命令の意味とストール）は別リポジトリ [esp32s3-hw-mcp](https://github.com/dj-oyu/esp32s3-hw-mcp) にある。

| 文書 | 種別 | 中身 |
| --- | --- | --- |
| [pie-simd.md](perf/pie-simd.md) | 設計・記録 | PIE の性質（§1）、コストモデル（§2）、スカラーコードの値段（§3）、何を最適化するかの決め方（§4）、正確性（§5）、測定方法（§6）、出荷済みカーネルと実測（§7）、チェックリスト（§8） |
| [flash-removals.md](perf/flash-removals.md) | 記録 | 「我々だけが参照元」のライブラリ会員を測った結果（`ui/shell.c` の `__divdf3` は ROM エイリアスで 0 B、`solar_sail.c` の `remainder` 402 B は落ちるが差し替えが +508 B、`sqrtf` は 294 B 落ちるがビット一致の差し替えが収まらない）。map の「理由」≠「根」、幻の取り込み、道具の過小カウント |
| [trig-lut.md](perf/trig-lut.md) | 記録 | シーンの三角関数の LUT 化（`main/scene/fxmath.c`）: 表の大きさ×型×次数の掃引、Q31/Q15 の精度の床、採用した 216 区間 2 次、tan の除算を消した結果、ダンプハーネスの罠、**実機の1呼び出しコスト**（lut 171 / poly 237 / libm 1,550 サイクル） |
| [pie-opt-plan.md](perf/pie-opt-plan.md) | 設計 | 残った重いパスの PIE 化（`perf/pie-opt`）。モチベーション（なぜ今、per-pixel ごとベクタ化しかないか）、対象の絞り込み（T1 `bell` 帯棄却 / T2 装飾光線の厳密カーネル / T3 MP3 FIR）、ホスト4層での検証 |
| [builtins-census.md](perf/builtins-census.md) | 記録 | マップの3視点（配置・取り込み理由・`--cref`）で「我々だけが理由でリンクに入っているライブラリ」を全数調査。`compiler_builtins` 16 会員 46,329 B の取り込み理由と、我々が入口になっている 3 会員（13,680 B） |
| [flash-size-method.md](perf/flash-size-method.md) | 設計 | 容量削減の手順書: cref で薄さランキングを作り、薄い（我々だけが参照元の）ところから潰す。スタブビルドで「本当に落ちるか」を先に確かめる規律、罠、一般化まで |
| [kasane-image-transform-stretch-step.md](perf/kasane-image-transform-stretch-step.md) | 記録 | 拡大縮小画像スパンの画素ごとの 32bit 除算 2 本を、商と剰余の加算ステップ（切替つき・厳密・既定は有効）へ。1 画素 18 命令/除算 2 本 → 15-16 命令/0 本。調査は [kasane-image-transform-recon.md](perf/kasane-image-transform-recon.md)、回転側の同型の仕事は [kasane-image-transform-step.md](perf/kasane-image-transform-step.md) |
| [kasane-grid-sampling-host.md](perf/kasane-grid-sampling-host.md) | 記録 | 固定小窓の最近傍・bilinear・面積平均のhost画質と元画像アクセス量。登録時sampling policyの判断根拠と実機未測定の範囲 |
| [kasane-image-transform-anchor.md](perf/kasane-image-transform-anchor.md) | 記録 | 回転スパンのアンカーを行ごとの表にし、libgcc の `__divdi3` 2 本を消した（切替つき・厳密・既定は有効）。アンカー代 37.8 → 10.0 命令/画素、表は実行時構築で `.bss` +308 B（flash は 0 B 増） |
| [kasane-image-transform-reject.md](perf/kasane-image-transform-reject.md) | 記録 | 回転スパンの域外判定をスパン単位の区間判定にした（切替つき・厳密・既定は有効）。棄却画素の処理 −5.0 命令/転送画素、新しいデータは 1 バイトも持たない |
| [kasane-blend-pie.md](perf/kasane-blend-pie.md) | 記録 | 565 ブレンド／パックの PIE カーネル（候補 4a）: 16bit レーンモデル、恒等式の総当たり、`piesim` での全画素一致、命令数とストール。文書自体は結線前のホスト記録で、結線（`g_ksn_blend_pie`、既定 ON）は後続コミット |
| [compiler-builtins.md](perf/compiler-builtins.md) | 記録 | compiler_builtins cgu.13（10,244 B）は `__fixdfsi` の参照を消しても落ちない: マップの取り込み理由は会員ごとに1本しか出ず、参照元は18本、しかも `__fixdfsi` 自体は ROM に解決されている |
| [compiler-builtins-cgu04.md](perf/compiler-builtins-cgu04.md) | 記録 | cgu.04（`__gedf2`、2,248 B）も我々の側からは落とせない: 我々の5ファイルを置き換えても取り込み理由が `quickjs.c.obj` へ移るだけ（実測 −2 B） |
| [backlog.md](perf/backlog.md) | backlog | 未着手の性能候補（テキストのマスク合成、MP3 FIR、整数平方根、装飾光線の PIE 化など） |
| [kasane-pet-row-cache.md](perf/kasane-pet-row-cache.md) | 記録 | PET 画像 provider の重複復号を 64 行キャッシュで消した（切替つき、8,208 B、−70.4%） |
| [kasane-tile.md](perf/kasane-tile.md) | 記録 | 群のタイル面: 到達判定（3a）・ブロック幅（3b、16 画素は却下）・滑らかな層のブロック定数＋画素増分（厳密と近似の 2 段、命令数と動く画素の実測） |
| [kasane-lut.md](perf/kasane-lut.md) | 記録 | 群の直接ブレンド連鎖を量子化キーの表へ（solid は厳密・既定 ON、TEXT は 16 段の近似・既定 OFF。画素あたり命令と動く画素の実測） |
| [fpu-latency.md](perf/fpu-latency.md) | 記録 | **この石のスカラー FPU の性質**（`main/hal/fpu_latency.c`、ホーム画面で `F`）: 依存 4.07 / 独立 1.02 cy＝3サイクルのストール、パイプライン4段（2本織れば 2.04、4本で飽和）、FPU へ入る往復は **0**、`madd.s` は **fused**。手書きの順序は1画素で −24%・2画素で1.08倍。`-fschedule-insns` は10%遅い。**手書き版は書いて測って戻した** —— fused 縮約・検査インスタンス違い・ホストの第3の意味論という検証の穴3つが 0.3% に見合わなかった。浮動小数を書き直す前に読む必須手順つき |
| [flower-shade.md](perf/flower-shade.md) | 記録 | FLOWER の単項最大 `shade`（748 cy/hit）を割った結果。死んだ `garden_dither` 呼び出しで `rgbd` 214 → 139 cy/call。`sqrtf`/`__divsf3` の逆アセンブル（どちらも FPU の Newton 列で、整数平方根は実機 A/B で**遅い**）。`lsi` による定数の直接ロードは命令 688 → 587・flash −344 B だが**時間は動かず**。§10 にこのシーンで実行間比較が成立しない理由（`rays` が位相で 4,700〜7,067 cy/行）と、初出時の fps 主張の取り下げ |
| [ray-stall-census.md](perf/ray-stall-census.md) | 記録 | 「rays に余地は無い」をハードウェアカウンタで検算したら、**余地は rays の外にあった**。CPI 1.75 の内訳は命令退役0.55 / 命令RAM・ROMビジー0.28 / バブル0.10 / データ待ち0.02 で、**反復除算0.4%・反復乗算0%** —— 算術に余地は無い。そして §2.5: その「ビジー」は**16KBの命令キャッシュ**で、32KBにすると 0.28 → 0.05、IPC 0.55 → 0.75、**draw 31.38 → 23.56 ms（−25%、コード0行）**。代償は DRAM 16 KiB（`app_largest` 110,592 → 94,208、JSアプリ4本は通る）。`I_MEM_CACHE_MISSES` が常に0なのは「ミスが無い」ではなく S3 の flash キャッシュを LX7 のカウンタが見ていないからで、**`I_STALL_BUSY` はこの石では flash キャッシュミス待ちと読む**。§0 と §2.5 は同じ失敗を2回した記録 —— 厳密な0を「無い」と読んだ話 。§2.6 は**出荷ビルドに入っていたシーン自身の計測コード**（`ray_row` 内に14本の `esp_cpu_get_cycle_count`＋`"memory"` クロバー）を `SCENE_PROF` にして −1.87 ms、§2.7 は `flower_draw` の span 分割（2,991 → 1,199 B ＋ `bell_span` 1,463 B、bell を持つのは14種中4種だけ）で −0.5〜0.7 ms と、**種もビューも固定しないと2 msの差が測れない**ことを3回間違えて学んだ記録 。§2.8 で IRAM を単価比較して却下（0.28 対 0.33 ms/KiB）したあと、§2.9 で**結論が引っくり返る**: 作業集合は行ループ11 KBではなく**per-frame 27.5 KB 対 16 KB**で、1フレーム1回の `flower_build_botanicals`（9,725 B）を外すだけで crocus −3.18 / daffodil −4.72 ms —— 行ループ全体を外すより効く。§2.6 でこれを「実行時間0.4 msだから」と候補から外していたのが誤りで、**占有と実行時間は同じバイトの別の値段** 。**そして §2.10 で §2.6〜2.9 の時間の数字をほぼ全部撤回する**: `FLOWER_PREP_PAD`（IRAMへ移したうえで空いた番地に同サイズの nop を詰める）で勝ちが消え、占有ではなく配置だった。決定打は再測 —— **同じフラグ・同じ種・同じビューで 23.25 が 28.80 になった**。差分はピクセル完全一致・仕事不変・354 B 減のプリミティブ実体化だけ。**16 KB のままでは、絵を変えない変更がフレーム時間を5.5 ms（約20%）動かす**ので、削った効果を測る手段が無い。生き残るのは 32 KB の結果だけで、その価値は平均5.3 msより**フレーム時間がコード変更に対して安定すること** 。§2.11 が機構: 16 KB / 8 way / 32 B ライン = **64セット**で番地は **2,048 B ごとにエイリアス**、1フレームの命令フットプリント約20.6 KB = 643ラインは **1セット 10.1 ライン対 8 way** —— コンフリクトミスは構造的で、どれが誰を追い出すかは `番地 mod 2048` だけで決まる。32 KB なら 128セット・5.0ラインで収まり、**平均が速くなるだけでなく配置が意味を失う** |
| [flower-row-invariants.md](perf/flower-row-invariants.md) | 記録 | 5体の並行調査＋Fableの査定から実装した3件と、外した1件。植生の行不変量を1フレーム1回へ（`GardenVeg`、veg 11,219 → 4,636 cy/行、−3.3〜3.7 ms/frame、`GardenFrame` 448 → 1,244 B）、bell の高さ窓を平方根なしで事前判定（ホスト 83.2% 不要／誤棄却0、実機 bell 832 → 580 cy/visit・bell内sqrt −81%）、`garden_mix` の強制インライン（−316 cy/行、ただしビルド間比較）。**楕円体スパンの当たり区間は代数もホスト検査（40.9%棄却・取りこぼし0）も通ったが実機で測定限界以下で、外した** —— 落とすのがループ中で最も安い画素だったため |
| [kasane-text-damage.md](perf/kasane-text-damage.md) | 記録 | テキストの過剰再描画を3段で潰した記録。編集欄の1打鍵が全画面64,800Bだった件（`ksn_core_invalidate_bands`）、damage が帯ごとの列範囲を持つ件（`ksn_damage` / `present_rect` / 16画素丸めと閾値192、狭い窓が転送経路を落とす実測つき）、TEXT の damage が変わった字だけになる件（`ksn_text_port.advance`）。hello の1桁更新で 11,520 B → 768 B、3.22 → 1.92 ms |
| [kasane-text-span.md](perf/kasane-text-span.md) | 記録 | テキスト span のインクループをセルの列範囲へ（厳密。1 列 70→27 命令・除算 4→0、歩く列はアプリの 4 テキストで 7.6%。実機の計器が span を render_ms の 48% と指したのが根拠で、実機のミリ秒は未取得） |
| [system-pie-survey.md](perf/system-pie-survey.md) | 調査 | システムAPI（`main/system/`）に PIE の余地があるか。結論はほぼ無い —— 728 行は 4〜9 エントリの有界な帳簿処理で、1 フレーム 700〜1,500 命令 ≒ `render_ms` の 0.2%。唯一の大きい項は `sys_clock_snapshot` の 64bit 除算 2 本（libgcc 223 命令/本）で、採算はゲストの呼び出し回数が決める |
| [libgcc-64bit-division.md](perf/libgcc-64bit-division.md) | 調査 | libgcc はこのイメージで **102 B** しか無い（要求しているのは IDF の gpio/efuse）。64bit 除算は Rust の compiler_builtins から来て、そのメンバは別の理由で既に入っているので **C 側を消しても 0 B**。C 側の呼び出し 36 箇所 / 17 ファイルの一覧と、本当に大きいのは compiler_builtins の 46,381 B（libm の数学関数が主）であること |
| [kasane-alpha256.md](perf/kasane-alpha256.md) | 記録 | スカラー 565 ブレンド／パックの /255 を 255→256 の粗スケールへ（切替つき・既定は厳密。α は両アームで不変、動く画素の実測） |
| [kasane-opt-survey.md](perf/kasane-opt-survey.md) | 調査 | 描画経路の棚卸しと候補の選別（境界ごとに「何が律速か」と却下の理由） |
| [kasane-opt-integration.md](perf/kasane-opt-integration.md) | 記録 | 最適化分岐の統合記録（順序・衝突の解決・各段の数値・オブジェクト増減・既定値の一覧） |
| [kasane-consolidation.md](perf/kasane-consolidation.md) | 記録 | kasane 系統の一本化の判定。12 分岐のユニーク 14 コミットは **すべて `perf/kasane-opt` に内容として入っており cherry-pick は 0 本**（`git cherry` の `+` 18 行は統合が衝突を両側残しで解いたため patch-id が一致しないだけ）。元→統合コミットの対応・切替と既定・ホスト検証（suite 71 PASS / ビルド rc=0 / bin 2,210,544 B）・削除してよい 12 枝の一覧 |
| [pie-consolidation.md](perf/pie-consolidation.md) | 記録 | **`perf/mp3-fir` に何が入っているかの一覧と検証状態**（実機で測った値／ホストだけ／未計測の別、数字の出所つき）。flower-decor の性能19本の取り込みで何を両立させ、何を no-op とし、何を入れなかったか |
| [flower-decor-cost.md](flower-decor-cost.md)（docs 直下） | 記録 | 装飾光線のコストを `decor` の内訳（rays / vegetation / rest）まで割った実機計測。`perf/flower-decor` の性能コミットが足し込み続ける文書なので、`docs/` の組み替えで `pie-simd.md` に吸収された後も名前ごと残している（`docs/perf/pie-consolidation.md`） |
| [flower-optimisation-options.md](flower-optimisation-options.md)（docs 直下） | 記録 | flower のフレームをこれ以上どう縮めるかの検討。各候補に「実機 / [obj] / 推定」の別と上限がつく |
| [flower-fixed-point-pipeline.md](flower-fixed-point-pipeline.md)（docs 直下） | 設計 | 固定小数点平方根を caller まで整数で持ち回す案（B=8 の実測が「時間は動かない」だった理由の続き。未実装） |

作業ブランチの装飾光線コストと部分別計測は[flower-decor-cost.md](perf/flower-decor-cost.md)に保持する。
群の合成を 1 枚のアフィン写像へ畳む候補 3c（不透明群はビット一致、非不透明への近似拡張は別段）は
[kasane-group-affine.md](perf/kasane-group-affine.md)。
検証の道具は [`tools/pie/README.md`](../tools/pie/README.md)。

## 主線3: デザインシステム Kasane — [`kasane/`](kasane/)

固定容量の C 描画基盤、その上の汎用 mount/source/presenter、Systemとの所有権境界を扱う。履歴資料と日別実験ログは縮約し、動的描画の構想は現行 v1 の判断資料と分けて置く。

| 文書 | 内容 |
| --- | --- |
| [入口](kasane/README.md) | 読む順番、現在地、機械可読の旧スキーマ例の扱い |
| [境界と契約](kasane/architecture.md) | APIの考え方、owner、source、表示失敗、overlay、System |
| [判断台帳](kasane/decisions.md) | トレードオフ、不採用案と再検討条件 |
| [検証](kasane/verification.md) | 固定実機ゲート、実測の達成範囲、計測の落とし穴 |
| [残タスク](kasane/roadmap.md) | 実装・実測・未達を分けたロードマップ再評価用の表 |
| [動的描画ロードマップ](kasane/dynamic-rendering-roadmap.md) | 関数アート・描画面・動画を、FLOWERとKasaneの実測を参照しながら段階的に進める構想 |
| [手続き型描画の実機診断](kasane/procedural-device-probe.md) | オプトインの表示・負荷診断コード、起動方法とログ項目。実測前の準備 |
| [手続き型描画の上限緩和の実機検証](kasane/procedural-limits-device.md) | plan の動的登録・解除、新上限の全画素一致、最悪時 heap、step 時間、ターン予算で組み立て中のフレームが消える不具合とその修正（実効フレーム予算 8 ms→250 ms）、仕様上限の棚卸し（2026-09-29 実測） |
| [MEGADEMO Act II](kasane/megademo-limit-scenes.md) | 手続き面の上限を叩く3場面（ねじれ廊下・Apple II 風ZENITH・LIMIT）、場面ごとのplan登録/解除、プリミティブ網羅表、上限使用率とゲストヒープのhost実測（2026-09-29） |
| [MEGADEMO の負荷を実機で決める](kasane/megademo-device-limits.md) | 実機で初めて動かした結果（ZENITH で落ちていた）、場面×段階の JS/帯描画/転送/fps/heap 実測、破綻の境界（面の確保・plan の同時数・回転読み出し・登録時間・ゲストヒープ）、決めた `KN`/`LOAD` と既定段階、仕様上限が先に効く項目（2026-09-29 実測） |
| [DERBY WATCH の背景は何が重いか](kasane/derby-background-cost.md) | 観客席・観客・柵・刈り目の plan を外した差（カメラ×段階）、帯描画の費用モデル（1 線分・1 帯・1 画素）、VM の SIN が引数約 201 超で約 7〜9 倍遅い発見、画素キャッシュと透明な面の効果の上限とメモリ（2026-09-30 実測）|
| [面ごとの線分上限](kasane/surface-segment-cap.md) | `createSurface({maxSegments})` で面1のフレームを n 本分に縮める API、1 面の確保量（256 本で 5.4 KB、既定 1,024 本で 21.5 KB）、DERBY WATCH の評価直後に面1が確保できる境目と plan 登録への影響（2026-09-30 実測）|
| [JS 風の言語 → 手続き型 IR](kasane/js-to-ir.md) | plan を名前と式で書くコンパイラの試作（`tools/kasane_ir/`、手書き 13 本と出力一致、命令 −3%）、plan の文字列と `prog()` のゲスト常駐（評価後 7.1 KB、段階 3 で 8.1 KB）、native の plan が命令数に依らず 872 B である点、flash の plan を id で登録する設計と優先順位（2026-09-30 host 実測・推定）|
| [plan を命令数ぶんだけ確保する](kasane/plan-sized-alloc.md) | `ksn_proc_sized_plan`（40 + 12n B、以前は 872 B）、命令数ごとの heap のブロック、DERBY の 25 本 22.5 → 9.7 KB とターン内の最小 +11.4 KB、首振り段階 3 の LOADSTALL の解消、断片化（`lg`）、flash の id 登録への見通し（2026-09-30 実測）|

## JS API — [`api/`](api/)

| 文書 | 種別 | 中身 |
| --- | --- | --- |
| [common-api.md](api/common-api.md) | 仕様 | 共通 JS API `pocket.*`。冒頭に節ごとの実装状況（BLE 以外は実装済み）。`main/pocket/` はこれの実装 |
| [filesystem-api.md](api/filesystem-api.md) | 仕様 | `pocket.fs`。`app:` / `assets:` / `sd:` の3ボリュームとも実装済み |
| [backlog.md](api/backlog.md) | backlog | BLE、Opus の採否、SD の帯域、PC bridge の wire 形式など |

## 基盤と環境 — [`platform/`](platform/)

| 文書 | 種別 | 中身 |
| --- | --- | --- |
| [architecture.md](platform/architecture.md) | 設計 | 現在のディレクトリ構成、責務、アプリの状態遷移、起動と終了 |
| [hardware-constraints.md](platform/hardware-constraints.md) | 仕様 | ハードウェア仕様と開発上の制約（RAM 表、配線、UI ノード数の崖） |
| [build-environment.md](platform/build-environment.md) | 仕様 | Windows / EIM の開発環境とビルド手順 |
| [test-commands.md](platform/test-commands.md) | 仕様 | 実機テストとホスト側テスト（PIE 3層、WSL のみのもの）のコマンド一覧。守る規則は CLAUDE.md |
| [wifi-autostart.md](platform/wifi-autostart.md) | 設計 | Wi-Fi の自動起動: ホームのアイドルで一過性の時刻同期（アプリ優先で中断）、参照カウントの接続サービス `net_service`、常駐の背景サービスへの方針候補と実機で測る項目 |
| [keystate.md](platform/keystate.md) | 設計 | 物理キーの押下集合（HAL の keystate）と `pocket.input.keys`: 入力経路と消費者の一覧、ライフサイクル、却下案、実機で測る同時押し・ゴースト・FIFO あふれ |
| [idf-tls-txbuffer-report.md](platform/idf-tls-txbuffer-report.md) | 記録 | ESP-IDF の TLS 送信バッファの二重計上（上流への報告草稿、未送信） |
| [backlog.md](platform/backlog.md) | backlog | srcstore とエディタの保存まわりの不具合2件（コードで再現確認済み）、入力キュー、Docs 機能 |

## ホームと背景 — [`scenes/`](scenes/)

| 文書 | 種別 | 中身 |
| --- | --- | --- |
| [home-ui.md](scenes/home-ui.md) | 仕様 | ホーム UI: XMB 操作、Apps / Settings、背景の選択、遷移、エラー画面 |
| [xmb-settings-sound.md](scenes/xmb-settings-sound.md) | 仕様 | XMB の設定項目と操作音（起動時に焼き込む効果音テーブル） |
| [flower.md](scenes/flower.md) | 設計・仕様 | FLOWER 背景: 花の種、カメラとショット、森・光・雨、シーン遷移、装飾光線、メモリ |
| [solar-sail.md](scenes/solar-sail.md) | 設計 | SOLAR SAIL 背景: 惑星と衛星の軌道モデル、時刻同期の境界 |

## アプリと機能 — [`apps/`](apps/)

| 文書 | 種別 | 中身 |
| --- | --- | --- |
| [japanese-input.md](apps/japanese-input.md) | 仕様 | SKK 日本語入力と `pocket.input.text` |
| [player-overlay.md](apps/player-overlay.md) | 設計 | ホーム画面プレイヤー: オーバーレイの仕組みと費用の管理 |
| [pet-companion.md](apps/pet-companion.md) | 仕様 | Pet Companion: PC 連携、API、実装メモ |
| [pet-asset-design.md](apps/pet-asset-design.md) | 設計 | ペット画像の省容量化（PPT2 形式） |
| [mp3-implementation.md](apps/mp3-implementation.md) | 記録 | MP3 実装と実測 |
| [opus-feasibility.md](apps/opus-feasibility.md) | 記録 | Opus 復号の実現性調査と、実装後の答え合わせ |
| [lcd-catch.md](apps/lcd-catch.md) | 設計・記録 | LCD CATCH: 固定セグメントの液晶ゲーム。Kasane の ref 32・コマンド 80 に収める設計、host の台本再生、実機で調整する項目 |
| [derby-watch.md](apps/derby-watch.md) | 設計・記録 | DERBY WATCH: 線画の疑似 3D で観る競馬、再現できるレースのモデル、起動ごとの種、較正したオッズ、場面ごとの plan、host の全画素検証、`pocket.app.load` のチャンクへの分割 |
| [derby-corner-model.md](apps/derby-corner-model.md) | 試算 | DERBY WATCH の楕円コース案: カーブだけの内外差とばらつきの乗数、枠・本命・入れ替わりへの効き、楕円用のオッズの再推定（host の Monte Carlo） |
| [derby-pan-camera-cost.md](apps/derby-pan-camera-cost.md) | 記録 | DERBY WATCH の首振りカメラ・楕円コースの計算コスト: JS 演算と投影の単価（実機）、VM の Newton 逆数による投影、台数・自動ズーム・LOD 別のフレーム費用の見積もり（2026-09-30） |
| [big-wave.md](apps/big-wave.md) | 設計・記録 | BIG WAVE: 線分の疑似3Dで大波に乗るゲーム。世界固定の断面と 1/d の級数、host の台本とボット、全画素検証、負荷と上限の使用率、実機で詰める項目 |
| [backlog.md](apps/backlog.md) | backlog | チュートリアルの見直し、オーバーレイの残り、日本語入力の残り |

## 過去の知見 — [`archive/`](archive/)

| 文書 | 種別 | 中身 |
| --- | --- | --- |
| [findings.md](archive/findings.md) | 記録 | 削除した過去の記録から残した知見（M1 の受け入れ結果、背景描画の最適化手順、傾き連動と被写界深度で退けた案、XMB の設計根拠） |

過去の文書そのものは git の履歴にある（2026-09-15 の整理で削除）。

## 古いパスをわざと残した場所

`docs/<名前>` の参照はリポジトリ全体で新しいパスに書き換えた。次の2種類だけはパスを書き換えていない。ファイル名は変えていないので、名前で検索すれば見つかる。

- **JS のソース**（`apps/**/*.js`、`tools/vmtest/**/*.js`）。ゲストは起動時にソースを解析するので、バイト数がそのままヒープを使う。OOM の境界を突くテストは、ヒープの余りがバイト単位で動くと結果が変わる。節番号の書き換えは同じバイト長で行った。
- **上流へ出すパッチ**（`reports/upstream/*.patch`）。レビューした内容のまま保つ。
