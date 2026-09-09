# Pocket共通JS API仕様案 v0.1

作成: 2026-09-06。状態: **実装前の設計提案**。本書の `pocket.*` は新設する公開APIで、現在のファームウェアに存在するAPIではない。型表記は説明用TypeScriptで、デバイスのアプリは素のJavaScriptとする。

基準実装: `2b053b7`。進行中のチュートリアル実装を変更・前提化しない。[現在の構成](architecture.md)、[未解決事項](implementation-audit.md)、[ハードウェア制約](hardware-constraints.md)も参照する。

「必須」はこのAPI版を提供するときの契約。「初期上限案」は実測前の値で、現行ファームウェアの保証ではない。Wi-Fi/BLEを含む全機能の同時搭載・同時利用を約束しない。

## 1. 対象アプリと必要な機能

| アプリ | 共通APIへの要求 |
| --- | --- |
| Hello World・小さなゲーム | 文字・図形、方向／決定入力、フレーム更新、効果音 |
| Playground・チュートリアル・Docs | コード／文章の表示、SKK入力、検索結果のページ取得、サンプルの作業コピー、実行・ログ・編集復帰 |
| ペット | 時刻、状態保存、名前入力、軽量アニメーション、傾き、効果音。終了後は停止し、次回起動時に復元 |
| メモ・作品ライブラリ | UTF-8編集、名前付き保存、複製、前版復元、SD／PCへの書き出し |
| 水平器・センサーロガー | 単位と座標の統一、時刻付きIMUデータ、欠落検知、ファイルへの分割書込 |
| BLEセンサー表示 | scan、GATT接続・読取・通知、切断復帰、データ量制限 |
| リモコン・外付け機器 | IR送信、Grove I²C、EXT UART/SPI/GPIO、バス競合管理 |
| 天気・ネットワーク道具 | Wi-Fi接続、HTTPS、時刻同期状態、期限・キャンセル、オフライン状態 |
| Codex／Claude Code連携端末 | PCとのペアリング、指示／応答、作品転送、ログ。サービス認証とエージェント実行はPC側 |
| 録音・音の可視化 | マイク使用の明示、固定サイズPCM読取、SDへの逐次保存、音声資源の排他 |

Docs・チュートリアル自身を直ちにJS化する必要はない。ネイティブアプリも同じホストサービスと保存境界を使えるようにする。

## 2. 公開面・互換性・機能検出

公開ルートは読み取り専用の `globalThis.pocket`。内部の `ui.setProp(id, 97, ...)` や `__pjs_*` を教材の安定APIにしない。DOM、Node.js、ブラウザーfetch、Web Bluetooth互換を名乗らない。

```ts
pocket.apiVersion: string;                    // 例 "0.1.0"
pocket.device.info(): DeviceInfo;
pocket.capabilities.get(name: string): Capability;
pocket.capabilities.onChange(fn: (c: Capability) => void): Subscription;
type Capability = {
  name: string;
  supported: boolean;                         // このfirmwareに実装されている
  available: boolean;                         // 現在使える機器・設定・資源がある
  reason: string | null;                      // NO_DEVICE / DISABLED / BUSY等
  limits: Record<string, number | string | boolean>;
};
type DeviceInfo = {
  model: string; firmware: string;
  display: {width: number; height: number};
};
```

名前例: `ui.basic`, `input.text`, `storage.kv`, `fs.volume.app`, `fs.volume.assets`, `fs.volume.sd`, `sensors.imu`, `audio.tone`, `audio.capture`, `audio.playback`, `net.wifi`, `net.http`, `ble.central`, `ble.peripheral`, `io.i2c`, `io.spi`, `io.uart`, `io.gpio`, `io.ir`, `bridge.pc`。未知名はsupported=falseで返す。supported=falseの機能も名前空間／メソッドを持ち、呼出しはUNSUPPORTEDで失敗する。

availableは予約ではなく観測値。確認直後に資源が変わり得るため、open/acquireの結果が最終判断となる。認可状態は別であり、available=trueだけでは利用権を得ない。limitsはそのビルドのハード上限、取得ハンドルは実際に割り当てられた値を返す。

版0.xでは破壊的変更をminor版で明示する。アプリ登録情報に対応APIの範囲を持ち、不適合なら起動前に表示する。既存アプリは `legacy-pocketjs` 実行モードのまま維持し、共通APIを使う `pocket-app` へ明示的に移行する。同一アプリ内でlegacy frameと新しいフレーム登録を併用しない。

## 3. アプリ登録と権限

初期はビルド内の登録情報で十分。将来のインストールでも同じ情報モデルを使う。以下は登録形式の案であり、現状のmanifestや.pocket形式の変更ではない。

```json
{
  "id": "local.pet",
  "title": "Pocket Pet",
  "entry": "main.js",
  "runtime": "pocket-app",
  "api": ">=0.1.0 <0.2.0",
  "required": ["ui.basic", "storage.kv"],
  "optional": ["sensors.imu", "audio.tone"],
  "access": {"storage": "self", "sensors": ["imu"], "audio": ["tone"]}
}
```

アプリIDはホストが登録時に確定し、JSの自己申告で他アプリの保存領域に入れない。教材は自分の作業コピーを持ち、サンプルを開く操作でユーザー作品を上書きしない。Playgroundからの実行も固有の実行所有者を持つ。

マイク、BLE接続／広告、Wi-Fi設定、外部機器の駆動、PC接続、他作品の変更はホストが登録範囲とユーザー設定に照らして扱う。アプリが権限を要求したときは対象・目的を示すホスト画面を使い、一度許可した範囲での操作ごとに確認を繰り返さない。認証情報を含む保存値やWi-Fiパスワードをアプリの通常ログへ出さない。

API境界は能力の制限であり、C/Rustまで含む敵対的コードの完全な隔離を保証しない。上限検査とnative OOM対策が入るまでは任意アプリの配布基盤として完成扱いにしない。

### 3.1 オーバーレイアプリ（一部実装。2026-09-09に改訂）

**ホーム画面のUIそのものになるアプリ。XMBを終了させ、その場所に立つ。** シーンは下で描かれ続け、シェルは画面を手放さない（背景の代わりに走る通常アプリとは、そこが違う）。時計・いま鳴っている曲・観賞用の表示・シーン定義の編集など「背景を見ながら使うもの」がこの形になる。

```json
{ "runtime": "pocket-overlay", "required": ["ui.overlay"] }
```

**この種別を分ける理由は、合成ではなく統治にある。** 描画の重ね合わせはホーム画面が既に行っている（背景＋メニュー）。新しいのは、**ユーザーがホーム画面に居るあいだゲストが生きている**という点で、危険はすべてそこから出る。

#### 何が置き換わったか（2026-09-09）

この節は当初「オーバーレイはシェルのUIを覆えない」と書き、それを**描画順**で実装していた（`shell.c` が `overlay_paint()` を `paint_labels()` の前に呼ぶ）。**その規則は要件と実装を1つの文に潰していて、実装のほうが要件として読まれていた。**

分けると、片方は残り、片方は残らない:

* **残る:** オーバーレイは**同意を求める面**を覆えない。権限確認、Wi-Fiパスワード欄、フォルダー／ファイル／作品のピッカー、保存の確認。覆える面は、隠すことも真似ることもできる面になる。
* **残らない:** オーバーレイはXMBを覆えない。**XMBは同意の面ではなく移動の面**で、それが守っていたのは「オーバーレイを止める画面へ到達できること」という**到達可能性**である。描画順はその一実装にすぎず、しかも選ばれた理由は安全性ではなく確実さだった（`shell.c` のコメントが「幾何学的な答えは偽になる」と書いているとおり、メニューはスクロールでどの行の上にも来る）。

**そして描画順のままでは、この節が最初の段落で約束している形が成立しない。** ホーム画面の上に載るUIは、メニューが通らない端の余白しか使えない。約束を守るために規則のほうを直す。

#### 置き換え、統治

**オーバーレイはXMBを終了させて立ち上がる。** 重なるのではなく、入れ替わる。ホーム画面はどちらか一方の状態にあり、両方が同時に描かれることはない。だからオーバーレイが「偽のXMB」を描く余地は、覆う場合ほど大きくない — 本物はそこに居ないので、隠されているものが無い。

**シェル起因のモーダルはオーバーレイに勝つ。** 上に描かれ、入力を取り、閉じるまでオーバーレイのターンは回らない。何がモーダルかは列挙して固定する（実装では `tick_run()` が既にこの形を持っている）:

* `fs.requestFolder` のフォルダー選択、`fs.pickFile` のファイル選択、`workspace.pick` の作品選択
* `input.text` のホスト所有の編集欄（Wi-Fiパスワードを含む）
* 保存・上書き・権限の確認
* アプリのエラー表示

**列挙であって「シェルが描くものすべて」ではないことが要点。** 前者は増えたときに一行足す作業で、後者は増えたときに黙って穴が開く。検査は列挙のほうを見る。

**騙されないことは、上の列挙だけで足りる。** オーバーレイは偽の画面をいくらでも描けるが、**騙しが意味を持つのは、それで何かをさせられるときだけ**である。させたいこと（フォルダーの許可、パスワードの入力、保存の確認）は全部モーダルで、モーダルはオーバーレイの上に出る。偽のUIは時間を浪費させられても、何一つ取得できない。分割線は「XMBの上か下か」ではなく「**同意を求める面か否か**」で引く。

#### 入力と、戻る道

**XMBが走っていないので、残余はもう空集合ではない。** 旧版はここで `input.overlay` を「規則は正しいが指す集合が空」として配線しないと決めていた。それはXMBが全ナビゲーションキーを取っていたからで、**XMBを終了させる設計では前提ごと消える。** オーバーレイが立っているあいだ、シェルはメニューを操作していない。

**ただしシェルは戻るキーを1つ予約し、決して委譲しない。** それがオーバーレイを降ろしてXMBを復帰させる。到達可能性を保証するのは、もはや描かれた行ではなくこのキーである。

このキーには3つの性質が要る:

* **オーバーレイに配送されない。** 委譲対象から構造的に外し、検査で固定する。「渡さないことにしている」ではなく「渡す経路が無い」。
* **オーバーレイの協力を必要としない。** 降ろすのはシェルで、ゲストの応答を待たない。壊れたオーバーレイ、無限ループのオーバーレイ、応答しないオーバーレイでも戻れる。既存の `overlay_yield()` がこの降り方を持っている。
* **人が知っている。** 隠し操作ではなく、XMBに戻る道として文書と画面のどこかに出る。これが信頼の錨で、偽UIに対する最終的な答えでもある。

**能力の既定はフォアグラウンドのアプリより狭い。** ユーザーがアプリを見ていない間も走るので、音声再生・ネットワーク・外部機器の駆動は宣言と許可があっても既定では止め、明示的に有効化する。**許可の取り消しはホーム画面から常に到達できる**必要があり、オーバーレイが壊れている最中でも到達できなければならない — 上の予約キーがその経路である。

#### 実装の状態（2026-09-09）

**この節は実装された。** `shell_draw()` はoverlayが立っているとき `menu_layout()` を
呼ばず、ラベルも描かない。ESCはシェルの予約キーで、`ui_task` の配送ループの先頭で
`break` するのでゲストへ渡す呼び出しに到達しない。3つのピッカーはホーム画面の
ループでも配送される。`REGISTERED[]` は2行、Settingsは3択。overlayセッションには
`fs` と `av` が入り、`app_overlay_tick()` が対応するポンプを回す。

`overlay_yield()` の最初の呼び出し元は予約キーである。無線とストリームの側の
呼び出し元は依然として無い。

**予約キーは一度間違った場所に置かれた。** force stop（Ctrl+Alt+Del）の分岐に
書いたので、通常のBackは配送経路をそのまま通っていた——**この節が構造的だと言って
いる不変条件が、1ビルドのあいだ成立していなかった。** 見た目に問題が出なかったのは
ESCが `action_of()` で名前を持たず落ちたからで、それは偶然である。
**「構造で保証する」と書いたら、その構造がどの行かを名指しできること。**

`ui.overlay` の `limits` は `maxItems`、`maxTextChars`、`regionWidth`、
`regionHeight`、`keyListeners`、そして `reservedKeys`（`"back"`）を報告する。
最後のものは、待っても来ないキーをアプリが待たずに済むためにある。

#### 安全弁

**前提はホーム画面が生き残ること。** オーバーレイの失敗がシェルを巻き込むなら、ユーザーはそれを止める画面へ行けない。ホームはオーバーレイ抜きで完全に描けなければならず、その経路を検査で保つ。

**危険はクラッシュではなく「取り上げ」（dispossession）である。** この節は当初、Rust UIコアがメモリ不足を報告せずパニックして再起動することを理由に起動時の予約を求めていた。実装が示したのは、**その危険はオーバーレイには存在しない**ということだった — 背景の上に重ねるためにはRust UIコアを使えない（`render_strip` は描画前に領域を0で塗る）ので、実装にはそのコアが無い。ゲストのアロケータが尽きればQuickJSは例外を投げ、起動は普通のエラーとして断られる。**オーバーレイがメモリ不足で機体を落とすことはない。**

代わりに起きるのは、オーバーレイが**他が後で必要とするメモリを静かに取り上げる**ことである。無線が上がらない、ストリームが再生できない — そして障害は時計とは何の見た目の関係もない場所に現れる。予約が守るべきはこちらで、名前を正しく付けると、起動前の予約だけでは不十分であることがそのまま見える。

したがって二段構えにする:

* **起動前**は安価な足切りに留める。連続した1ブロックを要求する予約は誤りだった — QuickJSは小さな確保を積み上げるので連続ブロックを一度も要求せず、成立したはずの起動を断っていた。見るのは合計であって連続性ではない。
* **ゲストの上限（ceiling）と、実際にかかる費用（cost）を混ぜない。** `JS_SetMemoryLimit()` は `JS_NewContext()` **より前**に適用されるので、空のrealmを作る費用より小さい上限を置くと、小さなゲストではなく生成失敗（`ESP_ERR_NO_MEM`）が返る。2026-09-09、空きヒープ278,820バイトの機体で48KiBの上限がこれを起こした。**上限は天井なので大きく取っても費用は増えない**（QuickJSは使う分しか確保しない）。足切りに入れてよいのは費用の側だけで、天井ではない。
* **起動後に検証する。** ゲストが上がった時点で、消費量はもはや予測ではなく引き算である。**残りが他の常設要求の最大値を下回っていたら、その場で降りる。** 現行機では `NET_RADIO_MIN_FREE`（56KiB、2026-09-07実測、無線を上げる費用）がその値で、「オーバーレイが起動する前に機体ができたことを、今もできる」を意味する。**これができるのはオーバーレイだけである** — 起動のために何も追い出していないので、降りれば機体は元の状態に正確に戻る。前景アプリは上がった時点で置き換えた画面が既に無く、この撤退ができない。

**残る不確かさは設計目標ではなく、記録すべき限界である。** このタスク上のどんな測定も、1マイクロ秒後に別のタスクが動かすヒープについては何も約束しない。`pocket_api.h` は `available` について既に同じことを書いている（「取られた瞬間の観測であって予約ではない」）。上の二段と、次段落の譲渡は窓を狭め、失敗を可視かつ可逆にするだけで、閉じはしない。閉じられると書くほうが害が大きい。

**予測を改善するより、要求が来た瞬間に譲る。** 起動時に「後で誰かが必要とするか」を当てにいくのが予測で、無線の起動やストリーム開始という**実際にその瞬間**にオーバーレイを降ろせば、予測そのものが要らなくなる。降りる側の実装は `overlay_yield()` にあり、呼び出し側は要求する側のコードに置く。

**フレーム予算を超え続けたらシェルが止める。** 重いオーバーレイはホーム画面を操作不能にし、ユーザーは「それを止める設定」へ辿り着けなくなる。超過が続いたら停止し、停止したことを表示する。黙って止めると故障と区別がつかない。

**起動中に落ちるオーバーレイを自動で再武装しない。** これが最も重要な弁で、無いと**起動ループで機体が使えなくなる**。起動の直前に「起動中」フラグを不揮発へ書き、一定時間の健全動作を確認してから消す。**起動時にフラグが立っていたら自動起動しない**。再有効化は人間の操作だけで行う。

**ゲストの寿命はホーム画面が所有する。** `app_session.c` の逆順破棄の契約はそのまま効く — ゲストのコールバックを保持するモジュールは、ゲストが死ぬ前に reset される。変わるのは寿命の定義が「アプリ画面への出入り」から「ホーム画面の所有」になる一点で、この差を曖昧にしたまま実装しない。

**背景との組み合わせは登録では決めない。** 重い背景と重いオーバーレイは同時に成立しない（FLOWER は 41ms/フレームを使う）。これは断る条件ではなく**表示するべき事実**で、フレーム時間はユーザーが見て決められる場所に出す。

## 4. 共通の値・エラー・非同期契約

```ts
type Subscription = { close(): void };        // 冪等。以後callbackなし
type CancelToken = object;                   // ホスト生成、JSで偽造不可
pocket.cancel.source(): {token: CancelToken; cancel(): void};
type Options = {timeoutMs?: number; cancel?: CancelToken};
type PocketError = Error & {
  code: string; operation: string; retryable: boolean;
  outcome?: "not-applied" | "applied" | "unknown";
};
```

同期関数は引数検証・状態取得・小さなUI変更に限る。待ちを伴うI/OはPromiseを返す。Promiseを返すメソッドは引数エラーもrejectへ統一する。同期関数はthrowする。JSへESP-IDFの数値エラーを直接返さず、診断情報にだけ保持する。

共通code: INVALID_ARGUMENT、UNSUPPORTED、NOT_AVAILABLE、PERMISSION_DENIED、BUSY、LIMIT_EXCEEDED、OUT_OF_MEMORY、TIMEOUT、CANCELLED、CLOSED、DISCONNECTED、NOT_FOUND、CORRUPT_DATA、IO_ERROR、AUTH_FAILED、TLS_ERROR、CONFLICT。retryableは自動再送の指示ではない。書込や外部操作の結果が不明ならoutcome=unknownとし、再試行で重複し得ることをアプリが判断できるようにする。

- 時間は単調時計のミリ秒、ファイル位置と長さはbytes。JSの安全な整数範囲でのみ受け付ける。
- 文字はUTF-8へ変換したバイト数で上限判定。不正UTF-8、孤立サロゲートは文字APIでINVALID_ARGUMENTとし、バイナリAPIならそのまま扱う。
- バイナリはUint8Array。送信側は呼出し受付時に上限内でホストへコピーする。呼出後のJSによる変更を転送へ反映しない。
- 受信結果はJS所有の独立した値。ドライバーの借用ポインターやDMAバッファをJSへ渡さない。
- read系は上限を引数に取り、読み手が次を要求するまで追加バッファを無制限に積まない。1ハンドルの同種操作は原則1件、重複はBUSY。
- timeoutMsは呼出受付からの全体期限で、キュー待ちを含む。省略値は機能ごとに定義。上限超過の要求は黙って丸めず拒否する。
- cancel/close後の処理完了は1回だけ。closeは新規操作を即拒否し、保留操作をCANCELLED/CLOSEDで終了させる。ネイティブの停止待ちはホストが所有する。

## 5. ライフサイクルとイベントループ

```ts
pocket.app.start(hooks: {
  start?: () => void | Promise<void>;
  stop?: (reason: "back" | "replace" | "shutdown") => void | Promise<void>;
}): void;
pocket.app.exit(): void;
pocket.app.onFrame(fn: (f: {timeMs: number; deltaMs: number}) => void): Subscription;
pocket.time.now(): number;
pocket.time.wall(): {unixMs: number | null; source: "unsynced" | "host" | "network"};
pocket.time.sleep(ms: number, options?: Options): Promise<void>;
```

startはソース評価中に1回登録する。新ランタイムではglobalThis.frameをホストが用意し、Promiseだけを待つアプリもイベント処理を継続できる。start hook終了まで状態はStartingだが、I/O完了とキャンセルは配送する。onFrameはRunningでのみ呼ぶ。

1ターンは停止要求→入力／I/O完了→上限付きPromise job→frame→描画の順。Promise連鎖を含むJSターン全体に期限を設ける。frameは同期関数で、Promiseを返したらINVALID_ARGUMENTとしてアプリを停止する。I/Oのawaitはstart、別のasync関数、イベントから開始する。

全JS callbackとPromise解決はJS所有タスクで行う。ISR、音声、Wi-Fi、BLE、I/O workerからQuickJSを直接呼ばない。イベントはappSessionId・generation・requestIdを持ち、終了済みセッションの結果を捨てる。

通常終了は新規入力を止め、stop hookを最大200ms（初期案）だけ待ち、I/Oキャンセル、購読解除、画面解放、資源解放、guest破棄へ進む。強制停止・例外・電源断ではstop hookの実行や保存を保証しない。重要な状態は変更時に明示保存する。終了中に残ったnative workerはJS値を持たず、停止完了までホストがメモリを保持する。無理にfreeしない。

イベント／completion用の制御領域は一般データキューと分け、Promise完了や停止通知を黙って落とさない。センサーの最新値は統合できるが、入力・通信の欠落はカウンターとoverflow状態で通知する。入力overflow時はheld状態をリセットして古いイベントを破棄する。

## 6. UI・画面遷移・入力

```ts
pocket.ui.screen({background: number}): Screen;
type Screen = {
  text({x,y,width,height,text,font,color}: TextSpec): TextNode;
  rect({x,y,width,height,color,radius?}: RectSpec): Node;
  list({x,y,width,height,items,selected}: ListSpec): ListNode;
  close(): void;
};
type Node = {remove(): void};
type TextNode = Node & {setText(text: string): void; setPosition(x: number,y: number): void};
type ListNode = Node & {setItems(items: {id:string;label:string;detail?:string}[]):void;
                        select(id:string):void};
pocket.ui.push(screen: Screen): void;
pocket.ui.pop(): void;
pocket.ui.toast(text: string, options?: {durationMs?: number}): void;
pocket.input.onAction(fn: (e: ActionEvent) => void): Subscription;
pocket.input.onKey(fn: (e: KeyEvent) => void): Subscription;
pocket.input.held(action: string): boolean;
pocket.input.text.open(options: TextOptions): TextSession;
```

座標は左上原点、x右／y下、単位px。色は0xRRGGBBAA。fontは `small`（英字）、`body`（日本語12px）、`compact`（日本語8px）。文字サイズを自動縮小しない。TextSpecは矩形のほかtext、font、colorを必須とし、表示は矩形でclipする。折返し・省略は初版で暗黙実施せず、複数行は改行で指定する。ListSpec.itemsは上記配列、selectedはid。リストは選択変更の描画だけを担当し、操作イベントとの接続はアプリが行う。

screenは非表示で生成、pushで有効化。popは画面を破棄し直前へ戻る。最後の画面からpopはapp.exitと同じ。最大画面深度を制限する。異なる画面のノードを混ぜた操作はINVALID_ARGUMENT。削除済みノード操作はCLOSED。表示・フォント確保のnative予算を先に確認してから変更を適用する。超過時は旧表示を維持する。

ActionEventは `{action:"left"|"right"|"up"|"down"|"accept"|"back", phase:"press"|"repeat"|"release", timeMs:number}`。KeyEventは `{key:string, code:string, modifiers:{shift,ctrl,alt,fn,opt}, phase, timeMs}`で、文字の確定は含めない。key/codeの一覧は実装時にキーマップから公開する。onActionはIMEやホストに消費されなかった操作だけを受ける。back未購読時はホストがpopを行い、購読時はアプリが処理を担当する。ForceStopは購読不可で、アプリより先に処理する。

TextOptions: `{rect:{x,y,width,height}, initial:string, maxBytes:number, multiline:boolean, ime:"off"|"on", onEdit:(e:{text:string})=>void, onSubmit:(e:{text:string})=>void, onCancel:()=>void}`。
TextSessionは `{getText():string, close():void}`。ホスト所有の小さな編集欄を画面へ合成する。IME未確定文字はgetText/onEditへ含めず、確定／削除／移動はホストが管理する。確定EnterはonSubmitへ二重配送しない。単行の非変換EnterのみonSubmit、複数行は改行。変換Escは取消、非変換EscはonCancel。onSubmit/onCancelで自動closeし、画面変更時もcloseする。フォーカス世代で遅延commitを拒否する。同時に1セッション。

初版は高水準の本文入力欄まで。Playgroundの大きなコード編集バッファを毎打鍵JSへ全コピーする設計にはしない。コードエディタはネイティブのまま共通ホストサービスを利用する。

## 7. 保存・ファイル・作品

```ts
pocket.storage.get(key:string, options?:Options): Promise<{value:unknown;revision:number}|null>;
pocket.storage.set(key:string, value:unknown, options?:Options & {ifRevision?:number}): Promise<{revision:number}>;
pocket.storage.remove(key:string, options?:Options): Promise<void>;
```

storageはアプリIDごとに分離した小さな状態保存。valueはJSON互換のnull/boolean/有限number/string/配列/plain object。関数、循環、undefined、BigIntは拒否。keyは1〜64 UTF-8 bytes。未作成はnull、保存したnullはvalue:nullのレコードで区別する。revision不一致はCONFLICT。成功応答は永続化完了後。旧値か新値のどちらかに復旧できる原子的置換を必須とする。終了hookに依存しない。

ファイルシステムは **pocket.fs** に統一する。初稿のpocket.filesは採用しない。[ファイルシステムAPI詳細](filesystem-api.md)をファイル操作の正規仕様とし、本書の共通規則より具体的な契約・上限・既定値は詳細仕様を優先する。

fsは `app:/`（アプリの永続ファイル）、`assets:/`（同梱の読取専用素材）、`sd:/`（許可フォルダーを仮想ルートにしたSD）を扱う。stat/list/mkdir/remove/rename/copy、open/read/write/seek/flush/commit/close、readText/writeText、媒体状態と世代を定義する。listはページ取得、読書きはチャンク単位で、全量RAM読み込みを前提にしない。

openはread/create/replace/append。create/replaceは一時版へ書き、commitで公開して自動close。空ファイルとNOT_FOUNDを区別する。appendは末尾追記で、電源断による部分書込を許容する用途に分ける。通常運転中のatomicReplaceと電源断復旧のcrashSafeReplaceを別featureとし、既定の安全な保存を黙って弱い保証へ落とさない。媒体抜去でハンドルを無効化し、再挿入でも復活させない。JSへformatや生Flash操作は提供しない。

作品管理は別のホストサービスとする。`pocket.workspace.pick({kind:"source"}) -> Promise<SourceRef|null>`、`read(ref) -> Promise<{text,revision}>`、`create({title,text}) -> Promise<SourceRef>`、`save(ref,text,{ifRevision}) -> Promise<{revision}>`、`run(ref,{returnState}) -> void`を後段で追加する。SourceRefはホスト発行の不透明参照。教材はcreateでコピーし、既存作品へのsaveは明示的な選択とrevision確認を必要とする。

runは参照・能力・JSON互換returnState（最大1024bytes）を同期検証し、失敗時はthrow、受付後は新規入力を止めて現在のセッションを終了し対象へ遷移する。同時JS実行や、破棄したJSのPromiseを後から解決する仕組みは使わない。呼出元へ戻る場合は新セッションを作り、`pocket.app.launchContext()`が `{returnState, result:{reason,errorCode?}}` を返す。通常起動時は両方null。対象の起動失敗もホストが同じ復帰経路へ渡す。編集カーソル等はreturnState、作品本文は保存サービスが所有する。ネイティブPlaygroundの既存復帰はこのJSセッション規則とは別のホスト実装である。

現行srcstoreの16スロットは初期バックエンドであり、公開APIにスロット番号を露出しない。保存失敗・空文書・破損復旧の課題を解消してからstorage/workspaceの契約を提供する。

### ログと診断

`pocket.log.write(level, text)`（同期、levelはdebug/info/warn/error）は上限付きリングへ記録し、console.log/warn/errorとprintも同じ経路へ接続する。1レコード256 UTF-8 bytes、32レコードを初期上限案とし、文字境界で切り詰めてtruncated=trueを記録する。連続出力には毎秒のbyte上限を設け、超過は破棄数で報告する。USBへ同期で全量出力してJSターンを止めない。

`pocket.log.read({afterSequence?,limit})`は同期で `{records:[{sequence,timeMs,level,text,truncated}],dropped}` を返し、1回最大8件、自己アプリのログだけを読める。`pocket.device.metrics()`はキャッシュ済みの `{heapFreeBytes,largestFreeBlockBytes,jsHeapBytes,fps,ioDropped}` を返す。取得不能値はnull。システム全体の例外・機器状態はネイティブ診断画面が担当し、任意アプリへ他アプリのログや秘密値を公開しない。

### 7.1 `jsHeapBytes` が何でできているか（2026-09-09）

**この数字はこの機体のメモリ議論のほぼ全部が参照するので、何を測っていて何を測っていないかを
書いておく。** `jsHeapBytes` は `JS_ComputeMemoryUsage()` の `malloc_size` で、
`app_report()` が出す `MEM ... js=` と `pocketjs_guest_stats()` の `heap_used` は
**同じ値**である（`guest.c:435`）。だから3者は直接比べてよい。

**中身は「大きな固定の床」＋「ソース1バイトあたり約4.5バイト」である。**
ホストで実物のquickjs-ngを使って測った（`tools/test_js_ledger.c`、実機不要）:

```
runtime only              26,480
+ context/intrinsics     103,856   (+77,376)      ← アプリを1バイトも読む前

source                     bytes   +ledger   比
deskclock.js                 695     3,360   4.83x
hello/main.js              1,448     6,512   4.50x
streamplay.js              4,121    19,952   4.84x
opusplay.js                4,921    22,112   4.49x
```

**実機の床はホストの床とは違う**（ビルド構成が違う）。実機側から逆算すると、
`deskclock`（695バイト、`js=80,653`、`frames=0`）で **約77.3KiB**。
ホストの `+77,376` とほぼ一致するが、**これは偶然であって同一物の測定ではない**——
ホストのその数字はintrinsics分だけで、runtimeの26,480を含んでいない。
一致を根拠に何かを結論しないこと。

**アプリ作者にとっての意味は1行である: ソースを1バイト削るとゲストヒープが約4.5バイト空く。**
コメントと空白は0バイトなので、削る対象はコードのほう。CLAUDE.mdが
「6.5KBのアプリはゲスト107KiB、7.6KBは評価に失敗する」と書いているのは、
モデルが出来るずっと前に失敗から書かれた観測だが、
**77,525 + 6,500×4.5 ≈ 109KiB** でそこに乗る。モデルに合わせて作った数字ではない。

**`js=` が見ていないもの。** アロケータのラッパーとブロックごとのヘッダは
QuickJSの帳簿の外にあるので、**実機が失うバイト数は `js=` より多い**。実測（deskclock、
オーバーレイ起動）: 機器のfreeが **87,996** 減ったときの `js=` は **80,653** で、
**差は7,343（約9%）**。ほとんど何も確保しないアプリでこれなので、
`js=` を機器コストとして引き算に使うときは1割ほど足りないと思っておくこと。

**`free` と `js` は足し算できない。** ゲストのバイトは free の**中から**出ている。
2つのビルドを比べるときは free 対 free で比べる。

**48KiBの上限は文脈を作れない。** `JS_SetMemoryLimit()` がintrinsics構築中に効いていると
`JS_NewContext()` が NULL を返す——床が上の表のとおりだからで、実際にそれで
`ESP_ERR_NO_MEM` が出た（2026-09-09、オーバーレイ）。**上限を置くなら床より上に置くこと。**

## 8. センサー・時刻・電源

```ts
pocket.sensors.imu.latest(): ImuSample|null;
pocket.sensors.imu.watch({rateHz:number}, fn:(s:ImuSample)=>void): Subscription;
type ImuSample = {timeMs:number; sequence:number; dropped:number;
  accel:{x:number;y:number;z:number}; gyro:{x:number;y:number;z:number}|null;
  tilt:{roll:number;pitch:number}|null};
pocket.power.status(): {millivolts:number|null; percent:number|null;
                       charging:boolean|null; timeMs:number|null};
pocket.power.onChange(fn:(state:ReturnType<typeof pocket.power.status>)=>void):Subscription;
pocket.power.keepAwake(options:{reason:string;durationMs:number}): {close():void};
```

IMUの公開座標は画面を正立させた本体基準でx右、y上、z画面手前。基板座標からの変換はHALが行う。加速度は重力を含む加速度計出力、m/s²。角速度はrad/s、右手系。rollはatan2(ay,az)、pitchはatan2(-ax,sqrt(ay²+az²))、rad、静止時の推定傾斜。運動中の絶対姿勢として保証しない。画面上向きで水平静止したときazが約+9.81となる向きを受け入れ試験で確認する。

BMI270は6軸なので磁気方位・絶対yawを返さない。実装していないgyro/tiltはnull。latestは新たなI²C読取を行わない。watchは購読レートで最新値を配送し、古いサンプルをキューへ溜めない。droppedは購読開始からの配送省略数、sequenceは取得順、アプリはtimeMsで鮮度判定する。要求レートを満たせなければopen時にLIMIT_EXCEEDED、実行中の低下はtimestampとdroppedで分かるようにする。

電池ADCから電圧は取得候補だが、校正・電池曲線がない状態で残量%を推測表示しない。充電検出も取得経路がなければnull。keepAwakeは期限付きで、終了時に自動解除。deep sleep、シャットダウン、時刻設定はアプリの直接操作ではなくホスト設定画面へ委譲する。

## 9. 音声・マイク・IR

```ts
pocket.audio.cue(name:"move"|"accept"|"back"): boolean;
pocket.audio.tone({frequencyHz:number,durationMs:number,gain:number}, options?:Options):Promise<void>;
pocket.audio.capture.open({sampleRate:24000,channels:1}, options?:Options):Promise<Recorder>;
type Recorder = {read(maxFrames:number, options?:Options):Promise<Int16Array>;
                 close():void};
pocket.io.ir.send({carrierHz:number,durationsUs:number[]}, options?:Options):Promise<void>;
```

cueは既存合成音の非同期受付で、ミュート・キュー満杯ならfalse。再生完了を意味しない。toneは単音の再生完了Promise、gainは0〜1でホストの音量上限を掛ける。操作音の有効設定とアプリ音量を別の設定として扱い、現在の単一設定からの移行を決めてから公開する。サンプルレート・DMA供給・発振はnativeで行い、JSでPCMを毎フレーム生成させない。

録音は認可済みのセッション中だけ有効。画面にホストの録音状態を表示する。**サンプルレートはアプリの選択ではなくホストの値で、24000。**マイクはES8311のADCから再生と同じI²Sコントローラで入ってくるので、録音のクロックは再生のクロックそのものだからで、他のレートは丸めずにINVALID_ARGUMENTで断る（根拠と実装は9.2）。readは音声frames数上限で、時系列連続のPCM。overflow時は黙って連結せずLIMIT_EXCEEDEDで録音を閉じる。初期は録音／アプリ再生を排他とし、録音中のUI cueは鳴らさない。全二重は別capabilityと測定が必要。長時間録音はSDへ分割保存する。

IRは送信のみ。durationsUsはmark/space交互で先頭mark、全要素正値、合計時間・要素数・carrier範囲を制限する。タイミング生成はnative。受信機能を本体搭載扱いしない。

### 9.1 短尺クリップの再生

音楽プレイヤー、教材音声、ペットの短い音声、PCからの音声応答を対象にする `audio.playback`。
WAV（PCM16／IMA ADPCM）、Opus CELT（§9.1.2）、MP3（§9.1.3）を実装済み。
`player` の形は open→info/play/pause/seek/status/onState/close。形式ごとの制約は各節を参照。

圧縮形式については、旧版がここに「この機体では圧縮音声のデコードが成立しないことが計測で確定した」と書いていた。2026-09-08に測り直した結果、その根拠は4つとも成立しない。下の「MP3/Opus/FLACの再測定」に訂正と現在の値を置く。

```ts
pocket.audio.player.open({source:string}, options?:Options):Promise<Player>;
type Player = {
  info(): {codec:"wav/pcm16"|"wav/ima-adpcm"|"opus/celt"|"mp3";sampleRate:24000;channels:1;
           durationMs:number|null;seekable:boolean};
  play():Promise<void>;
  pause():Promise<void>;
  seek(positionMs:number,options?:Options):Promise<void>;
  status(): {state:"ready"|"playing"|"paused"|"ended"|"error";
             positionMs:number;underruns:number};
  onState(fn:(event:{state:string;error:PocketError|null})=>void):Subscription;
  close():void;
};
```

sourceは `app:/` / `assets:/` のWAVファイルで、24kHz・1ch・16bit PCMまたはIMA ADPCM（WAVのブロック配置）。**再生はストリーミングで、長さの上限はこの面には無い。** openはヘッダだけを範囲読みで検査し、バッファも音も取らない。playは出力受付の完了、pauseは停止位置の保持完了を返し、曲の終わりはonStateで通知する。位置は消費したPCM framesから求める。endedからのplayとseekはNOT_AVAILABLE（再生し直すにはopenし直す）。close後はすべてCLOSED。1プレイヤー・1音声で、toneとは排他（tone中のopenはBUSY）。セッション終了時は自動close。

seekは実装している——クリップは全部RAMにあるので、seekable=falseにする理由がない。ただしADPCMはブロック先頭からしか再開できない（ブロック先頭の4バイトが予測器を再初期化する）ので、要求位置を含むブロックの先頭に落ちる。`seekBlockAligned` がそれを言う。pause/resumeも同じ丸めを受ける。

underrunsはもう常に0ではない。生産者（`pocket_av_pump()`）がリングを満たし損ねた128フレームのブロック数を数える。1回は5.3msで、**落ちるのではなく伸びる**——無音が挿入されるだけで音は1サンプルも失われず、positionMsもその間止まる。pause/seekを跨いで累積する。

出力は本体スピーカー。ミュート中のクリップは長さを保ったまま無音になる（toneと同じ規則）。ミキシングはしない。別アプリへ移っても続くBGMは、ホスト所有のメディアサービスとして別段階の設計。

### 9.1.1 ストリーミング再生（実装、2026-09-08）

**上限はコーデックではなくバッファだった。** 旧実装は `PLAYER_MAX_BYTES`＝24,576バイトのmallocにファイル全体を読み、その場でデコードしていた。ADPCMで2.05秒、PCM16で0.51秒。これを「圧縮率の問題」と読んで良いコーデックを探す動きが続いたが、**バッファより長い音は圧縮率に関係なく鳴らない**。上限はバッファである。

なのでストリーミングにした。ソースを2,048バイトのスロット3本（**6,144バイト**）でリングに流し込み、音声タスクが順に取ってデコードする。リングは**再生を要求されている間だけ**存在し、openしただけのプレイヤーは0バイト。

**責任の所在は2回動いた。これを記録しておく。** 最初は「コーデックの制約」と読まれ、その訂正が「**上限はバッファである**」だった。訂正は正しかったが、まだ足りなかった——もう一段測ると、`app:/` では**ファイルシステムのクォータ**である。`FS_MAX_FILE`＝24,576バイトは16セクタのクォータと「replaceは新旧2版を同時に保持する」要件から導かれた値で（`pocket_fs.c`）、旧 `PLAYER_MAX_BYTES` はそれに**合わせて**設定されていた。**2つの数が等しいのは偶然ではなく導出である。** 片方を動かして長いクリップを期待した人は、何も変わらない理由を説明できない。各段が、次を測るまでは答えに見えた。もう1段あると思っておくこと。

**それでも今日の時点で長さは本当に動く。`sd:` が実装されたからである。** カード上のファイルにこのクォータは掛からない。そして旧実装ではカードのクリップは**そもそも鳴らせなかった**——ファイル全体をRAMに載せる必要があり、アプリ実行中の最大連続空きは73,728バイトだから。**ストリーミングは、sd: 上の長いクリップを「短くする」のではなく「再生可能にする」機能である。** `assets:/`（フラッシュにマップされたファームウェア同梱データ）にも上限は無い。動かないのは `app:/` だけで、そこは最初から24,576バイトだった。

メモリ側も動く：再生中のヒープ確保が24,576→6,144バイト、openしただけなら0。そして**このプレイヤーはもうファイル長に意見を持たない**——より大きなボリュームが来ても、ここを書き直す必要はもう無い。

- **生産者はJS/uiタスク**（`pocket_av_pump()` → `player_feed()`）。`pocket_fs.c` の読み出しはこのタスクのものだから（ストアの索引もブロック走査もスレッド安全ではない）。1回の補充は最大2,048バイトの `esp_partition_read` 1回で、PCM16の48,000バイト/秒に対して毎秒約23回、フレームあたり多くて1スロット。**実測した（2026-09-08）。** `apps/streamplay` が同一バイナリ上で「60フレームのidle」と「1回の再生」を計り、それを繰り返す。連続13サイクル：

```
underruns=0   deltaTenthMs -2..-4   worstFrameMs 35..36
idleTenthMs 337..338                playTenthMs 334..335
```

`deltaTenthMs` は「再生中の平均 − idleの平均」を0.1ms単位で表したもので、供給のコストはわずかに**負**に出た。これは高速化ではなくノイズである。正直な言い方は、**供給のコストはこの測定器の分解能より小さい**——予測していた上限1msに対して、検出できない。**−0.3msを「速くなった」と引用しないこと。**

**13サイクルであることが1サイクル目より重要である。** 各サイクルはopen/closeをやり直すので、サイクルを追うごとにdeltaが増えるならプレイヤーの寿命を跨いで何かが漏れていることになる。13回同じだったことがそれを否定する。1回の測定では両者を区別できない。
- **消費者は既存の音声タスク**（優先度7）。描画タスクの上で長い処理をしないという規則は `sound_capture_probe()` が別タスクになった理由と同じで、ここでも守られている——**ただしフラッシュ上のソースに限る。下を見ること。**

**この設計が届かない先があり、それは `sd:` である。** 数字は `sd-surface` のもので、**すべて算術であって実測ではない**——カード上のストリームを実機で計った者はまだいない。

- バスは400 kHzなので、2,048バイトのスロット1本は**それを要求したフレームの中で**約41msのバス時間になる。`pocket_fs_read_at()` がカードで毎回行うディレクトリ走査とopenはこれに加算される。33msのフレームはこれを収容できず、**終わらないフレームは止まった画面**である——`sound_capture_probe()` を別タスクへ追い出したのと同じ失敗。
- 24kHz・1ch・PCM16は48,000バイト/秒＝384 kbit/sで、400 kbit/sのバスに対して**誰が供給してもリアルタイムには成立しない**。ADPCMの12,000バイト/秒は帯域には収まるが、1回の補充がフレームを止める長さは変わらない。

したがってこのフィーダーが served するのは `assets:/` と `app:/` で、**カード上のソースは綺麗に失敗せず、途切れながらフレームを引きずる**。出口は2つあって別々の作業である：SDのクロックを上げる（`sd_media.c` はそれを編集ではなく測定だと書いている。MISOは配線されているのでソフトウェアで確かめられる）か、供給を優先度4の専用タスクへ移す（fs面をスレッド安全にするのが先で、だからここではやっていない）。**どちらも算術だけで着手する価値は無い。まずカードの読みを実測すること。**
- **スロットはADPCMブロックの整数倍**。ブロック先頭の4バイトが予測器を再初期化するので、スロット境界がブロック境界なら引き継ぐ状態が無い。逆に**スロットに収まらないブロック（2,048バイト超）は分割せず断る**——これがストリーミングが新たに拒否する唯一のもので、`maxBlockBytes` がそれを言う。

**試験素材が、アンダーランの何割が聞こえるかを決める（一般論、2026-09-09）。**
これはこのクリップの話ではなく、耳で音を検査するとき常に成り立つ話なので、ここに置く。

`assets:/chime.wav` はクリック列で、「事象の列の切れ目は分かりやすい」という理由で選ばれた。
その理由は**事象については正しく、事象と事象の間については間違っている**——
**クリック列はほとんどが無音であり、無音に無音を挿入しても何も変わらない。**
クリックの上に落ちたアンダーランだけが知覚され得る。8秒に411パケット、短い過渡音が
並ぶだけの素材では、それは時間軸のごく一部でしかない。つまり「列の切れ目を聴け」という
指示は、**全部を検査したような顔で数%を検査していた**。

持続音には無音の瞬間が無いので、**どのアンダーランも必ず信号の上に落ちる。**
さらに `stretch` がそれを強くする: `stretch` は**ソースを進めずに**無音を挿すので、
持続音は中断された位相のまま再開する——波形の段差、つまりポップになる。
耳は「抜けた事象」より「ポップ」のほうがはるかに得意である。
**`stretch` を正しい方針にした性質そのものが、持続音では露出しクリック列では隠れる。**

そして**周波数は任意ではない**。アンダーランはちょうど128フレーム＝5.333ms＝1/187.5秒
なので、**187.5 Hzの整数倍の音は、アンダーランのあと完全に同位相で再開する——段差が無く、
ポップも鳴らない。** 1000 Hzは5.333×187.5で、3回連続のアンダーランが完全に同位相になる。
375 Hzと562.5 Hzは何回でも無音。656.25 Hzは1回なら最大の段差だが偶数回では無音。
`tools/make_tone_asset.py` は400〜1200 Hzを探索して**541.7 Hz**（1〜8回のアンダーランに
対する最悪の位相差が0.111周期）を選び、187.5 Hzの倍数に近い指定は**拒否する**。

**両方置く。** クリック列（`chime.pok`、8.2秒）はタイミングのずれを聴くのに向き、
持続音（`tone.pok`、15秒・541.7 Hz）は**時間軸のどこでも**アンダーランを捕まえられる
唯一の素材である。`apps/opusplay` はサイクルごとに交互に鳴らし、`OPUS DONE` の `src=` が
どちらだったかを言う。位置は耳ではなく計器が言う（`positionMs` と `underruns` が並んで
出る）ので、持続音は掃引でも唸りでもなく**定常**でよい——唸りは振幅の谷を作り、
クリック列と同じ死角を作り直してしまう。

**アンダーラン時に何が起きるか（`underrunPolicy="stretch"`）。** この設計が実際に持つ失敗はこれなので、選択肢と理由を書く。**音声タスクは無音で埋め、数え、ソースを進めない**——つまりアンダーランは音を**伸ばす**のであって、1サンプルも落とさず、繰り返しもしない。positionMsもその間止まる。

- **繰り返し**（直前ブロックを再生）は、ソースに無かった音を捏造する。採らない。
- **読み飛ばし**（実時間を保つために先へ進む）は音を黙って失う。これは録音側でDMAオーバーフローを跨いで繋ぐことの再生側の双子で、9.2が同じ言葉で拒否している。同じ理由で採らない。
- **1ブロック遅れたら失敗**は、遅いフレーム1回で再生を殺す。生産者は描画タスクの上にいて、そのタスクは33msの予算に対して39.9ms使うと実測されている。**たまに遅れるのはこの系の正常状態**であって例外ではない。

回復できない唯一のケースは、生産者が**完全に止まった**とき（セッションがストリームを抱えたまま終わる、ソースが消える）。`maxStarveMs`＝2,730ms のあいだ1スロットも供給されなければストリームは未完了で終わり、`onState` はI²S失敗と同じ `error` を出す。止まったまま無音を鳴らし続けるより、止まったと言うほうが正しい。

`limits` の変更：`streaming=1`、`ringBytes=6144`、`maxBlockBytes=2048`、`underrunPolicy="stretch"`、`maxStarveMs=2730` を追加し、**`maxSourceBytes` を削除した**。削除が報告である——この面はもうソースの大きさを何も強制していないので、強制していない数を publish するのは限界を持たないより悪い。ソースの上限はボリュームのもので、`fs.volume.app` の `maxFileBytes` が最初からそれを言っている。

費用（実測、object単位、`xtensa-esp32s3-elf-size`）：静的DIRAM **+44バイト**（`pocket_av.c.obj` の `.bss` 150→190、`sound.c.obj` の `.bss` 60→64、`.data` は両方不変）。Flash code +1,049バイト。**永続DRAMで長さを買っていない**ことがこの数字の要点である。

**鳴らせるものが在庫に無かったので、置いた。** `assets:/chime.wav`——24kHz・1ch・PCM16、**150,554バイト・3.135秒**。旧上限24,576バイトの6.1倍で、旧実装なら `open` が `LIMIT_EXCEEDED` で断っていた大きさである。ADPCMにしない理由は2つあって、圧縮すると約37KBに縮んで「上限を超えている」ことの分かりやすさが落ちること、そしてPCM16の48,000バイト/秒が生産者にとって**最悪ケース**でありテストすべきはそちらだからである。素材がクリック音なのも意図で、**アンダーランの穴はクリック列では耳に明らかだが和音では見逃す**。生成は `tools/make_wav_asset.py`（44.1kHzステレオから、11kHzのwindowed-sinc低域通過を掛けてから24kHzへ。掛けずに間引くと12kHz以上が可聴帯へ折り返す）。

Flash実測：app 2,008,416→**2,159,152**バイト（+150,736）、`tools/check_flash.py` は `spare=986,576` で通る。**`EMBED_FILES` はNULを付けない**——`_binary_chime_wav_end - _binary_chime_wav_start` は150,554でファイルサイズちょうどだった。`EMBED_TXTFILES` 前提の `asset_size()` が引いている `-1` は、バイナリ資産では最後の1バイトを落とす。

検証は `tools/test_stream.c`（実機不要、WSL、ASan/UBSan）。`main/hal/sound_stream.h` を——音声タスクが走らせるのと同じ行を——ホストのgccでコンパイルし、**「ソースをスロットに切っても出てくるサンプルは1つも変わらない」**を、遅れない生産者・16回中15回サボる生産者・短い最終ブロック・途中で読めなくなったソースについて確認する。アンダーランは音を落とさず伸ばすことも同時に確認している。**音そのものの正しさはソフトウェアだけでは確認できない**ので、実機での試聴は物理確認として別に依頼する。

**MP3/Opus/FLACの再測定（2026-09-08）。** 旧版はここに4つの制約表を置き「どれか1つでも足りる」としていた。4つとも誤りだった。3つは**書いた時点で**誤りで、1つは**後から**誤りになった。どちらも数字だけが載っていて日付も方法も併記していなかったので、読み返しても気づけない。訂正を先に置く。

- **MP3 28KB** をdecoder heapとして挙げていた。minimp3の実体は状態6,668バイトとscratch 16,236バイトで、後者は `mp3dec_decode_frame` の**stack局所変数**である。heapとして数えた値の大半がstackだった。
- **Opus 26.6KB** は**ステレオ**の値である。この機体の出力は1chで、`opus_decoder_get_size(1)` は実測18,436バイト。
- **FLAC 89.4KB** は約20倍高い。組込み向け実装（blocksize 1024・1ch）で4,560バイト。
- **PCMリング19,200バイト** は48kHz/16bit/**2ch**で計算していた。出力は24kHz・1ch固定＝48,000バイト/秒で、100msは4,800バイト。さらに**Opusはリングもリサンプラーも要らない**——`opus_decoder_create(24000,1)` が出力形式のまま復号する。旧版が「44.1/48kHzへ寄せるならリサンプラーが要る」と書いたのは、寄せる必要のない形式にまで適用されていた。
- **最大連続空き23,552バイト** はその時点の実測として正しい。静的DIRAMを削った結果、いまは **73,728バイト**（`app_largest`、2026-09-08）。上の3つのheap値はどれもこれを超えない。

つまり**heapはもう選別の理由にならない**。選別しているのは音源の置き場だけである。

| 形式（24kHz・1ch） | bytes/秒 | `app:/` の24,576バイトが買う長さ | 測り方 |
| --- | --- | --- | --- |
| Opus 24kbps | 3,050 | **8.06秒** | libopus固定小数点で8秒を符号化して計数 |
| MP3 32kbps | 4,000 | 6.14秒 | ビットレートからの算術 |
| IMA ADPCM（実装済み） | 12,000 | 2.05秒 | 4bit/sample |
| FLAC（可逆） | 約26,000 | 0.94秒 | 生48,000の55%前後 |
| PCM16（実装済み） | 48,000 | 0.51秒 | 24000×2 |

**FLACを採らない理由はこの表の1行で足りる。** 可逆は、すでにファームにあるIMA ADPCMより1秒あたりのバイト数を減らせない。デコーダーとライセンスとFlashを足して、実装済みの形式より**短い**クリップしか作れない。heapが4,560バイトでも結論は動かない。MP3はOpusより短く、加えて16,236バイトのscratchを要求する。

**Opusは成立する。** 実機実測（2026-09-08）：静的DIRAM **0バイト**、復号状態18,436バイトと復号stack 10,420バイトを**再生中だけ**heapから取り、CPUは20msフレームあたり3.2ms（realtimeの16%、最悪3.7ms）、Flash +80,376バイト、ライセンスBSD-3。既存の音声タスクのstackは4,096バイトなので、載せるならstackを増やすか復号を別タスクにする必要がある。

**実現性の調査（2026-09-09）は [opus-feasibility.md](opus-feasibility.md)。** 上の3.2msの出所（ハーネスの場所、ビルド条件、ベクタがCELT WB 20msであること、覆っていない範囲）、P4のカーネルが移らない理由、PIEを書かない根拠、判断を動かす5つの走行はそちらにある。

**この節はOpusの採否を決めない。** 16%のCPUと8秒の音を交換するかは、レンダラーが33.3msの予算に対して39.9msを使っている現状と併せた判断で、まだ決まっていない。

**静的な費用と実行時の費用を1つの数で比べないこと。** Opusは再生していない間は0バイトで、BLEは接続を開かないアプリにも16,912バイトの `.bss` を課す。同じ「KiB」でも別種の支出である。

24,576バイトという上限は `app:/` の1ファイル上限そのものである。旧版の8,192は `app:/` ではなく当時の最大連続空き23,552から来ていた。その値が73,728になったので、24,576対73,728は8,192が23,552に対して持っていた余裕とほぼ同じ比になる。断片化していないときだけ通る機能は人の前で落ちる機能になる——上限の根拠をヒープ側に置く規則は変えていない。

実装のコスト（`tools/memlog.py`、対 7c67504）：静的DIRAM **+160バイト**（`pocket_av.c.obj +133`、`sound.c.obj +12`、残りはalignment）、Flash code +4,588バイト。再生中はクリップ1つ分のmalloc（最大24,576バイト）だけが増える。デコードは既存の音声タスクとI²Sをそのまま使い、リングを持たない。IMA ADPCMの表は194バイトのFlash、状態は音声タスクのstack上に20バイト。

デコーダーの検証は `python tools/test_ima.py`（実機不要）。`main/hal/ima_adpcm.h` をホストのgccで同じ行のままコンパイルし、独立に書いた参照デコーダーとサンプル単位で照合し、正弦波の往復SNR（実測32.3dB）を測り、「ブロック単体のデコードが通しのデコードと一致する」——seekとresumeが立っている前提——を確認する。**音そのものの正しさはソフトウェアだけでは確認できない**（`board_capture` はフレームバッファしか見えず、ES8311に何が届いたかは見えない）ので、実機での試聴は物理確認として別に依頼する。

**未完了として残っているもの（引き継ぎ用）。**

- **音そのものを誰も聴いていない。** ソフトウェアでは確認できない（`board_capture` はフレームバッファしか見えない）。`apps/streamplay` はクリック列を繰り返し再生するので、**穴が無いこと**を人が耳で確かめるのが唯一の検査。
- **`sd:` 上のストリーミングは成立していない**（上のバス帯域の節）。出口は「クロックを上げる」か「供給を専用タスクへ」の2つで、**どちらもまずカードの読みを実測してから**。
- **WAVヘッダの範囲走査（`wav_parse` / `player_at`）にホストテストが無い。** `tools/test_stream.c` が検査しているのはリングとスロット走査であって、チャンク走査ではない。`JUNK` チャンクを持つファイル（元素材がまさにそうだった）や、`data` が4KB先にあるファイルは、実機でしか通っていない。
- **Opusの採否は未決**（9.1の再測定を参照）。ストリーミングはこの判断を**強制ではなく任意**にした——バッファはもう制約ではないので。

将来: SDは `fs.volume.sd` が扱う別の面で、音源の置き場の上限を動かすのはそちらである。圧縮形式はOpusだけが候補として残っていて、判断は未了。旧版はこの位置に「23.5KiBの制約が動いてから」と書いていたが、その制約は動いた——上の表を書き換えたのは機体ではなく、静的DIRAMを削った作業である。

### 9.1.2 Opus復号（実装、2026-09-09）

`limits.codecs` に **`opus/celt`** が増えた。名前が `opus` でないことが仕様である——下の「断るもの」を読むこと。

**形は何も変わっていない。** `pocket.audio.player.open({source:'assets:/chime.pok'})` が
WAVと同じ手順で開き、同じ `info/play/pause/seek/status/onState/close` を返し、同じ6,144
バイトのリングへ流れる。**並行の経路を作っていない**——復号器が吐くのはPCM16なので、
`main/hal/sound.c` の音声タスクは自分がコーデックの下流にいることを知らないし、
`sound.c` は1行も編集していない。

**置き場所は再生中だけ生きる復号タスクである。** `ui` のpumpで復号する案は、33.3msの
予算に対して39.9ms使っているフレームに約5.3msを足すので採らなかった。`sfx` タスクの
4,096バイトstackを増やす案も採らなかった——あれは `sound_init` でheapから取るので、
鳴っていない間ずっと約12KiBのidle税になる。**復号タスクは `play()` で作られ、
終わりか `close()` で消える。**

経路は 生産者 → リング → 変換 → リング → 消費者:

| タスク | 優先度 | やること |
| --- | --- | --- |
| ui / JS（`pocket_av_pump`） | 5 | ファイルを読み、**圧縮パケット**のリングへ published |
| `opusdec`（再生中だけ） | 6 | `opus_decode`、PCM16を既存のリングへ published |
| `sfx`（既存） | 7 | 既存の `SOUND_STREAM_PCM16` の道 |

**コンテナはOggではない。** 独自の `POK1`（`main/pocket/opus_feed.h` に配置図、
`tools/make_opus_asset.c` が書く）で、ヘッダ＋索引＋`[u16 length][payload]` の並び。
Oggを採らない理由は**この機体での費用**である: 4KBごとに27バイトのページヘッダとlacing表と
CRC32が付き、**すでにこのファームで最も切迫しているタスクの中で**全バイトのCRCを回すことに
なる——フラッシュとSDが下で既に検査している破損に対して。Oggが本当に効くのは長さの分からない
ネットワーク流と再同期で、こちらはファイルシステムが長さを言うファイルである。索引が買うのは
`seek()` で、無ければseekは描画タスクの上でファイル全体の長さ接頭辞を歩くことになる。
索引の粒度（5パケット＝100ms）がそのままseekの丸めで、ADPCMのブロック丸めと同じ形。

**断るもの、そしてそれが設計判断であること。** stackの実測 **10,420バイト**は
CELT・WB・20ms・monoについての値で、SILKやhybridに対しては**上限ではなく下限**である。
測っていない下限からstackを決めるのは、人の前でoverflowするstackを決めることである。
なので**プレイヤーはTOCバイトを読み、CELT・20ms・mono・1フレーム以外を名指しで断る**——
スロットより大きいADPCMブロックを断るのと同じ形で、`open()` は最初のパケットを見て
`INVALID_ARGUMENT`「this host decodes CELT 20 ms mono Opus only」を返す。これで10,420は
推測ではなく**境界**になり、SILKとhybridは臨界経路から消える。`limits.codecs` が
`opus` ではなく `opus/celt` なのはそのためで、feature-testしたアプリが `play()` で
驚かないようにするためである。符号化側（`tools/make_opus_asset.c`）も
`OPUS_APPLICATION_RESTRICTED_LOWDELAY` でCELTを強制し、**書いた全パケットを実機と同じ
述語で検査してから**ファイルにする。

**費用。**

| 項目 | 値 | 区分 |
| --- | --- | --- |
| libopusの静的DIRAM | **0バイト** | 実測（`idf.py size-components`、`libopus.a` の DIRAM/.bss/.data が全部0） |
| glueの静的DIRAM | **+100バイト** | 実測（`pocket_av.c.obj` の `.bss` 190→266、`opus_feed.c.obj` +24） |
| libopusのFlash | **75,362バイト**（code 54,589＋rodata 20,773） | 実測。デコーダー限定のソース表で、ハーネスの全globより約4.2KB少ない |
| 復号状態（heap、再生中だけ） | 18,436バイト、1ブロック | 実測（2026-09-07） |
| 復号タスクのstack（heap、再生中だけ） | 14,336バイト | 実測10,420＋3,916 |
| 圧縮パケットのリング（再生中だけ） | 6,144バイト | 既存のリングと同じ構造・同じスロット幾何 |
| 再生中heapの合計 | 約45.6KiB、最大ブロック18,436 | 導出 |
| 再生していない間 | **0バイト** | openしただけのプレイヤーもリングを取らない |

**圧縮ringを新しく書かずに `sound_stream_t` を使い回した**のは、あの atomics が
「間違いが聞こえない」部分だからで、`tools/test_stream.c` が既にホストで回している。
小さい専用ringを手書きすれば再生中heapを4KiB節約できたが、その代わりに複製するのは
**あのヘッダが複製されないために存在している**コードそのものだった。6,144バイトが買うのは
128msではなく**2.0秒**である——Opusの3,204 B/sはPCM16の16分の1なので、同じ器が16倍長く持つ。

**8秒の資産。** `assets:/chime.pok`——`chime.wav` を8.2秒ぶん繰り返して符号化した
**26,274バイト**。§9.1の表は「Opus 24kbps・3,050 B/s・24,576バイトが8.06秒」と予測して
いた。**実測は3,204 B/sで7.67秒**、5%短い。差はコーデックではなく容器である:
ヘッダ36＋索引332＋長さ接頭辞411×2＝822の計1,190バイト（4.5%）を引くと
**中身は3,059 B/s**で、予測の3,050とは0.3%しか違わない。予測が測っていたのは裸のパケット列で、
このファイルは索引とフレーミングを持っている。

**検証。** `tools/test_opus_pak.c`（実機不要、WSL、ASan/UBSan）が `opus_feed.h` を
——復号タスクが走らせるのと同じ行を——ホストのgccでコンパイルし、
(1) ヘッダが通ること、(2) **全パケットが実機と同じゲートを通ること**、
(3) **スロットに切っても出てくるパケット列が1つも変わらないこと**（`test_stream.c` が
リングについて証明していることの、容器版）、(4) 索引の全エントリがパケット境界に落ちること、
(5) 壊したヘッダが断られること、を確認する。**音そのものの正しさはソフトウェアだけでは
確認できない**ので、実機での試聴は物理確認として別に依頼する。

**起動時のアンダーランと、単位を間違えた話（解決、2026-09-09）。**

最初の実機走行で `underruns=5`（`faults=0`）が出るサイクルと出ないサイクルがあった。
原因は**音声タスクが空のPCMリングから引き始めていたこと**で、`player_launch()` が
復号タスクを作った直後に `sound_stream_start()` を呼んでいた——`opus_feed_start()` は
**タスクを作って戻るだけ**なので、あれが順序付けていたのは2つの仕事ではなく2つの呼び出し
だった。WAV経路が同じ形で安全なのは、あちらの生産者 `player_feed()` が同じタスクの上の
関数呼び出しで、次の行が走る時点で本当に終わっているからである。**タスクは関数を呼んでも
prime できない。**

修正は音声タスクの開始を `player_pump()` へ移し、**最初のスロットが published された
フレームで鳴らし始める**こと。メモリも増えず、描画タスクも止めない。§9.1が `play()` を
「出力が受け付けられた」で解決させていて「音が鳴った」ではないので、契約は変わらない。
実測: **14サイクル連続で `underruns=0`**（冷たい1周目を含む）。静的DIRAM +8バイト。

**そして26.7msという数字は私の単位の間違いだった。記録しておく価値があるので残す。**
「5アンダーラン＝5×128フレーム＝26.7ms」と換算したが、**その5.33ms/ブロックは定常状態の
消費速度**である。ストリーム開始時の音声タスクは定常状態にいない——TX DMAが空だからで、
`emit()` はDMAに空きが無くなるまで待たない。

- `sound.c:1020` の `dma_desc_num=4, dma_frame_num=128`、`emit()` は512バイト書く。
  **1回の `emit()` がちょうどDMA記述子1本**で、4本＝21.3ms分。
- だから開始時、最初の**4ブロックは即座に**通り、5本目が5.33msを待つ。
- 実測の `prime_us=7,928` に対して: 4（無料）＋1（5.33msで）＝**5ブロック**。
  6本目は10.67msで、priming が終わったあとに来る。**上限はちょうど5である。**

`prime_us=7,928` 自体も復号の仕事だけで説明が付く: `opus_decode` 2回（2×3,724＝7,448µs）
＋ `opus_decoder_create`。**キャッシュの温まり方ではなく復号そのもの**で決まるので、
1周目も14周目も同じ7.7〜9.6msに留まる（実測）——観測されたその安定性が、この説明の
裏付けになっている。

つまり **`prime_us` と「5アンダーラン」は同じ窓を2つの単位で測ったもの**で、間違って
いたのは換算だけだった。**この説明が反証されるのは、修正前のコードで6以上のアンダーランが
観測されたときである**（上の算術が禁じている）。

**まだ測られていないもの（引き継ぎ用）。**

- ~~**復号タスクが描画タスクから何を引くか。**~~ **測った（実測、2026-09-09、R5）。**
  レンダラーの横で `mean_us` は **3,724〜4,005**（14サイクル、2ビルド）、ハーネス単独の
  3,208に対して **+16〜25%**。20msの音に対する取り分は単独16.0%が**実機で18.6〜20.0%**。
  **2つのビルドの間の差（4,005→3,724、−7%）は主張しない**——同じカーネルが命令キャッシュの
  アラインメントでビルド間15%動くことをCLAUDE.mdが明記しており、−7%はその帯の中である。
  差を主張するなら同一バイナリでの比較が要る。差は命令キャッシュの
  取り合いで、これが「1コアの16%」という見出しの、この機体での本当の値である。
  描画側への跳ね返りは `deltaTenthMs` 8〜13＝**0.8〜1.3ms**（WAVストリーミングは0）、
  `worstFrameMs` 41〜45。stackは10,472〜10,476 / 14,336。`worst_us=7745` は mean のほぼ2倍で、これは `opus_decode` の中で
  優先度7の音声タスクや同格の `input` に横取りされた時間を含む壁時計だからである。
  **スロット1本は40.0msかけて消費されるものを8.0ms（最悪15.5ms）で作るので、
  定常状態には80msの余裕がある**——ここが枯れることは無い。
- ~~**`-O2` かどうか。**~~ **決着した（実測、2026-09-09、R1）。** 同一ツリー・2つのビルド
  ディレクトリ・変数1個で、`-Os` は mean 3,208µs／worst 3,670／stack 10,420、
  `-O2` は mean 3,089／worst 3,491／stack 10,484。**mean −3.7%、Flash +11,920バイト。**
  走らせる前に書いた予測は「−10〜−25%」で、**外れた**。同時に書いた棄却条件が
  「5%未満なら-O2は何も買っていない」だったので、**`-Os` のまま**でFlashを残す。
  ついでに分かったこと: `-O2` でも stack は 10,484 で、`opus_feed.c` の 14,336 は動かない。
- **CELT以外のstack最高水位。** 測っていないし、**ゲートがあるので測る必要がなくなった**。
  ゲートを外すなら先に測ること。
- **実機での復号コスト。** 復号タスクがストリームの終わりに1行出す:
  `OPUSDEC packets=... frames=... faults=... mean_us=... worst_us=... prime_us=... stack_used=... of 14336`。
  `prime_us` は復号タスクが生まれてから**最初のスロットを published するまで**で、
  何も聞こえ得ない唯一の窓である。
  これが**このビルド自身の**数字で、ble-wtハーネスの3,208µs（別のtree、別の最適化）を
  継承したものではない。

### 9.1.3 MP3復号（実装、2026-09-09）

`pocket.audio.player.open({source:"assets:/test-tone.mp3"})` で通常のMP3を開ける。
`app:` と、既存のフォルダー権限を得た `sd:` も同じファイル読み出しを使う。
HTTP(S)は従来のOpus専用経路であり、MP3には未対応。

固定revisionのminimp3を再利用。MPEG-1/2/2.5 Layer III、通常のCBR/VBR、
モノラル/ステレオを復号し、ステレオ平均と必要なフィルター・レート変換を通して
既存の24kHzモノラルPCMリングへ送る。I2S・ES8311設定は変更しない。
先頭ID3v2.2〜2.4と末尾ID3v1は読み飛ばす。free-format、途中でのレート変更、
破損フレーム、APEタグ、ギャップレス用encoder delay/paddingの除去は未対応。

MP3の `info().durationMs` は **Xing/Info/VBRIタグの総フレーム数**から返す。
タグが無ければ `null` のままで、**推測はしない** — CBR前提の
「バイト数÷ビットレート」は定ビットレートなら正確で可変なら黙って外れ、
どちらであるかをファイルは述べていない。全フレームを数えれば正確に出るが、
それはカードから数MBを読むことで、openを頼まれたターンの中でやることではない。
**述べられているときに述べ、述べられていないときは黙る。** 嘘をついたバーは
消せない。
`seekable:false` で `seek()` は `NOT_AVAILABLE`。
一時停止からの再開は専用タスクで先頭から復号し、既に再生したサンプルを捨てて
bit reservoirを復元するため、長い曲の後半では再開に時間がかかる。
描画タスクで全曲を走査せず、インデックスも推定しない実装上の制約。

`audio.playback.limits.codecs` に `mp3`、`mp3Container:"mpeg-layer3"`、
`mp3Seekable:false` を追加。共通の `sampleRate:24000, channels:1` は出力形式。
再生中だけ復号状態・PCM作業領域・圧縮フレーム・24KiBスタックを確保する。
確保できなければ従来同様 `OUT_OF_MEMORY` とfree/largestを返す。

Apps末尾の **MP3 PLAYBACK** は3種類の合成音源を巡回し、交互にpause/resumeを検査する。
実測、制約、再現手順は [mp3-implementation.md](mp3-implementation.md) を参照。

### 9.2 録音（実装）

`audio.capture` を実装した。形は上の型のままで、変わったのは**サンプルレートだけ**。

```ts
pocket.audio.capture.open({sampleRate:24000,channels:1}, options?):Promise<Recorder>;
type Recorder = {read(maxFrames:number, options?:Options):Promise<Int16Array>; close():void};
```

**なぜ16000ではなく24000か。** この機体のマイクは独立したPDM周辺ではなく、ES8311のADCから**再生と同じI²Sコントローラ**（I2S_NUM_1、BCLK=41 / WS=43、ASDOUT=46）で入ってくる。ESP-IDF v6.0.1は、2本目のチャネルのクロックとスロット設定が1本目と完全に一致するときにだけ全二重として成立させ、BCLK/WSを共有する（`i2s_std.c` の "Constitude full-duplex on port %d"、および `i2s_common.c` の `i2s_acquire_controller_obj` / `i2s_take_available_channel`）。一致しなければRXはTXが既に予約したピンをもう一度駆動しに行く。**1つのコントローラに1つのサンプルレートしかなく、DACのそれは24000。** リサンプラーは無い（9.1と同じ理由でRAMが無い）ので、他のレートは丸めずに `INVALID_ARGUMENT` で断る。`limits.sampleRate` はコードが強制している値そのもの。

`limits`: `sampleRate=24000`、`channels=1`、`maxReadFrames=2048`（1回のreadの上限。85 ms分）、`maxTimeoutMs=30000`、`concurrent=1`、`fullDuplex=false`、`playsWhileRecording=false`。全部この面かHALで実際に検査している値。

**リングは持たない。** I²SのDMAリングが唯一のバッファで、`read` はそこから0タイムアウトで引く。descriptor 6本 × 256フレーム = **6,144バイト（64 ms）**。スロット設定がTXのものなので1フレームは4バイト（ステレオ）で、片チャネルは取り出す途中で捨てる——この2倍がコントローラを共有する代金。64 msはこのファームの33 msフレーム約2つ分で、アプリが1フレーム丸ごと読み損ねても失うものはない。チャネルもDMAも `open` で作り `close` で消すので、**録音していないアプリの負担は0バイト**。

この性質は一度手放して、取り戻した。「セッション内の2回目の録音が無音になる」という疑いが出て、`i2s_del_channel` が全二重の成立を戻さないという機構をドライバのソースから読み、チャネルを再起動まで持つように変えた（6,144バイト常駐）。**その不具合は製品側には存在しなかった**——見えていたのは音をまったく読めていない診断の中だけで、その対照行も無音だった。アプリで open/close/open を3回繰り返すと毎回同じ音量が録れる（`apps/miccheck`: peak 14,433 / 15,414 / 14,696）。**ドライバのソースから読んだ機構は、製品の経路に訊くまでは仮説である。**

**overflowは連結しない。** リングが未読の音声を上書きしたことは、タイミングからの推測ではなくドライバ自身の `on_recv_q_ovf` が報告する。それが立った `read` は `LIMIT_EXCEEDED` で失敗し、**同じ手番でrecorderを閉じる**。穴の前後を黙って繋いだ録音は、止まった録音より悪い。コピーの前と後の両方で見るので、コピー中に跨いだ場合も断る側に倒れる。

**排他。** 録音中は `sound_play()` が false を返し、`sound_tone` / `sound_clip_start` が `SOUND_ERR_BUSY` を返す（`main/hal/sound.c`）。UI cueが鳴らないのはこの1か所の結果で、呼ぶ側が覚えている規則ではない。逆に、何かが鳴っている／キューに積まれている間の `open` は `BUSY`。

**録音表示とレベル。** 画面右上の赤い点を `board_present()` から描く。パネルへの転送路はここ1本しかないので、アプリが何を描いてもその後に載る。アプリが描画を止めた場合に備えて、録音中は0.5秒に一度だけ再描画を要求する。

点の左に6セルのレベル計を出す。録音中の本人が知りたいのは「歪んでいないか」と「小さすぎないか」の2つで、セルは倍々（対数）、最下段が約1/64。**色が変わるのはクリップのときだけ**（最上段が琥珀）で、赤い点は「録音中」の意味と色を保つ——この隅で色が変わったら必ず何かが良くない、という規則にする。レベルは `sound_capture_read()` が読んだ音から取り、150msで消える。読むのを止めたアプリは古い値を出し続けるのではなく、レベルの主張をやめる。

ログのマーカー（`tools/` の契約）: `CAPTURE START rate=%d dma=%dx%d`、`CAPTURE OVERFLOW frames=%u`、`CAPTURE STOP frames=%u`、`CAPTURE CLOSE delivered=%u`。

**マイクはアナログ入力側にある（`0x14=0x1a`）。** これは一日の最後に判明したことで、それまでは逆（PDM、`0x5a`）だと信じて全部を測っていた。実測（150msバケットごとのrms、話しながら）:

```
analog  2033  885  309  228  342  1312  587  1152  928  865   <- 声
pdm     3660 3660   25    0    0     0    0     0    0    0   <- 声ではない
```

analogはバケットごとに動く——声はそうなる。pdmは300msのあいだ**桁まで同じ数**で、その後0へ落ちる。つまり定数であって音ではない。ADCのDCオフセット除去（`0x1c`）を切るとpdmの全バケットが32,767に張り付く。**PDMピンにはDCオフセットが乗っているだけでマイクは繋がっておらず、ハイパスがそれを約450msかけて食っていた。**

したがって**PDMで取った測定値はすべて無効**——`0x14` を選んだ最初の比較（analog 9 / pdm 227）も、6行のゲインスイープも、`apps/miccheck` のpeak（14,433 / 15,414 / 14,696）も、全部そのオフセットを読んでいた。miccheckの録音長は0.64秒で、450msの窓にほぼ収まるため、たまたま声のように見えていた。

レジスタ値の出どころは**Espressifの `es8311` コンポーネント**（esp-bsp、IDFの `i2s_es8311` 例から辿れる）で、データシートから組んだ最初の表は3か所違っていた（`0x17` が0xbf、`0x16` を書いていない、駆動しない `0x15`/`0x1b` を書く）。**ゲインは `0x16=7` / `0x17=0xc8`。** アナログ経路では `0x16`（PGA）が量子化の前、`0x17`（デジタル音量）が後にあるので、「クリップしない範囲で最大のPGA、足りない分を `0x17`」が規則。

**ただしレベルは条件付きの測定であって機体の性質ではない。** 同じレジスタで、5行のスイープ時は最大peak 8,395（フルスケールの26%）、その数分後の `miccheck` では 22,539 / 30,528 / 31,779（69% / 93% / 97%）だった。違いは話す距離と声量だけで、4倍動く。普通の声での3録音は peak 17,816 / 22,559 / 27,825（54/69/85%）で `clip=0`。**正確に言えば「普通に話す分には歪まない、声を張れば歪む」**で、「歪まない」ではない。単独のpeak値から「余裕がある」と結論してはいけない——それはこの一日に繰り返した失敗そのもの。動かす必要が出たら**下げる**方に倒す: このゲインは変換の後なので、下げて失うのはint16のレンジだけでS/Nは失わないが、上げると歪みで音そのものが壊れる。診断はUSBに `9` を送って動かす。

ホストで確かめられる範囲——読み出しの時系列連続性、overflowが閉じること、close後がCLOSED、セッション終了でHALが戻ること、再生との排他、インジケータの点灯範囲——は `tools/test_pocket_capture.c` が実物のQuickJSと実物の `pocket_capture.c` / `pocket_api.c` をASan/UBSanで回して見る（`bash tools/build_pocket_capture_test.sh`）。マイクは計数ランプのfakeなので、「連続している」は判断ではなく引き算になる。

全二重（録音しながら再生）は別capabilityで、DMAをもう1組と実測が要る。ここでは実装しない。

## 10. 外部I/Oと共有バス

```ts
pocket.io.ports(): PortInfo[];
pocket.io.i2c.open({port:string,address:number,hz:100000|400000}, options?:Options):Promise<I2cDevice>;
type I2cDevice = {transfer({write,readBytes}:{write:Uint8Array;readBytes:number},options?:Options):Promise<Uint8Array>;
                  close():void};
pocket.io.spi.open({port:string,mode:0|1|2|3,hz:number},options?:Options):Promise<SpiDevice>;
type SpiDevice = {transfer(tx:Uint8Array,options?:Options):Promise<Uint8Array>;close():void};
pocket.io.uart.open({port:string,baud:number},options?:Options):Promise<SerialPort>;
type SerialPort = {read(maxBytes:number,options?:Options):Promise<Uint8Array>;
                   write(data:Uint8Array,options?:Options):Promise<number>;close():void};
pocket.io.gpio.open({port:string,mode:"input"|"output",initial?:0|1},options?:Options):Promise<Pin>;
type Pin = {read():0|1;write(value:0|1):void;
            watch({edge:"rising"|"falling"|"both",debounceMs:number},fn:(e:{value:0|1;timeMs:number;dropped:number})=>void):Subscription;
            close():void};
```

PortInfoは `{id:string, protocols:string[], reserved:boolean, available:boolean, limits:object}`。GPIO番号を直接受け付けず、検証済みのポート名・ピン組だけをホストが列挙する。Grove/EXTのピン組設定変更はホスト側のボードプロファイルで行う。起動時出力値・close時の安全な入力復帰状態をプロファイルで定義する。

内部I²Cはキーボード・IMU・コーデックで共有する。JSへ無制限なscanや予約アドレスへの書込を公開しない。EXTと内部I²Cが同じ物理バスならホストで直列化し、キーボード処理を優先する。I²C transferはwrite→repeated START→readを1トランザクションとし、片方が0なら片方向。7bitアドレスのみ。

EXT SPIとSDはバス共有なので、CS・mode・clockをトランザクション単位で設定し直す。CSを保持したままJSへ制御を戻さない。SPI受信長はtxと同じ、読みだけならダミーbyteを送る。UARTは初版8N1。readは1byte以上の受信かTIMEOUT、空配列をEOFと混同しない。GPIO callbackはISRから直接呼ばず、レート制限とdebounceを行う。

本体のLCD・Flash・USB・電源・起動関連ピンは予約して公開しない。外部ADC/PWM、UARTパリティ、SPI複数セグメントは後段の能力として設計し、未対応引数を無視しない。

## 11. Wi-FiとHTTP

```ts
pocket.net.wifi.acquire({profileId:string}, options?:Options):Promise<WifiLease>;
pocket.net.wifi.scan(options?:Options):Promise<{networks:{ssid:string;rssiDbm:number;secure:boolean}[];truncated:boolean}>;
type WifiLease = {status():{state:"connecting"|"connected"|"disconnected";address:string|null};
                  onChange(fn:(state:ReturnType<WifiLease["status"]>)=>void):Subscription;
                  close():void};
pocket.net.http.request({wifi,url,method,headers?,body?}:HttpRequest, options?:Options):Promise<HttpResponse>;
type HttpResponse = {status:number;headers:Record<string,string>;
                     read(maxBytes:number,options?:Options):Promise<Uint8Array|null>;
                     close():void};
```

profileIdはホスト設定で作るWi-Fi接続先参照。アプリにパスワードを返さない。初期は2.4GHzのSTAのみ。acquireはIP取得まで待つ。既存の同じプロファイル接続は参照共有、異なる接続先への変更要求はBUSYでホスト設定へ誘導する。closeはアプリの使用権だけを返し、他の利用者の接続を切らない。scanは既定5秒、上限16件、SSIDをUTF-8にできないネットワークは初版では選択対象外としtruncatedに反映する。

HttpRequestは `{wifi:WifiLease,url:string,method:"GET"|"POST"|"PUT"|"DELETE",headers?:Record<string,string>,body?:Uint8Array}`。有効なlease必須。ヘッダ取得までrequestが待ち、その後bodyはreadで取得する。4xx/5xxはstatusで返し、transport/TLS失敗のみreject。HTTPの自動再送、redirect、gzip展開は初版では行わない。max response総量を超えたらLIMIT_EXCEEDEDで閉じる。ヘッダは小文字化し、複数値ヘッダの完全互換は提供しない。cookie jarは持たない。

HTTPSは信頼根とホスト名検証を必須とし、証明書検証を黙って省略しない。時刻が必要で未同期ならTLS_ERRORを返す。接続先は登録されたscheme/host/portの範囲で検査する。HTTP平文は登録で明示されたローカル機器用途に限定する。

Wi-Fi切断で保留HTTPをDISCONNECTEDにし、処理済みか不明な更新要求はoutcome=unknown。HTTP bodyを全量文字列化する便利関数は初版に置かず、アプリが小さい応答だけを上限内で組み立てる。

## 12. BLE

```ts
pocket.ble.scan({serviceUuids?:string[],durationMs:number}, options?:Options):Promise<{devices:BleDeviceInfo[];truncated:boolean}>;
pocket.ble.connect(deviceId:string, options?:Options):Promise<BleConnection>;
type BleConnection = {
  discover(options?:Options):Promise<{services:BleServiceInfo[];truncated:boolean}>;
  read(characteristicId:string,options?:Options):Promise<Uint8Array>;
  write(characteristicId:string,value:Uint8Array,options?:Options):Promise<void>;
  subscribe(characteristicId:string,fn:(e:{value:Uint8Array;timeMs:number;dropped:number})=>void):Promise<Subscription>;
  onDisconnect(fn:(e:{reason:string})=>void):Subscription;
  close():void;
};
```

Bluetooth LEを対象とし、Classic Bluetooth/SPP/A2DPは提供しない。ホストスタックはNimBLEを第一候補とし、採用時のRAMを測定する。deviceIdはscan結果のセッション内参照で、名前やMACを恒久的な信頼IDにしない。BleDeviceInfoは `{id,name:string|null,rssiDbm:number,serviceUuids:string[]}`。BleServiceInfoは `{uuid,characteristics:{id,uuid,readable,writable,notifiable}[]}`。UUIDは正規化した128bit文字列。

scanは最大10秒・16機器、discoverは最大8サービス・32 characteristic（初期案）。接続1件、通知購読4件。read/writeはATT MTUと属性上限を確認し、上限超過は拒否する。writeは応答ありを初版とし、サーバー応答完了でresolveする。通知は最大4件の上限付きキュー、溢れた場合は古い通知を捨てdroppedを累積する。欠落できないアプリはプロトコル側にsequence/再取得を持つ。close時に購読と接続を解放し、旧接続のcharacteristicIdはCLOSED。

pairing/bondはホスト画面と設定が所有する。接続ごとの暗号化要求は登録ポリシーで決め、要件を満たせない場合はAUTH_FAILED。無人の自動ペアリングを暗黙に開始しない。

Peripheralは次段階。`pocket.ble.peripheral.open({name,services}, options) -> Promise<Peripheral>`を予定する。静的GATT定義、ホスト側の読取キャッシュ、write受信イベント、notify、closeを備える。BLE stackの同期read callbackでJSを待たない。BLE HIDキーボード、Mesh、長距離PHY、Central/Peripheral同時運用は別機能として検証する。

## 13. USB・PC bridge

```ts
pocket.bridge.connect({transport:"usb"|"wifi",peerId:string},options?:Options):Promise<Bridge>;
type Bridge = {
  request(method:string,payload:unknown,options?:Options):Promise<unknown>;
  onEvent(topic:string,fn:(event:{sequence:number;payload:unknown})=>void):Subscription;
  close():void;
};
```

peerIdはホストでペアリングしたPC参照。方式とサービスを切り離し、Codex/Claude Code向け処理はPCアダプターが実装する。最初のmethodは `agent.submit`（指示受付→jobId）、`agent.status`、`agent.cancel`。長時間の生成は要求を保留し続けずjobイベントで返す。送信内容・応答には上限を設ける。

作品転送はホストのworkspace経由でstage→長さ・hash・版検証→publish。受信直後の自動実行や既存作品への無条件上書きを行わない。APIキーやPCのシェル実行権限をデバイスに複製しない。

現行USB Serial/JTAGは診断文字とログを共有する。新bridgeはフレーム種別、length、sessionId、requestId、CRC、応答を持ち、診断文字列と分離する。1フレーム最大1024 bytes、ソースは分割し、完了まで公開しない。切断後は同じrequestIdの再送に対してPC側が重複を検出する。Wi-Fi transportは認証付きの暗号化経路を使い、ローカルネットワークにいるだけで信頼しない。具体的wire形式は別仕様とする。

## 14. 資源・予算・無線共存

以下は小さく開始するための初期上限案で、すべての枠を同時に使えるという予約ではない。ホストは利用開始前にnative RAM・最大連続領域・DMA・ドライバー状態を確認し、資源仲介でBUSY/OUT_OF_MEMORYを返す。必要な回復用余裕は実測後にビルドのresource profileへ固定する。JSヒープ上限だけでnative使用量を制限したことにしない。

| 資源 | 初期上限案 |
| --- | --- |
| JS | 現行128KiBヒープ・20KiB stackを出発点に再測定 |
| UI | 生存64ノード、画面深度4、JS字形160字、1文字列1024 UTF-8 bytes |
| 非同期 | アプリ全体8操作、最大16購読。完了通知は別の固定領域を確保 |
| TextSession | 1個、最大1024 bytes |
| KV | 1値4096 bytes、合計16KiB/アプリ。変更ごとのFlash書込頻度を制限 |
| File | 開いているハンドル2、read/write 1回1024 bytes |
| IMU | watch最大50Hz、通常10〜25Hz推奨。高レート要求は別profile |
| 音 | tone 1声、20〜4000Hz、最大2000ms、gain 0〜1。録音は16kHz mono、read最大512 frames |
| I²C / SPI / UART | 1transfer 256 / 1024 / 1024 bytes。待ち既定100 / 100 / 1000ms |
| IR | 最大256 durations、合計200ms、carrier 20〜60kHz |
| HTTP | 同時1要求、送信body4096bytes、header合計2048bytes、受信総量64KiB |
| 無線期限 | Wi-Fi接続15秒、BLE接続10秒、HTTP全体15秒、body read 5秒。最大30秒 |
| その他のPromise | 既定1000ms、最大30000ms。sleepは指定msが終了時刻で、timeoutはms以上を要求 |

HTTPの全体期限はbody読み終わりまで継続し、各read期限との早い方を採用する。独立したイベント購読はtimeoutで終了せず、close/アプリ終了まで有効。keepAwakeは最大60秒で更新可能、toneはduration+500msを既定期限とする。

GPIO割込の上限や録音リング容量など未固定の値はcapability.limitsで公開し、初期化時に決まっていない値を無制限として扱わない。

ESP32-S3はWi-FiとBLEでRF資源を共有し、IDFの共存機構にも組合せ別の制約がある。最初の無線profileはWi-Fiのみ／BLEのみを別々に成立させる。両方有効なprofileは対応設定と実機試験後に提供し、不対応profileで2つ目を要求したらBUSYとする。録音・TLS・JS日本語再構築を含む重い組合せは別途評価する。[IDF v6.0.1 共存ガイド](https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32s3/api-guides/coexist.html)

無線使用中に背景品質を下げる判断はホストが行う。アプリの論理時刻をFPSから推測しない。消費電力・接続速度・センサー精度はAPIシグネチャとは別の性能表で記録する。

## 15. アプリ例（提案API。現行では実行不可）

```js
const p = pocket;
let count = 0;
let output;
let input;
p.app.start({
  async start() {
    const saved = await p.storage.get('count');
    count = saved ? saved.value : 0;
    const screen = p.ui.screen({background: 0x071425ff});
    output = screen.text({x: 16, y: 40, width: 208, height: 24,
      text: String(count), font: 'body', color: 0xf0f8ffff});
    p.ui.push(screen);
    let saving = false;
    input = p.input.onAction(e => {
      if (e.action !== 'accept' || e.phase !== 'press' || saving) return;
      saving = true;
      const next = count + 1;
      p.storage.set('count', next).then(() => {
        count = next;
        output.setText(String(count));
        p.audio.cue('accept');
      }).catch(err => p.ui.toast('保存失敗: ' + err.code))
        .finally(() => { saving = false; });
    });
  },
  stop() { if (input) input.close(); }
});
```

この例は保存成功後に表示を確定する。終了時保存の保証に頼らない。UI生成失敗・startのrejectはホストの起動エラー画面へ戻る。

## 16. 実装段階と対応表

| 段階 | 実装する公開機能 | 現行コードとの接続／条件 |
| --- | --- | --- |
| A: 共通土台 | error、capabilities、session、cancel、lifecycle、input action、基本UI、time、cue | app_session / main / shell / sound。保存・停止・キューの既知課題を先に解消 |
| B: 作る・残す | storage、TextSession、workspace、ログ、動的日本語表示 | srcstore / editor / codeedit / jsconsole / jsfont。教材をユーザー領域から分離 |
| C: 本体を使う | IMU、電池、tone、IR、SD、外部I/O | motion / board / 新規ドライバー。共有バスと電源断の検証 |
| D: PC接続 | USB bridge、分割転送、ログ、停止、実行 | 診断USBと新protocolを分離。PCアダプターは別コンポーネント |
| E: 無線 | Wi-Fi STA＋HTTP、BLE Centralを各単独で提供 | 新規native services、stack/RAM測定、TLS／bond管理 |
| F: 拡張 | 無線共存、BLE Peripheral/HID、録音、必要ならAP・WebSocket・TCP/UDP・ESP-NOW | 個別capabilityとprofile。詳細仕様・資源上限・受け入れ試験を追加 |

すべての名前を最初に実装する必要はない。AをHello World、Bを小さなメモ、Cをペット／水平器で確認する。チュートリアルにはそのビルドでsupported=trueの機能だけを実行可能として掲載し、将来APIは別ページへ分ける。

## 17. 受け入れ条件

1. 旧Hello Worldがlegacyで動き、新API版が同じ表示・操作を行える。数値prop IDをアプリが書かずに済む。
2. 起動・終了100回、途中終了、I/O待ち中ForceStop、例外、Promise連鎖の後にホームへ復帰し、資源・購読・Wi-Fi参照数が増え続けない。
3. 終了後のI/O完了、BLE通知、IME確定が次のアプリに届かない。キュー満杯時に完了Promiseが消失しない。
4. 保存失敗で未保存状態を維持し、空文書を保持する。二面の片側破損→復旧→次回保存の電源断でも正常版を残す。
5. UTF-8境界、字形上限、長い本文、ノード上限、native確保失敗を再現し、誤表示・メモリ破壊・黙った成功がない。
6. 全I/Oで正常・timeout・cancel・close・切断・権限外・上限超過を確認する。外部出力の終了状態がプロファイルどおりになる。
7. IMUの軸・単位・静止値・時刻・欠落数を実機確認。マイク取得時と停止後の表示・資源解放を確認する。
8. Wi-Fiのみ、BLEのみ、両方、TLS接続中、フォント再構築中のRAM最小値／最大連続領域／stack／FPS／入力遅延をcommit・profile付きで記録する。成立しない組合せはavailableに反映する。
9. PC切断・再接続・重複要求・不完全な作品転送で既存作品が変わらない。別アプリの保存領域・許可外の接続先・予約ピンへアクセスできない。

## 18. 根拠と今後決めること

搭載I/OはBMI270、ES8311、マイク、microSD、IR送信、Grove、EXTを前提とする。API名・上限・配置は本プロジェクトの提案で、M5Stack公式APIではない。[Cardputer ADV公式仕様](https://docs.m5stack.com/en/core/Cardputer-Adv)

BLEスタック候補と共存の制約はIDF v6.0.1を基準に確認した。NimBLEはBLE向けの軽量スタックとして記載されているが、本ファームウェアでの実測採用は未実施。[IDF Bluetooth API](https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32s3/api-reference/bluetooth/index.html)

実装時に固定する残件: native回復用のRAM余裕、ポート一覧と電気的制約、SDの復旧方式、証明書・時刻の供給方式、Wi-Fi/BLE profile、PC wire形式、画像／スプライトAPI、任意アプリ向け文字・UIリソース予算の強制経路。後段機能はこの仕様の保証を緩めず、版とcapabilityを増やして追加する。
