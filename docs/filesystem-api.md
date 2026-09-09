# ファイルシステムAPI仕様案 v0.1

作成: 2026-09-06。**実装前の提案**。[共通API](common-api.md)の保存・ファイル節を具体化する。公開名は `pocket.fs` に統一し、初稿の `pocket.files` は採用しない。現行srcstoreをファイルシステムと見なさず、進行中のアプリ実装にも変更を要求しない。

## 1. 役割と境界

- `pocket.storage`: ペットの状態や設定など、小さなJSON値の保存。
- `pocket.fs`: ファイル・ディレクトリ、逐次読書き、メディア、ログ、素材の管理。
- `pocket.workspace`: ソース作品のID・名前・版・教材コピー・実行の管理。内部ファイルの配置は公開しない。

どれもアプリIDとセッションに所有者を持つ。FSを使って別アプリのKV、教材の原本、workspaceの管理情報へ迂回アクセスできないようにする。ネイティブアプリ・プレイヤー・PC転送も同じホストのアクセス検査と媒体管理を利用する。

## 2. 仮想マウントとパス

| マウント | 内容 | 初期方針 |
| --- | --- | --- |
| `app:/` | アプリ自身の永続ファイル | アプリ別quota。保存バックエンド導入後に提供 |
| `assets:/` | 同梱アプリの読み取り専用素材 | Flashの借用／逐次読取。JSへ全量コピーしない |
| `sd:/` | ユーザーが許可したSD上のフォルダー | そのフォルダーをアプリの仮想ルートとして公開 |

同じ `app:/memo.txt` でもアプリIDが違えば別ファイル。同じアプリIDの再起動では同じ領域を参照する。sdへのアクセス許可はホストのフォルダー選択で作成し、アプリは物理パスを指定してルートを広げられない。カード識別が確実でなければ差し替え時に以前の許可を自動流用しない。

パスはUTF-8、絶対仮想パスのみ。schemeは小文字、区切りは `/`。NUL、制御文字、バックスラッシュ、`.` / `..` 要素、空の中間要素を拒否し、URLのpercent decodingは行わない。`%2e%2e`は文字どおりの名前であり親参照にはしない。ルート以外の末尾 `/` は1つだけ許して除去する。

初期上限は1要素64 UTF-8 bytes、全パス256bytes、深さ8。`<>:"|?*`と末尾の空白／ピリオドはポータブルな名前として拒否する。Unicode正規化は暗黙に行わず、大小文字同一視の有無はvolume.caseSensitiveで返す。同一視される別表記を新規作成しようとした場合はALREADY_EXISTS。一覧は保存された表記を返す。外部で作られたAPI非対応名は一覧で返すがaccessible=falseとし、別名へ勝手に変換しない。

シンボリックリンク、hard link、実パス取得、POSIX権限、デバイスノードは初版では提供しない。バックエンドがリンクを持つ場合も辿らない。予約ファイル・一時ファイル・内部ジャーナルは一覧から除外する。

## 3. 共通契約と媒体情報

全ての待ちを伴う関数はPromise。共通のOptions、CancelToken、PocketError、Subscriptionは[共通API](common-api.md)に従う。同期なのはキャッシュ取得のvolumesとハンドルの状態取得／closeだけ。

```ts
type FsOptions = Options;
pocket.fs.volumes(): VolumeInfo[];
pocket.fs.onVolumeChange(fn:(volume:VolumeInfo)=>void):Subscription;
type VolumeInfo = {
  id: "app"|"assets"|"sd";
  state: "ready"|"absent"|"unmounted"|"error";
  generation: number;
  readOnly: boolean;
  caseSensitive: boolean;
  capacityBytes: number|null;
  freeBytes: number|null;
  quotaBytes: number|null;
  usedBytes: number|null;
  features: {read:boolean;write:boolean;directories:boolean;seekRead:boolean;
    replace:boolean;atomicReplace:boolean;crashSafeReplace:boolean;
    append:boolean;rename:boolean};
};
pocket.fs.space(path:string,options?:FsOptions):Promise<VolumeInfo>;
pocket.fs.requestFolder(volumeId:"sd",options?:FsOptions):Promise<string|null>;
pocket.fs.pickFile(volumeId:"sd",
  options?:FsOptions&{extensions?:string[]}):Promise<string|null>;
```

`requestFolder` は**許可を作る唯一の入口**で、`sd:` にだけ意味がある。ホストがカード直下のフォルダー一覧を描き、**人が選んだ行がそのまま許可になる**。アプリはフォルダー名を渡せない——引数はvolume idだけで、ホストは列挙した候補以外を許可しない。解決値は仮想ルート `"sd:/"`、人が断ったときはnull（失敗ではない）。カード上の実際のフォルダー名は返さない。人はホスト画面でそれを見ており、アプリは仮想ルート越しにしか到達できないからである。timeoutMsは既定120秒・上限300秒で、期限切れはTIMEOUT。**マウントはこの呼び出しの中でしか起きない。** したがって人が選ぶ前は、カードが刺さっていてもいなくても `volumes()` のsdは `absent` であり、アプリはカードの有無を観測できない。許可はセッション限りで、アプリ終了時にアンマウントとともに落ちる。

`pickFile` は**許可を作らない**。既にある許可の中で、人に1つのファイルを選んでもらうだけの画面である。`requestFolder` との違いはここに尽きる——あちらは認可を作り、こちらは認可の中の便宜を作る。だから**許可が無いときは画面を開かずに `PERMISSION_DENIED` で断る**。許可されていないカードを閲覧させることは、ピッカーをピッカーの迂回路にすることだからである。断る順序も `requestFolder` と同じで、**許可の判定がメディア状態より先**にある（`sd_path.h` の不変条件1。逆にするとエラーの種類がカードの有無を漏らす）。

解決値は `"sd:/フォルダー/曲.mp3"` のような仮想パスで、人が断ったときはnull。`extensions` は `[".mp3",".opus"]` のように**ドット込みの接尾辞**を最大8個で、アプリが言えるのはこれだけである——フォルダー名は渡せず、画面の文言も指定できず、**人が選ばなかった行は返らない**。したがってフィルターは人のための便宜であって、カードの中身への問い合わせではない。照合は大小文字を無視する（`caseSensitive` がsdでfalseなのと同じ理由で、FatFsが長い名前の照合で畳むため）。

フォルダーは行として現れ、Enterで入る。ESCは**まず1階層上がり**、根で初めてキャンセルになる。深さの上限は4で、返せるパスは `FS_MAX_PATH`（256バイト）に収まるように作られている——`fs.open` が拒否するパスを返すピッカーは、そのファイルを最初から出さないピッカーより悪い。timeoutMsは `requestFolder` と同じく既定120秒・上限300秒。表示中にカードが替わったら（世代が動いたら）その場でキャンセルする。

画面そのものは `main/ui/pickmodal.h` にあり、works picker・フォルダーピッカー・ファイルピッカーで共有する。行・カーソル・スクロール窓・期限だけがそこにあり、**選択が何を意味するかは各呼び出し側が持つ**——許可を作るのか、パスを返すのか、参照を返すのかは、間違えたときに漏れる場所が違うからである。窓は24行で、ディレクトリ全体をRAMに載せない。

volumesは新しい媒体アクセスを発生させない。未対応volumeは返さず、`fs.volume.app` / `fs.volume.assets` / `fs.volume.sd` のcapabilityで検出する。stateやfeatureは認可とは別。spaceは媒体へ問い合わせて更新した値を返す。freeBytesは物理volumeの空き、quotaBytes/usedBytesは自アプリの上限と使用量。nullは取得不能で0と区別する。

generationはマウント／抜去・再挿入ごとに変わる。ハンドルと一覧cursorは世代を保持する。抜去時は保留I/OをDISCONNECTEDで終了し、再挿入しても旧ハンドルを有効にしない。ホストが明示的に再マウントする。アプリへformat、partition操作、任意mount、Flash生読書きは提供しない。マウント失敗で自動formatもしない。

**この機体のSD（実測 2026-09-08）。** microSDはSPI接続で、EXTバスとSPI信号を共有する（CS=12、MOSI=14、CLK=40、MISO=39。LCDは別系統なのでカードの転送が画面を止めることはない）。64GB SDHC・512バイトセクタを400kHzでマウントし、ルート列挙とファイルの作成・書込・削除まで確認した。資源は**マウントで7,032バイト、ハンドル2本でさらに1,640バイト**——いずれもheapで、アンマウントすると全量返る（3周でリーク0）。**カードに触れないアプリはこれを一切負担しない。** 静的DIRAMではないので、`.bss` として全アプリに課金される種類の費用ではない。

**カード検出ピンが無い。** ピンマップにcard detectは無く、抜去はイベントとして受け取れない。観測できるのは「次のコマンドが失敗した形」だけである。したがって `fs.volume.sd` の `available` は**何かを試みた瞬間の観測値**であり、ホストはポーリングしない——ポーリングは、ハードウェアが報告できない生存性を発明することになる。アプリ側の設計上の帰結は、`available=true` を見てから実際に使うまでの間にカードが消えていても、それは異常ではなく通常の経路だということ。抜去を検知した時点で保留I/OはDISCONNECTED、ハンドルとcursorは無効化し、**grantも落とす**（差し替えられたカードは、その人が許可したフォルダーではない。再挿入はホストの再マウントと選択のやり直しを要する）。

## 4. メタデータ・一覧・ディレクトリ

```ts
type Entry = {
  name:string; path:string; kind:"file"|"directory";
  sizeBytes:number|null; modifiedUnixMs:number|null;
  revision:string; accessible:boolean;
};
pocket.fs.stat(path:string,options?:FsOptions):Promise<Entry>;
pocket.fs.list(path:string,options?:FsOptions & {
  limit?:number; cursor?:string;
}):Promise<{entries:Entry[];nextCursor:string|null}>;
pocket.fs.mkdir(path:string,options?:FsOptions & {recursive?:boolean}):Promise<void>;
pocket.fs.remove(path:string,options?:FsOptions & {ifRevision?:string}):Promise<void>;
pocket.fs.rename(from:string,to:string,options?:FsOptions & {ifRevision?:string}):Promise<void>;
pocket.fs.copy(from:string,to:string,options?:FsOptions):Promise<Entry>;
```

statの未存在はNOT_FOUND。ディレクトリのsizeはnull。時刻未同期・媒体に値がない場合のmodifiedUnixMsもnull。revisionは内容hashではなくホストの不透明な版トークン。同一マウント世代・ホスト経由の変更に対して有効で、再起動後の比較用途には使わない。外部からの同時更新を検出できないバックエンドは共有書込を禁止する。将来USB mass storageを有効にするときはホストFSをアンマウントして所有権を切り替える。

listは直下の項目だけを返す。既定8件、最大16件。順序はバックエンド列挙順で、全件ソートしない。nextCursorは同一アプリ／パス／媒体世代専用で、30秒の無操作で期限切れ。1アプリ2個まで。変更をホストが観測したディレクトリのcursorはSTALE_CURSORとし、途中一覧を完全なsnapshotとは扱わない。nextCursor=nullで列挙終了。大きなSDの全曲を1つのJS配列へ載せる設計にしない。

mkdirは既定recursive=false。親がなければNOT_FOUND。既存ディレクトリなら成功、同名ファイルならALREADY_EXISTS。recursive=trueも深さ上限内だけで、途中まで作成後の失敗はロールバック保証なし。

removeはファイルまたは空ディレクトリのみ。非空はNOT_EMPTY、ルートはPERMISSION_DENIED。再帰削除を初版に入れない。ifRevisionを指定したら対象検査と削除を同じホストロックで実施し、不一致はCONFLICT。

renameは同一volumeのファイル／ディレクトリに限り、上書きしない。移動先があればALREADY_EXISTS、異なるvolumeならCROSS_DEVICE。自分の子へのディレクトリ移動を拒否する。開いている対象または配下のハンドルがある場合はBUSY。媒体の電源断耐性とは別に、ホスト観測上は操作完了まで中間状態を公開しない。

copyはファイルだけで、上書きしない。別volumeも許可するが、元のreadロックと先の新規作成ロックを取る。nativeの固定サイズバッファで分割転送し、完了まで宛先名を公開しない。失敗・cancel時は一時版を破棄し、元を変更しない。全量RAMへ読まない。異volumeのmoveは提供しない。必要なアプリはcopy成功確認後に元をremoveするが、2操作は単一トランザクションではない。

## 5. ファイルハンドル

```ts
pocket.fs.open(path:string,options:FsOptions & {
  mode:"read"|"create"|"replace"|"append";
  ifRevision?:string;
  durability?:"synced"|"crash-safe";
}):Promise<File>;
type File = {
  read(maxBytes:number,options?:FsOptions):Promise<Uint8Array|null>;
  write(data:Uint8Array,options?:FsOptions):Promise<number>;
  seek(offsetBytes:number,options?:FsOptions):Promise<number>;
  tell():number;
  flush(options?:FsOptions):Promise<void>;
  commit(options?:FsOptions):Promise<Entry>;
  close():void;
};
```

| mode | 開く時 | データ公開 | 終了 |
| --- | --- | --- | --- |
| read | 既存ファイル必須 | 変更なし | close |
| create | 対象名が未存在必須。一時版を作る | commit成功時に新規公開 | 未commitのcloseは破棄 |
| replace | 既存ファイル必須。旧版を保持して一時版を作る | commit成功時に置換 | 未commitのcloseは破棄 |
| append | 既存ファイル必須。末尾のみ追記 | write受付ではなく書込完了時に可視 | flush後にclose。closeで追記を巻き戻さない |

読み手は同一ファイルを複数開けるが、writerは1つ。初版はreadとwriterの同時openもBUSYとし、copyやプレイヤーの読取との競合を単純にする。writer保持中はrename/removeもBUSY。read時のifRevisionは開く際の検査、replace/appendは検査と書込権取得を一体で実施する。createにはifRevisionを指定しない。

- read: 要求1〜1024bytes。EOFのときだけnull。短い読取は正常なので呼出側が繰り返す。0bytesのファイルは最初からnull。mode不一致はINVALID_ARGUMENT。
- write: 1回0〜1024bytes。成功なら入力長を返し、0bytesは副作用なし。成功時は全量転送済みだが媒体への永続化はflush/commitまで保証しない。呼出受付時にバッファをコピーする。失敗時の追記は一部が残り得るのでerror.outcomeとbytesTransferred（判定不能ならnull）を返し、失敗したwriterを閉鎖状態にする。
- seek: 初版はreadのみ、先頭からの絶対位置で0〜file size。終了位置への移動は可能。範囲外はINVALID_ARGUMENT。ファイルの穴あけや書込seekは提供しない。操作中は同じハンドルの別I/OをBUSYにする。
- tell: 最後に完了したI/Oのbyte位置。read/create/replaceは0開始、appendは既存末尾開始。保留I/O中は完了前の値。失敗後は位置を信頼せず、ハンドルを開き直す。
- flush: 書込バッファをバックエンドへ同期する。create/replaceは一時版の同期で公開ではない。readではINVALID_ARGUMENT。同期成功と電源断後の保証は区別する。
- commit: create/replaceのみ。同期→検証→公開の後にresolveし、自動的にハンドルを閉じる。二度目はCLOSED。0bytesのcommitも正当な空ファイル。部分版を公開しない。
- close: 冪等で、新規操作とcallbackを即時無効化。保留I/Oをcancelし、native資源の停止完了はホストが追跡する。同期voidなのでflushの代用にしない。アプリ終了時も自動close。

appendはセンサーログ等の用途で、原子的な文書保存には使用しない。電源断で末尾が欠け得るため、レコード長・sequence・CRC等はログ形式側で定義する。durability=crash-safeのappendは初版ではUNSUPPORTED。

## 6. 原子的保存と媒体ごとの保証

用語を区別する:

| feature | 保証 |
| --- | --- |
| replace | 一時版を作るAPIをバックエンドが実装する |
| atomicReplace | 通常運転中、読取／一覧には旧版または新版だけを見せる |
| crashSafeReplace | プロセス停止・電源断から回復した後も、旧版か検証済み新版が残る |

replaceはatomicReplace=trueのvolumeだけで提供する。durability既定値はcreate/replaceでcrash-safe、appendでsynced、readでは指定不可。crash-safe非対応時はUNSUPPORTEDで、黙って保証を下げない。SDで弱い保証を受け入れる用途は明示的にsyncedを指定する。途中のカード抜去で媒体自体が壊れた場合までデータ保持を保証しない。

PC由来の一般的なSDファイルシステムにrenameがあるだけでcrashSafeReplace=trueと宣言しない。バックエンドごとにジャーナル／二面・復旧規則を実装して電源断試験を通すまでfalse。

**`sd:` の具体：`crashSafeReplace=false`、そして既定のopenは断られる。** FATにジャーナルは無く、この機体で電源断試験も通していないので、上の規則どおりfalseを宣言する。`atomicReplace` は一時ファイル＋renameで満たすのでtrue。**帰結として、既定のdurability（create/replaceではcrash-safe）を使った `open()` はUNSUPPORTEDで失敗する。** SDへ保存するアプリは `durability:"synced"` を明示的に指定しなければならない。

**実装（2026-09-08）。** create/replaceは対象の隣の一時ファイル `<name>.pkt-tmp` へ書き、commitで `rename` して公開する。FatFsの `rename` は既存の宛先を拒むので、replaceは `unlink` してから `rename` する二段になる。この隙間は**このAPIからは観測できない**——1回のJSターンの中で、他の操作が始まれない——ので `atomicReplace=true` は満たす。隙間での電源断こそが `crashSafeReplace=false` の意味するところである。`.pkt-tmp` で終わる名前は**アプリの名前空間から除外**する（一覧から隠すだけでなく、statもopenもINVALID_ARGUMENT）。隠すだけでは、公開直前のrenameの宛先にアプリが座れてしまう。

これは驚きだが、意図した驚きである。黙って弱い保証へ落とすほうが安全に見えるのは、壊れるまでの間だけで、電源断で人の作品が消えるのはSDに保存した側である。**最初の呼出しで断られるのは安く、破損してから気づくのは高い。** ただし断り方には条件がある——エラーはUNSUPPORTEDを返すだけでなく、**受け付ける値（`durability:"synced"`）をメッセージに含める**。仕様書を読み直させる `UNSUPPORTED` と、直し方を教える `UNSUPPORTED` は、文字列1個分しか違わない。replace失敗は、公開前ならnot-applied、公開確認後ならapplied、媒体応答を失って判定できなければunknown。unknownのあとに自動再試行してユーザーの変更を上書きしない。

一時版の容量もquotaへ計上する。置換には旧版＋新版＋管理情報が同時に収まる必要があり、満たさなければNO_SPACE/QUOTA_EXCEEDEDで旧版を維持する。GCを呼んでFlash空きを作れると仮定しない。起動時復旧はホストが公開前に行い、孤立一時版は有効な旧版を確認してから回収する。

## 7. 小さいテキスト用の便利関数

```ts
pocket.fs.readText(path:string,options:FsOptions & {maxBytes:number}):Promise<string>;
pocket.fs.writeText(path:string,text:string,options?:FsOptions & {
  mode?:"create"|"replace";
  ifRevision?:string;
  durability?:"synced"|"crash-safe";
}):Promise<Entry>;
```

readTextは最大8192bytes、maxBytes必須。途中で上限超過した場合は切り詰めずLIMIT_EXCEEDED。不正UTF-8はCORRUPT_DATA。BOM／改行は自動変換しない。writeTextはUTF-8最大8192bytes、mode既定create、内部でopen→逐次write→commitを行い、自動上書きしない。mode=replaceなら既存対象必須。JSON用のread/writeはこの上にアプリが実装できるが、小さな状態はstorageを使う。

```js
// 提案APIの例。空ファイルも正常に保存・読込できる。
const fs = pocket.fs;
await fs.mkdir('app:/notes');
const saved = await fs.writeText('app:/notes/旅.txt', '海を見た。', {mode: 'create'});
await fs.writeText(saved.path, '海と波を見た。', {
  mode: 'replace', ifRevision: saved.revision
});
const text = await fs.readText(saved.path, {maxBytes: 4096});
```

上例は新規ファイルが存在しない初回を想定する。再実行時のALREADY_EXISTSは呼出側で処理し、既存メモを暗黙に初期化しない。

## 8. 上限・性能・エラー

初期案: JSで開けるFileは2件、list cursorは2件、1チャンク1024bytes、パス256bytes、app quota 64KiB、最大ファイルサイズはmin(volume制限, quota余裕, 2GiB−1)。assetsとSDの読取にapp quotaは適用しない。SD書込は別の許可quotaをホストが設定する。2GiB−1はAPIの算術上限で、そのサイズのファイルを本体Flashへ置ける意味ではない。

nativeプレイヤーやcopyも同じ全体資源仲介で数え、JS用2件とは別に無制限のハンドルを作らない。入力応答を維持するため大きなコピー／一覧走査をnative workerで分割する。全体のFS同時操作は2件、追加要求はBUSY。同期的な長時間SD待ちをJSタスクへ持ち込まない。

既定期限はメタデータ・1チャンク1000ms、copy/commit/テキスト便利関数は10000ms、最大30000ms。共通APIの既定1000msより本節を優先する。大容量copyは明示的な期限内で完了しなければTIMEOUTとなり、一時版を破棄する。長時間のバックグラウンドコピーは将来のjob APIに分離する。

共通エラーに追加: ALREADY_EXISTS、NOT_DIRECTORY、IS_DIRECTORY、NOT_EMPTY、READ_ONLY、NO_SPACE、QUOTA_EXCEEDED、CROSS_DEVICE、STALE_CURSOR。媒体なし／抜去はDISCONNECTED、FS破損はCORRUPT_DATA、形式未対応はUNSUPPORTED。認可前に他領域の存在を返さずPERMISSION_DENIEDとする。

各処理はアプリ終了時にキャンセルし、旧世代からの完了結果を捨てる。途中まで適用されたappend/remove/mkdirについては、キャンセルがロールバックを意味しない。確定点を越えた場合は可能なら成功を返し、判定不能ならoutcome=unknownとする。

## 9. メディア・Docs・PC転送との接続

プレイヤーはfsの許可検査・readハンドルをnativeで取得する。JSのreadループを音声供給の必須経路にしない。ファイルのbyte seekとMP3/Opus等の時間seekは別物で、fs.seek対応を根拠にplayer.seekable=trueとしない。

音楽一覧はfs.listをページ単位で読み、metadata索引は必要なら別の小さな永続ファイルへ作る。大きなジャケット画像やDocs索引を無制限にJSへ読み込まない。assetsは読取専用で、教材編集はworkspaceのコピー操作を経由する。

PC転送はcreate/replaceの一時版へ分割writeし、長さ・hash・対応版を検証してからcommitする。未完了ファイルを作品一覧やプレイヤーに公開しない。転送中断で既存ファイルを消さない。PCへSDをmass storageで共有する方式は別機能であり、初版bridgeに含めない。

## 10. 実装順序と受け入れ試験

1. volume／path／所有者／世代／errorを定義し、assetsのstat/list/read/seekで読取経路を通す。
2. SDはread-onlyで一覧・メディア読取・抜去を検証する。
3. appの永続バックエンドを導入してcreate/replace/commit、空ファイル、quota、電源断復旧を検証する。
4. SDのmkdir/copy/rename/remove/appendを段階追加し、保証レベルをvolumeに反映する。
5. workspace・プレイヤー・PC bridgeを同じホスト層へ接続する。

**実行されたものとされていないもの（2026-09-09）。** 実機で動いたのは**ドライバ**（マウント・アンマウント・ルート列挙・作成/書込/削除）と、2026-09-08時点の `stat`/`list` まで。**その後に足した面は一度も実機で走っていない**——picker、open、書込、commit、mkdir/remove/rename、volumes/space、pump、nativeのranged read。ホスト側で検査できるのは認可規則だけ（`tools/test_sd.c`）で、残りはカードが要る。`apps/pocketfs/README.md` にカードと人が要る5手順を書いた。

**400kHzが決めること。** バス帯域は約50,000バイト/秒。24kHzモノラルPCM16は48,000バイト/秒なので、**カードからの実時間ストリーミングは成立しない**（Opusなら約1/30で余裕がある）。2,048バイトの1回読みは約41msで、これは呼んだフレームの中で消える時間である。以上は**計算であって実測ではない**——この機体でカード読み出しを計時した者はまだいない。速度を上げるのは編集ではなく測定で、`main/pocket/sd_media.c` の `max_freq_khz` に理由を書いた。

**`onVolumeChange` は配信される（2026-09-09）。** sd:の状態は許可の瞬間と抜去の観測時に動くが、その2箇所はどちらもネイティブのfs呼び出しの内側で、そこからゲストのcallbackを回すと、いま処理中のハンドルをlistenerが閉じられてしまう。そこで配信は `pocket_fs_pump()`（`app_tick()` から毎フレーム）へ寄せた:状態が最後に通知した値と違えば1回だけ配る。**1フレーム内で動いて戻った変化は通知しない**——§3が状態を観測値と定めている以上、誰も観測していない。picker表示中はゲストがtickされないので、許可は**人が選んだ次のフレーム**、すなわちアプリが最初に行動できるフレームで届く。

**進捗（2026-09-08）。** 1と2は完了。3のうちappのcreate/replace/commit・空ファイル・quotaは動作（電源断復旧は未検証）。4のSDはread/write両方が通り、`requestFolder`・stat・list・open(read/create/replace/append)・readText/writeText・mkdir/remove/renameを実装、`fs.volume.sd` は `supported=true`。未実装は**SDを含むcopy**（1ターンで400kHzの全量転送をすると入力が止まるため、将来のjob APIへ回す。UNSUPPORTEDで断る）と**crashSafeReplace**（電源断試験が未実施なのでfalseのまま）。`caseSensitive` はSDだけ **false**——FatFsは長い名前の照合で大小文字を畳むので、アプリが別名だと思う2つが同じ1ファイルになる。

ファイルシステム実装の選定は別途行う。現行storageは0x590000から2496KiBで、srcstoreの16スロット（合計384KiB）も使用する。同じ領域へFSをそのままformatしない。既存スロットを予約して残りに載せるか、バックアップと明示的な移行を行うかを決めてから導入する。API要求だけを理由にSKK辞書2MiBやアプリ3MiBを縮めない。

必須試験:

- 不正パス、名前上限、日本語名、大小文字衝突、ルート削除、他アプリ領域へのアクセス。
- 0bytes、EOF、短いread、seek先頭／末尾、UTF-8境界と破損、metadata時刻なし。
- 1000件以上の一覧でページ上限・cursor無効化・RAM一定、ディレクトリ変更中の再開。
- 読取／書込／rename競合、quota・物理容量不足、一時版分の不足、失敗後の旧版保持。
- create/replaceの各段階で電源断、復旧直後の再保存、空ファイルの置換。
- append途中の失敗、copy途中cancel、commit直前／直後のキャンセルとoutcome。
- SD抜去・別カード挿入で旧ハンドル無効化、破損媒体で自動formatしないこと。
- アプリ終了100回、プレイヤー／PC転送併用時にハンドル・バッファが残らないこと。

未採用: 再帰削除、任意format、ファイルwatch、書込seek、メモリマップのJS公開、POSIX互換、圧縮アーカイブ展開、別volumeの原子的move。必要なアプリが現れた時点で能力と失敗契約を追加する。
