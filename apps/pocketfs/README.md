# pocket.fs セルフチェック

`pocketfs.js` は `docs/common-api.md` §7 と `docs/filesystem-api.md` の実装
（`main/pocket_fs.c`）を、実機のログだけで検証するためのアプリ。ソースは
**2989 bytes**。ゲストは起動時にソースを解析するので、この数字自体がゲスト
ヒープのコストになる（`apps/pocketui/README.md` と同じ理由で短く書いてある）。

シェルのアプリ一覧には**入っていない**。`main/shell.c` と `main/main.c` は
別セッションが編集中で触れないため、Playground（コード編集画面）に貼って実行
する。`pocketui.js` と同じ扱い。

## 読むべきログ

```
FS_CAP {"maxPathBytes":256,...,"durability":"synced"} {"id":"app","state":"ready",...}
FS_QUOTA <n> QUOTA_EXCEEDED       ← quotaを使い切って「落ちずに断る」
FS_RESULT <pass> fail=0
```

`FS_FAIL <名前> <実際に返ったコード>` が出たら、その名前が下の表の行。

## 各チェックが証明すること

| 名前 | 証明する主張 |
| --- | --- |
| `cp` | `fs.volume.app` / `fs.volume.assets` は supported かつ available、**`fs.volume.sd` は supported=false**。ピン表があるだけでSDをsupportedと名乗らない（`main/pocket_fs.c` の capability 節） |
| `vol` | `volumes()` は2件（sdは列挙しない）。`caseSensitive=true`、`atomicReplace=true` かつ **`crashSafeReplace=false`**、assetsは `readOnly` で `quotaBytes` は `null`（0ではない） |
| `app:/a/../b` | `..` は親参照として解釈されず、要素として拒否される |
| `app:/a\b` | バックスラッシュはポータブルな名前として拒否 |
| `sd:/a` | 未認可領域は存在を漏らさず `PERMISSION_DENIED` |
| `assets:/%2e%2e/x` | percent decodingを**しない**。`%2e%2e` は6文字の名前なので、親に上がるのではなく `NOT_FOUND` |
| `ent` | `writeText` が `Entry` を返す。`海を見た。` は15 bytes（UTF-8のバイト数で数える） |
| `2nd` | `mode:'create'` は既存を暗黙に上書きせず `ALREADY_EXISTS` |
| `old` | `ifRevision` 不一致は `CONFLICT` |
| `rev` | 置換で revision が動く |
| `txt` | 日本語の往復。BOM・改行を変換しない |
| `dur` | **`durability:'crash-safe'` は `UNSUPPORTED`**。§6の既定値からの意図的な逸脱で、理由は `pocket_fs.c` の `take_durability()` に書いてある |
| `max` | `readText` は `maxBytes` 超過を切り詰めず `LIMIT_EXCEEDED` |
| `nil` | 0 byteの `commit` は正当な空ファイル。`readText` が `''` を返し、`NOT_FOUND` とは別物 |
| `str` | `assets:/hello.js` を 1024 bytes ずつ**逐次**読む。ファイル全体をJS文字列に載せない |
| `eof` | EOFのときだけ `null`、`tell()` が末尾、`seek(0)` 後に先頭から読み直せる |
| `cls` | `close()` は冪等、以後の操作は `CLOSED` |
| `pg` / `cur` | `list` は `limit` でページを切り、`nextCursor` で続きが読める |
| `ne` | 非空ディレクトリの `remove` は `NOT_EMPTY` |
| `copy` / `ren` | volumeをまたぐ `copy`（assets→app）と、同一volumeの `rename` |
| `bsy` | writerが開いている間の `remove` は `BUSY` |
| `app` | `append` は既存末尾から始まり（`tell()===2`）、`flush()` で公開される |
| `quo` | quotaを使い切ると `QUOTA_EXCEEDED`。パニックではなく拒否 |

## 実行するとFlashに残るもの

`app:/k`（`hello.js` のコピー）と `app:/g`、`app:/e`、`app:/n` は**わざと消さず
に終わる**。永続することがこのAPIの要点なので、次回の起動時に先頭のクリーン
アップループが消す。`app:/q*` はquota試験の残骸なのでその場で消す。

## 一読が要る数字

- `app:` は `storage` パーティションの **0x60000 から 256 KiB**。その手前
  384 KiB は srcstore の16スロットで、`pocket_fs.c` の `_Static_assert` が
  重なりを build 時に止める。
- 1ファイルは最大 **24576 bytes**、アプリのquotaは **65536 bytes**。quotaは
  **4096 byteのセクター単位**で課金され、1ファイルにつきメタデータ用の1
  セクターが余分に要る（`FS_CAP` の `allocationBytes`）。
- 名前索引とブロック表は**Flash側のinode**にあり、RAMに常駐するのは
  確保ビットマップ2語とオブジェクト1件あたり6 bytesだけ。索引全体で
  **504 bytes**、しかも**最初のファイル操作のときだけ**確保して `app_stop()`
  で返す。値は `pocket_fs.c` の `_Static_assert` でビルド時に固定。
  内訳は 336 bytes がデコード済みinodeのwindow 3枚（`FS_WINDOW` で調整可）、
  144 bytes がダイジェスト、16 bytes がビットマップ。
  writerを開いている間だけ 4072 bytes のステージングバッファが1本増える。
  セッションをまたいで常駐するのは `.bss`+`.data` の **481 bytes** だけ。
- 代わりに名前の解決はFlash読みを伴う。1要素につきinode 1件（約120 bytes）で、
  ハッシュと親で絞ってから読むので通常は候補1件。`list` は返す件数だけ読む。
  `open` した後の `read()` はinodeのブロック表をハンドルが持つので追加の
  読みは無い。**このFlash読みの実レイテンシは未実測。**
- `read(maxBytes)` のホスト側コストは最大1024 bytesのスタックバッファ1本だけ。
  アプリが持ち帰るのは要求したバイト数の `Uint8Array` で、ホストは何も残さない。
- 書き込みはブロック（4072 bytes）が埋まったときだけFlashに触る。4 KiBの消去は
  実測されていないが数十msのオーダーで、1フレームの予算を超える。大きな保存は
  フレームをまたいで分けるか、落ちるフレームを許容する。
