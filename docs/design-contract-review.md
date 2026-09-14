# 新DS: 公開契約の試作と評価

2026-09-14追記: 現行の利用窓口は[利用API v0.3](design-api.md)。
core/cache/modalの後処理を`ds_view_host`へ統合し、layer別提出結果と未完builder取消を実装した。
以下の初期prototype・段階別記録は履歴であり、現在の公開API一覧としては同文書を参照する。

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
| レイヤーごとの世代 | 各レイヤーにepochを保持し、発行元はプロセス寿命で共有する。abort/discard/resetで再利用せず、上限ではLIMIT。枯渇境界の回帰検証済み |
| 2バンクと通知待ち | 共有builderにより通知がBUSYになる。未完トランザクションをJSが長く保持する場合の取消/期限を定める必要がある |
| 強いJS境界 | clientを生成時にAPP/SYSTEMへ固定し、beginからlayer指定を除去した。QuickJS adapterへAPP clientだけを渡す配線は未実装 |
| モーダルcapture | 同期APIは呼出順の検証には使えるが、帯ごとの継続・取消・期限応答を表せない。長いcaptureが測定された場合のhost状態機械が必要 |
| backdropの所有 | v0.2で明示的capture handleとtransactionへのattachに決定。現C prototypeの暗黙「次replaceへ適用」は置換対象 |
| モーダル復帰 | 最新状態からAPPを再構築し、present成功後にcaptureをreleaseする。古いpet_view参照は再利用不可。復帰失敗時はモーダルを維持する |
| 画像scratch | read_spanはRGB565とalphaの両出力を要す。ペット64画素で192 Bとなり、従来の128 B行だけとは異なる。512 B共用scratchの使用順を検証する |
| 任意字形 | font portは未定義。coverage形式、baseline、欠損字形、Flash寿命を既存フォントと接続してから確定する |
| アニメーション完了 | animate/stopは定義したが、完了ビットのpoll APIは未定義。JS側の寿命・失敗・再利用を含めて設計する |
| API補助操作 | clip/offsetのbuilder、整数表示、schema生成物、focus/command routingは本prototypeの外。例では座標と文字を明示生成する |

現段階で「APIが完成している」とは評価しない。rendererへの提出ID付きコピー読出しを追加したが、資源検証、modal寿命、長時間builderの扱いは実アプリ統合前の設計課題。
32 Bのstatic_assertは保存候補1件のサイズだけを検査する。native全体16 KiB、stack、JS heapの達成を証明しない。
呼出側が予約する`ds_core`は9,216 B。画像登録表16件をこの予約内へ収め、共有IDカウンタ12 Bを含め、公開limits/statsは9,228 Bを保守的に計上する。複数coreでも共有カウンタは12 Bのみ。
命令読み出し用の`ds_frame_command`と`ds_frame`は呼出側の一時領域で、この常駐容量には含まない。描画器は命令数分の配列を作らず、比較用2件までを再利用する。
patch開始時は表示bank全体約4 KiBを構築bankへコピーする。heap確保はないが、局所更新としての実機時間は未測定でありAstraレビュー対象。

## 検証の範囲

`bash tools/ds_contract/run.sh`でC11ホストのASan/UBSan付き実行とC++17からのheader読込みを検査する。
正常なペット構築、0/100/範囲外更新、通知layer、BUSY、作成各段階とendでのOOM、modal fallbackとabort cleanupを対象とする。
doubleは画素・世代・本物の2bank・LCD ack・割当を実装しない。実コアの別テストは2bank、レイヤー別世代、固定文字領域、poison/abort、提出の採用/破棄を検査する。画素・LCD ack・animationは検査しない。
ESP32-S3向けにはuse_casesとprobeをオブジェクトまでコンパイルして型と32 Bサイズを確認する。ファームウェアのリンク/実機表示試験は次段階。

今回の結果: WSLのGCCでASan/UBSan実行PASS、C++17 header構文検査PASS。
`xtensa-esp32s3-elf-gcc` 15.2.0でコア・ユースケース・テストを`-Os -Wall -Wextra -Werror`コンパイルPASS。
新コアを`main/CMakeLists.txt`へ追加したESP-IDF v6.0.1全体ビルドPASS。コアのインスタンスはまだ作らないため、リンク時のDIRAM増分は測定対象外。
Solの固定コアは`e072ff8`でコミット済み。以下はその実装に対するAstraレビューと修正。vm/mainへのmerge、push、実機書込みはこのレビューに含めない。

## Astraレビュー: 参照寿命と描画への引渡し

2026-09-13。以下の3件は修正前コアに対する`test_review.c`で失敗を再現した。

- **P1: レイヤーを跨ぐ操作。** SYSTEMのtxをAPP endpointに渡すとadd/abortできた。全tx操作でendpointのlayerを検証し、他所有者のトランザクションを変更しない。
- **P1: PATCH追加の参照再利用。** 追加をabort/discardすると次の追加が同じindex/epochを使用する。構造変更をREPLACEに限定し、PATCHのaddをINVALIDにした。命令ごとの世代表は増設しない。
- **P1: resetによる参照復活。** initで番号を初期値へ戻していた。プロセス寿命の発行元を用い、再初期化でも世代とtxを再利用しない。上限到達ではwrapせずLIMIT。共有の単一owner taskを前提とする。

併せて修正した点:

- 宣言されたbyte配列を無関係な構造体にcastする保存方式を、unionの型付きmemberに変更した。固定容量は維持。
- 長すぎるSET_TEXTをUTF-8走査前に拒否し、空文字のNULLをmemcpyへ渡さない。未実装animationのtracks上限は0を返す。
- 起動時の空APPに黒背景を設定し、APP構築前のSYSTEM通知を可能にした。APP REPLACEには明示背景を要求する。
- `ds_core_frame/read`は提出IDで旧/新状態を検証し、descriptorと文字を呼出側へコピーする。返したdescriptorの文字ポインタはその出力オブジェクト内を指すため、構造体コピー後には再読出しが必要。
- `presented/discard/failed`も提出IDを検証する。古いackが次の提出を採用する問題を防ぎ、転送失敗フラグはdiscardでも維持する。初回/reset後も全面描画を要求する。

ASan/UBSanと`-O2 -fstrict-aliasing`で、レイヤー違反、再初期化、ID枯渇、旧ack、文字コピー、旧/新bank読出し、失敗→破棄→再提出を検査する。
これはLCD転送の実測ではなく、hostが正しくfailed/presentedを呼ぶための状態契約の検証である。
修正後のホストテスト全件、C++17ヘッダ検査、ESP-IDF v6.0.1の`build_ds_contract`ビルドはPASS。
最終ELFを`nm`で確認するとDSコアのシンボルはなく、未接続のためリンク時に除去されている。ファームのDIRAM増分0をDSの使用量とは解釈しない。

残る主要な設計課題:

1. **画像の登録・寿命検証は実装済み。** ホスト登録16件、所有レイヤー、variant/frame・行範囲を検証する。providerはresetまで不変の借用とし、追記のみで提出破棄による解放はしない。実コアのペット例も登録済みIDを使用する。残るのはPPT2等の実providerとcrop/scaleの描画接続。
2. **damageと矩形の部分転送は実装済み。** 前後の命令・文字の実バイト列・背景・full_redrawから17帯maskを算出。矩形alpha合成、無変更時の転送ゼロ、IO失敗後の全帯修復を実装。ホストでは150回の移動について全面参照描画と全画素一致を確認した。文字・画像・角丸等の画素生成は未実装。IO失敗した提出を破棄する場合、ホストが次の提出を予定する責任は引き続き持つ。
3. **アプリの参照公開と提出破棄の連携。** v0.2で固定の提出結果stateをpollし、adapterが対応表を確定/破棄してから次更新へ進む契約を決定。実装は未完了。REPLACE提出破棄後は候補参照を捨て最新domain stateから再構築する。
4. **長時間builder・資源・modal・animation。** v0.2でownerへ戻った未提出builderのabort、captureの250 ms期限と明示handleを定義した。これらの実装、およびfont port・track完了pollは残る。今回の固定コアだけで16 KiB全体予算や実機速度の達成を主張しない。

## v0.2で追加した必須要件

[コンポーネント合成仕様](design-composition.md)を実装の規範とする。
部分/全体の重なり、部品全体の透過、solid/dim-live modal、明示cacheの出し入れと複数instance表示をMUSTとした。
現矩形レンダラの命令alpha試験だけではグループopacityの完成を意味しない。
固定4 KiB RAM cache、template/instance各8件、矩形系templateの複数表示・移動・非表示・release/abort/resolveは実装・実機検証済み。
2026-09-14: 矩形の隔離グループopacity、提出結果poll、native modal入力scopeとfocus復帰を追加した。全256段階の透過画素比較、転送失敗復旧、取消・容量不足をホストで検証し、実機でも透過画素とmodal開閉を検証済み。
text/image/gradient cache、fork、Flash定義、矩形以外のrenderer、QuickJS bindingと実アプリのowner/input接続は未実装。native modalのみでMUST全体完了とはしない。軽量JS参照APIとblur/変形/3D平面投影はBETTERとして別に追跡する。
