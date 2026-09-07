# pocket.ui / pocket.input セルフチェック

`pocketui.js` は `docs/common-api.md` §6 の実装（`main/pocket_ui.c`）を、
実機のログだけで検証するためのアプリ。ソースは 3071 bytes。ゲストは起動時に
ソースを解析するので、この数字自体がゲストヒープのコストになる。

## 読むべきログ

```
UI_CAP {"maxNodes":32,"safeNodes":15,...} input={"actions":"accept",...}
jsfont: slot 2 now holds N glyphs (M bytes)     ← 画面全体で1回だけ
UI_BUDGET 0 OUT_OF_MEMORY                       ← ノード予算が「落ちずに断る」
UI_RESULT pass=15 fail=0
UI_ACTION accept press <timeMs>                 ← ENTER を押すたび
```

`UI_FAIL <name> <code>` が出たら、その名前が下の表の行に対応する。名前には
実際に返ったコードが付く。

## ノード予算 — このアプリが存在する最大の理由

Rust製レイアウトは taffy のノードを**1本の倍々Vec**（1要素 ≒1853 bytes）に
持つので、再レイアウトは**1個の連続ブロック**を要求する。ホスト上で
カウント用アロケータを噛ませて 1〜40 ノードで実測した段差:

| コアノード数 | 再レイアウトの最大単一ブロック |
| --- | --- |
| 15以下 | 1 KiB 未満 |
| 16〜32 | **29,648 bytes** |
| 33以上 | **59,296 bytes** |

ゲストが立った後の実機の最大連続空きは約 23.5 KiB なので、**最初の段差で
すでに届かない**。`pocketjs_idf_rust_alloc` はそこで NULL を返し、Rust は
報告せず abort する（`Rust core panicked` → 再起動）。

だから `maxNodes` は §14 の 64 ではなく 32 で、`safeNodes`（15）以下なら
大きなブロックを一切要求しない。`UI_BUDGET` の行はその境界で
**パニックせずに断る**ことを確かめている。この画面はちょうど 15 ノード。

## 各チェックが証明すること

| 名前 | §6 の規定 | 期待 |
| --- | --- | --- |
| `caps` | §2 機能検出 | ui.basic と input.action が supported、input.text は false |
| `compact` | 日本語8pxのアトラススロットが無い | `UNSUPPORTED`（勝手に12pxで描かない） |
| `badfont` | font は列挙値 | `INVALID_ARGUMENT` |
| `needtext` | TextSpec は text を必須とする | `INVALID_ARGUMENT` |
| `ascii` | small/large は固定ラテンアトラス | `INVALID_ARGUMENT`（豆腐を描かない） |
| `long` | 1文字列 1024 UTF-8 bytes | `LIMIT_EXCEEDED`、**旧表示のまま** |
| `noitem` | ListNode.select は id 参照 | `NOT_FOUND` |
| `onKey` / `textOpen` | 未実装の面は名前を持ちUNSUPPORTEDで失敗する | `UNSUPPORTED` |
| `badheld` | 未知のアクション名 | `INVALID_ARGUMENT` |
| `notheld` | held() は購読なしでも動く | `false` |
| `closed` | 削除済みノード操作 | `CLOSED`（`remove()` は冪等） |
| `repush` | 同じ画面の二重 push | `INVALID_ARGUMENT` |
| `popped` | pop は画面を破棄する | `CLOSED` |
| `budget` | ノード予算はネイティブ確保の前に断る | `OUT_OF_MEMORY` または `LIMIT_EXCEEDED` |

`long` と `ascii` が投げたあとも `Hello, World!` が画面に残っていることを
**目で**確認する。「超過時は旧表示を維持する」はログでは見えない。

## 目で確認すること

- `ようこそ` のトーストが2秒で消える。
- リストは2行表示で3項目。ENTER のたびに ALPHA → BRAVO → CHARLIE と選択が
  動き、CHARLIE で1行スクロールする（`detail` は右寄せ）。
- `SECOND` は一瞬も見えない（push の直後に pop、評価は1ターンで終わる）。
- ESC でホームへ戻り、`APP_STOPPED` のあと `MEM free=` が起動前へ戻る。

## 既知の逸脱

§6 は「表示は矩形でclipする」と書くが、コアの `overflow:hidden` は
**子孫だけ**をシザーし、自分のグリフ列は先に発行される
（`engine/core/src/draw.rs`）。ラベルごとにclip箱を足せば規定どおりになるが
それはラベル1つにつき2ノードで、上の表のとおりノード木こそがこの基板で
唯一払えない予算なので採っていない。はみ出した文字は画面（とリストなら
リスト枠）でclipされる。
