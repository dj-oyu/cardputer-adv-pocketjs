# Kasane実装記録

実機確認方針（2026-09-15ユーザー指示）: シリアルポートは別タスクで使用中。
書込み・シリアル診断は後でまとめて実施し、各checkpointはhost試験とESP-IDFビルド後に
commit・pushして進める。実機未確認は各記録に残す。

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
