# Kasane実装記録

実機確認方針（2026-09-15更新）: ユーザーから実機利用可能の連絡を受け、COM3での
書込み・シリアル診断を再開した。以前の保留項目は実行したものだけ確認済みに更新する。
各checkpointはhost試験とESP-IDFビルド後にcommit・pushして進める。

## checkpoint 5 — input service分離（2026-09-15）

- `pocket_input.c/.h`へonAction/onKey/held、購読4枠、repeat、capabilityとlazy namespaceを抽出。
  input初回参照から旧nodeクラス初期化への依存を除去。UI pumpはtoastの期限更新だけを担当。
- sessionはinputをtextより先に登録し、guest破棄前にinputをresetする。
  pump順序、press/release/repeat、listener例外時close、既存ログtagは維持。
  scope別購読・modal Back配送はCP15。既存capabilityのactions表記がacceptのみという
  過少申告も今回は維持しており、配送契約の整備時に合わせて修正する。
- K診断のEnterでtext sessionを開く。Kasaneとinputを使い、旧node APIは呼ばない。
  text描画のSYSTEM移植はCP16であり、今回のtext試験は入力とcallbackの接続を検証する。

検証:

- `bash tools/build_input_test.sh && /tmp/test-pocket-input`: ASan/UBSan PASS。
  `CFLAGS="-O2 -fstrict-aliasing" OUT=/tmp/test-pocket-input-o2 bash tools/build_input_test.sh`
  と生成exeもPASS。出荷VMスイッチの実QuickJSと実subscription実装を使用し、
  node/Taffyなしで2 realm、held、400/120 ms repeat境界、例外、自身のclose、quota、reset、
  古いclose handleを検証。namespace登録はhost stubなのでproduction lazy順序の試験ではない。
- `bash tools/build_pocket_text_test.sh && /tmp/test-pocket-text`: 全9ケースPASS。
- ESP-IDF 6.0.1 `-B build_ds_contract build`: PASS。ESP32-S3、PSRAMなし、native probe有効。
  app 2,197,488 B、partition空き948,240 B、DIRAM137,276 B。
  inputの静的156 Bは旧UIからの移動でDIRAM増分0。既存GNU-stack警告のみ。
- COM3へflashしhash一致。`tools/kasane_input_device_test.py --port COM3
  --out .cache/kasane-cp5-input-final`: PASS。helloのEnterカウント1/2、pet起動・左右移動・
  home復帰、Kのright/accept press/releaseとheld、text編集hi/submit/cancelを確認。
  K終了・再起動を含む2回とも成功。petへの給餌や選択変更はしていない。
- USB試験はログ直後の画面切替前に入力すると取りこぼすため、host側の入力間隔を150 msにした。
  初回失敗ログも`.cache/kasane-cp5-input*`へ保持。repeatの時間境界はhost fake clockで確認し、
  USBの瞬間押下を長押し試験とは数えない。実LCDのtext描画の目視確認は含まない。
- `tools/kasane_device_test.py --port COM3 --ticks 300 --out .cache/kasane-cp5-animation`:
  PASS。modal開始・終了と300ターンの描画を確認。10計測窓の平均turn 3.29 ms、
  render 12.60 ms、send 3.38 ms、Kasane nativeBytes 12,908 B。終了後HOME_READY、port解放済み。

## checkpoint 6 — guestの直接dispatch（2026-09-15）

- `app_tick`は旧UI bindingのturn関数を経由せずguest frame/continueを直接呼ぶ。
  buttons、analog中央0x8080、touchなしを従来と同じ引数で渡す。
  JS実行後にKasaneがactiveなら旧core tick/drawを省略し、旧UIアプリでは維持する。
- 継続jobを先に処理する順序、Backの最終保存turn、deferred入力、cleanup、OOM報告、
  watchdogと表示間隔は保持。guestの実行中に初めてKasaneへ切り替わる場合も同じ判定を使う。
- `python3 tools/test_session_dispatch.py`: 実run_pumps/dispatch_guest/app_tickを抽出し、
  ASan/UBSan・O2、compat/fairの4構成でPASS。旧/Kasane経路、切替、frame/continue/draw失敗、
  cleanup順序、Back、deferred、runaway、repairと表示間隔をdeterministic portで検証。
- `VMTEST_OUT=/tmp/kasane-sync-vm bash tools/vmtest/build.sh o2`と同OUTの
  `bash tools/vmtest/run.sh --variant o2`: 63 PASS、0 FAIL。
- 通常ESP-IDF構成: PASS。S3、PSRAMなし、native probe有効、VM probe無効。
  app 2,195,968 B、DIRAM137,276 B。CP5比flash -1,520 B、DIRAM増分0。
- 通常構成実機: `kasane_input_device_test.py` PASS（hello/pet/K/text、2回再起動）。
  `kasane_device_test.py --ticks 300` PASS。10窓平均turn 2.88 ms、render 12.59 ms、
  send 3.38 ms。直前CP5のturn 3.29 msから約12%減（同診断の観測値）。
  記録は`.cache/kasane-cp6-input`と`.cache/kasane-cp6-animation`。
- VM probe構成: `-B build_ds_vmprobe -D SDKCONFIG=build_ds_vmprobe/sdkconfig build` PASS。
  app 2,205,264 B、DIRAM143,276 B。通常比6,000 Bは計測buffer等。
- probe実機: `vm_l0_capture.py --conditions base --workloads DF --seconds 4 --reps 1`
  成功。Promise chainとasync generatorの各3窓を収集。最大連続空きの観測最小は36,864 Bと
  55,296 B、stack high-waterはともに23,804 B。全VM機能の実機網羅試験とは区別する。
  同構成のK 300ターンもPASS。ログは`.cache/kasane-cp6-vmprobe.jsonl`と
  `.cache/kasane-cp6-probe-k`。CP6実装commitは`380e1d6`、push済み。

## checkpoint 7 — Kasane-only診断profile（2026-09-15）

- root CMakeの`-D KSN_ONLY=ON`で旧UI core/binding/RGB565 rendererを依存探索から除外。
  選択をIDF build propertyで早期の別プロセスにも伝える。通常構成は既定OFF。
  manifestの無条件旧UI edgeを除き、通常構成のCMake REQUIRESに集約する。
- Kasane-onlyでは旧node API、jsfont、petの旧texture adapter、旧render acceleratorをコンパイルしない。
  native pet通知の共有ヘッダから不要なui_core includeも除く。入力・text serviceは独立して残す。
- この段階で未移植のforeground appは、評価前に`APP_REFUSED KASANE_ONLY`と表示理由を返す。
  USB Kと既存のTaffy非依存overlayだけを許可する。native home/editor等の描画はまだ旧native経路。
- `tools/prepare_dependencies.py --kasane-only`は旧PocketJS checkout/Rustを準備しない。
  BMI270、opus、minimp3は従来どおり。`tools/test_kasane_prepare.py`はsubprocessをmockして検証。
- clean診断build: `idf.py -B build_ksn_only -D SDKCONFIG=build_ksn_only/sdkconfig
  -D KSN_ONLY=ON -D POCKETJS_SOURCE_DIR=C:/devs/m5stack/design-contracts-wt/.cache/absent-legacy-ui build`
  PASS。指定した旧checkoutのパスは存在しない。native probe有効、VM probe無効、PSRAMなし。
  app 1,900,384 B、DIRAM135,916 B。通常構成はapp2,196,352 B、DIRAM137,276 BでビルドPASS。
- `tools/check_kasane_link.py --build build_ksn_only --nm <S3-toolchain-nm>` PASS。
  component graph、build.ninja、compile_commands、map、demangle済みELFに旧UI/Taffyがなく、
  Kasane/guest/inputの実シンボルが存在する。旧ソースやarchiveの削除はしていない。
- session dispatchのhost回帰4構成PASS。Kはdevice情報、seeded random、input capabilityを
  実際に使い`KASANE_SERVICES PASS legacy=false`を出す。
- `kasane_only_device_test.py --cycles 100`: 実機PASS。未移植helloの明示拒否、Kのサービスと
  right press/releaseを毎回確認。終了後heapは全100回241,004 B、最大連続空き120,832 Bで一定。
  `.cache/kasane-cp7-only`に全ログとmemory.jsonを保存。全機能併用時の安全性を示す値ではない。
- 同構成`kasane_device_test.py --ticks 300`: PASS。modal/透過/patchとhome復帰、
  平均turn2.92 ms、render12.65 ms、send3.44 ms。`.cache/kasane-cp7-animation`に保存。
  CP7実装は`76dac40`としてpush済み。出荷からTaffyを削除するCP25は未実施。

## checkpoint 8 — JS角丸・枠線・gradient（2026-09-15）

- tx.roundRect/strokeRect/gradientとfeaturesを公開。既存DrawRefのPATCH・取消・例外原子性を利用。
  色/opacity等の整数検証は保持し、座標だけ仕様どおり最近接・half-away-from-zeroへ変更。
  APIのフィールド、既定値、上限は[design-api.md](design-api.md)に記載。
- Q: `tools/build_kasane_test.sh`と生成exe、ASan/UBSan・O2 strict-aliasingでPASS。
  新APIのnative記述子、正負の半分、gradient両端の画素、PATCH、clip、不正radius/width/axis/
  dither、NaN/overflow、throwing getter、catch後の全体取消、committed画素維持、cancelを追加確認。
- 通常/診断ESP-IDFビルドPASS。appは2,197,600 B / 1,901,664 B、
  DIRAM137,276 B / 135,916 Bで増分0。Kasane-onlyのlink監査も再度PASS。
- K診断を動く角丸・内側枠・ディザ付き水平gradientへ更新。
  Kasane-only実機で300ターン・modal遷移・home復帰PASS。
  平均turn3.09 ms、render13.62 ms、send3.36 ms。10/13命令でnativeBytes12,908 B。
  `.cache/kasane-cp8-gallery`に保存。実装commitは`4e07887`、push済み。

## vm/main同期 — 2026-09-15

- 同名リモートの`77e95c8`をfast-forward後、`origin/vm/main`の`bd0fa43`を統合。
  VMの中断・再開、OOM記録、guest確保ヘッダ、pet capability、背景PIE、非同期LCD転送を取り込む。
- gardenの計測関数宣言は両側を保持。Kasaneの最新文書を`docs/kasane/`へ集約し、
  装飾光線の追加記録は`docs/perf/flower-decor-cost.md`へ保持。索引と相対リンクを更新。
- 非同期`board_present`の成功はqueue成功なので、Kasaneの同期send契約には直接使わない。
  `board_present_sync`で先行転送と当該帯の完了を回収し、エラー時は書込み位置を無効化する。
  APPとKasane実機probeに接続。帯間のCPU/SPI重畳はKasaneでは行わず、完了ackを保証する。
- QuickJS adapterのhost試験を出荷時と同じSEGFRAMES/FLATCALLS有効へ揃えた。
  オブジェクトcacheを専用化し、ヘッダとbuild script変更時も再コンパイルする。
- CP3aの途中変更は未完成・未検証。同期前にstash
  `ed394313ce340cfbc250c538a01606f206020210`へ既存lock差分とともに保全した。
  この同期には含めない。再開時はcache/view/probe/testsの変更を取り出し、JS側lazy確保、
  共有static IDの計上、失敗注入試験を完成させる。CP3bのcore分割は未着手。

検証:

- H: `bash tools/kasane_contract/run.sh` PASS（ASan/UBSan、O2、frost/PIE、C++ヘッダ）。
- Q: `bash tools/build_kasane_test.sh && /tmp/test-pocket-kasane`、
  `CFLAGS="-O2 -fstrict-aliasing" OUT=/tmp/test-pocket-kasane-o2 bash tools/build_kasane_test.sh`
  と生成exeはPASS。QuickJS本体は出荷VMスイッチ有効・O1、adapter/core/testは各指定構成。
- VM: `VMTEST_OUT=/tmp/kasane-sync-vm bash tools/vmtest/build.sh o2`、
  同じOUTで`bash tools/vmtest/run.sh --variant o2`: 63 PASS、0 FAIL。
- `python3 tools/test_board_present_sync.py`: PASS。先行転送失敗・queue失敗・最終帯完了失敗・
  再試行を実関数の抽出とSPI stubで検証。SPIハードウェアの検証ではない。
- `tools/test_garden.c`、`tools/test_flower.c`を`main/scene/canopy_pie.c`とリンクして
  `gcc -O2 -Wall -Wextra -Werror ... -lm`で実行: PASS。
  `python3 tools/pie/test_kernels.py`: 7 PASS。
- ESP-IDF 6.0.1 `-B build_ds_contract build`: PASS。S3、PSRAMなし、KSN_DEVICE_PROBE無効。
  app 2,171,696 B、partition空き974,032 B、DIRAM 123,404 B。
  DIRAMはcheckpoint 2から7,856 B増。主因はboard非同期転送の2本のbuffer（7,680 B）と管理情報。
  16 KiB基本予算の達成値として扱わず、次のメモリ設計で同時ピークへ加算する。
- 実機検証はユーザー指示で保留。シリアル操作なし。

## checkpoint 3a — optional/lazy cache（2026-09-15）

- stashの途中変更を引き継ぎ、cacheなしのcoordinatorとJS初回cache.createの遅延確保を完成。
  cacheはdescriptor/metadata、commands、textの借用3ブロック。固定領域callerもbindして利用する。
- 初回template検証・JS wrapper確保・native確保後にのみhostへattachする。
  失敗時は追加領域を回収し、表示済みbank、参照、pollを保持。release後の予約はresetまで保持。
- C統計は共有ID20 Bをcache未使用時も計上。JS nativeBytesは実際の予約heapを示し、
  cache.reservedBytesで任意cache分を確認できる。仕様のメモリ節に計上境界を記録した。
- S3 ELF型情報: 基本10,344 B、cache追加3,056 B（496/1,536/1,024）。
  cacheなし4,096 B減、cacheあり1,040 B減。base単一確保10,344 Bとstack peakはCP3の未完部分。
  cache.createのdraw配列はstack1,920 B。CP3bでcoreを分割し、実機stackは後日確認する。
- H: `bash tools/kasane_contract/run.sh` PASS（ASan/UBSan、O2、PIEモデル、C++ヘッダ）。
- Q: `bash tools/build_kasane_test.sh && /tmp/test-pocket-kasane`と
  `CFLAGS="-O2 -fstrict-aliasing" OUT=/tmp/test-pocket-kasane-o2 bash tools/build_kasane_test.sh`
  および生成exeはPASS。3個別確保の各失敗、全回収、再試行、既存ref、JSなしrepairを確認。
- ESP-IDF `-B build_ds_contract build`: PASS。app2,172,032 B、空き973,696 B、DIRAM123,404 B。
  static DIRAM増分0。実機未確認、シリアル操作なし。

## PIE追補 — 背景と不透明矩形の連続塗りつぶし（2026-09-15）

- ユーザーのPIE推奨を受け、CP3a `af8da25`のpush後に追加。CP3bのcore分割は次の課題。
- 背景帯と不透明RECTを連続RGB565 fillへまとめる。矩形の色変換は範囲ごとに1回。
  半透明、group、角丸、gradientの量子化は既存経路で処理する。
- S3では先頭をscalarで16-byte境界へ揃え、8画素単位をPIEでstore、端数はscalar。
  出力の先頭画素へ色を置いて`EE.VLDBC.16`でbroadcastするため、色表や追加scratchはない。
  q0とハードウェアループを使うowner-task専用関数。ポインタはearly-clobber制約を持つ。
- ESP32-S3 HW MCPへstdio接続し、`knowledge_routes`、`get_instruction`、
  `example_measured_semantics`、`analyze_sequence`、`pie_cost_estimate`を利用。
  サーバーは`dj-oyu/esp32s3-hw-mcp`の`fb3561ee79c6b5af3a2dbe01d18e04d8e8d804cb`。
  TRM v1.8 p170のVLDBC.16と、同MCPの実機記録が確認した128-bit accessの下位4bit切捨てに従う。
  TRM: https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf
- コストモデルのstore増分0.6を使うと、240×8画素帯のvector本体は240×1.6=384 cyclesという
  **推定**になる。整列処理・関数呼出し・色seed・タスク切替は別。速度の実測値ではない。
- H: `bash tools/kasane_contract/run.sh` PASS。Cのscalar/model両経路で0..1,920画素、
  全8種のuint16開始alignment、7色、両端guardを検査（ASan/UBSanとO2）。
  `fill_pie.py`は実asmをpiesimで実行し、1/2/3/29/30/210/240ブロックの画素・ポインタ・guard一致を確認。
  既存合成・repair・ディザ・frost・C++ヘッダもPASS。
- Q: `bash tools/build_kasane_test.sh && /tmp/test-pocket-kasane` PASS。
- ESP-IDF `-B build_ds_contract build`: PASS。app2,172,256 B、空き973,472 B。
  DIRAM123,404 Bで増分0。objdumpでbroadcast→loopgtz→単一VST→returnを確認。
  asm loop本体3 B、追加vector領域なし。実機速度・画素確認は後日。シリアル操作なし。

## checkpoint 3b — 基本領域の分割予約（2026-09-15）

- coreを移動不可の借用commands/text各2ブロックへ分割。resetはアドレスを保持し、
  bank切替はメタデータと内容をコピーして候補bankと表示bankを独立に保つ。
- JS adapterは基本5ブロックの全確保・初期化後に公開。途中失敗では全回収、resetも全解放。
  通常PATCH/presentではnative追加確保なし。cacheの遅延3ブロック確保は維持する。
- S3 ELF型情報: 管理1,644 B、commands 3,072 B×2、text 1,024 B×2、基本合計9,836 B。
  cache込み12,892 B。CP3aから各508 B減、個別確保上限3,072 Bを満たす。
  allocator管理情報は別。core単体は管理516 B＋借用8,192 B、C statsの共有IDも引き続き計上。
- S3 objdumpの関数単体stack frame: ensure_state/core_begin/core_init 48 B、core_bind 32 B。
  cache.createは1,984 B（draw配列1,920 Bを含む）。描画追加stack 1 KiB目標の達成は未認定。
  VM・子関数・board buffer・Wi-Fi/audioを含む同時ピークとlargest blockは実機確認待ち。
- H: `bash tools/kasane_contract/run.sh` PASS（ASan/UBSan、O2、PIE、C++）。
  追加bind失敗・reset試験はtest_coreをASan/UBSanとO2で別途実行しPASS。
- Q: `bash tools/build_kasane_test.sh && /tmp/test-pocket-kasane`と
  `CFLAGS="-O2 -fstrict-aliasing" OUT=/tmp/test-pocket-kasane-o2 bash tools/build_kasane_test.sh`
  および生成exeはPASS。基本5確保＋cache3確保の各失敗、再試行、全回収、stats実予約一致を確認。
- ESP-IDF `-B build_ds_contract build`: PASS。app2,172,240 B、空き973,488 B、DIRAM123,404 B（増分0）。
  CP3のhost/build側は完了。実機100回起動とstack/断片化確認は保留、シリアル操作なし。
  次はCP4のhost core/coordinator所有とAPP attach/detach。

## checkpoint 4a — SYSTEMを保持するAPP終了機構（2026-09-15）

- `ksn_view_host_reset_app`を追加。APP builder/submissionを取消し、両bankのAPP、
  APP cache/画像登録、modal/focusを解放。予約ブロックは維持し、終了でheap確保・ID発行をしない。
- SYSTEMの確定済み表示、構築中/送信待ち更新、cache instance、画像登録、pollを維持。
  APPを黒背景に戻し、全面repairを要求。display/provider callbackからの終了・present再入はBUSY。
- H: `bash tools/kasane_contract/run.sh` PASS（ASan/UBSan、O2、既存描画/PIE、C++）。
  新規test_app_teardownは6状態、全画素、部分転送失敗、再入拒否、cache compaction後のSYSTEM
  PATCH、各状態100回のAPP資源確保/終了でquota回収を確認する。実機100回起動とは別。
- Q: `bash tools/build_kasane_test.sh && /tmp/test-pocket-kasane` PASS（ASan/UBSan）。
  新終了APIはまだJS/session未接続のため、Qは既存adapter回帰確認に限定。
- ESP-IDF `-B build_ds_contract build`: PASS。app2,172,240 B、空き973,488 B、DIRAM123,404 B。
  S3 coordinatorは88 B（+4）、adapter基本9,840 B、cache込み12,896 B。個別確保上限は維持。
- **CP4全体は未完**。次の4bで領域所有をJS adapterからhostへ移し、世代付きAPP lease、
  attach/detachとsession終了を接続する。現在の低レベルendpointポインタ自体は失効しない。
  native homeへの遷移やSYSTEM実サービスへの接続は、この基盤だけでは変更しない。
- 実機確認は保留。シリアル操作なし。

## checkpoint 4b — host runtime所有とAPP lease（2026-09-15）

- `ksn_runtime.c/.h`へcore/coordinator/cacheの所有を移した。QuickJS依存なし。
  JS adapterはwrapper管理＋4 BのAPP leaseのみ。初回mutating callでattachし、
  既存app_sessionの`pocket_kasane_reset`経由でAPPのみdetachする。
- SYSTEM取得後はguest終了・QuickJS破棄後も領域とSYSTEMを保持。SYSTEM取得のない
  APP-only利用はdetachで全解放する。native shutdownはAPP/pending/drawing中BUSY。
- APP leaseはprocess lifetimeで再利用せず、操作ごとにviewを解決する。
  古いleaseのdetach/end_turnは新しいAPPへ影響しない。ID枯渇は確保前にLIMIT、wrapなし。
  生viewは呼出し中の借用であり、lifecycleをまたいでキャッシュしない。
- APPのreturn/yield cleanupはSYSTEM builderをabortしない。cache初回3確保と原子的attachもhostへ移管。
- H: `bash tools/kasane_contract/run.sh` PASS（ASan/UBSan、O2、PIE）。
  新規test_runtimeはhost5確保の各OOM、SYSTEM-only描画、APP再接続、古いlease、
  SYSTEM builder維持、描画再入拒否、shutdown、ID枯渇を検証。runtimeヘッダのC++17検査もPASS。
- Q: `bash tools/build_kasane_test.sh && /tmp/test-pocket-kasane`、
  `CFLAGS="-O2 -fstrict-aliasing" OUT=/tmp/test-pocket-kasane-o2 bash tools/build_kasane_test.sh`
  と生成exeはPASS。基本6確保・cache3確保の各OOM、SYSTEM併存時APP OOM、確定済み/
  pending/部分IO状態のAPP終了、古いJS参照、guest heap全解放後のSYSTEM PATCH/描画を確認。
- ESP-IDF `-B build_ds_contract build`: PASS。app2,173,536 B、空き972,192 B、DIRAM123,404 B。
  S3 ELF型情報: host管理616 B＋借用8,192 B＝8,808 B、APP adapter1,044 B。
  APPあり9,852 B、cache込み12,908 B。CP4a比12 B＋allocator1件増。個別3,072 B以下を維持。
  runtime staticは8 B追加だが、ELF全体DIRAMはalignment込みで増分0。
- CP4のhost/build側は完了。実機home/app往復は保留、シリアル操作なし。
  home/通知実サービスとguestなしの描画pumpのproduction接続は後続checkpoint。
  次の実装はCP5（入力service切り出し）。

## UI smoke — native / JS同画面比較（2026-09-15）

- `tools/kasane_ui_smoke.c`のnative APP lease呼出しと、`tools/kasane_ui_smoke.js`の
  実QuickJS呼出しで同じUIを描く。SYSTEMの右上indicatorは両経路でnativeから構築。
- 5状態: 通常、メーターPATCH、dim-live modal、modal close、APP detach。
  各経路162,000画素のRGB565と転送量が一致。PATCH対象外不変、PATCHとclose時の
  全再構築の一致、scopeのAPP→MODAL→APP→HOST、無変更時転送0を確認。
- 各経路の転送Bは64,800 / 7,680 / 64,800 / 64,800 / 64,800。
  PATCHは全面比約88%削減。これは転送量であり実機処理時間の測定ではない。
- ASan/UBSanとO2 strict-aliasingでPASS。出力PNGを目視し、矩形の透過重なり、
  modal背景の減光、SYSTEM indicator維持、終了時APP消去を確認。
  hostの実レンダラ試験であり、実LCD確認ではない。シリアル未使用。

再実行（WSL、repository root）:

```sh
mkdir -p .cache/kasane-ui-smoke
TEST_SOURCE=tools/kasane_ui_smoke.c OUT=/tmp/kasane-ui-smoke bash tools/build_kasane_test.sh
/tmp/kasane-ui-smoke .cache/kasane-ui-smoke
python3 tools/kasane_ui_preview.py .cache/kasane-ui-smoke
```

O2はbuild時に`CFLAGS="-O2 -fstrict-aliasing"`を追加する。
previewは上段native・下段JS、左から上記5状態。標準PythonのみでPNG化し、画像はgit対象外。
firmwareコードに変更なし。テスト用build scriptは`TEST_SOURCE`未指定時に従来のQ試験を構築する。
ESP-IDF `-B build_ds_contract build`もPASS。app2,173,536 B、DIRAM123,404 Bで前回から変化なし。

## CP4b後の実機UI確認（2026-09-15）

ユーザーから実機利用可能との指示を受け、COM3へnative診断有効のfirmwareを書込み・照合。
native `~`とJS `K`（300 tick）ともPASS、home復帰。ユーザー目視も正常。
native転送前162,000画素一致、600フレームでheap/min/largest変化なし。
JS側のnative予約12,908 Bを実機ログで確認。診断スクリプトのcache期待値を更新し、
JS側に`--out`で全serialログの保存を追加した。
実測値、再実行コマンド、実機に残した診断構成のメモリ差は
[design-device-probe.md](design-device-probe.md)の2026-09-15節を参照。
100回起動やWi-Fi/audio併用試験は未実施。次の実装対象は引き続きCP5。

## checkpoint 0 — JS失敗の原子性（2026-09-15）

- 有効な所有transactionで起きた引数検証・getter・確保失敗は、JSでcatchしても更新全体をabortする。
- 古いtransactionは現在のbuilderを取消しない。callback後処理まで再入buildを防ぐ。
- ticket/template/instance wrapperをnative公開前に確保し、返却OOMによる部分成功やquota漏れを防ぐ。
- poll/features/statsのプロパティ生成失敗を検出し、部分オブジェクトを破棄する。
- 破棄済みDrawRefのnative枠を即時回収し、resetをまたいで識別IDを再利用しない。
- QuickJSが遅延生成時に参照するcacheメソッド定義を関数内`static const`へ変更する。
  自動記憶域の定義は初回`cache.create`で最適化構成のabortを引き起こしていた。

検証:

- `bash tools/build_kasane_test.sh && /tmp/test-pocket-kasane`: PASS。
- `CFLAGS="-O2 -fstrict-aliasing" OUT=/tmp/test-pocket-kasane-o2 bash tools/build_kasane_test.sh`
  と生成実行ファイル: PASS。
- 上記はadapter/core/test側のASan/UBSan・最適化構成。リンクするQuickJS本体は既存host cacheのO1 object。
- 各構成で129箇所のJS確保失敗、native calloc失敗と再試行、100回のabort後の枠回収を確認。
  allocator注入はメソッドを解決してから行い、QuickJS自身の遅延メソッド生成失敗は対象外。
- Astraが`tools/kasane_contract/run.sh`のASan/UBSan・O2、frost/PIE、C++ヘッダ試験成功を確認。
- ESP-IDF 6.0.1 `-B build_ds_contract build`: PASS。S3、PSRAMなし、KSN_DEVICE_PROBE無効。
  app 2,167,648 B、partition空き978,080 B。既存GNU-stackリンカ警告のみ。
- 実機K診断: 未確認。COM3への書込みを試行したが、portが存在せずopenに失敗。
  新firmwareは未書込み。USB再接続後に300ターン診断を実行する。

commit: `0227ac0`。`origin/vm/design-contracts`へpush済み。

## checkpoint 1 — committed stateの再描画・修復（2026-09-15）

- guest submissionがなくても、host invalidateから確定済みbankを再描画する。
- private repairはbankをswapせず、APP/SYSTEMのpoll・cache・ref・modalの確定状態を変更しない。
- 転送中に届いたinvalidateはack後も保持する。途中IO失敗は同じ画面を保持して再試行する。
- 転送前OOM/unsupportedではprivate repairの占有だけ解除し、修復要求を残す。
- `app_force_redraw`を接続。hostのみの修復成功後はJSへ進み、連続するrecording更新で
  guestが実行されなくなるのを防ぐ。Backの最終保存turnはIO修復待ちでも配送する。
- board側pet/recording overlayはSYSTEM移植まで既存合成を使用する。

検証:

- H: `tools/kasane_contract/run.sh`、ASan/UBSan・O2ともPASS。
- Q: `tools/build_kasane_test.sh`と生成exe、ASan/UBSan・pure O2ともPASS。
- 全17帯の転送失敗→cancel→owner repair失敗→成功をJS更新なしで確認。
  画素一致、poll/cache/ref/modal保持、callback中のinvalidate、OOM回復、reset、初回cancelも確認。
- 実sessionの連続invalidate時dispatchはコードレビューとESP-IDFコンパイルで確認。
  実機のrecording併用・picker終了・capture試験はUSB未接続のため未確認。
- ESP-IDF 6.0.1 `-B build_ds_contract build`: PASS。S3、PSRAMなし。
  app 2,168,048 B、partition空き977,680 B、DIRAM 115,548 B。既存GNU-stack警告のみ。

commit: `c787a6c`。`origin/vm/design-contracts`へpush済み。

## checkpoint 2 — group内gradientの最終ディザ（2026-09-15）

- グループの中間premultiplied RGBA8にはディザを適用せず、背景への最終合成後だけRGB565へ量子化する。
- 画素ごとのbitで、最後の不透明上書き以降のディザ付きgradientの寄与を記録する。
  半透明の子はbitを維持し、不透明なディザなしの子はbitを消す。
  不可視・clip外・実効alpha 0のgradientや離れたrect領域へ適用を広げない。
- 実効group alphaが0ならRGB565背景をそのまま返す。Bayer位相は絶対画面座標へ固定する。
- 64画素のRGBA8タイルは256 Bのまま。追加は2×uint32のディザbit 8 Bで、heap確保はない。
  正確な規則は[合成仕様](design-composition.md)の「グループ内gradientのディザ」に記載した。

検証:

- H: `bash tools/kasane_contract/run.sh`、ASan/UBSan・`-O2 -fstrict-aliasing`ともPASS。
  独立scalar参照で全256 group opacity、16 Bayer位相、混合子、透明画素、角丸・clip、
  32/64画素境界、PATCHとfullの画素一致を確認。既存rectグループの全opacity試験もPASS。
- 修正前rendererでは、新試験の画素(12,7)で`0x532f`と参照`0x532e`の不一致を再現した。
- Q: `bash tools/build_kasane_test.sh && /tmp/test-pocket-kasane`、および
  `CFLAGS="-O2 -fstrict-aliasing" OUT=/tmp/test-pocket-kasane-o2 bash tools/build_kasane_test.sh`
  と生成exeはPASS。JSのgradient公開はcheckpoint 8であり、このQは既存APIの回帰確認。
- ESP-IDF 6.0.1 `-B build_ds_contract build`: PASS。S3、PSRAMなし。
  app 2,168,208 B（0x211590）、partition空き977,520 B、DIRAM 115,548 Bで前checkpointと同量。
- 実機画素比較はユーザー指示により後日まとめて実施。シリアル操作は行っていない。
