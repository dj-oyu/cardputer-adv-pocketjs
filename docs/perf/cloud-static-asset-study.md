# AYAM 静的定義の事前変換 — ローカル比較

状態: db4fbb82を固定した後の未採用prototype。製品のcloud entry/chunksは変更していない。実機性能・flash・画像取得の結果ではない。

## 比較するもの

旧db4 sourceをGit blobから保存したfixture、新utility上の同じsource、定義と更新を分けたSOURCE、事前変換PACKEDを分ける。SOURCE/PACKEDは同じcloud_update.jsを使う。通常JS object graphへbinを再展開せず、検証済み初期bufferをruntime nodeへ直接初期化する。動的state、材質変更、描画commitのsnapshotは削除しない。

雲19node（group3、ellipsoid13、blend3/14lobes）のruntime backingは6144 B。binは7248 B（header32、record912、name158、padding2、storage6144）。これはファイルとbackingのサイズであり、guest全体の削減量ではない。

## ローカルAPIと所有権

`pocket.ayatsuri.assetCreate(arrayBuffer)` は入力をguest管理の私有領域へ1回copyしてから検証する。呼出後の入力変更・detachはassetへ影響しない。read-only count/bytes、`describe(index,scratch)`、`initialize(index,exactSizeArrayBuffer)`、`dispose()`を持つ。disposeは冪等。describeのscratchはsetter例外時に部分変更し得るがassetは不変。initialize失敗はtarget不変。再入はBUSY、破棄後の操作は拒否。

Cの`ayatsuri_model_asset_open`はimmutable spanを同期借用する。呼出側は最終describe/initializeまで寿命と不変性を保持する。host試験用static factoryはprocess/firmware lifetimeのconst領域専用で、mallocした一時file bufferを渡して解放する経路ではない。

`scene.instantiate(asset)` は全node成功後だけsceneへ公開し、失敗時に既存sceneを保持する。返り値はrecord順のhandle配列。初期化完了後assetをdisposeできる。runtime nodeと可変材質は独立して保持する。prototypeはmodel構築時にlazy factoryを呼び、finallyでassetをdisposeする。

## AYAM v1

little-endian、header `<4sHH6I` 32 B。magic AYAM/version1/headerBytes32/totalBytes/count/recordOffset32/recordStride48/stringOffset/stringBytes。

record `<HHIhBBBBHII3d` 48 B: nameOffset、storageBytes、storageOffset、parentIndex、nameLength、kind、flags、lobeCount、reserved16、RGB24、reserved32、thickness/opacity/edgeSoftness（F64）。F64はsource scalarの丸めを維持するためで、geometry bufferは既存F32等の形式を保つ。

count1..256、asset最大512KiB。親は-1または先行record。nameは一意なprintable ASCII1..127 B。kindはgroup/ellipsoid/blend、blend lobe1..8。名前は連結、4byteまでzero padding、storageはrecord順で連続、重複/trailing bytesなし。予約値・finite・scale/radii・サイズ・offset/count境界を検証する。packed structへのcastや非整列float loadを使わない。未知version/patternは拒否する。

## 計測の注意

m32/160KiB/no explicit GCで、load、構築、初評価、初publish、定常、dispose前後、reload、realm解放を別記録する。自動GCは有効。QuickJS malloc_size、memory_used_size、requested allocator bytesは別指標。malloc_sizeはrequested bytesと1確保あたり8 Bの管理費を含む。bytecode/object/property/shape等は包含関係があるため単純加算しない。binary_object_sizeはArrayBuffer合計ではない。native free/largest/stackはこのhost observerでは未計測。

static routeでもbin7248 Bは追加const dataとして記録する。copy routeは入力ArrayBuffer、owned asset copy、runtime storageの同時生存を含める。host Cconstを実機flash対応済みと称さない。

## 実機の次候補で埋めるギャップ

db4では外乱・pause/replayは実機未検証。既存appテスト経路から有限fixtureを選ぶ案: EnterでREST / RETARGET / ROOT IMPULSE / DRIVE-OFF SETTLE / PAUSE-REPLAY、Uでreset。固定seed/step番号のeventを一度だけ消費し、pauseはphase/stateを凍結、resumeは大dtを入力しない。新broker機能を要しない。今回のbin A/BにはUI変更を含めず、別候補として検証する。

## 固定した前候補の実機報告（bin試作とは別）

db4fbb82の実機担当報告: 2026-10-30 13:52:50、hone110-flow-flash-db-approved-20261030成功、3領域readback。BIN c7a677284fbd137cd99d62f91ed8602b53ea4349484c46118b95c724f511bcb1、ELF 6dbb42aa7e15def797f0fbdf00e01e1ba8773bad90650850a936d3054f8265cd、receipt 33fce1d2a7d7852f19bcbf97408b8f6ae92ded4f7ed4772b356c3df082946839。

CLOUD66秒で6mode/reset/呼吸/STOP/CLOSE、CHARACTER人狐雲、MEGADEMO60秒、HELLO1からfresh HOME成功。follow/dynamic実ロード確認。CLOUD終了free207780 B/largest69632 B/JS0。30frame集計13窓render64.10–102.77 msで、FPSやpeakの測定ではない。外乱/pause/replayは正式経路なしで未試験、画像captureと物理LCD画質は未受入。最終source db4fbb82、owner/job/queue/sessionなし、serial closed。これは実機担当の報告で、本資料のbin試作を実機検証したものではない。

## 最小A/B結果（2026-10-30、未採用）

同一m32/SSE2バイナリ SHA256 `cba28846cce20d07350c0d0f05b891391ed8459eab951f6902693b7139a4d880`、guest上限163840 B、自動GCあり/明示GCなし、通常app UI付き、301frame要求。common binaryには7248 BのCconstとlazy factoryを全条件共通で含む。これはhostの比較で実機flash対応ではない。

| 条件 | app読込後 malloc_size B | 最終観測 malloc_size B | 管理費込み確保peak B | 結果 |
|---|---:|---:|---:|---|
| 元db4 source（common binary） |110732|159600|163100|301frame PASS|
| 更新処理を分離したSOURCE |117360|121188|163816|初frame失敗|
| PACKED static const |111024|160948|163836|初publish成功、後続frame失敗|
| PACKED owned copy |111024|161124|163824|初publish成功、後続frame失敗|

FAIL経路の「最終観測」は成功した定常値を意味しない。例外表示はnullで、現時点ではOOMと確定診断していない。要求301frameは完了数ではなく、旧observerに完了counterがないため失敗tickは未特定。全経路runtime解放後のguestは0。

copy経路ではcaller input作成時126832 B、owned assetとinputの同時保持時134540 B、input解放後127164 B。最大単一要求はinput7248 B、owned asset7268 B。binのcopyを隠してゼロcopyと称さない。

事前変換はrefactored SOURCEの読込時から6336 B減らしたが、元db4 sourceの全体基準を下回る効果は立証できなかった。共通utility変更だけでも通常2169frame試験のlive最大が160144→164032 Bとなり、strict160KiBの初frameが失敗した。unlimitedでは2169frame/差分描画/解放PASS。**このprototypeは未採用、製品entry/chunksの切替・pushはしない。** 次の検討では任意ロードのpacked extensionによる共通utility費用削減と、モデル更新側の保持費用を分けて測る。削減見込みを実測値とは扱わない。

旧preasset observerの別runはapp定常159408 B、requested allocator peak146500 Bで、管理費込みpeakは未計測だった。common binaryとの定常差192 Bは共有factory等の費用を含むため、binの効果から除外する。新observerはcheckpointでQuickJS malloc_size = requested bytes + 8×live blocksを照合し、確保イベント単位の管理費込みpeakを記録する。

### 画像・境界・製品link

元db4／refactored SOURCE／PACKEDの3619frame描画引数packetと19node初期bufferは完全一致。native rendererの28固定tick PNGは196608 Bの別診断条件でSOURCE/PACKEDがbyte一致した。163840 Bではcapture診断も両経路失敗したため、画像一致を容量門PASSへ読み替えない。

compiler18malformed/256node上限/決定性PASS。nativeの7248切断/5000変異/非整列/失敗不変、QJS入力copy/detach/immutable target/reentry/dispose/4runtime再生成はASan/UBSan/leak検査とm32でPASS。独立review指摘の未対応pattern silent lossはsurface[4]非zero拒否とlow/high byte mutation試験で修正した。

製品linkは追加APIを含むローカル作業状態でPASS（DIRAM179924 B、mapped flash1589984 B）。通常blobの挙動を変えないpattern拒否追加後の再linkは下記の別枠結果を参照。最終候補CIは未実施。公開headはdb4fbb82のままで、ローカル試作を同head CI成功と扱わない。

内部証拠: `.cache/cloud-memory-ab-*/report.json`、`.cache/cloud-bin-capture-192-result.json`、`.cache/ayatsuri-model-asset-tests/{asan,m32}/results.json`。これらはconsumer-local成果物で公開URLではない。

### 別枠192KiB診断と採用判断

同じbinaryで上限だけ196608 BとしたPACKED static通常UIは301frame PASS、定常160780 B／管理費込みpeak164280 B。160KiBを通ったことにはしない。

UIを除いたmodel構築・301frame・dispose・reloadも196608 Bで別実行した。元db4 sourceの構築155236 B／定常159148 B／dispose後GC前118000 B／reload159192 B、PACKED staticは156588／160328／118648／160372 B。両経路runtime解放後0。後者は定常で1180 B大きく、runtimeの可変nodeとupdate関数を残す費用をbin化だけでは消せていない。これは全工程のpeak内訳や実機native freeを測ったものではない。

この段階の判断は、**現在のbin prototypeを製品へ切り替えない**。形式・validator・transactional constructorは試作成果として保持し、常駐utility/保持closureの費用を減らす追加案を別途比較してから採否を再検討する。効果の薄い新loaderを導入すること自体を目的にしない。

pattern拒否修正後も製品link PASS、DIRAM179924 B／mapped flash1590000 B。このuncommitted試作のbuildであり、公開db4の実機成果物と混同しない。

SOURCEとowned-copyの192KiB追加診断は未実施。runnerのm32_sysroot.sh準備がexit2となり、試験binaryへ到達していない（`.cache/cloud-memory-ab-source-diagnostic192.log`、`...packed-copy-diagnostic192.log`）。これはappの追加FAILではない。最小4条件の160KiB比較とstatic192/model比較は完了しているため、この準備失敗を迂回・再試行せず区切った。以後は同一sysrootの並行準備を避け、reuse binary時の不要な準備を整理する。


未採用runtime/sourceのコピーとtracked patchは `.cache/cloud-bin-rejected-db4/` に退避し、製品ツリーから除外した。後続CLOUD PROBE候補には比較記録だけを含める。

## 非採用の範囲と次の比較

今回採用しなかったのは、source定義をbinへ変換して既存JS runtime node構造へ直接初期化するラッパー試作である。画像bin一般の可否や、静的parameterをnative immutable assetとして保持・共有する設計を評価・棄却した結果ではない。

次の最小比較ではstatic assetとmutable pose/velocityを分離し、JSはopaque handleとdirty targetだけを渡す境界を検討する。既存native snapshot/validation/失敗時committed不変を守り、guestの外へ移したbytesも別計測する。packed保存形式とPIE向け実行layoutは同一でなくてよく、必要な変換はload/prepare側で行い、毎frameの詰替えを増やさない。Astraのlayout評価と合わせ、fixtureの検証・公開・引渡し後に進める。

画像側の具体例は[petのPPT2設計](../apps/pet-asset-design.md)、`tools/pack_pet_compact.py`、`main/pet/pet_pixels.c`、`main/pet/ksn_pet.c`。現assetは7601 B。PPT2は共有基本形・palette・模様差分をnative providerで読むため、PNG等の元画像形式や大きなJS object graphへの復元と違う。RLE行の復号は残る。現providerの[64行cache](kasane-pet-row-cache.md)は8192 Bの静的RAMを使い、heap確保0をRAM使用量0と混同しない。文書の98304→7601 Bは既存報告で今回の独立再測定ではない。

狐2.5Dの画像・coverage/material mask等は引き続き画像bin化の適用候補。今回のfixture作業で画像warpや別方式の実装を始めたわけではない。
