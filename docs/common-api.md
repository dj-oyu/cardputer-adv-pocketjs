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

音楽プレイヤー、教材音声、ペットの短い音声、PCからの音声応答を対象に `audio.playback` を提案した節。**実装しているのはWAV（PCM16／IMA ADPCM）だけ**で、提案のままだった `player` の形（open→info/play/pause/seek/status/onState/close）はそのまま残っている。変わったのは入力の形式と大きさだけ。

圧縮形式については、旧版がここに「この機体では圧縮音声のデコードが成立しないことが計測で確定した」と書いていた。2026-09-08に測り直した結果、その根拠は4つとも成立しない。下の「MP3/Opus/FLACの再測定」に訂正と現在の値を置く。

```ts
pocket.audio.player.open({source:string}, options?:Options):Promise<Player>;
type Player = {
  info(): {codec:"wav/pcm16"|"wav/ima-adpcm";sampleRate:24000;channels:1;
           durationMs:number;seekable:true};
  play():Promise<void>;
  pause():Promise<void>;
  seek(positionMs:number,options?:Options):Promise<void>;
  status(): {state:"ready"|"playing"|"paused"|"ended"|"error";
             positionMs:number;underruns:number};
  onState(fn:(event:{state:string;error:PocketError|null})=>void):Subscription;
  close():void;
};
```

sourceは `app:/` / `assets:/` のWAVファイルで、24kHz・1ch・16bit PCMまたはIMA ADPCM（WAVのブロック配置）。**最大24,576バイト**——ADPCMで約2.05秒、PCM16で約0.51秒。openはファイル全体を1回だけ読み、ヘッダを検査し、再生は始めない。playは出力受付の完了、pauseは停止位置の保持完了を返し、曲の終わりはonStateで通知する。位置は消費したPCM framesから求める。endedからのplayとseekはNOT_AVAILABLE（再生し直すにはopenし直す）。close後はすべてCLOSED。1プレイヤー・1音声で、toneとは排他（tone中のopenはBUSY）。セッション終了時は自動close。

seekは実装している——クリップは全部RAMにあるので、seekable=falseにする理由がない。ただしADPCMはブロック先頭からしか再開できない（ブロック先頭の4バイトが予測器を再初期化する）ので、要求位置を含むブロックの先頭に落ちる。`seekBlockAligned` がそれを言う。pause/resumeも同じ丸めを受ける。

underrunsは常に0で、これは正直な0である。クリップは再生開始前に全部RAMにあり、I²Sへの供給が間に合わなくなる生産者が存在しない。フィールドを残すのは、ストリーミング入力を足したときに0でなくなるから。

出力は本体スピーカー。ミュート中のクリップは長さを保ったまま無音になる（toneと同じ規則）。ミキシングはしない。別アプリへ移っても続くBGMは、ホスト所有のメディアサービスとして別段階の設計。

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

**この節はOpusの採否を決めない。** 16%のCPUと8秒の音を交換するかは、レンダラーが33.3msの予算に対して39.9msを使っている現状と併せた判断で、まだ決まっていない。

**静的な費用と実行時の費用を1つの数で比べないこと。** Opusは再生していない間は0バイトで、BLEは接続を開かないアプリにも16,912バイトの `.bss` を課す。同じ「KiB」でも別種の支出である。

24,576バイトという上限は `app:/` の1ファイル上限そのものである。旧版の8,192は `app:/` ではなく当時の最大連続空き23,552から来ていた。その値が73,728になったので、24,576対73,728は8,192が23,552に対して持っていた余裕とほぼ同じ比になる。断片化していないときだけ通る機能は人の前で落ちる機能になる——上限の根拠をヒープ側に置く規則は変えていない。

実装のコスト（`tools/memlog.py`、対 7c67504）：静的DIRAM **+160バイト**（`pocket_av.c.obj +133`、`sound.c.obj +12`、残りはalignment）、Flash code +4,588バイト。再生中はクリップ1つ分のmalloc（最大24,576バイト）だけが増える。デコードは既存の音声タスクとI²Sをそのまま使い、リングを持たない。IMA ADPCMの表は194バイトのFlash、状態は音声タスクのstack上に20バイト。

デコーダーの検証は `python tools/test_ima.py`（実機不要）。`main/hal/ima_adpcm.h` をホストのgccで同じ行のままコンパイルし、独立に書いた参照デコーダーとサンプル単位で照合し、正弦波の往復SNR（実測32.3dB）を測り、「ブロック単体のデコードが通しのデコードと一致する」——seekとresumeが立っている前提——を確認する。**音そのものの正しさはソフトウェアだけでは確認できない**（`board_capture` はフレームバッファしか見えず、ES8311に何が届いたかは見えない）ので、実機での試聴は物理確認として別に依頼する。

将来: ストリーミング（`audio.streaming`）は未着手。SDは `fs.volume.sd` が扱う別の面で、音源の置き場の上限を動かすのはそちらである。圧縮形式はOpusだけが候補として残っていて、判断は未了。旧版はこの位置に「23.5KiBの制約が動いてから」と書いていたが、その制約は動いた——上の表を書き換えたのは機体ではなく、静的DIRAMを削った作業である。

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
