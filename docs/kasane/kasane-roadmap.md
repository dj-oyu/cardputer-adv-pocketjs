# Kasane カバレッジ・完全移行・将来設計 v0.1

> 2026-09-14 Astra評価: 実装順序・完了条件・surface寿命は
> [Astra実装計画](kasane-astra-plan.md)で改訂した。以後の実装ではそちらを優先し、
> 各ビルド・試験成功区切りでcommit・pushしてから次へ進む。

2026-09-14。対象は240×135、PSRAMなしのCardputer ADV。
[Kasane本体仕様](design-system.md)、[合成仕様](design-composition.md)、
[利用API](design-api.md)に対し、実装済み範囲とTaffy完全除去までの順序を定める。

## 1. 名前と到達点

新デザインシステムの名称は **Kasane（かさね）**、JS公開名は`pocket.kasane`とする。
矩形、画層、透明度、部品、modalを順に重ねて最終画素を作る性質から付けた。
`ksn_*`はC ABIの短い接頭辞として維持する。

最終到達点は、APPとSYSTEMの全画面をKasaneが所有し、旧PocketJS UI core、
RGB565 renderer、UI QuickJS adapterと、それらが取り込むTaffyをリンクしない構成である。
QuickJS VM、VM scheduler、`pocket.*`の時計・保存・通信等はUIとは別の責務であり、
移行中は残してよい。プロジェクト名からPocketJSを外す作業はUI除去後に行う。

推奨するプロジェクト表示名は **Cardputer Kasane Runtime**。`Kasane`は描画系、
`JS Runtime`はQuickJS実行系としてモジュール名を分ける。JSの`pocket.*`互換名は
リポジトリ名と同時に変更せず、別のAPI互換性判断として扱う。

## 2. 現在の機能カバレッジ

2026-09-23時点。checkpoint番号は[Astra実装計画](kasane-astra-plan.md)の表、数値と検証範囲は
[kasane-progress.md](kasane-progress.md)による。2026-09-14版の本節（gate 9/13、アプリ移植0/5、
Taffyリンク中）はgit履歴に残る。

### 2.1 実装済み

| 項目 | 状態 | 現在の契約 |
| --- | --- | --- |
| 固定容量core | 完了 | APP 80命令、SYSTEM 16命令、2 bank、1命令32 B、heap確保なし |
| 原子的REPLACE/PATCH | 完了 | submit、present、discard、cancel、世代付きticket。失敗時はbuilder全体をabort（CP0） |
| committed stateの修復 | 完了 | 転送失敗→cancel→JSなしで再描画（CP1） |
| 矩形・clip・重なり順 | 完了 | append順にsource-over。自動layoutや部品木なし |
| 透明度 | 完了 | RGBA、命令opacity、隔離group opacity。group内gradientの最終ディザ（CP2） |
| 角丸・枠線・gradient | 完了 | QuickJS公開済み（CP8） |
| 文字 | 完了 | jpfontのcoverage renderer、日本語/fallback、JS TEXTとsetText/setReveal（CP9–10） |
| 画像 | 完了 | crop/scale/span合成、PPT2 pet provider、JS image resource、伸縮・回転、native自動補間（CP12–13、17a/b） |
| 差分更新 | 完了 | 前後命令を比較し、240×8の17帯をdirty maskで再描画 |
| 軽量参照API | 完了 | `DrawRef`のsetRect/setClip/setColor/setVisible。公開参照は32件上限 |
| 明示cache | 完了 | 48命令、8 template、16 instance。lazy確保（CP3） |
| modal | 完了 | 1個、入れ子なし、solid/dim-live、表示確定と入力scope/focusを同時commit |
| QuickJS API | 完了 | `pocket.kasane`。同期build callback、thenable拒否、opaque wrapper、poll/cancel |
| host所有・APP lease | 完了 | guest破棄後もSYSTEMが継続（CP4）、input service分離（CP5）、guest直接dispatch（CP6） |
| SYSTEM layer | 完了 | 電源・時計・通知・相対timer・録音表示・壁時計/鳴動をSystem ownerへ移管（CP14a–14e2）。Taffyなしフル受入試験FULL_PASS（CP14f） |
| アプリ移植 | 完了 | hello（CP11）、imucal・bridge・companion・pet（CP19–22）、TUTORIAL/Playground（CP23） |
| 旧UIとTaffyの除去 | 完了 | 旧`ui.*`は起動前に`APP_LEGACY_UI`で拒否（CP24）、出荷物から削除（CP25） |

### 2.2 一部実装

| 項目 | 現在あるもの | 足りないもの |
| --- | --- | --- |
| 入力routing | `app/modal/blocked`を到着時に判定し、修復中を遮断 | scope別購読とfocusの原子的確定、modal Back配送（CP15） |
| textfield | `pocket.input.text`の編集欄をKasaneの帯へ合成 | SYSTEMでのIME表示（preedit/caret、優先quota、CP16）。編集欄の実機表示は未確認 |
| animation | 画像のnative自動補間と回転座標の加算化（CP17a/b） | 汎用animation track、JS APIとnative wake統合、reduce-motion（CP17–18） |
| すりガラス | 2,048 B縮小snapshot、radius 1/2 blur、PIE span kernel | 明示capture（F0）、frost modalへのattach（F1） |

### 2.3 未実装

- replay可能なnative scene source（CP26）
- home/shell/status/menuの移植（CP27）
- deskclock/player overlayの移植（CP28）。overlay sessionは`pocket.kasane`を注入しない
  （`app_session.c`がoverlayでKasaneのinstallを飛ばす）ため、native側の変更が先に要る。
  経緯は`apps/deskclock/README.md`と`apps/player/README.md`
- picker/Wi-Fi/editor/tutorial chrome/consoleの移植（CP29）、全LCD書込み経路の所有権監査（CP30）
- transform、blur一般化、projective quad、3D renderer
- pixel stream、frame mailbox、buffer lease、動画resource
- schema loaderと薄いJS component library
- PocketJS名称の整理（M5）

JSアプリの描画はすべてKasaneへ移った。残りはnative画面（home、overlay、picker、editor等）の
所有権で、Astra計画の三段階では「出荷Taffy-free」まで到達し、「全UIのKasane所有」が未達である。

実機で未確認のもの（CP24–25の記録）: TUTORIAL/Playgroundの実機操作、`pocket.input.text`編集欄の
実機表示、pet/companionの実機起動。いずれも保存データやNVSを書き換えるため試験から外した。

## 3. Taffyの除去（完了）

2026-09-17のCP24–25で、旧`pocketjs-core`（Taffy 0.11を直接依存に持つRust UI core）の
prebuilt archive 2本と、`pocketjs_ui_qjs`、`pocket_ui.c`、`render_accel.c`をファームから外した。
image 2,211,968→1,923,812 B（実測）。`pocketjs_guest`はQuickJS所有・job drain・watchdog・
VM schedulerの実装でTaffyには依存しないため残り、`js_runtime`等への改名はM5で扱う。
除去前の依存図はgit履歴にある。

## 4. 完全移行計画

### M0: Kasane縦断経路を固定する（完了）

- `pocket.kasane`、lazy native arena、display owner切替を導入する。
- hostの実QuickJS契約テストとUSB診断`K`を回帰テストにする。
- 完了条件: ASan/UBSan、ESP-IDF build、300ターン実機診断が通る。

### M1: UI構築に必要な描画面を閉じる

1. TEXTを`jpfont`のspan rendererへ接続する。文字列はbankの896/128 Bへコピーし、
   glyph cacheやfont atlasを部品ごとに持たない。
2. IMAGEを`ksn_image_port`へ接続する。pet assetは64×64 RGB565+A8のproviderへ変換する。
3. round-rect、1/2 px stroke、2色gradientをQuickJSへ公開する。C endpointとrendererは実装済み。
   これらは既存画面の同等表示とデザインschemaに必要だが、layout engineは追加しない。
4. SYSTEM endpointに通知、時計、電池、host textfieldを載せる。APPより後に合成する。

完了条件は、旧rendererを一切呼ばないKasane-only診断で文字、画像、日本語、textfield、
SYSTEM通知を表示し、静的DIRAM、最大連続空き、dirty帯数を記録すること。

### M2: JS turnを旧UIから切り離す

- foreground ownerを`pocketjs_ui_turn/continue`から`pocketjs_guest_frame/continue`へ変更する。
- Kasaneのschedule/presentをguest job drainとは独立に呼ぶ。native animationや動画だけが
  更新されたフレームではJSの`frame()`を起こさない。
- `pocketjs_ui_qjs`、旧font mount、旧node guardを無効にした`CONFIG_KASANE_ONLY`を作り、
  この時点でTaffy非リンク構成を継続ビルドする。

完了条件はmapとlink commandに`pocketjs_ui_qjs`、`pocketjs_ui_core`、Taffyのsymbol/archiveが
なく、VMのyield/continuation/leave保存テストが従来どおり通ること。

2026-09-14時点では、`main/CMakeLists.txt`の旧UI依存を`LEGACY_UI_COMPONENTS`へ分離したが、
無効化するbuild optionはまだ追加しない。`app_session.c`が起動ごとに`pocketjs_ui_core`と
`pocketjs_ui_qjs`を生成・mountし、通常turnとcontinuationをそれぞれ
`pocketjs_ui_turn`/`pocketjs_ui_turn_continue`へ渡しているためである。さらにhello、imucal、
bridge、companion、petの5アプリに旧node APIが計32呼出し残る。Astra側ではguest単独の
frame/job-drain境界を先に定め、この32呼出しを移植してから`LEGACY_UI_COMPONENTS`を条件付きで
空にし、link mapで旧3 componentとTaffy archiveの不在を確認する必要がある。

### M3: アプリを小さい順に移植する

順序は`hello` → `imucal` → `bridge` → `companion` → `pet`とする。
先頭3本で文字・矩形・動的更新、companionで時計とpet画像、petで画像animation・保存・
textfieldまで覆う。各アプリは座標をJSの薄いcomponent関数で解決し、native node treeを作らない。

各移植の完了条件:

- 旧`ui.createNode/setProp/insertBefore`が0件
- 起動、入力、Back保存、100回session再起動が成功
- 表示の目視またはpre-SPI pixel比較
- JS heap、native current/peak、最大連続空きを移植前後で記録
- 平常PATCHのdirty帯数と30 fps deadline missを記録

### M4: compatibility層とTaffyを削除する

- `pocket_ui.c`の旧global `ui` bridge、`jsfont`、旧pet overlayを削除する。
- CMake/idf manifestから`pocketjs_ui_qjs`、`pocketjs_ui_core`、
  `pocketjs_render_rgb565`と二つのprebuilt archiveを外す。
- 旧renderer専用`render_accel`を削除し、使えるPIE kernelはKasane側の名前と契約へ移す。
- `tools/uibudget`は履歴資料へ移し、出荷依存の検査対象から外す。

完了条件はclean checkoutで依存準備からbuildでき、`rg`とmap/nmの両方で出荷物にTaffyがなく、
5アプリとhome/overlay/通知の実機smokeが通ること。

2026-09-17: 実施（branch `kasane/remove-taffy`、CP24〜25）。PIE kernelは移さず`render_accel.c`ごと削除した
（Kasaneは`ksn_blend_pie.c`を持つ）。記録は[kasane-progress.md](kasane-progress.md)の checkpoint 24–25。

### M5: PocketJS名称を整理する

UI除去後にprojectを`cardputer_kasane`、VM componentを`js_runtime`へ段階的に改名する。
C ABIは一度に全面置換せず、薄いrename wrapperを置いてからcall siteを移す。
`pocket.*` JS namespaceはアプリ互換性のあるsystem API名なので、名称変更する場合は
API versionを上げ、alias期間を設ける。Taffy除去の完了条件には含めない。

## 5. 将来のresource/surface設計

### 5.1 2種類の描画入力

Kasane commandは最終的に次の2種類を参照する。

1. **Replayable resource**: font、pet、静止画、file-backed MJPEG frame。任意の帯を
   同じ内容で再読出しできるpull型。現在の`ksn_image_port.read_span`を発展させる。
2. **Live surface**: camera、video decoder、3D renderer。producerが最新frameをpublishし、
   compositorが短時間leaseして帯を読む。queueは深さ1のlatest-winsとする。

JSへpixel pointerや書込み可能なArrayBufferは渡さない。JSはresource/surfaceのopaque handle、
bounds、crop、opacity、transformと再生状態だけを操作する。buffer leaseはnative provider APIに限る。

```mermaid
flowchart LR
  JS[JS domain state] -->|opaque handle / placement| Cmd[Kasane command bank]
  Media[media service] --> Mail[latest-frame mailbox depth 1]
  ThreeD[3D service] --> Mail
  Asset[font / image / file] --> Res[replayable resource]
  Cmd --> Comp[strip compositor]
  Mail --> Comp
  Res --> Comp
  Comp --> Loan[shared RGB565 output strip]
  Loan --> LCD[LCD transfer]
```

### 5.2 frame mailboxとbuffer lease

frameは`FREE → PRODUCER → READY → COMPOSITOR → FREE`の所有状態を持つ。
handleはresource ID、generation、slotを含み、古いreleaseや二重submitをSTALEにする。
producerは`READY`が残る場合に新frameを上書きせず、旧READYを明示dropしてから最新をpublishする。
compositor取得後はLCD成功、discard、session resetまで内容を不変にする。

```mermaid
stateDiagram-v2
  [*] --> FREE
  FREE --> PRODUCER: acquire
  PRODUCER --> READY: publish pts + damage
  PRODUCER --> FREE: abort
  READY --> COMPOSITOR: acquire latest
  READY --> FREE: drop superseded
  COMPOSITOR --> FREE: presented / discard
```

初期native contract案:

```c
typedef struct {
    uint32_t generation;
    uint64_t pts_us;
    ksn_rect damage;
    uint16_t width, height, stride;
    uint8_t format, flags; /* RGB565 first; OPAQUE, REPLAYABLE, SEQUENTIAL */
} ks_frame_desc;

typedef struct {
    ksn_result (*acquire_latest)(void *ctx, ks_frame_desc *out);
    ksn_result (*read_strip)(void *ctx, uint32_t generation,
                            uint16_t y, uint16_t rows,
                            uint16_t *rgb565, uint8_t *alpha);
    ksn_result (*rewind)(void *ctx, uint32_t generation);
    void (*release)(void *ctx, uint32_t generation, bool presented);
} ks_surface_port;
```

`read_strip`には既存の240×8出力帯を貸し出せる。surfaceが最背面、RGB565、不透明、等倍、
full-widthならproviderがその帯へ直接decodeし、後続UIを同じ帯へ重ねる。透明、crop、変形、
順序入替が必要な場合だけ固定のsource tileを使う。貸出pointerはcallback終了まで有効で、
保持、別task利用、JS公開を禁止する。

LCD途中失敗では通常resourceは同じgenerationを`rewind`し、全dirty帯を再生成する。
inter-frame codec等で再現できないlive frameはその提出をdiscardし、次のreplay可能frameまたは
keyframeでfull redrawして初めて入力を復帰する。再現不能frameを論理baselineとしてcommitしない。

### 5.3 動画

初期対応はfile-backed MJPEGまたは連番RGB565のように、frame境界と再読出し位置が明確な形式を
優先する。media serviceがPTSとfile offsetを所有し、`DIRTY_MEDIA`と次PTS deadlineだけをownerへ渡す。
JS timerや`frame()`を30回/秒起こさない。遅れたframeはlatest-winsでdropし、音声clockがある場合は
音声をmasterとする。H.264等のinter-frame codecはdecoder stateと再送契約のRAMを測ってから追加する。

### 5.4 3D投影とeffect

2段階に分ける。

- **2.5D**: 不変cache/surfaceへ固定小数点3×3 homographyを適用するprojective quad。
  nearest samplingをMUST、bilinearとPIE化をBETTERとする。z-bufferは不要。
- **full 3D**: 3D serviceがband/tile単位でRGB565 surfaceを生成し、Kasaneは最終2D合成だけを担う。
  depthも240×8等の固定帯に限定し、mesh、texture、triangle binには明示上限を設ける。

共通effect順は`source → filter → transform/project → clip → opacity → source-over`。
filter最大1、transform/project最大1から始め、循環参照、feedback、任意shader、暗黙full-frame bufferを
禁止する。すりガラスだけは明示captureをsourceとし、現在の2,048 B snapshotを再利用する。

### 5.5 owner scheduling

ownerはsystem runtimeのpoll + dirty maskに統合する。

| dirty bit | 起床理由 | JSを起こすか |
| --- | --- | --- |
| `DIRTY_APP_COMMANDS` | JSがsubmit | すでにJS turn内。追加起床なし |
| `DIRTY_SYSTEM` | 時計、電池、通知 | 原則native SYSTEM rebuildのみ |
| `DIRTY_MEDIA` | 新video frameのPTS | 起こさない。native surfaceをpresent |
| `DIRTY_ANIMATION` | native track deadline | 起こさない。該当commandだけ評価 |
| `DIRTY_REPAIR` | LCD途中失敗 | JSを止め、full repairを優先 |

次deadlineがなくdirty maskも0ならKasaneの通知main loopは動かさない。この原則により、
動画・animationを追加してもJS callback、Promise、仮想木diffをフレームごとに生成しない。

## 6. メモリ方針

現在の実測14,440 Bはcore、cache、coordinator、参照2世代分を含む。
既存LCD帯3,840 Bは共有し、Kasane専用full framebuffer 64,800 Bは追加しない。
将来機能の初期予算は次のように個別opt-inで計上する。

| 機能 | 初期追加予算 | 方針 |
| --- | ---: | --- |
| frost snapshot | 2,048 B | 現在の固定値 |
| RGB565 source tile | 960–3,840 B | 2–8行。必要なsurfaceだけ |
| alpha tile | 480–1,920 B | RGB565+A8時だけ |
| 3D depth tile | 480–3,840 B | 8/16 bit、帯高に連動 |
| mailbox/lease metadata | 256 B以下/active surface | pointerではなく世代handle中心 |

これらは同時に常駐させず、feature取得は0 B、resource登録または再生開始時に予約する。
失敗はOUT_OF_MEMORY/BUSYとして旧表示を維持し、fallbackを暗黙に選ばない。
