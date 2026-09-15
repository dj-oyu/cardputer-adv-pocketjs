# ホーム画面のプレイヤー（overlayアプリ）

`apps/player`はホーム画面の上に音楽再生を乗せる**overlayアプリ**（[common-api.md](../api/common-api.md) 3.1）で、通常の前景JSアプリとは別枠で動く。MP3/Opus再生がJSセッションと寿命を共にする以上（同時に1セッションしか無い、[CLAUDE.md](../../CLAUDE.md)のアーキテクチャ節）、再生をホーム画面から離さずに続けるにはoverlayに置くしかない——好みではなく、他に置ける場所が無い。

## 仕組み

- `app_session.c`がoverlayセッションにも`fs`/`av`をinstallする。無線・マイク・バスはoverlayに無い。
- `app_overlay_tick()`が`pocket_overlay_pump()` / `pocket_fs_pump()` / `pocket_av_pump()`を`app_tick()`と同じ順で回す。
- 3つのモーダル（ピッカー等）は`ui_task`が配送し、**モーダルがoverlayに勝つ**。
- `REGISTERED[]`（`main/ui/overlay.c`）は現在2行: DESK CLOCK / MUSIC。Settingsの3択で切り替える。
- `shell_draw()`はoverlayが立っているとき`menu_layout()`を呼ばず、XMBのラベルも描かない。
- ESCはシェルの予約キーで、配送ループの先頭で`break`する。ゲストへ渡す呼び出しに到達しない。

**予約キーは実装時に一度間違えた場所に置いた教訓が残っている。** 最初force stop（Ctrl+Alt+Del）の分岐に書いてしまい、通常のBackは配送経路をそのまま通っていた——実装したつもりの不変条件が1ビルドのあいだ成立していなかった。動いて見えたのはESCが`action_of()`で名前を持たず落ちていたためで、偶然だった。予約キーを増やすときはこの経路を明示的にテストすること。

## ターンの費用管理

`overlay_budget`もゲストの割り込み期限も実時間を測るので、`ui`タスクが復号器（優先度6）・オーディオ（7）・カード読み出しに横取りされている時間もターンの費用として計上される。**暴走検知（割り込み期限）と費用制御（`budget_us`）は別の数で扱う。** 混雑を暴走と区別できず、割り込み期限を短く取ると重い背景（例: FLOWER、`kernel=53ms`）の下で正当なJSループが`InternalError: interrupted`で落ちる実害が過去にあった。前景アプリと同じ250msに揃えて解消済み。

費用側も「連続超過回数」の生カウントでは同じ理由で誤爆する（前景が重いフレームが続くと、overlay自身の消費が小さくてもカウントが積み上がる）。そこで課金は**割合**にした: ターンがフレームの1/4を超えないなら、そのフレームが遅かったのはoverlayのせいではないとしてカウントをリセットする（`overlay_budget_turn()`、`main/ui/overlay.c`、`over_limit=60`連続）。超過が続くと`OVER BUDGET`で停止し、Settings行に状態を表示する——3.1の要件「overlayがホーム画面を使えなくしてはならない」の停止行はここにある。

## 実装済みの機能

**音量。** Settingsに`VOLUME`行（QUIET/LOW/MID/HIGH/LOUD）と、ホーム画面のどこでも効く`-`/`=`キー（シェルが取る。プレイヤーは所有しない）。ES8311のレジスタ`0x32`を書く機器プロパティで、`pocket.audio`には出していない——アプリが自分で音量を上げられると、誰も見ていない間に上げられてしまう。変更は右上に1.5秒表示し、NVSへ保存する。

**曲の長さ。** Xing/Info/VBRIタグから総フレーム数を読む（`pocket_mp3_total_frames`）。無ければ`durationMs`は`null`のままで、推測しない。バーは長さが分かるときだけ描き、分からないときは位置を主張しない光の演出に切り替える。`tools/test_mp3_duration.c`がホストで検査する。

## 制約として残っているもの

前景アプリでの再生はセッションと一緒に死ぬ。MP3は`seekable:false`。overlayに音声関連のcapability（マイク、無線）は無い。

未解決の作業項目（性能改善・未測定範囲）は[backlog.md](backlog.md)を参照。
