# Cloud native Model/Instance比較C

本稿は初回host比較の履歴。3d31fb86後の明示回収・製品provider・実機比較は[後続fixture](cloud-memory-fixture.md)を参照する。2026-10-31の固定診断a4185e2bでは、同一bootのA→C→A→C、cold/warm計8run/504drawが実機PASS。CのSTEADY guest86000..86652 Bに対しA125696..130872 B、ESP largestはC34816 B/A23552..29696 B。これは当該診断条件の報告値で、下表の初回host値や後続の狐headへ転用しない。highwater/minimumのcold/warm累積、EXIT2残存、GC/stack/画質未計測は後続記録を優先する。

初回記録時の状態: ローカルhost比較。対照JSソースは7d21b302、当時は製品CLOUD PROBEのnative asset切替・同head CI・この方式の実機確認は未完だった。7d21b302の実機fixture成功は[別記録](cloud-finite-fixture.md)を参照。

本表は初回native比較（公開bb97749相当のローカルsource、詳細SHAはresult.json）の記録。後続の再利用guard・例外分類・二重dispose修正後のCI結果は別headとして確認し、旧表の数値を新headの実測へ読み替えない。

実機前の境界確認で、IDF6.0.1のguest allocatorが4-byte整列である一方、C核arenaは8-byte整列を要求する差も見つかった。JS bridgeは元allocationを保持して7 B余分に確保し、整列したarenaを渡す方式へ修正する。arena自体は6600 Bのままだが、新bridgeの要求は6607 Bとなるため、本表の6600 B最大要求を修正後の値と呼ばない。host fixtureの通常allocatorは16-byte整列で、この差を隠していた。

同じ境界で、F64 opacity入力もdoubleポインタの整列を仮定せず、LE binary64のbyte spanとして読む。選択値だけをscalarで読み、全配列の追加copy/確保は入れない。installed Xtensa compilerのcompile-only probeではdouble alignment=8、pointer alignment=4。IDFの `heap_caps_base.c` の既定4-byte alignmentとguest.cの呼出経路を読んで確認した。意図的な4 mod 8 arenaと外部ArrayBufferの試験はASan/UBSan PASS、C核の未整列入力/NaN atomic回帰はm32もPASS。最終核SHAは `5b256aea2a199d23d2c44afe56d8969487ebf8a381953845734ff4e09e29648d`、旧表の核70435e4cとは区別する。

## 比較条件

Aは固定した7d21b302のJS scene/形状定義、Cは同じ雲をoffline exportしたreadonly AYNMとnative Instance。19nodes、16描画packet、14blend lobes。Cのblobは2766 B、Instance arenaは6600 B（既存work slot6144 B＋metadata456 B）。arenaはjs_malloc経由でguest会計へ含める。速度/位相は既存個体別follow actorを維持し、JSが演技とtargetを指示する。

Linux host i386/m32・SSE2、FMA contraction OFF、160 KiB=163840 B quota、通常GCあり・明示GCなし。両経路で同じ単一バイナリを使い、同じcamera/世界光、BODY・呼吸OFF、tick 0..300を30Hzのapp clockへ変換して提出する。カメラ倍率は両方とも明示F32値。一般のJS Number cameraとの完全同義性や全表情の受入ではない。UI controllerは構築せず、同じdriverで個体生成/描画/破棄を行う容量比較であり、通常app全体の測定と混同しない。

Aも新bindingを含む共通バイナリである。旧製品bin対新版binの総差ではない。バイナリSHA256は `9b77f9b6884d129a3daf3a63e185a9cf600a23e7ddcd0785acf3f29e1ab123ac`。最終C核SHA256は `70435e4cb35949c8fb8436fcd84a9297662fbd194ac6ccc0547f939551b57893`。実行前後の関連source hashは不変、再利用時に埋込みblob/flags/source/build manifestを照合した。

計測後の独立レビューで、初版build manifestのsource対象にはrenderer/QuickJS等の不足が見つかった。今回のA/C間でこれらを変更した事実はなく、同一バイナリの画像/容量結果は保持するが、旧manifestを一般のstale-source拒否証拠には使わない。再利用guardは別の修正で広いcompile入力と生成物/object/binaryを対象にし、旧schemaは拒否する。新guardと旧計測の実行事実を区別する。

schema2 guardはTemporaryDirectory内の24 positive/24 stale-inputケースで検証し、独立レビューでも再実行PASS。renderer、QuickJS/header、generator入力、生成物、object、binary、flags、blob、toolchain、旧schemaの変更で再利用を拒否する。新C経路の2個体目構築例外は再throwして試験FAILとし、source Aの拒否も理由を記録してCONSTRUCTION_REJECTEDと呼ぶ。null例外だけでOOMとは断定しない。

## Guest容量

`requested/current`はhost guest allocatorのTLSF長へ丸めた現在量、`accounted peak`はQuickJSのlive blockごとの8 B quota管理費も含む全確保時点の最大値。毎phaseのQuickJS malloc_size/countと式の一致を確認した。単なるframe末尾sampleの最大ではない。

| 指標（B） | A: JS source | C: native static asset |
| --- | ---: | ---: |
| app chunk load後 requested current | 96992 | 70384 |
| app chunk loadまで accounted peak | 114172 | 83456 |
| 個体生成後 requested current | 136324 | 95856 |
| 単体301frame後 requested current | 139172 | 98624 |
| 単体301frame後 accounted current | 154716 | 109280 |
| 単体301frameまで accounted peak | 156124 | 110572 |
| 最大単一guest要求（load含む） | 8496 | 6600 |
| 2個体目の生成 | 制限下で拒否、既存個体維持 | 成功・独立描画 |
| 破棄/再生成を含む全試験 accounted peak | 163828（拒否試行含む） | 134208 |
| runtime解放後 requested current | 0 | 0 |

単体のaccounted peak差は45552 B、requested steady差は40548 B。binファイルの圧縮だけの寄与ではなく、JS authoring graphと形状評価helperをnative側へ移した経路全体の差である。Cのhot arenaが零になったわけではない。以前のbin→通常JS再展開Bは現対照へ移植しておらず未計測。[旧Bの不採用](cloud-static-asset-study.md)と今回Cの効果を混ぜない。

Model.disposeは論理close。保持されたModelのcopy版cold dataはGCまで残り、static版はROM spanとして残る。今回Cは2766 Bをcompiled constから直接消費し、FS読込やJS ArrayBuffer経由のcopy版を計測していない。単体/2個体の個別disposeとruntime全解放を分けた。

## Guest外・静的領域・stack

malloc/calloc/realloc/freeのlink wrapperで追跡した明示native要求peakはA/Cとも41994 B、最大単一要求10260 B、runtime解放後0 B。guest custom allocator内部の確保とobserver自身は除外する。libc内部の非interpose確保、allocator管理費、実機free/largestは未計測。この値をguest量へ足して物理RAM総量としない。

自己試験でguest確保がnative側へ混ざらず、native確保が記録され、解放後に戻ることを確認した。初回 `first-ac` はcompiler最適化によりguest/nativeの分離が崩れていたため、native値を採用しない。volatileな観測境界と実際に呼ぶwrapperの自己試験を入れ、`final-ac` のA/Cを同一バイナリで再実行した。guest/imageの初回結果と最終結果は一致するが、本表の根拠は最終結果に限定する。

観測registryはhost fixture BSSの32768 B、panel/stripは68640 Bであり製品へ追加されない。ローカルESP-IDF製品linkのDIRAMは179924 B（7d21b302の179908 Bから+16 B）、mapped flashは1607364 B。これは新core/bindingを組んだdirty buildで、cloud blobはhost専用、製品へまだ埋め込んでいない。BINサイズや実機free heapとは別の指標である。

m32 `-fstack-usage` の関数単体値はevaluate 1088 B、submit bridge 896 B、target batch 112 B。call chain全体のpeak、Xtensa stack、実機水位ではない。packet snapshot copyは維持したが、copy総bytes・局所更新/光変更の依存別実行量・ESP速度は今回未計測。変更なし評価は省く一方、変更があれば全node再評価する現段階を最適化完成とは扱わない。

## 描画・所有権の結果

A/Cの最初の301frameについて、240×135 RGB565 panelの各frame FNV64ハッシュ列がすべて一致（233種類）。各経路内ではincremental/full-reference全画素一致を確認した。A/C間のraw packet全byte比較や全camera/表情の網羅ではない。

Cは同じModelの2Instanceを160KiB内で生成し、片方dispose後の他方描画、再生成、最終解放まで成功。Aの2個体目はこの制限下で生成を拒否されたが、残った個体の画像を維持し再生成/解放へ進めた。このdriverは生成例外の種類を記録しておらず、quota直前のpeakだけでOOMを独立確定したとは扱わない。Cのparserは全2766切詰め・5000mutation、型/section/ID検査、atomic target、owner/generation、出力alias、visibility、disposeを検証。JS bindingは実QuickJSの4逐次runtime、2Instance、copy/static寿命、SAB/切離し拒否、再入、192故障位置中35実確保失敗、同runtime復帰と残留0を確認した。1000回の変更なしtarget/evaluate/submitはguest確保0。

bindingのisolated packet recorderによるrollback確認は、実rendererのbudget失敗後にcandidateがcommit可能へ戻る証拠ではない。既存契約どおりcandidateを破棄してnew beginで再試行し、旧committed画像を維持する。この新経路の実機故障注入は未実施。

新bindingを組んだ通常のhost経路でも、従来CLOUD PROBEを160KiBの独立2プロセスで再試験し、297成功描画・digest e12e1890・pause15・commit失敗時の旧画像保持と次turn復帰・解放0を維持した。これはnative版controllerへの切替試験ではない。最終sourceのparserとbindingを再実行する `run_model_contracts.py` はASan/UBSan PASS、core単独m32の最終alias回帰もPASS。

## 判断と次工程

Cは容量の面で有効だったため、ハイブリッドModel/Instanceの経路を継続する。今回は既存production entry/chunksを変更せず、native版entryは実験用。次は組込み資産providerと通常CLOUD PROBEへの接続、同head回帰/CI、実機候補の引渡しへ進める。全キャラの造形や将来asset形式の完成とは扱わない。

再実行入口は `tools/ayatsuri/model/run_model_study.py --variant A --m32 --frames 301` と、同build manifestを検証して再利用するC経路。WSLではASCIIの `M32_SYSROOT=/tmp/m32sys` を同一呼出内で準備する。並列helperを走らせない。証拠は `.cache/native-model-study/final-ac/{A,C}-result.json`、`{A,C}.jsonl`、`{A,C}-pixels.csv`、`.cache/ayatsuri-native-core/final-alias-family-results.json`、`.cache/ayatsuri-model-qjs/{tiny-asan,cloud-asan}.log`、`.cache/native-model-product-build-manifest.json`。
