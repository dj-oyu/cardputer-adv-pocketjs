# Kasane 汎用 presenter v1 — アプリ詳細を持たない表示契約

2026-09-23。Astra の敵対的レビューを反映した実装仕様。従来の `kind` 列挙、
`make_player` / `make_pet`、`bind('playback')` / `bind('wallClock')`、help、status の
優先順位、ペット画像の制約は Kasane の責務ではない。別ファイルへ移すだけでは
汎用化したことにならない。新しいアプリは Kasane の C ソースを変更せずに画面を
定義できなければならない。

2026-09-24追記：下の「未実装」は当初の段階記録である。汎用source/bindと
dirty-nodeは後続ロードマップで実装を進めた。数値source用の`u32`は
データslotとして追加し、`u16`座標・page・revealへ暗黙変換しない。
JS descriptorは`version:1`を維持し、native C schema/source ABIは
slot構造と型集合の変更に合わせてv2へ上げた。

## 実装段階

汎用の不変 C descriptor、型付き slot、条件付き node、文字幅連動 plate、
借用値からの REPLACE 提出と、core の確定 bank を直接読んで完全一致比較する
PATCH/REPLACE 判定は `ksn_schema.h/.c` に実装した。PATCH は変わった property
だけ提出し、表示値を二重に保持しない。静的 descriptor をアプリ所有 asset として
解決する入口と汎用 `set` を Hello・imucal・bridge・companion・pet に接続した。
deskclock はアプリ所有の native source が型付き値を供給する。任意の JS 定義からの
`mount(definition, initial)` も同じ C descriptor と update 経路へコンパイルできる。
slot→node 依存表と、値を保持せず revision/viewport で clean turn を返す
`ksn_schema_session` も実装した。旧描画器は Kasane core から app 互換層へ隔離した。
player はまだ旧 presenter 経路を使い、native source の汎用化と依存表を使った
dirty-node 限定処理は未実装である。これらと host parity、実機 A–B gate は残る。

## 公開インターフェース

```js
const view = pocket.kasane.mount({
  version: 1,
  slots: {
    caption: {type: 'text', capacity: 47},
    fill: {type: 'rect'},
    showing: {type: 'bool'}
  },
  nodes: [
    {type: 'text', bounds: [8, 8, 220, 24],
     text: {slot: 'caption'}, color: 0xe2f0ffff},
    {type: 'rect', bounds: {slot: 'fill'},
     visible: {slot: 'showing'}, color: 0x78c8ffff}
  ]
}, {caption: '', fill: [12, 109, 12, 111], showing: false});
view.set({caption: '任意のアプリの値'});
```

`backgroundSlot` には color slot 名を指定できる。node の `rectAdd` は
`[x0,y0,x1,y1]` 各辺に加算する u16 slot 名または `null` の4要素で、固定の
図形構成だけで可変長のバーを描ける。font は `0=caption, 1=body, 2=display`、
image は `resource`・`variant`・`frame`・`sourceWidth`・`sourceHeight` を指定する。
page は u16 slot と `pageEquals`、visible は bool slot、reveal は u16 slot とする。
現行の mounted view はアプリ終了時に解放する（個別 `dispose()` は未提供）。
組込み画像は `pocket.kasane.resource('pets')` のようにアプリ側 registry の名前から
取得する。Kasane の描画コアはその名前や画像の内容を知らない。

slot 名はアプリが自由に決め、mount 時に型検証して数値 ID へ解決する。定義は不変で、
元の JS オブジェクトを後から変更しても表示に影響しない。実行時には JS の定義木を
保持しない。組込み画面は同じ C descriptor 形式を flash の不変 asset として借用し、
ユーザー定義画面は mount 時に上限付き領域へ一度だけコンパイルする。両者の実行経路は
共通にする。`createScene` / `replace` は低レベル escape hatch として残す。

定義の語彙は rect、roundRect、text、image、可視条件、排他的 page、文字幅に連動する
plate に絞る。座標・色・文字・image variant/frame/reveal はリテラルまたは型付き
slot 参照にできる。音楽・時計・ペットという演算子、任意の JS callback、CSS、実行時
レイアウト木は導入しない。0 幅や非表示の命令は提出せず、帯走査にも載せない。
初期実装の `plateText` は旧画面との画素一致を優先して caption font のみ受け付ける。
BODY/DISPLAY の文字幅・高さを扱うまでは、誤った板幅で描画するより mount 時に拒否する。

## 値と native source

JS の `set(partial)` は全項目を検証してから原子的に反映する。未知名、型違い、UTF-8
不正、容量超過のいずれも一部反映しない。文字の保持は宣言 capacity 分だけとし、
数値 slot に 48 B の文字領域を割り当てない。submitted/displayed のために descriptor
や plan を複製せず、既存 Kasane の candidate/displayed bank と ticket を使う。
文字列は UTF-16 長で上限を先に検査し、巨大入力を UTF-8 に変換しない。画像は
resource の有効世代・variant/frame・source 寸法を core の読み取り専用検証で
`set()` の正本更新前に確認する。`nativeBytes` は mounted schema と runtime descriptor
の所有領域も加算する。

JS を毎 frame 呼ばずに音声・時刻を表示する用途は、Kasane 外の producer が世代付き
opaque handle と型付き値を owner turn 中だけ公開する。アプリは source の field と
slot の対応を指定する。producer が時刻整形、再生状態の文言、status の期限・優先順位を
所有し、Kasane は source 名、アプリ ID、domain の意味を知らない。安全な不変 snapshot
なら読取り専用ポインタで借用し、提出時の core bank 以外へコピーしない。

## 更新と性能の不変条件

1. slot→命令の依存 mask を mount 時に構築する。変更がない owner turn は key 比較だけで
   終え、定義全体を走査しない。dirty 命令だけを解決し、解決後の可視値を正確に比較する。
   hash／revision だけでは描画省略を決めない。
2. 命令数・順序・不変属性（font、clip、radius、image resource、text capacity）が同じ
   場合のみ PATCH。可視条件や page の切替で topology が変われば REPLACE。
   PATCH 時は変更された属性だけを提出し、現在の全項目 PATCH を踏襲しない。
3. pending 中の更新は latest 値へ合流し、並列に transaction を作らない。PRESENTED 後
   だけ refs を確定し、DISCARDED は最新値から再構築する。A 提出→B→A の往復も検査する。
4. APP lease、overlay viewport/backdrop、入力 gate、失敗時 repair は現行契約を維持する。
   PIE は renderer の実装詳細であり descriptor に lane 幅を漏らさない。
5. 既存上限 APP 80命令・文字896 Bを守る。descriptor/slot/ref/依存表/一時領域の
   peak と最大連続空きを別々に計測する。静的 presenter の現行容量（1,280 B以下）より
   大きくなる場合は理由と実機測定を要する。

同一バイナリ A–B–B–A の実機計測で guest/native 更新、描画 p95/p99/max、12 ms 超過、
dirty 帯、音声 underrun、free/largest、JS/native heap を比較する。画素一致と host の
ASan/UBSan、ESP-IDF build だけでは「描画性能を劣化させない」の証明にしない。

2026-09-23 のホスト確認: QuickJS 統合テスト（任意の第8アプリ、pet、画像の無効
variant/旧世代handle、長文拒否、失敗後の復帰、nativeBytes）と schema/session、
旧音楽 presenter、SYSTEM notice の単体テストが PASS。pet の mounted schema 用単一
確保はホスト64-bitで 1,280 B。ESP-IDF 6.0.1 の S3 build は PASS、DIRAM +0。
COM3 は利用しない指示のため開いておらず、実機 A–B 性能 gate は未実施。
