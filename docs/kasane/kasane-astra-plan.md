# Kasane Astra実装計画

2026-09-14。Astraが`88afb1c`の実コードと仕様を評価した結果。
この文書をロードマップの実装順序・完了条件の改訂として適用する。
評価はコードレビューであり、AstraによるESP-IDFビルドの再実行はしていない。
Solのhost契約試験は成功済みだが、改名・プリミティブ追加後のESP-IDFビルドは未確認。

## 必須の進め方

- 各チェックポイントは独立してビルド可能な変更にする。
- ESP32-S3・PSRAMなしのESP-IDFビルドと必要なhost試験が通った時点で、必ず
  `vm/design-contracts`へcommitし、`origin/vm/design-contracts`へpushする。
  push成功を確認してから次のチェックポイントへ進む。複数機能の未コミット差分を積み上げない。
- 実機確認が必要な区切りは、その結果も記録する。ビルド成功と実機確認済みを区別する。
  実機不具合の修正もビルド・試験後に別commit・pushする。
- 資料のみの変更はリンク・差分検査後にcommit・pushする。
- checkpoint 7以降は通常構成とKasane-only診断構成の両方をビルドする。
- 変更ごとに試験コマンド、結果、未確認事項、commitを記録する。復旧は直前のpush済み
  状態へのrevert commitを基本とし、force-pushしない。
- 無関係な既存変更や生成物を含めない。現在の`dependencies.lock`変更は対象外。

## 評価と先行修正

方向は妥当だが、現在の縦断経路には以下の欠落がある。機能追加より先に修正する。

| 優先度 | 問題 | 根拠と修正方針 |
| --- | --- | --- |
| P1 | 強制再描画が旧経路だけ | `app_session.c`の`app_force_redraw`は旧flagのみ。`pocket_kasane_present`はsubmissionなしでは描かない。picker終了や静止画面上のindicator更新、部分転送後cancelを、JS更新なしでも確定済み状態から修復する |
| P1 | JS失敗の原子性 | 引数検証の例外をcallbackがcatchすると部分更新をsubmitできる。ticket wrapperはsubmit後、template/instance wrapperはnative作成後に確保される。OOM時の公開順序と巻戻しを統一する |
| P1 | ownerがguestに従属 | coreはJS adapter内にありSYSTEMだけでは動かせない。hostへ所有権を移しAPP attach/detachとSYSTEM存続を分離する |
| P1 | 入力scope未配送 | `pocket_ui_pump`は全購読へ配送し、通常Backはmodalより前にapp終了扱い。到着時scopeとsession世代を保持し、focusと表示を同時確定する |
| P1 | TEXT/IMAGEの実装境界 | `jpfont_draw_clip`はRGB565上書きでalpha/group非対応。coverage portが必要。petのPPT2はRGBA nibble形式で、RGB565+A8への変換とframe中の選択固定が必要 |
| P2 | group gradientのdither消失 | `ksn_render.c`の`render_group`は最終量子化へ常にfalseを渡す。中間premultiplied色には適用せず、group最終出力の規則を決める |
| P2 | メモリ仕様との不一致 | 14,440 B単一callocと常設4 KiB cacheは、最大個別確保3,072 B・cache opt-in方針と不一致。分割予約・失敗回復・stackを含む計上を先に閉じる |

SYSTEMの文字領域128 Bへ、既存textfieldの256 B入力とpreeditを丸ごとコピーできない。
編集bufferはtext serviceに保持し、計測した可視UTF-8 runだけsnapshotする。通知等の優先枠も予約する。
JS座標は現実装が小数拒否、仕様が最近接丸め（tieは0から遠い方）であり、公開API追加時に揃える。

## 完了条件を三段階に分ける

### このセッションの到達点（2026-09-15ユーザー指定）

Taffyを依存から最終削除する直前まで進める。CP25の出荷依存削除は留保し、
CP6–24とCP26–30の呼出し経路を完成させる。旧レイアウトエンジンのflex/gridを
再実装する意味ではなく、座標・矩形ベースの合意済み設計で既存画面を表現できること。

- 表現: 文字・日本語・画像・角丸・線・gradient・clip・重なり・透過・modal・cacheと
  既存アプリのアニメーションをnative/JS両方から利用できる。capture/frostも結線する。
- 経路: JSアプリ、教材、probe、SYSTEM通知・textfield、native home/overlay/picker/editorを
  Kasaneへ接続する。旧保存プログラムは黙って壊さず明示的に扱う。
- メモリ: 基本/任意領域とstack、JS heap、SPI buffer、画像/文字scratchを同時ピークで集計。
  PSRAMなしで100回起動・終了、最大連続空き、Wi-Fi/audio併用、OOM回復を測定し、
  必要な最大個別確保を満たす余裕を実測で示す。基本arenaのサイズだけで安全とは判定しない。
- 最終確認: Kasane-only構成のclean build・link/map/nm検査と実機の全画面遷移を通す。
  出荷の旧依存削除を残しても、各機能の未確認を完了扱いにはしない。

1. **診断Taffy-free**: K診断と非UIサービスが旧core/archiveなしで動く。未移植アプリは明示的に利用不可。
2. **出荷Taffy-free**: アプリ・埋込み教材・probe・保存プログラムの扱いを解決し、clean buildから旧依存を除去。
3. **全UIのKasane所有**: native home、overlay、picker、editor等も含め、全描画をcommand/resourceで管理。

5アプリだけで構築系`createNode/setProp/insertBefore`は50 call site、全`ui.*`は60。
旧資料の32は行数ベースの過少計上。さらに`lessons.c`の埋込みプログラム、VM probe、
保存済みユーザープログラムを調査する。home/overlay/native画面はTaffy除去と全UI所有を分けて追跡する。

## 実装チェックポイント

2026-09-15進捗: CP0–2とCP3a/bのhost試験・S3ビルドを完了。
CP3は基本5ブロック/cache任意3ブロックとも個別3,072 B以下。実機のstack・断片化・
100回起動試験はシリアル使用可能後にまとめて実施する。次の実装はCP4。
CP4は4a（SYSTEMを保持するAPP終了機構）と4b（host領域所有、世代付きAPP lease、
JS/session接続）へ分割する。4aのみではAPP attach/detach完了とはしない。
CP4bのhost runtime/JS接続まで実装。host/QuickJS試験ではguest破棄後のSYSTEM継続を確認し、
CP5のinput service抽出、host試験、S3ビルドと実機hello/pet/K/text入力確認まで完了。
CP6の直接dispatch、host/VM corpus試験、通常/probeビルドと通常構成の実機確認まで完了。
probe実機確認後、次の実装対象はCP7。scope別購読とmodal Back配送はCP15で扱う。
数値と検証範囲は[kasane-progress.md](kasane-progress.md)の各checkpointを参照。

各行を1 commit以上とし、大きい場合は行内もビルド可能な単位へ分割する。
基本依存は直前の行。H=Kasane host契約（ASan/UBSan、O2）、Q=実QuickJS adapter試験、
V=VM corpusに加えて実session dispatchの順序試験。全コード行でESP-IDFビルド必須。

| 番号 | commitの範囲 | 追加検証・実機確認 |
| --- | --- | --- |
| 0 | JS検証/OOMの原子性修復 | Q: catch、throwing getter、各確保失敗、native quota回復。実機K |
| 1 | committed stateのinvalidate/repair | H/Q: 全帯で転送失敗→cancel→JSなし修復。実機picker終了・capture・recording |
| 2 | group gradient量子化規則 | H: 独立参照、混合child、透明画素、Bayer位相、PATCH/full一致。実機画素比較 |
| 3 | bounded予約・lazy cache | H/Q: 各予約失敗、reset、cache未使用。実機100回起動、最大連続空き、stack、Wi-Fi/audio併用 |
| 4 | host core/coordinator所有、APP attach/detach | H/Q: guestなしSYSTEM、pending中終了、stale endpoint。実機home/app往復 |
| 5 | input serviceを旧node APIから抽出 | listener例外、reset、held/repeat回帰。実機hello/pet/text入力 |
| 6 | guest frame/continueを直接dispatch | V: Back保存、job継続、eval/pump cleanup、watchdog。通常/probe build、両backend実機 |
| 7 | Kasane-only診断profile | root/main CMake、manifest、依存準備まで分離。link command/map/nm/component/archive検査。実機Kと非UIサービス |
| 8 | roundRect/strokeRect/gradientのJS API | Q: 丸め、clip、容量、取消、capability。実機gallery |
| 9 | native TEXT coverage renderer | H: 日本語/fallback、clip、alpha/group、strip境界、reveal。実機文字比較 |
| 10 | JS TEXT・setText/setReveal | Q: UTF-8、容量、stale、繰返し確保。実機PATCH/メモリ |
| 11 | hello移植・domain/poll/retry helper | 実app harness: BUSY、失敗REPLACE、入力。実機parity・100回再起動 |
| 12 | IMAGE crop/scaleとspan合成 | H: RGB565+A8、1x/2x/half、境界、provider失敗、retry。実機test pattern |
| 13 | PPT2 pet provider・JS resource | 全pet/mood、世代固定、reset、APP/SYSTEM所有。実機atlas・scratch peak |
| 14 | 通知・時計/電池・recordingをSYSTEMへ | H: APP BUSY、独立poll、表示除去、転送失敗。実機modal/通知/audio併用 |
| 15 | scope購読・focusの原子的確定 | Q/input: deferred/held/repeat漏れ、cancel/repair、通常Back/force-stop。実機modal操作 |
| 16 | textfield/IMEのSYSTEM表示 | 256 B入力、可視run/preedit/caret、優先quota。既存text試験、実機日本語編集 |
| 17 | native animation tracks | H fake clock: easing/loop/stop、damage、retry中sample固定、quota。実機deadline/RAM |
| 18 | animation APIとnative wake統合 | Q/V: JS frame追加なし、completion一度、reduce-motion。実機idle/wake計測 |
| 19 | imucal移植 | sensor/error/BUSY harness、実機校正・100回起動 |
| 20 | bridge移植 | 通信失敗/burst/遅延callback、実機PC連携・100回起動 |
| 21 | companion移植 | 既存試験、NVS/clock/timer失敗、実機全page/alarm・100回起動 |
| 22 | pet移植 | domain保存と表示失敗の分離、既存試験、実機Back保存・全flow・100回起動 |
| 23 | lesson/VM probeのUI移植 | 全埋込みJS評価、V、probe build、実機tutorial編集/実行 |
| 24 | 旧node API利用を廃止 | capability/API version、旧保存プログラムの明示拒否、サービス回帰 |
| 25 | 出荷構成から旧UI/Taffyを除去 | 旧checkout/Rust archiveなしclean準備・build、map/nm証明。全appと通知/audio/Wi-Fi実機 |
| 26 | replay可能なnative scene source | 時刻/seed固定、任意帯再読出し、retry。実機scene性能 |
| 27 | home/shell/status/menu移植 | 既存設定試験、全home modeとapp遷移 |
| 28 | deskclock/player overlay移植 | attach/detach、guest喪失、quota、音声実機 |
| 29 | picker/Wi-Fi/editor/tutorial chrome/consoleを各画面別commitで移植 | 各既存試験、可視行quota、全操作と復帰 |
| 30 | 全LCD書込み経路の所有権監査 | production pixelはKasane command/resource経由。全画面遷移・部分転送修復 |

17–18は低wake設計に必要だがTaffyの依存除去そのものの条件ではない。
初期移植で既存JS timingを維持する場合は25以降へ移動できる。
文字・画像・入力・SYSTEMを使う各アプリの移植では、JS/native peak、最大連続空き、
dirty帯数、30 Hz deadline missを移植前後で記録する。

## 後続機能

- F0: host所有/TEXT/IMAGE/SYSTEM後に明示capture。APPだけを固定世代から帯単位生成し、
  deadline/cancel/resetを定義。fake clockと実機2 KiB予算を検証してcommit・push。
- F1: captureをfrost modalへattach。表示中とpendingの参照を保持し、close成功後release。
  timeout/OOM/転送失敗と明示solid fallbackを試験してcommit・push。
- S0: retained surface lease。READYは深さ1でもPRODUCER/COMPOSITOR/active世代の同時所有を
  別途予算化。present後もactive commandが再描画できる世代を保持する。
- S1: 動画。I/O/decodeをrender禁止区間の外へ置き、PTS、seek/drop、破損入力、音声同期を検証。
- S2: projective quad。独立参照、特異行列、clip、共有辺、旧/新damageを検証。
- S3: 別予算のband/tile 3D producer。mesh/texture/bin/depth上限とreplay性を検証。

surfaceはSYSTEM更新・modal close・capture・repairでもactive frameを再生成できることが必須。
dropしたframeのdamageは最後に表示した世代との差へ累積する。単なる「present後release」は不可。
schema、cache拡張、プロジェクト改名はTaffy除去後に進められる。
