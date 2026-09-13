# 新DS: 公開契約の試作と評価

2026-09-13。ブランチ`vm/design-contracts`、vm/mainの`d4b1d73`を基点に仕様コミットを取り込んだ。
最初の段階ではCヘッダと固定関数表、呼出側の例、記録用doubleを追加した。続く自明な実装範囲として、固定容量の更新コアを追加した。実アプリへの接続は行わない。

## 構成

- `main/ui/ds/ds_types.h`: 型、作成/更新の引数、容量、32 B保存形式の候補。
- `main/ui/ds/ds_api.h`: アプリ向け更新とホスト向けpresent/schedule/resetを分離。
- `main/ui/ds/ds_ports.h`: 共用LCD帯、Flash画像の行供給、任意backdropの境界。
- `tools/ds_contract/use_cases.c`: ペット初期表示・育成値更新・通知表示・モーダル表示の呼出例。
- `tools/ds_contract/probe.c`: API呼出を記録し、失敗を注入する検証専用double。
- `main/ui/ds/ds_core.c`: 固定2バンク、レイヤー別容量と世代、更新検証、提出/採用/破棄。
- `tools/ds_contract/test_core.c`: 実コアに対する容量・参照・失敗契約の検証。

仮想関数はCの関数表。1 backendにつき1表で、命令ごとのvtableやheapは作らない。
ヘッダと更新コアにESP-IDF/FreeRTOS/QuickJS/PocketJSのincludeはない。QuickJS binding、ピクセル生成、damage、資源登録、animationは未実装。
pet画像は汎用image portへ登録し、variant/frameをペットadapterが配色/表情へ解釈する。DSにpet domainの依存を入れない。

## 呼び出し例から決めたこと

1. 作成引数は一時的な大きなdescriptorでよい。保存形式32 Bと同一にすると文字ポインタや型安全性に無理が出る。
2. client生成時にlayerを固定し、beginはtransaction tokenだけを返す。SYSTEMのreplaceがAPPを消してはならない。
3. 表示参照はend成功後だけアプリ状態へ反映する。途中OOMでは候補参照を捨てる。
4. textはポインタ＋バイト長で渡し、その呼出中にコピーする。スタック上の数値表示文字列も安全に扱える。
5. endは提出の成功。実LCD転送成功と区別し、後者はhost.presentが所有する。
6. BUSYはアプリのドメイン更新を巻き戻す理由にしない。未表示状態をアプリが保持し、次の機会に最新値を送る。
7. モーダルの演出能力不足はSOLIDを返して表示可能にする。例のcaptureは常にSOLIDで、ぼかし実装を偽装しない。

## レンダラを作る前に解決すべき事項

| 事項 | 評価・次の設計作業 |
| --- | --- |
| レイヤーごとの世代 | 固定領域とレイヤー別epochを実装済み。replace開始で世代を消費し、abortでも再利用しない。長期wrapのレビューは残る |
| 2バンクと通知待ち | 共有builderにより通知がBUSYになる。未完トランザクションをJSが長く保持する場合の取消/期限を定める必要がある |
| 強いJS境界 | clientを生成時にAPP/SYSTEMへ固定し、beginからlayer指定を除去した。QuickJS adapterへAPP clientだけを渡す配線は未実装 |
| モーダルcapture | 同期APIは呼出順の検証には使えるが、帯ごとの継続・取消・期限応答を表せない。長いcaptureが測定された場合のhost状態機械が必要 |
| backdropの所有 | 暗黙の「次replaceへ適用」は使い方を間違えやすい。production実装前に明示的capture handleとtransactionへのattachを検討する |
| モーダル復帰 | 最新状態からAPPを再構築し、present成功後にcaptureをreleaseする。古いpet_view参照は再利用不可。復帰失敗時はモーダルを維持する |
| 画像scratch | read_spanはRGB565とalphaの両出力を要す。ペット64画素で192 Bとなり、従来の128 B行だけとは異なる。512 B共用scratchの使用順を検証する |
| 任意字形 | font portは未定義。coverage形式、baseline、欠損字形、Flash寿命を既存フォントと接続してから確定する |
| アニメーション完了 | animate/stopは定義したが、完了ビットのpoll APIは未定義。JS側の寿命・失敗・再利用を含めて設計する |
| API補助操作 | clip/offsetのbuilder、整数表示、schema生成物、focus/command routingは本prototypeの外。例では座標と文字を明示生成する |

現段階で「APIが完成している」とは評価しない。基本の描画更新と容量境界は動くが、rendererへ提出bankを安全に公開する内部interface、modal寿命、長時間builderの扱いは実アプリ統合前の設計課題。
32 Bのstatic_assertは保存候補1件のサイズだけを検査する。native全体16 KiB、stack、JS heapの達成を証明しない。
現在の内部構造体は8,320 B、呼出側が予約する`ds_core`は9,216 B。公開limits/statsは実際に保持する9,216 Bを返す。
patch開始時は表示bank全体約4 KiBを構築bankへコピーする。heap確保はないが、局所更新としての実機時間は未測定でありAstraレビュー対象。

## 検証の範囲

`bash tools/ds_contract/run.sh`でC11ホストのASan/UBSan付き実行とC++17からのheader読込みを検査する。
正常なペット構築、0/100/範囲外更新、通知layer、BUSY、作成各段階とendでのOOM、modal fallbackとabort cleanupを対象とする。
doubleは画素・世代・本物の2bank・LCD ack・割当を実装しない。実コアの別テストは2bank、レイヤー別世代、固定文字領域、poison/abort、提出の採用/破棄を検査する。画素・LCD ack・animationは検査しない。
ESP32-S3向けにはuse_casesとprobeをオブジェクトまでコンパイルして型と32 Bサイズを確認する。ファームウェアのリンク/実機表示試験は次段階。

今回の結果: WSLのGCCでASan/UBSan実行PASS、C++17 header構文検査PASS。
`xtensa-esp32s3-elf-gcc` 15.2.0でコア・ユースケース・テストを`-Os -Wall -Wextra -Werror`コンパイルPASS。
新コアを`main/CMakeLists.txt`へ追加したESP-IDF v6.0.1全体ビルドPASS。コアのインスタンスはまだ作らないため、リンク時のDIRAM増分は測定対象外。
実装変更はworktree内の未コミット状態。基点へ仕様を取り込んだコミット以外のcommit/push/mergeは行っていない。
