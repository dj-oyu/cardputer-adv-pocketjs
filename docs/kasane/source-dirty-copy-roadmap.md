# Kasane 次期ロードマップ：汎用 source、dirty-node、コピー削減

2026-09-23。Astra との設計レビューに基づく実装順序。対象は Cardputer ADV
（240×135、PSRAM なし）の `mount` 経路。これは計画であり、下記の API や性能目標が
実装済みであることを意味しない。実装は各段階のゲートを通過してから次へ進む。

## 到達点と現状

- Kasane core/schema はアプリ名・source 名を知らず、アプリ側が不変 descriptor と
  型付き slot を提供する。JS は画面全体を保持・再構築せず、必要な base 値だけを `set` する。
  native producer は読取り専用 snapshot を公開し、slot の現在値を自動更新できる。
- 変更slotに依存するnodeだけを解決する。ただし topology、背景、viewportの変更は
  安全な全体 REPLACE に戻る。画素、transaction、失敗時 repair は現行と一致する。
- コピー回数は範囲を隠さず計数する。動的 native payload は producer 公開後から
  **各core destinationへの受理まで**最大1回を先に実現し、複数destinationへの
  合計も開示する。producer 原データから LCD 描画完了までの
  **全経路1回以下**は別の厳格な到達点であり、二重bankと描画scratchを含めて判定する。
  達成できなければ「未達」と報告し、目標を狭めて完成扱いにしない。
- PIEはrenderer内部の選択であり、source API、descriptor、slotへ命令幅を漏らさない。
  音声taskをUI snapshotのために待たせず、未制限の確保や保持を導入しない。

現状では `ksn_schema_session` が依存表を保持するが、更新時の
`ksn_schema_update` は全可視nodeを解決・比較し、PATCHでは再度走査する。
`set` は一時UTF-8→schema所有文字→coreへ進む。PATCH開始時にはcoreの
command/text bankの使用済みprefixを複写し、通常描画readは文字をscratchへ複写する。
一方、revision不変時はO(1)でskipし、確定bankとの比較は借用を使っている。
clockの`source_read`とmusicの`bind('playback')`は個別実装である。
[使用済みprefix複製の実機比較](live-bank-clone-optimization-20260923.md)では
helloのclone bytesを93.7%減らしたが、1-copyやdirty-nodeは未達。

## P0：計測・正しさの基準を固定

2026-09-23の[実機予備基準](p0-device-baseline-20260923.md)で
helloと無音overlayの分位点、計測箇所別のcopy分類を取得した。ただし
producer snapshotやQuickJS内部の全コピーは未計数。追加の
[SD再生予備測定](p0-audio-baseline-20260923.md)で約248秒の音声共存値を得たが、
その後の同一バイナリ追試でunderrun 0と再生時p99約16 msを全件ヒストグラムで
確認した。追加の同一診断版測定で、再生時draw p99 16.127 msを
LCD送信p99 11.775 ms・送信以外p99 11.135 msに分離し、
無音時はそれぞれ8.575 / 5.247 / 3.455 msだった。
どちらも1フレーム64,800 B・17帯の全画面転送である。
各分位点は別フレームなので加算しない。copy内訳は計数した範囲で
取得したが、後追加したmusic model構造体、QuickJS内部等が未網羅。
診断ビルドの時間上乗せ、新経路との同一曲A/Bも未評価。
以下の出口には未到達。

2026-09-24のP0(1)更新：負方向gradientの`span << 16`を等価な
64-bit乗算へ変更し、参照テスト内の同じUBも修正した。
`run_group_tile.sh`にASan/UBSanを加え、負方向を含む約544万算術ケースと
画素一致を診断0件で通過。`kasane_contract/run.sh`のASan/UBSan・O2、
実QuickJS統合のASan/UBSan・`-O2 -fstrict-aliasing`、診断OFFのESP-IDF
ビルドも同一コードでPASS。これはP0(1)の対象経路を確認した結果であり、
P0(2)～(4)や実機ゲートの完了を意味しない。

P0(2)の計数境界は[copy計数の監査表](p0-copy-coverage-20260924.md)に置く。
明示的な構造体代入・schema参照確定・描画view複製を追加計数したが、
QuickJS内部や全producerの複写は依然未網羅であり、合計はpartialのまま。
[計数あり/なしのABBA実機比較](p0-copy-probe-abba-20260924.md)では、
音声再生時draw p99の悪化は見えず、LCD送信p99に128 µsの差が出た。
以後の時間基準にはcopy計数なし版を使う。
[通常アプリ・音声overlayの固定時間ゲート](p0-timing-gates-20260924.md)は
P1以降の実装前に採取した。長時間音声、SD抜去、全アプリの画面・更新、
24slot最大構成の実機基準はまだ不足しており、P0全体の出口は未達。

1. `ksn_render.c` の既知のsigned-shift UBSan警告を解消し、sanitizer診断0件を
   成功条件にする。ASan/UBSanと`-O2 -fstrict-aliasing`、QuickJS統合、ESP-IDF buildを
   同じcommitで確認する。
2. 計数を **JS UTF-8変換、producer snapshot生成、adapter/schema所有copy、
   core提出copy、bank clone、render scratch copy** に分ける。回数とbytesの双方を記録し、
   合計も表示する。`memcpy`以外の等価な書込みも数える。計数なしビルドとの実行時差も測る。
3. 無変化、1slot、全24slot、最大47-byte文字、hidden/page切替、提出中の連打を
   再現可能なworkloadにする。通常アプリと音楽overlayの**現行版の基準**を取り、
   binary SHA、画面、設定、ログ、SD曲、電源条件を保存する。未実装の新経路との
   同条件A/BはP2・P5の出口で行う。新経路をP0の前提にすると工程が循環するため、
   比較要求そのものは削らず実施位置を移す。
4. p95/p99/max、12 ms超過数、dirty帯・LCD bytes、free/largest/min heap、
   stack high-water、音声underrunの数値閾値を、基準測定の揺らぎを見て**実装前に固定**する。
   測定後に閾値を緩めない。短期の平均値だけで合格にしない。

出口：診断0件と再現可能な基準値。計測機構が描画のp99を変えるなら別ビルドで
検証し、測定値を混同しない。P0は性能最適化ではない。

## P1：アプリ非依存の native source / subscribe

アプリ・systemサービス側のregistryがopaque source handleを発行する。Kasaneは
source名、音楽・時計などのdomain、status文言を解釈しない。`mount`したviewは
「型付きslot→source field」の対応を宣言して購読する。例えば
`view.bind({source, bindings:{time:'face', syncLabel:'sync'}})` は概念APIであり、
正式なJS/Cシグネチャは実装前に固定する。権限確認はregistryで行い、文字列名だけで
他アプリの非公開sourceへ接続できないようにする。

producer契約：

- owner turn内で`acquire(cursor)`→不変の型付きfield viewを参照→`release`。
  snapshotはgeneration、単調revision、consumerごとのchanged-field mask、validity、
  次の期限/起床時刻と、対応fieldの再取得可能な**完全な現在値**を持つ。
  ポインタはlease終了後に保持しない。読み手がrevisionを
  飛ばしたら累積差分を返すかall-dirtyへ戻す。global dirty maskを一読者が消さない。
- `acquire`だけでconsumer cursorを進めない。全fieldの検証成功後、dirtyと最新値を
  再取得できる状態を確保してから読取りcursorを進める。表示済みcursorは別に持ち、
  PRESENTEDでのみ進める。preflight失敗・BUSY・DISCARDEDでも変更を落とさない。
  不正snapshotはbase/effective slotを部分更新せず、直前の有効表示を維持する。
- 別taskが生成する場合は固定容量のpin可能snapshot等で寿命を保証する。
  `const`だけを不変保証とみなさない。pool枯渇はboundedなcoalesce/skipとし、
  音声taskがUIを待つ、無制限確保する、古いsnapshotを上書きする、を禁止する。
- v1では1slotにつきnative writerは1つ。複数bindingの同一slot書込みは拒否する。
  JS `set` はbase値、native値がvalidな間はそのslotのoverride、失効・detach時は
  最新base値へ復帰する。期限切れもdirtyとしてnative schedulerが処理し、
  JSの`frame()`を起こさない。無制限のpriority stackは作らない。
- 同時に整合すべきfieldは同じsnapshotに束ねる。異なるsource間の原子的同時性は
  v1では保証しない。版付き構造体・明示サイズ・世代handleを境界にし、将来PIE実装や
  producer交換のために内部pointer配置を公開ABIにしない。

最初にclockを共通sourceへ移す。偽sourceのCテストとQuickJS統合で、更新、欠落、
期限、base復帰、detach/reset、権限拒否、型不一致、古い世代、複数consumerを確認する。
既存clockとの画素一致、JS turn増加なし、常駐/peakメモリの上限を出口とする。

2026-09-24途中経過：内部C契約とclock移行、host試験、helloと音声の実機gateを
[P1途中結果](p1-source-clock-20260924.md)に記録。公開bind、複数source、
期限scheduler、別task producerの固定snapshot、画素一致は未達。
音声描画gateは同一実行payloadでも合否が揺れたため未合格。
配置原因説はバイト比較で棄却し、LCD転送とSD readの同時性を調べる。
[同一診断バイナリでのLCD送信分解](p1-lcd-sd-contention-20260924.md)で、
遅い試行のreap待機とSD read重複の相関を確認した。ただし物理DMA競合か
task復帰遅れかは未確定で、P1の性能gateは未合格のまま。
[SPI2完了callbackを用いた追試](p1-spi-isr-wakeup-20260924.md)では、
遅い/速い試行でcallback前p99は同じ4,607 µs、callback後p99は
7,807/383 µsだった。主な不安定性は物理転送時間よりUI task復帰側に
ある。[task配置の独立比較](p1-spi-isr-wakeup-20260924.md)で、decoderを
core 0に固定するとSPI2 ISRが既定core 0のままでも45秒×3試行と240秒の
音声再生でdraw p99 9.471 ms、fault/underrun 0を確認し、MP3経路の
既定配置へ採用した。出力taskやISRだけの移動は遅延を解消しなかった。
P1全体では公開bind、複数source、期限scheduler、固定snapshot pool、
画素一致と新経路の同条件A/Bが引き続き未達。

[固定snapshot pool primitive](p1-fixed-source-pool.md)を追加した。
3 slotを事前確保し、producerが空きslotへ直接書いて公開、readerは
immutableな世代をzero-copyでpinする。枯渇時はproducerを待たせず`BUSY`。
並行host試験と診断OFF buildは通したが、providerへの接続と実機gateが
未実施なので、上記「固定snapshot pool未達」の判定は変えない。
汎用source providerへのpool adapterと複数consumerのhost契約試験も
追加した。実際の別task producer、公開bind、期限scheduler、実機gateは
残っており、P1全体の完了判定は変えない。
2026-09-24：不正snapshotの最終slot検証が失敗した際、途中まで
上書きした`effective`候補をbaseへ戻すよう修正した。成功時のmetadata/
payloadコピー回数は不変。先頭fieldだけ有効で後続fieldが不正な場合と
長すぎる文字列をhostで確認し、実QuickJS統合は0失敗。
診断OFFのESP-IDF buildはPASSし、静的DIRAMは159,788 B。
公開bind・mountへの複数source接続は引き続き未実装。
複数sourceのC合成primitiveも追加した。最大4購読を異なるregistryから
同時にpinし、base全体のmetadata複製1回と有効なbound slotごとの
metadata代入でdisjoint slotへ重ねる。UTF-8 payload転記は0回。
全source取得後に値を1回検証し、後続sourceの
不正値・権限拒否・古い世代では全leaseを解放し候補をbaseへ戻す。
cursorはbundle全体を受理した後にだけ進める。hostのASan/UBSan・O2で
重複slot拒否、期限切れ、失敗後のpin/cursor回収を確認した。
2026-09-24：アプリ所有の不変asset定義を複数source宣言へ拡張し、
`mount`がdisjointなslot購読を登録してbundle合成するよう接続した。
登録表はmount内で全sourceが1つを共有し、source数に比例する重複表を持たない。
単一sourceのclockは小さい既存lease経路、sourceなしアプリは従来の
allocation/更新経路を維持。host限定の2 source QuickJS試験で、同時更新を
1提出にまとめること、提出中の再取得抑止、後続source失敗時の無提出・
全pin解放・復旧を確認した。公開JS `bind`、期限scheduler、別task producer、
実機gateは未達で、P1出口の完了判定は変えない。Kasane契約一式の
ASan/UBSan・O2、2 source QuickJS統合のASan/UBSan・
`-O2 -fstrict-aliasing`はすべて0失敗。診断OFFのESP-IDF buildもPASSし、
静的DIRAMは159,788 Bのまま。COM3は使用していない。
2026-09-24：source snapshotの最も近い将来期限1件をmountが保持し、ownerの既存待機を
その期限まで短縮する経路を追加した。期限到来でnative overrideから最新の
JS baseへ戻り、別sourceの画素を変えず、native更新後に再びoverrideへ戻る
一連の動作を2 sourceの実QuickJS host試験で確認。期限後や提出失敗時に
0 tick待機を返してbusy-spinしない。新たなtimer/taskは設けていない。
これは通常のowner frameを起点にした期限処理であり、別task producerの
到着通知や実機の期限精度・音声共存gateはまだ未検証。公開bindも未実装。

## P2：dirty-node更新を既存bankのまま導入

2026-09-24の実装途中結果：JS `view.set`は検証用の全slot候補を維持しつつ、
受理時に**変更slotだけ**をschema所有値へ書き戻す。そのmaskとnative sourceの
dirty maskをsessionに渡し、依存nodeだけのresolve/ref比較/PATCHを実装した。
可視性・page・文字の空/非空で命令数が変われば全REPLACEへ戻す。
node→命令数は2 bitset（計8 B）で保持し、PRESENTEDで候補mapを昇格、
DISCARDEDではinflight dirtyをpendingへ戻す。clean turnのrevision/viewport
一致は従来どおりO(1)。24 slot・hidden/page・47 byte文字・提出中更新・
discard・viewport変更と120フレームを全画面参照との画素比較で通した。
1 slot更新では対象nodeだけを比較・適用の2回resolveした。
実QuickJS統合のASan/UBSan・O2は各0失敗、pet allocationは1256 Bで
従来1280 B以内、Kasane契約テスト一式も両構成でPASS。
診断OFF buildの静的DIRAM増加は0。
ただし実機の同条件A/B、音声共存、固定閾値は未実施でP2出口は未達。
2026-09-24：実機A/B用に直前版`65c2770`とdirty-node版`31b78f8`を
同じ`KASANE_P0_PROBE=ON`、copy/bus probe OFF、`POCKET_PROBES=OFF`で
ビルドした。app image SHA-256は直前版
`399de067390b2e745561dbcdd303a140ca4db08b8b2b56efdf9ddd4c9be3dde7`、
dirty-node版
`0cc3bf24fbf45bb828890a587cb78acd699d0fdc6f9b7628d7056dc3afcebef8`。
Kasane契約テスト一式は同日のASan/UBSan・O2の両構成で再度PASS。
COM3は未使用で、実機gateの合否は未判定。

`set`とsourceからslot変更maskを受け取り、既存のslot→node依存表でdirty nodeを得る。
committed node→command範囲を固定容量で保持し、非dirty nodeのresolve/read/compareを
避ける。hidden nodeは0命令、`plateText`は複数命令なので、対応表は候補と確定を分け、
PRESENTED後だけ昇格する。可視性、page、背景、viewport、resource topologyに触れる
更新は最初は保守的に全REPLACEへ戻す。dirty PATCHに二重のresolved planを置かない。

提出中の変更は`pending_dirty`と`inflight_dirty`を分ける。submitで対象をinflightへ移し、
PRESENTEDはinflightだけackする。DISCARDEDはinflightをpendingへ戻す。
同じslotが提出後に再変更されても最新値とdirty bitを残す。source snapshotは
後のowner turnで再取得してよいが、maskを落としてはならない。

出口：full-scan参照実装との画素・可観測状態・原子性・終了結果・失敗時repair一致。
保守的REPLACEを許すため、ticket発行数とPATCH/REPLACE比率は一致条件でなく
性能測定対象とする。
1slot/24slot、hidden→visible、page、plateText、viewport、背景、A→B→A、
提出中更新、LCD失敗→discard→retryをhost fuzzと実機で確認する。
通常アプリのP0現行版とdirty-node版を、保存した画面・設定・workload・
電源条件で同条件A/Bし、固定閾値で判定する。
計算量はclean turn O(1)、変更turnは少なくとも検証O(slot数)＋解決O(dirty node数)；
core bank cloneは依然O(使用済みbank bytes)。全処理をO(dirty node数)とは呼ばない。
期待より遅い/容量超過ならfull-scanへ戻せる境界を残す。

## P3：native payloadの1-copyと寿命ゲート

2026-09-24のhost境界試験：`test_source_copy.c`で1つのnative UTF-8
payloadを2つのtext nodeに渡し、schema/adapterのpayload転記0回、
core受理時のtext転記2回（各destinationに1回、5 Bずつ）を計数した。
source lease解放後に元バッファを書き換えても、REPLACE/PATCHの提出済み値、
描画、後続のrepairが保持される。DISCARDED時は同revisionの完全snapshotを
再取得してdirtyを復元し、再提出できる。ASan/UBSan・O2でPASS。
これはhostのsource→core受理境界の証拠であり、producer内部の生成copy、
実Cardputerのpool-backed producer、音声共存、全経路1-copyの証明ではない。
P3出口は未達。

producerが公開済みの不変UTF-8をowner turnだけ借り、schemaの中間文字所有を
必須にせずcoreへ提出する。core受理時に1回コピーすればlease解放後も描画・repair
可能になる。静的flash値は借用のままでもよいが、動的値に寿命保証なしの0-copyを
適用しない。hidden値や提出待ちの最新値をどこが所有するかを明示する。

出口：**producer公開済みpayload→各core destinationの実測copyが1回以下**。
同じsource値を複数nodeが参照する場合は現coreでdestinationごとにcopyされるため、
source値全体では複数回になり得る。その合計も必ず記録する。値全体で1回を
P3の必須条件にするなら、共有text所有をP4から前倒しする必要がある。
producer側snapshot生成、bank clone、render scratchは同じ結果に別勘定と合計を併記し、
全経路1-copyを達成したと主張しない。source解放後のrender、DISCARDED、repair、
hidden→visibleで最新値、pool枯渇・音声併用を通す。問題があればowned-copyへ戻す。

## P4：JS所有とcore bankの全経路copyを個別判定

JS文字列にはUTF-16→UTF-8変換、getter/例外、GC、提出待ち、同じ値を複数nodeが
参照する場合がある。単一のadapter-owned UTF-8値、候補bankへ直接materializeする案、
固定容量の共有text arenaを比較する。stack上の最大24×48 B一時文字領域を減らすが、
一時heap増加・断片化・再入・原子性破壊を許さない。

coreのPATCH開始時の全bank cloneとrender readのscratch copyを残す限り、
**producer原データ→描画完了の厳格な全経路1-copyは未達**。copy-on-write bank、
不変text lease、rendererのborrow read等は別プロトタイプとして測り、
確定表示の不変性、途中IO失敗のrepair、ticket世代、メモリ上限、PIE整列を保つ。
複数nodeが同一文字を使う場合は「値ごと」と「destinationごと」の双方で計数する。
固定bankの方がp99・メモリ・複雑さで有利なら採用せず、全経路1-copyを未達として
ユーザーに判断を戻す。数値を良く見せるためだけに計数境界を狭めない。

## P5：music統合と最終実機ゲート

music固有の文言、help、再生projection、期限付きstatusはmusicモジュールに残す。
AV snapshotとslotの対応だけを共通source契約へ移し、汎用Kasaneにmusic条件を入れない。
既存providerは比較経路として保持し、画素・入力・状態遷移・性能が一致してから
不要部分だけ削除する。musicを無理に単一schemaへ収めることはv1の完成条件ではない。

最終実機は同一バイナリ内A/Bが可能なら優先し、旧→新→旧→新でも再確認する。
同一曲の長時間再生、pause/resume、seek、overlay/home/app切替、system status重畳、
画面修復、低空きheapを含める。p95/p99/max、12 ms超過、LCD bytes/帯、
free/largest/min heap、stack high-water、音声underrunをP0の固定閾値で判定する。
画素不一致、transaction/repair破壊、unbounded allocation、producer待機、
p99または音声deadline悪化なら切替を止め、原因を切り分ける。

## 完成判定

- **汎用subscribe + dirty-node v1**：P0–P2と対象アプリの実機ゲートが通過。
- **native公開済みpayloadの1-copy/destination**：P3の範囲と全copy内訳を実測で通過。
- **全経路1-copy**：P4でbank clone・描画scratch・必要なencoding/共有を含む
  定義と実測が通過した場合だけ認定。P5へ進めても未達なら未達と明記する。
- **Kasane FW全体**：さらにnative home/picker/editor等の所有権移行と既存ロードマップの
  完了条件が必要。この文書の3機能が完成しても自動的に全FW完成とはしない。

現行コード・計測の入口：`main/ui/kasane/ksn_schema_session.c`、
`main/ui/kasane/ksn_schema.c`、`main/ui/kasane/ksn_core.c`、
`main/pocket/pocket_kasane.c`、`main/pocket/app_view_assets.h`、
`main/pocket/app_music_view.c`、`tools/build_kasane_test.sh`、
`tools/kasane_contract/run.sh`、`tools/overlay_device_test.py`、
`docs/kasane/device-performance-gate-20260923.md`。
