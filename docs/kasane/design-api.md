# Kasane利用APIとowner境界 v0.4

2026-09-14。`ksn_view.h`と`ksn_view_host.h`、`pocket.kasane`は実装済み。
既存アプリの移植は未実装。この文書は既存PocketJS UIの
`pocket.ui`と区別し、Kasaneの利用窓口を定める。
画素と容量の規範は[合成仕様](design-composition.md)と[Kasane仕様](design-system.md)。

## 1. 再構成の理由と境界

従来はcoreのabort/present/discard、cacheのabort/resolve、modalのcancel/resolveを
利用側が正しい順序で呼ぶ必要があった。更新失敗やVM yieldで一つでも漏れると、
画面・instance参照・入力scopeの状態が食い違う。
`ksn_view_host`がその順序を所有し、アプリadapterはレイヤー固定の`ksn_view *`だけを受け取る。

```mermaid
flowchart TD
    App[アプリ状態 / 将来のQuickJS adapter] --> View[ksn_view: 更新・部品・modal・結果poll]
    Owner[UI owner] --> Host[ksn_view_host: end_turn / present / input route]
    View --> Host
    Host --> Core[ksn_core: 2バンクと世代]
    Host --> Cache[ksn_cache: 明示template / instance]
    Host --> Modal[ksn_modal: 入力scopeとfocus]
    Host --> Render[ksn_render: damage / 合成]
    Render --> LCD[既存の共用帯 / LCD port]
    Frost[ksn_frost: 補間＋tint / PIE] -. capture接続は未実装 .-> Render
```

公開型を`ksn_composition_types.h`へ分離した。`ksn_view.h`から内部bankやcore/cacheの
保存レイアウトを参照する必要はない。endpointはAPPまたはSYSTEMに固定され、
begin引数にlayerを渡さない。template/instanceも発行側layerに限定する。
関数呼出にheap確保はなく、部品ごとのvtable・汎用broker・イベントqueueを追加しない。

## 2. 公開操作

| 用途 | 実装済みC API | 契約 |
| --- | --- | --- |
| 能力と容量 | `ksn_view_features`, `ksn_view_get_stats` | rendererまで動く能力だけ公開。core保存形式の語彙と区別 |
| 更新開始 | `ksn_view_begin` | REPLACE / PATCH、共有builderは一つ。BUSY時はdomain stateを保持 |
| 基本描画 | `background`, `add`, `change`, `group`（`ksn_view_`接頭辞） | 背景、矩形、属性更新、隔離group opacity |
| 提出と取消 | `ksn_view_submit`, `ksn_view_cancel` | submitはLCD完了ではない。cancelはbuilderまたは未確定提出を取消 |
| 結果取得 | `ksn_view_poll` | layerごとに最後の`ticket/status/reason`を保持。別layerの提出では消えない |
| 明示部品 | `ksn_view_cache_create/release` | 不変命令templateのコピー。更新の外で作成・解放 |
| 部品の配置 | `ksn_view_instantiate/place/visible` | 複数同時表示、位置・clip・opacity・表示のPATCH |
| modal | `ksn_view_modal_open/close` | APP REPLACE内。solid/dim-live、表示成功後に入力scopeとfocusを確定 |

`draw_kinds`はRECT、ROUND_RECT、STROKE、GRADIENT、`cache_kinds`は先頭3種を公開する。
グループ透明度は対応、native animationとfrosted modalはfalse。文字・画像はコアに
保存できても描画APIではUNSUPPORTEDを返し、LCD提出まで進めない。
すりガラス画素フィルタのPIE対応はfrosted modalの完成を意味しない。

## 3. 原子的な更新と参照

新APIでは、所有中builderの変更に失敗した時点でcore/cache/modalをまとめてabortする。
旧低レベルAPIの「poisonをabortまで保持」は内部契約として残す。
別layerや古いticketでの呼出しは正しいbuilderを取消しない。
参照やinstanceの出力はその更新の候補値で、PRESENTED確認後にアプリの表示参照へ昇格する。
失敗したREPLACEの候補参照を次更新へ持ち越さない。育成値などdomain stateは戻さない。

```c
/* widthはdomain側で0..100へ検証済み。fillは前回PRESENTEDの参照。 */
ksn_tx ticket;
ksn_result r = ksn_view_begin(view, KSN_PATCH, &ticket);
if (r != KSN_OK) return r; /* BUSYなら次回、最新のwidthで再試行 */
ksn_change change = {.property=KSN_SET_RECT, .value.rect={8,110,8+width,118}};
r = ksn_view_change(view, ticket, fill, &change);
if (r != KSN_OK) return r; /* 新窓口が既に全builderをabort済み */
return ksn_view_submit(view, ticket);
```

submit後はSUBMITTED。ownerが必要な全帯を転送した時にPRESENTEDへ進む。
転送失敗はSUBMITTEDを維持し、再試行は全帯修復。cancelした場合はDISCARDEDになるが、
部分転送の修復要求は残り、修復完了までAPP入力は遮断する。
表示済みticketへの遅いcancelはSTALEを返し、成功した画面を巻き戻さない。
pollは消費型queueではなく固定state。同一layerの次submit前に結果を扱う。

## 4. cache / modal / VM境界

cache.createは更新の外で行う。templateは明示releaseまたはhost resetまで保持する。
template作成を後続REPLACEの取消に連動させない。表示中のinstanceを参照するreleaseはBUSY。
同じtemplateをREPLACE内で複数instantiateできる。PATCHはplace/visibleで更新し、
構造変更はREPLACE。REPLACEから省略されたinstanceは表示成功時にdetachされる。
abort/discardした配置・非表示変更は元に戻り、新instanceは破棄される。

solid modalはREPLACEの先頭、dim-liveは通常内容を追加した後にopenし、その後にmodal内容を追加する。
closeは最新domain stateを再構築するREPLACE内で行う。closeが失敗したらmodalを維持する。

ownerは**全JS復帰境界（正常・例外・yield）**で`ksn_view_host_end_turn`を呼ぶ。
未提出builderをabortし、提出済み状態は残す。
`ksn_view_host_present`は描画→core確定→cache反映→modal反映→layer結果保存の順で処理し、
途中でJSを呼ばない。入力は`ksn_view_host_route`で到着時に一度だけ配送先を決める。
初期化/resetの前にはguestを破棄し、古いendpointを持つJSを再開させない。
hostはcore/cacheを排他的に所有し、旧低レベルAPIとの混在更新を禁止する。

## 5. JS側の接続

既存仕様の`view.patch(tx => ...)`、軽量参照wrapperはこの窓口へ接続する。
JSのbegin/submit/cancel/pollがネイティブの同名操作に対応し、公開rootはAPP endpointだけを持つ。
共有prototypeと固定数のwrapperで参照を保持し、bankポインタをJSへ渡さない。
callback版patch/replaceは同期処理に限定し、PromiseやVM yieldを跨ぐbuilderを禁止する。
入力、時計、音声、保存は既存サービスを利用する。

JS adapterの対応契約:

| JS表現 | nativeへの対応 |
| --- | --- |
| `view.replace(build)` / `view.patch(build)` | begin→同期build→submit。例外/thenable/yieldならcancel。戻り値はticket |
| `tx.background(color)` / `tx.rect(spec)` | background / add。rectは候補DrawRefを返す |
| `ref.setRect(tx, bounds)` / `setClip` / `setColor` / `setVisible` | change。wrapperは作成時に一度だけ確保し、setterで増やさない |
| `view.cache.create(rects)` / `release(template)` | 更新外の不変template管理 |
| `tx.instantiate(template, placement)` | 候補InstanceRefを返す。placementはoffset、clip、opacity、visible |
| `instance.place(tx, placement)` / `setVisible(tx, bool)` | place / visible。template内部は変更しない |
| `tx.modal.open(spec)` / `close()` | modal_open / modal_close。specはbackdrop、color、focus |
| `view.poll()` / `view.cancel(ticket)` | poll / cancel。Promiseや自動再提出を生成しない |
| `view.features()` / `view.stats()` | features / get_stats |

buildが作る候補wrapperは提出結果と関連付け、DISCARDEDなら無効化する。
呼出前にwrapperのsession・所有view・状態を検証する。内部の数値IDを利用者が差し替えられる
普通のJS propertyに保持しない。参照上限32件は既存BETTER契約を引き継ぐ。
schemaの文字列ID解決は構築時に行い、毎PATCHで名前検索しない。
画面への自動レイアウト、暗黙cache、Reactiveな依存追跡は追加しない。

`main/pocket/pocket_kasane.c`がAPP endpointを`pocket.kasane`として遅延公開する。
features/statsだけではnative arenaを確保せず、最初のreplace/patch/cache.createで一括確保する。
公開DrawRefは32件。原子的REPLACE中に旧世代と候補世代を同時保持できるようnative slotは
2世代分を持つが、1更新が新規公開できる参照は32件を超えない。

session ownerは初回submitで旧RGB565 rendererを解放し、Kasaneのdirty帯だけを共用LCD stripへ送る。
ソース評価中の初回submitとLCD修復は次のJS turnより先に処理する。全JS復帰境界で
`pocket_kasane_end_turn`を呼び、guest終了前に`pocket_kasane_reset`する。

schema loader、文字・画像rendererは未実装。旧PocketJS/Taffyからの完全移行は
[Kasaneロードマップ](kasane-roadmap.md)に従う。

## 6. 検証とメモリ

`tools/kasane_contract/test_view.c`は二つのcache instance、透明度、PATCH取消、
VM yield相当の未完modal、新instanceのcleanup、別layerの干渉拒否、layer別poll、
転送途中失敗→cancel→全帯修復、modal開閉とfocus復帰を検証する。
ASan/UBSan、O2 strict-aliasing、C++17ヘッダ検査を`run.sh`へ組み込んだ。
実機の`VIEW PASS`も同じ窓口で二つのinstance、取消、無変更の転送ゼロ、modal開閉を通す。

core 9,216 B、cache 4,096 Bは維持。追加coordinatorはホスト64-bitで112 B。
S3の実サイズは実機診断で84 B。共有IDカウンタは20 B。
statsのnative_bytesはこれらの合計で、LCD帯・JS heap・frost snapshotを含まない。
QuickJS adapterの参照2世代分を含むS3実測は14,440 B。既存LCD帯3,840 Bを合算すると
18,280 Bになる。arenaはKasaneを使わないsessionでは0 Bで、静的DIRAM増分は52 B。
部品数ごとのnative追加heap確保は0。PIE側も保持snapshotは2,048 Bのまま。

`tools/test_pocket_kasane.c`は実QuickJSとASan/UBSanでreplace/patch、group、cache、modal、
参照失効、cancel、LCD失敗と全修復、thenable拒否、参照上限を検証する。
`tools/kasane_device_test.py`はUSB診断`K`を300ターン動かし、通常/modal/appのscope遷移と
30フレーム窓のturn/render/send時間を検査する。
