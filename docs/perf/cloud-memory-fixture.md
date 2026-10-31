# CLOUD MEMORY: 明示回収と実機A/C比較

基準は3d31fb86。ここからの明示回収・組込みprovider・診断counterを次候補として扱う。実機7d21b302の実績を流用しない。実機でのA/C完了比較・GC時間・物理LCD品質は未測定である。下記fde49fbの初期phase報告をsteadyや完了結果へ拡張しない。

`fde49fb8348742058138fa573554e775ec0dd4f6`は**専用診断候補**で、TRACEのCMake既定をONにした。登録device builderが任意CMake引数/CI artifact指定を受け付けないためのsource側対応であり、通常リリース設定ではない。同headの24 workflows/82 jobsは成功し、実機担当へ引き渡した。その後の通常候補a8eccbfbではTRACE/phase-onlyの既定をOFF/OFFへ戻している。今回のphase-only診断候補では再びsource既定をON/ONにするが、fde実測を新headの結果と呼ばない。既定fixtureはDEVICE_SHORT、旧1115drawのHOST_LONGと区別する。ba078e39のbuild成功と実機測定を混同しない。

## 寿命と観測範囲

固定cloudは2766 BのAYNM。`apps/cloud/assets/cloud.aynm`から再現可能なC byte initializerを作り、nativeのconst領域から直接読む。Instanceは可変arenaをguest allocator内に持つ。JSからnative化して160 KiBの外へ逃がす方式ではない。deviceのguest allocatorはheap_caps/TLSFとsmall-block cacheであり、160 KiBの独立固定arenaではない。

Modelのclose後、最後のInstance退役でowned cold bytesをallocatorへ返す。static資産はdetachだけでflashに残る。sceneは自身のInstance/buffer/参照を解放し、共有Modelのcloseは所有者が行う。保持されたdisposed scene/handleをtombstoneにでき、他の生存Instanceを壊さない。描画packetは既にsnapshotへcopyされており、資産回収に新しいframe待ちは不要。

`model.stats(out)`のowned/static bytesはattach中の資産量。物理ESP freeの増加量ではない。allocator cache、JS wrapper/owner、共有renderer資源、runtimeの残存量を分ける。CLOUD MEMORYは`maxSegments=1024`のprocedural資源をアプリ内で共用し、run終了後も保持することをログへ出す。sceneの回収とアプリ終了の回収を混同しない。

## 操作と有限fixture

アプリIDは`local.cloudmem`、typed catalog名はCLOUD MEMORY。既存HOMEメニューは増やさない。

1. fresh sessionで起動。LEFTでA（既存JS定義）、RIGHTでC（native資産）を選ぶ。実button maskはLEFT=0x80、RIGHT=0x20。
2. ENTERで開始（0x4000）。選択はsession中固定。反対方式のmoduleはロードしない。
3. 既定DEVICE_SHORTはwarm4 → steady32 → burst8 → clear/scene exit1 → recreate8 → clear1 → recreate8 → clear1。合計63成功draw。
4. COMPLETEではactor/ownedModelを解放し、直前のdisposed scene/root handleだけを意図的に保持。ENTERでwarm再試験、UP=0x10で途中取消・清掃してREADYへ戻る。

毎run固定seed=42、呼吸OFF、30 Hzの固定tick、同じF32 camera倍率・世界光を使う。入力・更新・描画成功でのみ進む。壁時計の再生時間や実機FPSを保証しない。失敗は旧committed表示を維持し、次turnでcleanupして明示再開始できる。待ち時間調整による同期は使わない。

HOST_LONGはwarm16/steady1000/burst32/clear1/recreate32/clear1/recreate32/clear1、1115drawを維持する。host harnessがruntime評価前にprivate初期値`globalThis.__cloudMemoryProfile='HOST_LONG'`を設定する。未指定はDEVICE_SHORT、未知名は資源生成前に拒否し、起動後の変更は反映しない。これは診断fixtureのhost初期設定で、汎用framework APIではない。ログ/readout/画面にprofileを出す。DEVICE_SHORTを1000frameの長期試験と呼ばず、A/Cは同じprofileで比較する。実機HOST_LONGは未測定。

A/Cのcold比較には必ずCLOSE→fresh OPENする。先にAをロードしてCへ切り替える方法では、AのJS code/graph残留をcold条件へ混ぜる。A→CとC→Aの順、warm再runとfresh runを別に記録する。

### fde49fb実機の中断と初期化分割

実機担当から2026-10-31の報告を受領した。fde49fb診断版のflashは03:39:34に成功し、同BIN/ELFと3領域readbackを確認したとの報告。原本`hone110-fde-device-failure-result-20261031.json`はこちらでは未取得で、以下は原ログ独立照合済みの実測とはしない。

AのSTART=85986ms、`cloudmem.loaded`=86234msで、loadまで248ms。その直後のvector→place→pose→updateで`InternalError: interrupted`となり、63draw/warmは完了しなかった。loaded時の報告値はguest gu84480B、allocator live77104B/high110284B、最大成功要求8496B、callback fail0、ESP free77544B/largest31744B/trace-window minimum36820B。callback fail0だけではQuickJS quota拒否を除外できず、これをOOMと断定しない。250ms turn watchdogとの近接から累積初回処理が有力候補だが、割込み理由の独立照合は未実施である。

旧fixtureはENTERの1callbackで全chunk load→scene/model作成→binding/sample作成→最初のupdate/evaluate/render/commitまで行っていた。後続fixtureはload slot、Model、scene、binding、first drawを別callbackへ分け、setup中に演技tick/draw数を進めない。A/Cの空slotも揃え、再生成も同じ分割を使う。`loaded`は全chunk完了後・作成前、`create.begin/model/scene/created`はそれぞれの境界を示す。63/1115という成功draw条件は変更しない。個々の段階が実機250ms以内になる保証ではなく、watchdog/heap cap/GC条件は緩めない。

分割版の最初のm32 SHORTは、Aが45draw後の再生成でquota拒否となった。canaryはquota5/allocator0、first request168B/used163752B。cold load用selectorを全load完了時に解放した次案も、再生成直後の更新でquota4/allocator0、request80B/used163816Bとなった。後者のA requested peakは147044B。Cだけが63draw×2を完了しても、この不完全なAとのA/C比較はFAILである。旧版のSHORT成功をこの分割版へ流用しない。

次案はfailure/cancelの同じcleanup処理と初期化表示を共有し、private readoutは`setupStep`から状態を導出する。scene constructorより前に所有recordを確保し、生成後は既存fieldへの代入でactorを公開する。constructor後のrecord確保失敗で明示disposeの機会を失う隙間も除く。全chunk完了後はcold loaderを保持せず、warm再runは同数の空load slotを進む。参照保持・draw数・tick・capを減らす対策ではない。

所有record/UIの修正だけの案もrequest80B/used163780B、quota4/allocator0でFAILした。最終案では初期化をframe内の1箇所へ置き、専用closureをなくし、入力recordは生成中に保持せず最初のsample時に確保する。再生成の開始は独立した待機callbackへ送るためrunごとに2 callback増えるが、A/Cで揃え、演技tick・成功draw・保持するdisposed scene/handleは同じである。各変更の節約量を単独測定したものではない。

この最終案のm32 DEVICE_SHORTはA/C各63draw×2、各81turnでPASS。全frameのincremental/full-reference、A/C画像列一致、実行中の依存hash不変、終了guest0を確認した。requested peakはA146656B/C101224B、最大成功要求8496B/6607B。上の旧分割前SHORT記録との差はA+16B/C-12Bだが、変更別の寄与を切り分けた比較ではなく、allocatorのrequested量でありQuickJS会計の残量やESPの最大連続freeとは異なる。画像列SHA256は`9d9cc354358c7afd3bec1f715e1c3758cf66fd024ce69858d6f03875bc45d909`。失敗した3案もcache内の別directoryへ保存し、成功記録で上書きしない。

同じ最終sourceのHOST_LONGもA/C各1115draw×2、各1133turnでPASS。全2230画像のA/C一致と全描画reference、依存hash不変、終了guest0を確認した。requested peakはA146740B、Cはrun1が101308B/run2が101312B、最大成功要求はSHORTと同じ。画像列SHA256は`4ccc1e196155f2aca5800c4f1120372269ad9c71b630ee865eeb1fdb7083abe3`。SHORTとLONGは別host binaryで、両profile内はそれぞれ同一binaryのA/C比較である。新しい分割fixtureの実機完了結果はまだない。

エラー後のAPP_STOPPED/HOME_READY/JS0も報告されたが、broker cleanupは`teardown_failed/outcome_unknown`、後続HELLO timeoutがあり、その後のHELLO_READYにもTRACEの連続出力による`serial_not_quiet_before_command`が伴った。Cは選択入力まで到達せず未測定。この状態報告を安全な終了・C測定成功と読み替えない。装置の状態照合は実機担当の工程で、ここからreset/flash/別回復経路を実行した事実はない。

## 最小診断build

### 正式builder向けの専用source候補と復帰対象

基準a8eccbfbの24 workflows/82 checks成功後、`main/CMakeLists.txt`のTRACEとphase-onlyの**両source既定をON**にした専用診断候補を作る。任意CMake引数やCI artifactからのflashを使わず、登録builderの既存コマンドだけでphase-onlyを再現するためであり、通常リリースの既定変更ではない。broker/profile/権限は変更しない。候補SHA、build設定、BIN/ELF hash、receiptを実機担当が対応付け、flash前に実値を照合する。ここではflashを実施しない。

CIは同じsource headから次の3構成を製品linkする。通常／従来TRACEの比較でphase-onlyが混入しないよう、OFFも明示する。他の通常firmware workflowも両方OFFを指定し、既存baselineでoption未定義でもcache型がBOOLとなるよう`:BOOL`を指定する。

| 構成 | TRACE / phase-only | 設定方法 |
| --- | --- | --- |
| 通常 | OFF / OFF | 両方の`-D`を明示 |
| 従来TRACE | ON / OFF | 両方の`-D`を明示 |
| 正式builder相当の診断 | ON / ON | fresh build directoryで両`-D`を付けずsource既定を使う |

正式builderと同じfresh構成は`idf.py -B output -D SDKCONFIG=output/sdkconfig build`。`output/CMakeCache.txt`も`output/sdkconfig`も存在しない独立したbuild先から開始する。古いdirectoryを一括削除する手順ではなく、新しいbuild先を使う。既存cacheのOFFはsource既定をONに変えても上書きされず、成功receiptだけでは診断構成を保証しない。登録builderの出力先が既に存在する場合も、実機担当はfresh性と実値を確認し、OFFのままなら計測候補として使わない。

`python tools/check_memory_build_profile.py --build-dir output --profile phase`で、両BOOL cacheがON、`app_session.c`に両compile defineが1、guestの`POCKET_GUEST_ALLOCATOR_METRICS=1`が実際に付いていることを確認する。normal/traceの検査も同じscriptへprofile名を渡す。欠落・古いOFF・矛盾するdefine・重複compile commandは拒否する。`--self-test`は実sourceのoption宣言を小CMake projectで使い、fresh既定と再利用OFF cacheの差、明示normal/trace、古いcache/欠落define等の負例を検証する。

測定後のrestore対象は次の3点。診断用source既定が通常版へ残らないよう、同じbranchの後続commitとして行う。

1. `main/CMakeLists.txt`のTRACEとphase-onlyの既定を**両方OFF**へ戻し、source注記と本節の状態表示も通常版へ更新する。片方だけ戻さない。
2. Character materialsのphase専用CI buildには再び両`-D ...=ON`を明示する。通常buildの明示OFFとprofile検査は残す。
3. fresh通常buildを`--profile normal`で確認し、同head CI・通常版BIN/ELFを確定する。診断ON cacheの再利用で通常復帰したと判断しない。

profile検査script、通常CIの明示OFF、初期化分割、phase-only機能自体は残す。診断候補commit全体を機械的にrevertしてこれらを消す手順ではない。a8eccbfbは通常既定OFF/OFFの参照SHAとして保持する。

実機HOMEはユーザーBack操作後の確認が残っているという報告であり、このsource候補のbuild成功をHOME復帰や安全なflash条件の確認に代えない。実機への操作は別担当が状態を照合して行う。分割によりwall時間・callback境界・GC機会が変わり、phase-onlyは観測処理も変えるため、旧fdeと性能条件が同一とは扱わない。同じ固定tick/draw payloadの回帰一致と実機性能比較を区別する。

`KASANE_MEGADEMO_TRACE=ON`で製品linkする。同一SHA・同一binary・160 KiB設定でA/Cを選ぶ。登録buildと同じ`idf.py -B output -D SDKCONFIG=output/sdkconfig build`のfresh構成で、CMakeCacheのTRACE=ONとguest compile define `POCKET_GUEST_ALLOCATOR_METRICS=1`を確認してから実機へ渡す。既存cacheのOFFを既定値だけで上書きできるとは仮定しない。通常版CIは`-D KASANE_MEGADEMO_TRACE=OFF`を明示する。trace OFFでは通常allocatorの余分なcounter処理/保存領域を除き、`memoryMark`も公開しない。比較appはその場合`METRICS UNAVAILABLE`を明示し、計測成功にしない。計測用binaryの時刻を通常buildの性能と呼ばない。

制御経路を静かに保つ診断には、追加の`KASANE_MEMORY_PHASE_ONLY=ON`をTRACEと共に使う。通常版の意図する既定はOFF/OFF、今回の専用source候補だけON/ONである。従来TRACEだけのper-turn MDT/MDTJ動作はphase-onlyを明示OFFにすれば維持する。phase-onlyは反復MDT/MDTJとそのheapwalkを出さず、counterをdrainする。明示MMK、通常の低頻度描画集計・開始/終了・エラー通知は残す。HELLOを含む非計測appの待機turnでも反復MDTを出さない。BGCOSTとの併用やTRACEなしのphase-only指定はCMakeで拒否する。phase-onlyからper-frameのJS/render/send分布は得られず、残るKASANE_PAINTの30paint集計をMDTの代わりとしない。

phase-onlyの`mn_scope=session`はguest session開始からのlocal minimumで、checkpointごとにはresetしない。通常MDTでは従来の`mn_scope=trace_window`を維持する。両scopeを同じ窓として比較しない。設定は別build directory/別SDKCONFIGで製品linkし、CMakeCacheとcompile defineを照合する。登録builderが任意引数を受理しない場合、必要な設定を持つ診断source候補を別SHAで用意する工程が残る。CI artifactや非正式なflashを迂回経路にせず、phase-only実機効果を未確認のまま成功とはしない。

`pocket.app.memoryMark(label)`は診断専用。英数字と`._-:`、1..32文字のprimitive stringのみ。owner同期checkpointで新しいJS result objectやGC/heapwalkを作らず、`MMK`を1行出す。ラベル変換で小さな確保が生じる可能性と、ログ費用は別に残る。

| 出力 | 意味 |
| --- | --- |
| gu / limit | O(1) QuickJS会計の現在値/設定上限 |
| live / high / maxreq | allocator callbackが所有するusable live/起動からhighwater/最大成功要求 |
| alloc / realloc / free_calls / fail | callback回数。failはallocator callbackの失敗で、QuickJS quota拒否とは別 |
| cache | available/enabled/held bytes/hit/miss/flush。heldはguest live会計外 |
| free / lg | INTERNAL\|8BITの空き/最大連続block |
| mn / mn_scope | 通常MDTはtrace_window、phase-onlyはsessionのlocal minimum。checkpointごとにresetしない |
| sample_us | counter/heap読取の時間。ラベル変換とログ出力を含まない |

allocator operation counterは飽和する。cacheの既存uint32 counterはwrapする。highwaterはcallback境界でのusable量で、backend realloc内部の一時的な二重確保やallocator metadata全体を捕捉しない。guest quota拒否は既存OOM canaryで判別する。固定layoutのInstance arenaにfree-listはないためarena largestfreeはN/A。`malloc_size-memory_used_size`を断片化量とは呼ばない。

runtime.ready、module.before/ready、fixture各phaseを記録する。既存MDTはturn時間・render/send・free/lg/min・guest会計、MDTJは64turnごとのheapwalkとそのcostを出す。GC回数/時間、stack peak、完全なpacket copy総量は今回N/A。BSS/rodataは同head map、stackは別の実定義に沿った計測が必要。

## ホスト検証

`node tools/character/test_cloud_memory.cjs`は選択固定・反対module未読込・2run・cleanup・load/create/commit失敗復帰を検証する。fake backendの画素は実native描画の証拠にしない。

`python3 tools/ayatsuri/model/run_cloud_memory.py --m32 --profile DEVICE_SHORT`と`--profile HOST_LONG`は実QuickJS/nativeへ通常buttonだけを送り、A/C別processで各2run、各frame incremental/full-referenceを比較する。SHORTは各126枚、LONGは各2230枚を比較する。明示frame GCなし、設定cap163840 B。以下の初回値は旧HOST_LONG条件：A/C各2230frameのhash列一致、runtime終了guest0でPASS。初回binary SHA256は`adb88b704dc3b76881f3bddd72e8935eee3bb10966a1226877bc0c7dde1eb7b4`。後続headのSHORT/LONG結果とは別記録である。

初回fixtureのrequested peakはA 145820 B/C 100412 B、最大成功guest要求は8496/6607 B。これはhost allocatorのrequested量で、device TLSF usableやESP heap、QuickJS会計総peakではない。画像一致も設定したBODY/変更fixtureの範囲であり、全表情や全cameraの保証ではない。

counter gate後のローカル再試験もPASSした。binary SHA256は`af5e49e330d34f29562d89e65949725d94248e1c15c3d8cdd3e1c28f56fba137`、画像列SHA256は`eee4b13bc33e579e2d08869156c24087453de97d9f2de59caaa869059c2407f3`。capacity/画像の結果は初回と一致。ただし当時のguardはcompiled sourceとcloudmem直下だけを対象にしており、参照先JS chunkの一部が不足していた。このguardのtrue値を全関連source不変の証拠には使わない。

修正版はentry/manifestに加え、chunks.txtから解決した両経路の全JS sourceをhashする。実行後に依存集合も取り直し、追加chunkと参照先変更を検出する。`--self-test-inputs`は共有treeを触らず、一時fixtureの全11入力の変更と追加依存（12負例）を検証する。修正後の最終head CI/再試験と旧画像結果は区別する。local trace ON製品linkの成功と実機確認も別工程である。

証拠は`.cache/cloud-memory-device_short-m32/`と`.cache/cloud-memory-host_long-m32/`の`{A,C}.log`、`{A,C}-pixels.csv`、`result.json`。旧`.cache/cloud-memory-m32/`は履歴として保持する。モデル単独のASan/UBSanはRAM即時返却、2Instance共有、static detach、GC cycleの両finalizer順、再入/OOM復帰を確認する。計測counterはhost/device相当×ON/OFFと実QuickJS診断bridgeを独立試験する。

SHORT初版はAの最初の再生成（45draw後）で失敗した。canaryはquota=4/allocator=0、first_req=48 B、first_used=163840 Bであり、null例外だけからの推測ではない。未公開profile選択APIと補助closureを小さい起動時設定へ置換後、同cap・通常GCのまま両profileがPASSした。code容量とGC履歴の寄与を単独に切り分けた測定ではない。

| 修正後host requested peak（B、2run全体） | A | C |
| --- | ---: | ---: |
| DEVICE_SHORT（各126画像） | 146640 | 101236 |
| HOST_LONG（各2230画像） | 146728 | 101324 |
| 最大単一成功要求（両profile） | 8496 | 6607 |

両profileでA/C画像列一致、全frame incremental/full-reference一致、runtime終了guest0、実行中の依存source集合/hash不変を確認した。host harnessのreadout/evalも含む値であり、実機のguest/ESP peakとは別である。cap増加・frameごとの強制GC・sleep調整はしていない。

## 実機受入

実機担当へ最終SHA、trace ONのbinary/ELF、CIとflagsを渡す。正式AppSessionOptionsの上限は90000ms（default30000/min1000）。300000msはvalidationで拒否され未実行だった。既定DEVICE_SHORTで起動→create→steady→burst→exit→recreateのMMK/MDTを同条件で取得し、phase終端、瞬間highwater、runtime終了を分ける。63drawも90秒以内完了を事前保証せず、COMPLETE前timeoutは未完と記録する。scene exitで不要なcold/work参照が戻ること、生存共有Instanceを壊さないこと、再生成反復のpostphase残留推移を受け入れる。終了JS0だけでpeak/断片化改善としない。broker/profile/認可変更や非正式flashを代替経路にしない。

強制GC後/cache flush後の追加測定やGC詳細計装は今回未実装。通常frameごとのGC無効化・強制GCは行わない。計測自身の完全な負荷比較と物理LCDの品質も未受入のまま残す。
