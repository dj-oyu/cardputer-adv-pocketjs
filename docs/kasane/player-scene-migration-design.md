# overlay 用 Kasane native presenter 設計と実装状況

2026-09-23 改訂。対象はまず MUSIC、再利用性の確認対象は DESKCLOCK。旧案の
「MUSIC の JS を `createScene({build,patch})` に移す」を撤回する。現行
`createScene` の controller は C だが、命令を作る build/patch は JS callback のままで、
「JS は表示値を渡し、Kasane が画面構成を解決する」という目的を満たさない。

COM3 は別プロセスが使用中。実装・host 検証でもポートの照会・接続・flash・実機計測を
しない。以下の性能判断は仮説であり、実機 gate は COM3 が空いてから行う。

## 実装状況（2026-09-23）

`ksn_presenter.c` に MUSIC/CLOCK の上限付き native 表示プラン生成、可視命令の
field 単位比較、APP REPLACE 提出を実装した。`pocket.kasane.mount(viewId)` と
`view.update(model)` は第一段階の互換経路。第二段階では `view.set`、登録済みsourceの
`bind`、native help状態、host status slotを追加した。latest/submitted/displayed と
ticket は分けて保持する。MUSIC と DESKCLOCK は JS 描画命令・表示modelを持たない。
QuickJS host と native 契約テスト、および ESP-IDF build は通過。実機の画素比較、
再生中の速度・メモリ計測、A/B 性能 gate は未実施。PATCH と descriptor 生成 host tool も
未実装であり、採用判断を「速くなった」と読み替えない。

## 第一段階の設計時点の現状と結論（履歴）

- MUSIC は `apps/player/player.js` で最大10個の矩形・文字命令を `list` に作り、
  dirty 時に `ui.replace()` 内で再度 `tx.rect/text` へ変換する。すでに Kasane の APP
  lease で表示しており、基盤の移植ではない。`pocket.overlay` は region と key の契約。
- DESKCLOCK は `createScene` を使用し、固定矩形と2文字を JS build/patch で扱う。
  どちらも Kasane core に音声・時計・入力の domain 判断を移す理由はない。
- Kasane が引き受けるべきは **表示定義から低レベル命令への展開、文字の寸法、
  条件に応じた表示構造、差分判定、提出・再試行**。アプリは domain state と表示する
  値（title/status/時刻など）を決める。任意の JS 関数を C で代行するのではなく、
  上限付きの宣言的部品だけを native presenter に追加する。
- 第一段階は native 側で現行と同数・同順の命令を dirty 時に REPLACE する。
  JS の命令組立てをなくすことと、PATCH を導入することを分離する。PATCH は
  `ksn_core` が REPLACE と同様に bank 全体をコピーするため、無条件には採用しない。
- native 背景は独立に動き、overlay 中は `overlay_kasane_present()` が毎フレーム
  invalidate する。JS 命令生成をなくしても背景合成・Kasane raster・LCD 転送は残る。
  よって JS 側の費用削減は見込めるが、総フレーム時間の改善は実測で判定する。

## 責務と API

```
MUSIC の音声・入力・status 文言 ──表示値──> pocket.kasane の薄い JS adapter
                                            ↓ 1回の update（値を直ちに C へコピー）
flash 上の検証済み表示定義 ──────────> ksn_presenter（C: 展開・差分・提出）
                                            ↓ 既存 ksn_view/ksn_core 命令
                                      ksn_render → overlay backdrop → LCD
```

提案するアプリ API は `pocket.kasane.mount(viewId)` と `view.update(model)`。
`mount` は owner の APP lease と現在の overlay viewport を束ねる。`update` は固定形の
表示値だけを読み、UTF-8・数値範囲・容量を検証して native snapshot へコピーする。
JS object/文字列への参照を持ち続けず、呼出し後に model を変更しても提出内容は変わらない。
JS 側で同じ model object を再利用でき、新規 object 確保は API の要件にしない。
公開形は `update(model)` に固定し、固定引数の別 API は計測で bridge が律速と
判明するまで増やさない。描画 callback と毎 turn の JS `flush`
は不要とする。現在は `pocket_kasane_pump()` という hook は存在しないので、
`app_tick()` の既存 present/input gate と順序を合わせた native presenter step を
新設する。top-level 評価中の初回 update は即時提出できる。overlay では
`overlay_tick()` の既存 present gate を通った後、`app_overlay_tick()` の冒頭で
presenter step を実行し、そこで新規提出したなら guest frame を回さず戻る。
通常アプリへ広げる場合も `app_tick()` の present gate と同じ規則で結線する。
step の所要時間は overlay の guest 側予算へ含め、初回評価時の提出も別記録する。
`update` は top-level 評価中・通常 frame 中・Promise の continuation 中のどこからでも
呼べる。各経路の既存 `pocket_kasane_end_turn()` より前に即時提出するか最新値を予約し、
次の `app_overlay_tick()` 冒頭で確定結果を回収する。submitted/repair 中は新しい
APP transaction を開始しない。step の失敗は `app_overlay_tick()` のエラーとして
`overlay_tick()` へ伝え、FAULTED と診断ログへ結び付ける。presenter の固定容量は
`mount` 時に確保し、overlay 起動後の `OVERLAY_COST`／free floor 検査より後へ
初回確保を遅らせない。

MUSIC でアプリが渡す値の最小案は `title`、`status`、`positionMs`、`durationMs`
（null 可）、`playing`、`phase`、`help`。音声 handle、パス、権限状態は渡さない。
`status` の文言優先順位（message があれば優先、なければ状態・秒・GAPS）は引き続き
アプリの表示判断とする。Kasane はその文字列を測り、板と TEXT を生成する。
`phase` は既存の 15Hz light の位置値であり、時刻や timer を Kasane に持たせない。
DESKCLOCK は `face` と `tag` だけを渡す。これは同じ presenter が固定板＋文字と
可変 progress を扱えるかの再利用確認であり、MUSIC 固有 presenter を core に埋めない。

表示定義は `design-schema.md` の「制作時の検証・コンパイル、flash 定数、端末で
JSON/オブジェクト木を展開しない」方針に沿う。ただし既存 JSON Schema／コンパイラは
実行時 presenter として未実装なので、これは新規実装範囲である。まず小さい固定長
descriptor を C `const` として MUSIC/DESKCLOCK に用意し、同じ形式を生成する host
tool を後続に分ける。定義に音声・時刻の式、eval、watch、任意 callback は入れない。
登録済み viewId のみ mount でき、未登録／version 不一致は明示エラーにする。
任意のユーザー作成 JS アプリは従来の `replace/createScene` を使い続けられる。

`ksn_presenter` は `main/ui/kasane/` の独立モジュールとし、QuickJS・audio・shell
に依存させない。JS adapter は model を C の固定値へ変換するだけ。renderer や
`ksn_display_port`、APP/SYSTEM の layer 数は変更しない。overlay の region-local
座標を既存 adapter で viewport と交差させ、`pocket.overlay` の key listener、予約
Back、shell-owned picker・音量 HUD の優先順位も維持する。Kasane modal は overlay
profile で非対応のまま。MUSIC の help は APP 内の黒い板と文字の別画面である。

## 表示定義の最小語彙

| 部品 | 定義時に固定するもの | update で変わるもの | 命令 |
| --- | --- | --- | ---: |
| `rect` | bounds、色、重なり順 | なし | 1 |
| `label` | bounds、font、色、byte capacity | text | 1 |
| `plateLabel` | 起点、padding、板色、font、文字色 | text と板幅 | 0または2 |
| `meter` | track bounds、色、fill 色、未知長の3片 | 値／mode／phase | track 1＋0〜3 |
| `page` | 排他的な命令列 | active page | active 側のみ |

これは汎用レイアウト木ではない。親子再配置、CSS、動的ノード生成、alpha group、
offscreen buffer を持たない。descriptor は flash 常駐、session 中の native state は
最新 snapshot、latest plan、pending ticket に固定した submitted plan、表示済み plan、
必要な refs/提出状態だけに制限する。容量は
実装時に map と largest block で算出し、既存 Kasane bank を二重に確保しない。
全命令は既存 Kasane の制限（APP 80命令、文字予約896 B、TEXT 1命令128 B）内で
定義時と update 時の双方で検証する。文字切詰めは UTF-8 scalar 境界で行い、
不正値を音声失敗と取り違えない。

MUSIC の descriptor は通常画面の title/status/help hint の plateLabel、track、
既知長 fill または未知長 light、help 画面の黒 rect＋7 TEXT を表現する。
`durationMs` は現行と同じ truthy 判定で既知長を選ぶ。`drop()` 後も `total` が残る
現行意味論をアプリ側が渡す値で保持する。status が空なら板・文字を出さず、fill が
幅0なら命令を出さない。未知長の画面端では0〜3片を出し、毎回現行と同数・同順の
命令にする。初期実装は表示が変わったときの native REPLACE とし、非表示 refs を
常駐させて band ごとの走査を増やさない。

板幅は現在の `walk()` と実際の caption advance が異なる。移行の速度比較に画素変更を
混ぜないため、まず現行の板幅・clip・色・命令順を host で再現する。UTF-16 surrogate
分断と非 ASCII 幅の修正は別の表示変更として行い、旧経路にも同じ修正を適用してから
性能比較する。最終的には font port の advance を単一の真実とし、JS には幅計算を
残さない。region を超える描画は既存 viewport で切る。

## native 更新状態機械

1. `update(model)` は全 field を検証・正規化してから最新 snapshot を原子的に置換する。
   表示に効く値が同じなら transaction を作らない。比較対象は生の `positionMs` など
   ではなく、導出した文字列・矩形・page の表示 plan とする。各 field は長さ付きで
   byte 比較し、padding を含む構造体 `memcmp` や衝突し得る hash だけで判定しない。
2. pending がなければ、選択 page と表示値から現行と同じ低レベル命令を C で展開し、
   不透明な論理 APP background を設定してから APP REPLACE を提出する。overlay host
   はこの fill を現在の native backdrop に差し替える。native submit 成功時は現行
   `run_build` と同様に `state->submitted`、`state->active`、APP lease activation を
   更新する。これを欠くと `overlay_kasane_active()` が false のまま描画・予算計上を
   すり抜ける。提出時の plan は ticket と共に不変の submitted plan として保存する。
   pending 中の update は latest plan だけを更新し、
   同時に二つの APP transaction を始めない。guest の domain state は戻さない。
3. PRESENTED ではその ticket の submitted plan だけを displayed plan へ昇格させ、
   latest plan との差が残れば次を提出する。DISCARDED は submitted plan を捨て、
   latest plan で再構築する。BUSY は次の owner turn で再試行する。
   A 提出→pending 中に B、A→B→A、DISCARDED 後の C を host で検査する。
   失敗中の古い表示を途中で壊さない。validation、
   quota、OOM など BUSY 以外は握りつぶさず app session へ報告し、可観測なエラーにする。
4. overlay 終了時は pending・refs・snapshot を native 側から破棄し、guest が死んだ
   後に JSValue を参照しない。APP の owner は presenter 一つに限定し、同一 lease へ
   直接 `replace/patch` や JS scene を混ぜる場合は BUSY／明示エラーにする。

PATCH は第二段階の候補に留める。安定した命令構造で文字・fill の変更だけを更新でき、
実測で native 展開・band 走査・メモリが悪化しない場合に限り使う。命令数と順序が
変わる help、status 有無、未知長の片数では REPLACE を維持してよい。`setRect` は
初期 clip を広げないため、PATCH の可変 refs は明示 viewport clip を持たせる。
refs は PRESENTED 後だけ昇格し、patch 失敗・DISCARDED から再構築できることを
host で証明する。PATCH を使わなくても native presenter の目的は達成できる。

## PIE と描画性能

PIE は renderer の実装詳細で、descriptor/API に 8 pixel 幅や 16-byte alignment を
露出しない。現行 renderer には aligned な RECT fill と単色 blend の PIE 経路がある。
MUSIC の黒い板と進捗は不透明 RECT、文字は glyph coverage 付き TEXT である。
後者は既存の「一定 alpha の8画素 blend」へそのまま渡せず、文字 PIE 化を前提にしない。
細い2px bar／18px light を lane 幅へ丸めると画素が変わるので、可視 bounds は維持し、
renderer 内で alignment check と scalar head/tail を行う。命令分割や見えない常駐
命令を増やさず、現在の 8-row strip／最終7-row と backdrop 合成を維持する。

scene 移行の効果と PIE の効果は別実験にする。先に guest、bridge＋presenter、
core submit、backdrop、renderer の fill/span/blend/read、HUD、LCD send を分けて測る。
PIE 新 kernel は hotspot がある場合だけ別変更で検討し、scalar/PIE の同一バイナリ
runtime switch、host の完全画素一致、16-byte alignment と guard 検査、PIE model／
asm／schedule 検証を必須にする。既存 PIE blend の他画面での 1.09ms/painted-frame
という値は MUSIC に外挿しない。

## 実装順と gate

1. 端末に触れず、native descriptor と状態機械の設計・host model test を作る。
   MUSIC の全状態、既知/未知長、片数0〜3、help、長い和文・絵文字、picker復帰、
   BUSY/DISCARDED、短い→長い文字、増える fill、移動 light を画素と命令で比較する。
   native memory の静的増分・guest source bytes・最大連続確保を別々に集計する。
2. COM3 が空いてから現行再生中 baseline を採る。CP28 の無再生ログ（MUSIC free
   107,720 B、largest 53,248 B、JS 104,864 B、worst 11,791µs／12,000µs、
   安定窓29.7〜29.9fps）は再生中の比較値として使わない。
3. 診断バイナリに旧 JS 経路と native presenter 経路を両方 flash 埋め込みし、
   一度に一方だけ guest に評価する。MUSIC は同じSD・曲・背景・音量で既知/未知長を
   各2分以上、A-B-B-A のセッション順に測る。軽い背景と FLOWER を分ける。
   別ビルドの差は code placement／i-cache の変動が最大約15%なので効果と呼ばない。
4. `PERF` の2秒平均だけでなく、状態別の per-frame p50/p95/p99/max、dirty/clean、
   初回・help・pause・曲変更・picker復帰、12ms の生の超過数／比率／最大連続数、
   share免除後の連続数、fps、音声 underrun、free/largest/JS を同じ診断コードで採る。
   per-frame log や heap 確保を計器に入れない。caption 幅の表示変更、計器 overhead、
   guest route、renderer PIE switch を混ぜない。
5. native presenter の guest 時間と source/heap が改善しても、総 draw p95/p99、
   遷移最大、予算超過率、音声、最大連続確保、表示が再現性を持って悪化するなら
   採用しない。差がノイズ床以下なら「性能向上」は主張せず、JS 薄化の保守性と
   flash/DRAM 追加量を別に判断する。PATCH は REPLACE 版を基準に同じ gate を再実施。
6. DESKCLOCK に同じ presenter を適用可能か host で確認し、MUSIC 専用の隠れた
   仕様を洗い出す。実機性能 gate を通るまでは低レベル `ui.replace`／`createScene`
   API を互換経路として残す。

既存の Astra レビューは旧 `createScene` 案に対するもの。そこで見つかった可変 refs
の clip、状態別 p95、12ms 超過の生値、`durationMs` truthy 意味論は本案にも反映した。
本改訂の native presenter は host 検証済み・実機未計測であり、旧案のレビュー合格を
本案の実機性能合格と読み替えない。

## 第二段階: UI状態をJSから外す（host実装、実機未確認）

第一段階の `update(model)` は命令生成だけをnative化した。第二段階ではMUSICのJSから
`dirty`、`pos`、`tick`、`help` と表示用 `model` を除き、DESKCLOCKの時刻formatも
nativeへ移した。目的は汎用差分木ではなく、**値の所有者が変わったらpresenterが
表示を再解決すること**である。JSは再生操作・ファイル選択などのdomain判断を
行うが、画面の現在値を丸ごと複製・同期しない。

| 所有者 | 保持・更新するもの | presenterへの入口 |
| --- | --- | --- |
| アプリJS | 選曲、再生操作、アプリ固有の短い案内・エラー | `view.set({title?,message?})` |
| 音声サービス | 再生状態、位置、長さ、underrun | 現在のplayer IDに結び付く読取専用snapshot |
| 時刻サービス | UTC時刻、同期元 | 登録済み`wallClock` snapshot |
| presenter | help表示、案内の期限、未知長light位相、レイアウト、可視plan | boundedな内部状態 |
| シェル | 音量・FPS HUD、予約キー、picker、同意モーダル | 既存の最終合成／入力優先経路 |

実装APIは `view.bind('playback')`、`view.bind('wallClock')`、
`view.set({title?,message?})`、`view.toggleHelp()`、`view.dismissHelp()`。
音声は現時点で一度に1 playerなので、bind時の現在IDを記録し、close／再選曲で
旧IDを無効にする。再選曲後は新しいopen結果で再bindする。bindはJS callbackやJS objectを
presenterに保持しない。音声や時刻の意味論は各サービスに残し、Kasaneは
登録済みの小さな値型をレイアウトへ投影する。任意のJS式・監視グラフ・DOM風treeは作らない。

システムAPIが表示を一時的に上書きする場合、APP planを直接書き換えない。
`status`のように共有する箇所は所有者付きslotと固定の優先順位を定義する。
たとえば `system transient > app message > playback summary` とし、systemの期限が
切れたら、その間に更新されたapp／音声の**最新値**を再表示する。音量・FPSは
現行どおりAPP後のshell HUD、同意モーダルはさらに上で入力も止める。
どのsystem APIも任意のAPP nodeを黙って変更できない。この優先規則はviewごとに
明記し、権限境界を描画順の偶然に任せない。

owner turnで有効なsnapshotを読み、その時点の全slotから可視planを作る。
pending中は最新の源を合流し、PRESENTED後に再解決する。clockの分変更と
未知長lightの位相はnative owner turnで解決し、JSの毎frame pollや`dirty`管理を
不要にした。専用deadlineは追加せず、固定数の源をowner turnで確認する。
容量は固定し、frameごとのheap確保、新しい全画面buffer、汎用自動PATCHは導入しない。
現在はplan全体の完全一致比較ではなく、表示前の可視keyで無変更を抑制する。

`copy_text` はcoreと同じ制御文字・UTF-8 scalar検証へ寄せ、入力変換前の上限を
256 UTF-16 code unitsにした。`stats().nativeBytes`はpresenter確保分を計上する。
host試験は「JSを一度も呼ばずに音声／時刻が変わる」「host上書き中のapp更新と期限切れ」
「静止時に余計な提出がない」を含む。system status slotはhost専用C APIとして用意したが、
現行の音量/FPSは引き続きshellの最終合成を使い、このslotのproduction callerはまだ無い。
handle失効・再bind・pending中の連続更新を追加で検査する。
実機性能とメモリのgateはCOM3が空いてから行う。

## 所有権とコピー境界

表示の正本は、アプリ固有の短い文字列ならpresenterのapp slot、音声・時計の値なら
各サービスに一つずつ置く。presenterはサービスのsnapshotを保持しない。owner turn
中だけ読み、pendingの間に値が変わっても履歴を複製せず、表示完了後に正本を再読する。
以前の `latest / submitted / displayed` の3 plan保持と、その後の1 plan保持を廃止し、
現在は最後に提出した可視keyだけを保存する。DISCARDEDならkeyを無効化し、次の
owner turnで再構築する。

QuickJS文字列をJS objectの寿命から独立させる境界では、上限付きのapp slotへ一度
コピーする。音声と時計はその場で導出する小さな値型のsnapshotを一度受ける。
描画の非同期境界では、coreの候補バンクが文字と命令を所有する。ここは提出後に
producerが更新・解放されても同じ画面を描けるために必要なコピーであり、借用
ポインタをそのまま保持しない。REPLACE開始時のバンク継承は、直後に消去する
対象layerをコピーせず、残すlayerだけに限定した。

可視keyは文字列の内容が実際に変わった時だけ進むslot revision、表示中のstatus所有者、
音声summaryの秒・状態・underrun、既知長バーの画素幅、未知長ライトのクリップ後の
片位置、help、時計の表示分と同期タグから作る。同じkeyならplan生成とsubmitを
省略する。help中の隠れたapp更新やsubpixel進捗はkeyを変えず、help終了・host期限切れ
では最新の正本で再計算する。keyはhashではなく固定整数列の厳密比較であり、
異なるkeyが同じ画面を表す余分な提出はあり得るが、同じkeyで異なる画面を
見落とさないようにする。手動互換 `update(model)` は値型を比較してrevisionを進める
保守的な経路なので、不可視フィールドの変更で余分な提出が起き得る。

これでも経路全体の「最大1コピー」達成を意味しない。planは提出時だけ短命に作るが、
文字列の整形・一時値と、非同期レンダリングに必要なcore bankの所有コピーは残る。

将来、読み取り専用のアクターが**借用期間中の書込み禁止と生存**を型・owner turn・
世代で保証できる値は、短命な `const` 参照で0-copy購読してよい。ただし現行playerの
位置とunderrun、clockの現在時刻は進行中の値から導出されるため、永続的な不変
オブジェクトへのポインタとは見なさない。JS string、close可能なplayer、転送中の
候補バンクをまたぐ生ポインタ購読も禁止する。0-copyを名乗るために安全な所有権を
犠牲にしない。
