# UIノード予算のホスト検証ツール

Rust製UIコア（`.cache/pocketjs/engine/core`）とRGB565レンダラを**ホストで走らせ、レイアウトが要求する連続メモリを実測する**ためのツール群です。焼く前に「この画面はこの基板に載るか」を答えます。

存在理由はひとつの事実です。**コアはメモリ不足を報告せずabortします**（`pocketjs_idf_rust_alloc` がNULLを返し、Rustが `handle_alloc_error` で落ちる → `E pocketjs_ui_core: Rust core panicked` → 再起動）。落ちる大きさも、どこで段差になるかも、C側からは一切見えません。実際に `pocket.ui` の25ノード画面が最初の描画でこれを踏み、原因の特定にこのツールを書きました。

Rustツールチェインが要ります。**`tools/build_native.sh` がWSLで使っているものと同じ**（`rustup` の既定toolchainで可。S3用のクロスは不要、ホスト向けにビルドします）。`.cache/pocketjs` が要るので、先に `python tools/prepare_dependencies.py` を通しておくこと。フォントアトラスは `build.rs` が `tools/make_font.py` を呼んで生成するので、WSL側に `python3` も要ります。

```sh
wsl
cd tools/uibudget
cargo run --release --bin sweep     # 段差はどこか
cargo run --release --bin shape     # 段差は何で決まるか
cargo run --release --bin screen    # この画面は載るか
```

## 一覧

| ファイル | 役割 | 所要時間 |
| --- | --- | --- |
| `src/bin/sweep.rs` | **段差の位置。** ノードを1〜40個並べて再レイアウトし、アロケータが要求した**最大の単一ブロック**を出す。実機の最大連続空き（既定23,552バイト）に載るかを `fits` 列で示す | 即時 |
| `src/bin/shape.rs` | **段差が何でスケールするか。** 総数・1親あたりの子数（fanout）・深さを独立に振り、ブロックサイズがどれに従うかを見る。答えは**総数だけ**で、形も深さも効かない | 即時 |
| `src/bin/screen.rs` | **この画面は載るか。** `main/pocket_ui.c` が `apps/pocketui/pocketui.js` のために組む木を、要素ごとに積み上げながら再現する。実機と同じ上限を持つアロケータの下で走るので、**落ちる段が落ちる**。第2引数でノードを足して崖の向こうへ押し出せる | 即時 |
| `src/lib.rs` | 3つが共有するもの: 実機同様に上限を持つグローバルアロケータ、`app_start_test()` と同じ3スロットのフォントを積んだ 240x135 のコア、`board.c` の1本きりのストリップバッファで回す `app_tick()` の描画ループ | — |

3つで確かめているものが違います。

```
shape.rs  … 「何がコストを決めるか」  総ノード数。形も深さも関係ない
sweep.rs  … 「その段差はどこか」      16ノードで29,648、33ノードで59,296
screen.rs … 「うちの画面は越えるか」  実機と同じ上限で実際に組んで描く
```

## 測定結果（`sweep`、このcommit時点）

```
largest free block assumed: 23552 bytes
 nodes  taffy    biggest block     fits
     1      2             2048      yes
    16     17            29648       NO
    33     34            59296       NO
```

taffyがノードを**1要素≒1853バイトの倍々Vec1本**に持つので、再レイアウトは**1個の連続ブロック**を要求し、そのサイズがノード数で段になります。

| taffyノード数 | 再レイアウトの最大単一ブロック | 実機（最大連続23.5KiB）で |
| --- | --- | --- |
| 16以下 | 2,048バイト | 載る |
| 17〜33 | **29,648バイト** | **載らない** |
| 34以上 | **59,296バイト** | **載らない** |

`shape` によれば、これは**総数だけ**の関数です。1親に16個ぶら下げても4段の木に散らしても同じ:

```
 total  fanout   depth    biggest block
    15       2       4             2048
    15       8       2             2048
    16       2       4            29648
    16      16       2            29648
    32      16       2            29648
    33       4       3            59296
```

**taffyノード数 = ルート + 「rootに繋がっていて、かつ空でないテキスト」のノード数**です。空文字列のテキストノードはtaffyの木から外れます（`layout.rs` の `build()` が `None` を返す）。`main/pocket_ui.c` の `layout_block()` は生存ノードを全部数えるので**多めに見積もり、崖の手前で断ります**。ガードが間違えるならこの向きでなければいけません。

## いつ何を走らせるか

| 局面 | 走らせるもの |
| --- | --- |
| 画面にノードを**足した**（ラベル1つ、リストの行1つ） | `screen`。第2引数で足した数を渡し、`biggest block` が段差を跨がないことを見る |
| **`Rust core panicked` で再起動した**、`frames=0` で落ちる | `screen`。落ちた段がそのまま原因。落ちなければレイアウト以外を疑う（下の「前提と限界」） |
| `.cache/pocketjs` を**更新した**（`prepare_dependencies.py` のrevisionを上げた） | `sweep` と `shape` の両方。段差の位置も、何でスケールするかも、taffyのバージョンの性質であって契約ではない。`main/pocket_ui.c` の `layout_block()` と CLAUDE.md の表を実測に合わせ直す |
| `maxNodes` / `safeNodes` を**変えたい** | `sweep`。`safeNodes` は最初の段差の手前、`maxNodes` は最初の段差の内側の最後 |
| 新しいUIを**設計している** | `screen` の `build()` を書き換えて自分の木にする。`create_node` / `set_prop` / `set_text` / `insert_before` を順に並べただけなので、C側の呼び出し順をそのまま写せる |

## 使い方

```sh
# 段差の表。POCKETUI_CAP は fits 列の判定だけに使う（表は必ず全部出る）
cargo run --release --bin sweep
POCKETUI_CAP=40000 cargo run --release --bin sweep

# 何でスケールするか
cargo run --release --bin shape

# pocketui.js の画面を1段ずつ。実機と同じ23,552バイト上限で走る
cargo run --release --bin screen
cargo run --release --bin screen 6          # 6段目だけ
POCKETUI_CAP=none cargo run --release --bin screen

# 崖の向こうへ押し出す（実機の落ち方の再現）
cargo run --release --bin screen 9 3
```

最後のものが出すのは、実機が出したものと同じです:

```
memory allocation of 29648 bytes failed
```

`screen` を上限なしで走らせたときの出力:

```
step 0: a bare screen                                 1 nodes  block  18240  words    8  sw 17
step 6: + a list                                     13 nodes  block  18240  words  252  sw 55
step 9: + a toast (everything pocketui.js builds)    15 nodes  block  18240  words  315  sw 68
```

`block` の18,240は**フォントアトラス**（`font_large`、95字 x 12x16セル）で、レイアウトではありません。起動時に1回だけ確保され、実機でも通っています。15ノードの画面はレイアウトに大きなブロックを要求しません。

## 前提と限界

**これはレイアウトのアロケータのモデルであって、実機のヒープのモデルではありません。** 通ったからといって実機で通る保証にはなりません。

- **モデル化しているのは「1個の連続ブロックの大きさ」だけ**です。上限（`POCKETUI_CAP`、既定23,552）は実機が `app: MEM ... largest=` で報告した最大連続空きの実測値ですが、**断片化そのものは再現していません**。ホストのアロケータは上限以下なら必ず成功します。実機は同じ大きさでも空いていないことがあります。
- **フォントアトラスの再構築を追いません。** `jsfont.c` は新しい文字が出るたびにスロット全体を作り直し、その瞬間は完成後のアトラスの約3倍が生きています。ここでは最終形を1回積むだけです。**小さなアプリの最大の単発確保はこちらであることが多く**、レイアウトが通ってもアトラスで落ちることはあります。
- **ゲストのJSヒープを持ちません。** 実機ではQuickJSが100KiB前後を先に取っていて、その残りが上の `largest` です。ソースを増やせば `largest` は下がります。ここには反映されません。
- **PIEカーネルを通りません。** `main/render_accel.c` はC（とインラインasm）なので、`NoPpa` で全ops をRustのソフトウェア経路へ落としています。`sw` 列はその本数で、実機の `PAINT` 行の `software=` とは一致しません。アクセラレータ側のバグはここでは出ません（それは `tools/pie/`）。
- **絵は見ていません。** 日本語グリフは中空の四角で代用しています（形はレイアウトの確保量に効きません）。表示が正しいことはこのツールでは分かりません。
- **taffyのバージョンに依存した数字です。** 1853バイトも、16と33の段差も、`taffy 0.11` の性質であって契約ではありません。`.cache/pocketjs` を更新したら測り直すこと。
- `Cargo.lock` は追跡しています（taffyのバージョンが数字そのものなので）。`cargo update` を通したら、上の表を測り直してから通すこと。数字が動いたら `main/pocket_ui.c` の `layout_block()` と CLAUDE.md を直すのが正しい対応で、ツールを疑うのは後です。

関連: [docs/hardware-constraints.md](../../docs/hardware-constraints.md)、`main/pocket_ui.c` の `layout_block()`、`apps/pocketui/README.md`。
