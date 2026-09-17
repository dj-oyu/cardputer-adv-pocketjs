# DESK CLOCK — オーバーレイアプリの最初の一つ

`docs/api/common-api.md` 3.1 の実装。時計そのものは題材で、作っているのは**弁**のほう。

## これは何であって、何ではないか

ホーム画面の背景の上に、時刻の小さな箱を出し続ける。背景の代わりに走る通常アプリと違い、
シェルは画面を手放さない。ゲストの寿命を所有するのはホーム画面で、その一点だけが
`main/app_session.c` の契約から変わった（同ファイル `overlay_session` のコメントに書いてある）。

**`ui.basic` は使っていない。** 3.1 は使えと書いているが、コードと合わなかった:

* `ui.basic` は Rust UI コア経由で、`render_strip` は描画前に領域を 0 で塗る
  (`engine/backends/rgb565/src/lib.rs`)。背景の上に重なるのではなく、背景に黒い穴を開ける。
* `ui.setText` のフォントアトラス再構築は、3.1 自身が「最大の単発確保」と呼んでいるもので、
  シーンのスクラッチが生きている最中に欲しいものではない。

なので描画は `pocket.overlay`（`main/pocket/pocket_overlay.c`）というホスト側の
ディスプレイリスト経由にした。座標は領域相対で、はみ出しは切り取らずに投げる。
これは 3.1 の「切り取ると作者は自分が越えたことに気づかない」をそのまま形にしたもの。

## JSを短く保つ理由

ゲストは起動時にソースを解析するので、ソースのバイト数がそのままヒープを食う。
実測で 6.5KB のアプリがゲスト 107KiB、7.6KB は評価に失敗する（CLAUDE.md）。
オーバーレイはさらに厳しい: 背景シーンが動いている状態で確保するので、
`OVERLAY_GUEST_HEAP` は 48KiB しかない。だからこのファイルは 700 バイトで、
説明はここにある。

## 時刻の出どころ

`pocket.time.wall()`。`main/scene/solar_time.c` が時刻源で、
`solar_time_set_synchronized(true)` は SNTP 成功時にしか呼ばれない。
同期していないときは `unixMs` が null で返るので、時計は `--:--` と `NO SYNC` を出す。
**タイムゾーン変換はしない** — この層に足すものではないと `solar_time.c` が決めている。
だから表示は UTC で、そう書いてある。

## 領域

`main/ui/overlay.c` の `DESKCLOCK.region` = `{140, 46, 96, 22}`。
メニューが静止時に届かない場所を選んであるが、**それは見栄えの話でしかない**。
シェルのUIを覆えないことを保証しているのは領域ではなく描画順で、
`overlay_paint()` はシーンの後・メニューのラベルの前に走る。
矩形の選び方で保証しようとすると `main/ui/menu_rows.h` が言うとおり失敗する
（スクロール中は安全な行が存在しない）。

## 入力

受け取らない。3.1 の既定であり、この時計に必要もない。
残りキーの委譲（`input.overlay`）は**未実装**で、理由は報告に書いた。

## Kasane missing（CP28、2026-09-17調査）

`docs/kasane/kasane-astra-plan.md` のチェックポイント表は28行目に
「deskclock/player overlay移植」を置くが、`docs/kasane/kasane-progress.md` の
最新は14f（Taffyなしフルシステム受入試験）で、15〜27は未着手。CP28自体が
まだ届いていない番地であり、今回はそこへ向けた前提を1つ確認しただけ。

* `KSN-MISSING(overlay.attach)`: `main/app_session.c` の `app_start_test()` は
  `overlay_session` のとき `pocketjs_guest_quickjs_install_once(guest,"kasane",...)`
  を呼ばない（569行目付近、`goto surfaces_done` で599行目の kasane install を
  スキップする）。overlay guest に `pocket.kasane` 名前空間そのものが存在しない。
  ネイティブを変えない制約の下では、この1行を足す判断すら下せない —
  overlay 用の region/quota をどう切るか、host-owned core を背景シーンの上に
  合成する経路（現在の `pocket_overlay_paint()` 相当）を Kasane 側にまだ持って
  いるか（`ksn_render.c` が背景を0クリアせず合成できるかは未確認）、そのどちらも
  CP15以降・CP28で決めるべき設計であり、このJSファイルからは決められない。
* 上記1点が塞いでいるため、`pocket.overlay` から書き換える最小の入口すら開いて
  おらず、他の個別機能（attach/detach、guest喪失時の扱い、quota、音声実機）は
  検証以前の状態。移植は**全面的に不可能**と判断し、`apps/deskclock/deskclock.js`
  は旧実装のまま、ファイル冒頭にマーカーだけを足した。
