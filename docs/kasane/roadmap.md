# Kasane の残タスク — 再評価用

これは時系列の開発日誌ではない。各行に「目標」「実装」「実測」「残る判定」を分ける。詳細な数値と証拠の適用範囲は[verification.md](verification.md)、不採用理由は[decisions.md](decisions.md)。縮約した5文書だけを入力にしたAstraレビューを反映した。これは**資料の整合性と次の判定方法の提案**であり、コードや実機の独立再検証ではない。**ゲートの妥当性**と**実装の欠陥**を別々に評価し、過去の固定ゲートを事後変更しない。次回以降の試験条件を改訂するなら、改訂前に対象workload、独立boot数、試験時間/frame数、許容揺らぎ、全runの扱いを決める。

| 領域 | 目標 | 現在 | 残る判定 |
| --- | --- | --- | --- |
| P0 | 比較可能な正しさ/性能基準 | **基準確立済み**。hello、musicの固定ゲートと注入repair試験を定義。 | 最新候補の不合格はP0基準の未完成を意味しない。将来試験の統計手順は別作業。 |
| P1 | アプリ非依存 source と音声共存 | generic bind/typed snapshot/固定pool、decoder配置・heap順序対策を実装。host shimでarena/複数producer・readerを確認。実機の2コア同時arena初回確保は独立boot×2で重複確保の片方を解放し、全mutex回収後backing解放。確保失敗注入→fallback/回復、pool全pin→5回skip→release後再利用も確認。output sourceは停止未確認で旧serviceを保持・boot中BUSY、playback sourceは旧pin中BUSY・release後に回収。wall sourceも借用中の文字不変・旧pin中BUSYをhostで確認し、実機の通常分更新は21画素変化・領域外0。最新の全probe OFF製品imageはbuild/容量と20秒music→Hello smokeを通過。 | 30 Hz可視表示の製品要否、実heap全体のOOM、停止timeout/孤児leaseの実機再現、別task同時publishは別途判定する。 |
| P2 | dirty-nodeで変わった所だけ解決 | **24-nodeの定義済みworkloadでは達成**。依存表と差分解決、CPU利益、画素、注入修復を確認。 | 他画面への適用範囲は別に判断。overlay全画面転送、実GRAM、物理SPI故障をP2の直接合格条件へ混ぜない。 |
| P3 | **native text**公開済みpayload→各core destinationを≤1 copy | **限定したtext経路では達成**。実機1 destination、host複数destination、lease/非表示/pool枯渇を確認。 | 他source型を必須範囲にするなら型と操作を指定。複数destinationの実機は必要性に応じて選ぶ。seek可能音源はP3 copyよりP5統合の課題。 |
| P4 | producer原データ→描画まで全経路≤1 copy | 部分的clone/JS一時copyを削減したが厳格目標は未達。copy/変換/転送の操作的定義と既存計数器の重複範囲を固定。実機45秒のnative textはsource→core 46回/368 B、text bank clone・render scratch copy 0。実music 45秒でもtext clone/render scratch/decode copyは0だが、view decodeが13,359回/708,027 B（約498 B/frame）、命令・metadata等の構造copyが残る。同一診断imageの独立boot×2でview decodeは空括弧差引き154〜155 cycles/回、約5.8〜5.9 µs/frame。単純slot COWはborrow pointer契約と2bank予算で不成立。 | 現musicのview decode単独は危険な所有権変更を正当化しない。payload lineageと未計数のQuickJS内部、他の構造copyのCPU寄与を測り、利益上限から候補を絞る。小さければ厳格目標は未達・保留。必要な候補だけRAM、断片化、repair、安全性、描画p99で比較。 |
| P5 | 音楽と通常アプリを製品経路に統合 | 通常5アプリmount、music source-only、限定light PATCH、通知・pause・低heap・切替の複合試験まで進んだ。位相分類ありの45秒×2ではsend最大は両runとも通常frame（5,302/5,291 µs）、注入451/455 µs、復旧4,551/4,846 µs。位相分類を分離・OFFにした同条件の独立boot×2でもsend最大5,336/5,358 µs、draw最大9,510/9,415 µs、音声障害0で固定線を通過。静止music helpの注入修復はSPI直前の32,400画素が独立boot×2で一致。全probe OFFの製品imageも45秒music→Hello切替のsmokeを通過（decoder fault/IO ERROR=0）。旧5,506 µsは分類不能の不合格として保持。 | 故障注入付き診断imageの時間値を全probe OFF製品imageの性能と呼ばない。既知duration・SD抜去・物理GRAM等はリスクごとの代表条件を選び、全条件の直積を要求しない。全計画PATCHは現状不採用。 |
| PIE text | binary maskを追加copyなしで8 lane処理し、画素と描画時間を悪化させない | 0/255 port宣言、PIEカーネル、整列/ゼロskip、全256 maskの命令シミュレーションとhost画素A/B。整列日本語診断では描画p99 4,095→3,327〜3,455 µs、全画素一致。実music 45秒の同一binary・独立boot ABBAは音声障害0だが、overlay compute p50はOFF 3,839 / 3,839、ON 3,967 / 3,967 µsで利益なし。既定OFF。 | 現musicでは採用しない。高速化対象を明確化して小ブロックのPIE呼出し等を調べ、改善案があれば画素・音声・性能を再判定する。現時点で製品既定ONにしない。 |
| Framework全体 | 新アプリがKasane C core変更なしで安全にmountできる | 汎用schema/slot/descriptorと低レベルescape hatchはある。既存5アプリが例となる。 | v1で必要なcache/modal、native home/picker/editor、制作スキーマ/生成器、効果を**必須/研究/対象外**に分類。全部を暗黙の完了条件にしない。 |

## 直近の再現セット

詳細な数値・旧不合格の扱いは[verification.md](verification.md)。下表の診断imageと全probe OFF imageを同じ性能試験として比較しない。

| 判定 | image SHA-256先頭・条件 | 入口と証拠 | 結論の範囲 |
| --- | --- | --- | --- |
| P1異常停止の安全側 | 全probe OFF `7E5294D0`。hostでは停止未確認と旧playback leaseを模擬 | `tools/build_kasane_test.sh`→実QuickJS O2統合試験、`.cache/kasane-product-playback-lease-smoke-20260925` | output sourceはboot中BUSY、playback sourceはpin中BUSY・解放後再開。実機では20秒通常再生→Hello正常。実機停止timeout/孤児leaseは未再現。 |
| P1 wall不変snapshot | P0診断 `7CD8A196`、全probe OFF `F7C4F2FB` | `tools/kasane_wall_source_device.py`、`.cache/kasane-wall-source-immutable-20260925` | hostで複数pin中の更新保留、実機で1分後21画素変化・領域外0。孤児leaseの実機再現ではない。 |
| P4 view decode CPU | `26D7A956`、P0とdecode cycle ON、copy計数OFF | `tools/sd_async_compare_run.py`、`.cache/kasane-p4-decode-cycles-20260925` と `-run2` | 独立boot×2、空括弧差引き約5.8〜5.9 µs/frame。P4全copyの上限ではない。 |
| P5音声共存修復 | `048D9F91`、P0/低heap/故障注入ON、send位相OFF | `tools/sd_async_compare_run.py`、`.cache/kasane-p5-gate-no-phase-20260925` | 独立boot×2、固定線と音声障害0。故障注入のない製品性能ではない。 |
| P5修復画素 | 同じ`048D9F91`、静止help・音声なし | `tools/kasane_overlay_repair_pixels.py`、`.cache/kasane-p5-repair-pixels-20260925` と `-run2` | 独立boot×2、各32,400画素差分0。SPI直前でありGRAM読戻しではない。 |
| 全probe OFF通常経路 | 最新`F7C4F2FB`、build・実機smoke済み | `tools/sd_async_compare_run.py --product-smoke`、`.cache/kasane-product-wall-lease-smoke-20260925` | 20秒music→Helloの機能確認。時間分布・pause・修復は主張しない。 |

## Astraレビュー後の最小順序（提案、製品スコープは未決）

1. **v1対象を固定する。** 通常5アプリ・music・SYSTEM通知のうち実際に製品保証する操作、cache/modal、33 ms表示、home/editor、効果を必須/研究/対象外へ分類する。各必須項目に代表workload・合否・撤回条件を付ける。P4の研究目標を削除せず、v1出荷判定との関係を明記したら完了。
2. **repairの単発超過を切り分ける。** 旧5,506 µsのframeは既存ログでは特定不可。追加した独立boot×2の分類では最大値は通常frame、注入・復旧はそれより短い。この事実から旧runを再分類しない。位相分類をOFFにした独立boot×2の同条件は事前固定線を全run通過した。旧5,506 µsは不合格のまま残す。なお故障注入には診断buildが必須なので、この測定を全probe OFF製品imageの性能に外挿しない。38 msのrepair所要時間は製品許容値未設定の参考値とする。
3. **source/arenaの安全性穴を閉じる。** 真の並行activate、pool全pin、OOM、release/APP終了をhost・必要な実task条件へ分ける。有界BUSY/OOM、deadlock/UAFなし、lease破壊なし、復旧後に再利用可能、音声障害なしを合格条件にする。
4. **v1代表回帰をまとめる。** mount→更新→終了、music既知/未知duration、通知、pause/resume、低heap、repairから故障影響の大きい組合せを事前選定。SD抜去は製品で約束する復旧動作を定義してから追加。通常動作は音声障害0、画素/論理状態一致、既存性能線を確認する。故障注入時は事前定義したエラー通知・停止/復帰・UI応答と有界repairを判定し、意図したIO ERRORを通常動作の音声障害と混同しない。全アプリ×全曲×全故障の直積は要求しない。
5. **P4を別の研究判定で区切る。** copyの操作的定義を先に決める。文字生成、UTF-8 materialize、schema保有、bank clone、render decode/scratch、LCD送信の各操作について「データ複製」「変換」「転送」のどれを数えるか、各回数/bytes/CPU寄与を記録する。現行RAM余裕と実害を測り、改善上限が小さければ未達・保留とする。第3bankや新lease実装を自動的に必須にはしない。

レビュー時点で不足している材料は、v1必須機能一覧、send max 5,500 µsの製品上の意味、現在のcore/arena/stack余裕、sourceの鮮度要求と省略許可、cache/modal/効果の製品対応表、現行image/flags/スクリプト/結果を結ぶ短い証拠manifestである。資料から決められない項目を「実装済み」や「出荷阻止」と推測しない。

## 優先順位を決めるための質問

1. 「描画を劣化させない」はどの製品workloadか。通常musicの全画面送信をP2 dirty-nodeの成否に含めない。音声・低heap・通知・修復の同時条件を、頻度と故障影響に応じて最小の代表集合にする。
2. 33 ms可視 source は製品要件か研究要件か。約1 Hzの数値snapshotと15/30 Hzのアニメーションは同一要求ではない。頻度を下げて見かけ上合格としない一方、不要な30 Hz更新も課さない。
3. P4全経路1-copyを、RAM・複雑さ・p99を悪化させても追うか。まず残copyの発生頻度/bytes/所要時間を測り、性能上の利益の上限を示す。目標を残すことと製品出荷を止めることは別の判断。
4. send max単発6 µs超過の扱いをどう一般化するか。今回の不合格は維持し、今後は分位点・繰返し・worstの役割を事前に再定義する。音声fault、画素不一致、repair破壊は統計的揺らぎとして許容しない。
5. System/cache/modal/将来効果をどこまでKasane FW v1の完了条件にするか。実装済みAPI、実機検証済み能力、制作時の構想を混ぜない。

ロードマップの更新時は各タスクに owner、合格条件、ホスト試験、実機条件、予算増分、撤回条件を付ける。数値がない提案は「速くなる」「0 copyになる」と断言しない。
