# CLOUD MEMORY: 明示回収と実機A/C比較

基準は3d31fb86。ここからの明示回収・組込みprovider・診断counterを次候補として扱う。実機7d21b302の実績を流用しない。実機メモリ値・GC時間・物理LCD品質は未測定である。

現在の後続headは**専用診断候補**で、TRACEのCMake既定をONにする。登録device builderが任意CMake引数/CI artifact指定を受け付けないためのsource側対応であり、通常リリース設定ではない。計測後の通常候補ではOFFへ戻す。既定fixtureはDEVICE_SHORTへ変更し、旧1115drawのHOST_LONGと区別する。ba078e39のbuild成功と未着手の実機測定を混同しない。

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

## 最小診断build

`KASANE_MEGADEMO_TRACE=ON`で製品linkする。同一SHA・同一binary・160 KiB設定でA/Cを選ぶ。登録buildと同じ`idf.py -B output -D SDKCONFIG=output/sdkconfig build`のfresh構成で、CMakeCacheのTRACE=ONとguest compile define `POCKET_GUEST_ALLOCATOR_METRICS=1`を確認してから実機へ渡す。既存cacheのOFFを既定値だけで上書きできるとは仮定しない。通常版CIは`-D KASANE_MEGADEMO_TRACE=OFF`を明示する。trace OFFでは通常allocatorの余分なcounter処理/保存領域を除き、`memoryMark`も公開しない。比較appはその場合`METRICS UNAVAILABLE`を明示し、計測成功にしない。計測用binaryの時刻を通常buildの性能と呼ばない。

`pocket.app.memoryMark(label)`は診断専用。英数字と`._-:`、1..32文字のprimitive stringのみ。owner同期checkpointで新しいJS result objectやGC/heapwalkを作らず、`MMK`を1行出す。ラベル変換で小さな確保が生じる可能性と、ログ費用は別に残る。

| 出力 | 意味 |
| --- | --- |
| gu / limit | O(1) QuickJS会計の現在値/設定上限 |
| live / high / maxreq | allocator callbackが所有するusable live/起動からhighwater/最大成功要求 |
| alloc / realloc / free_calls / fail | callback回数。failはallocator callbackの失敗で、QuickJS quota拒否とは別 |
| cache | available/enabled/held bytes/hit/miss/flush。heldはguest live会計外 |
| free / lg | INTERNAL\|8BITの空き/最大連続block |
| mn / mn_scope | 既存MDT trace windowのlocal minimum。checkpointごとにresetしない |
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
