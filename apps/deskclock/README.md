# DESK CLOCK — オーバーレイアプリの最初の一つ

`docs/api/common-api.md` 3.1 の実装。時計そのものは題材で、作っているのは**弁**のほう。

## これは何であって、何ではないか

ホーム画面の背景の上に、時刻の小さな箱を出し続ける。背景の代わりに走る通常アプリと違い、
シェルは画面を手放さない。ゲストの寿命を所有するのはホーム画面で、その一点だけが
`main/app_session.c` の契約から変わった（同ファイル `overlay_session` のコメントに書いてある）。

描画は`pocket.kasane`。通常appと同じAPP layerを使うが、overlay profileでは背景色で帯を
消去しない。シェルが現在のnative sceneをbackdropとして帯へ描き、その上へKasane commandを
source-over合成する。`ui.mount('clock')`でアプリ所有assetとnative sourceを選び、
時刻の読取り・UTC書式はアプリ側producer、固定板と文字命令は汎用Kasaneが担当する。
`pocket.overlay`の旧display listは使わない。

座標は96×22のregion相対。adapterがLCD座標へ変換しregionでclipするので、guestはregion外の
画素を指定できない。OVERLAYという第三layerやfull-frame bufferは追加していない。

## JSを短く保つ理由

ゲストは起動時にソースを解析するので、ソースのバイト数がそのままヒープを食う。
実測で 6.5KB のアプリがゲスト 107KiB、7.6KB は評価に失敗する（CLAUDE.md）。
オーバーレイはさらに厳しい: 背景シーンが動いている状態で確保するので、
`OVERLAY_GUEST_HEAP` は上限160KiBだが、起動後のfree-heap floorが別にある。だからソースは短く保ち、
説明はここにある。

## 時刻の出どころ

nativeの`sys_device_clock_read()`。JSは`pocket.time.wall()`を毎frame呼ばない。
`main/scene/solar_time.c` が時刻源で、
`solar_time_set_synchronized(true)` は SNTP 成功時にしか呼ばれない。
clock snapshotが無効なら `--:--` と `NO SYNC`、PC由来の時刻なら値と `NO SYNC` を出す。
**タイムゾーン変換はしない** — この層に足すものではないと `solar_time.c` が決めている。
だから表示は UTC で、そう書いてある。

## 領域

`main/ui/overlay.c` の `DESKCLOCK.region` = `{140, 46, 96, 22}`。
regionは表示の所有境界でもある。シェルはXMBを終了してoverlayをその場所に立たせ、FPS/音量など
shell-owned surfaceだけをKasaneの後へ重ねる。region外を守るのは見栄えや描画順ではなく、
`pocket_kasane_set_viewport()`によるadapter側のclipである。

## 入力

受け取らない。3.1 の既定であり、この時計に必要もない。
残りキーの委譲は`pocket.overlay.onKey`で実装済みだが、この時計は購読しない。

## Kasane overlay（CP28）

2026-09-23に共通profileとともに移植済み。さらにnative presenterへ移行した。
guest喪失時のreset、region-local変換、live backdrop、転送失敗後のrepairは
deskclock固有コードではなくnative frameworkが所有する。実機での見た目は未確認。
