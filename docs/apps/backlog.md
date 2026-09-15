# apps/ の未解決事項

`docs/apps/`配下の各文書からコードを確認して残した、まだ開いている作業項目。閉じたら行を削除する（履歴はgitに残る）。各行は出所・現状・確認方法を持つ。

## チュートリアル見直し

出所: 旧`docs/apps/tutorial-review.md`（予約日2026-09-07）。

2026-09-07時点の着手条件は3つとも解消している: `apps/netcheck`（`pocket.net`、実機で往復済み・READMEに実出力ログあり）、`apps/bridge`（`pocket.bridge`のPC往復、同）、および§9.1圧縮音声（Opus実装済み、[opus-feasibility.md](opus-feasibility.md)）、§7 workspace（`pocket_workspace.c`実装済み）、§3 アプリ登録（`app_registry.c`実装済み）、§12 BLE（`pocket_ble.c`実装済み）。**着手を妨げるものは無い。**

未決定・未着手:

1. **チュートリアルに`pocket.*`の章を足すかどうかの決定。** 候補A: 既存9章の後ろに`pocket.storage`/`pocket.sensors`/`pocket.audio`を1行で完結する章として追加。候補B: 9章は初学者向けのまま据え置き、`pocket.*`は`main/ui/lessons.c`の`REFERENCE`表を拡張する形で導線を作る。決めてから着手すること。
2. **vim風エディタの操作を教える場所を作る。** ユーザーからの明示的な依頼で範囲に必ず入る。チュートリアル本編はinsertモードで開くため含めない。候補は`main/ui/codeedit.c`の`notice`（狭い）か、Tabのリファレンス表と同じ仕組みでPlaygroundにヘルプ面を1枚出す方式。実装済みの操作は`main/ui/vimcmd.c`が正で、visual/`.`/redo/`%`など未実装の操作は書かない。ノード数とDRAMは`tools/memlog.py`で測ること（未着手、2026-09-15時点で該当するヘルプ画面は存在しない）。
3. 章の文面の点検3件: 5章の`globalThis.frame`説明がライフサイクル節と整合するか、8章の`0x4000`直書きを共通APIの定数に寄せられるか、`REFERENCE`の数字表と`pocket.ui`の関係を一行加えるか。

着手時に読むもの: `main/ui/lessons.c`（章の本体）、`main/ui/tutorial.c`（進行と`CHECK_*`）、`main/ui/codeedit.c`、`main/ui/vimcmd.c`、[common-api.md](../api/common-api.md) §5, §6, §7。

## ホーム画面プレイヤー（overlay）

出所: [player-overlay.md](player-overlay.md)。実装の説明はそちらへ移した。ここは未解決の作業項目のみ。

1. **合成が毎フレーム全部描き直している。** `pocket_overlay_paint()`は帯の外の項目を飛ばす最適化のみで、表示リスト自体に差分機構が無い（`ovl=3.44ms/frame`実測、2026-09-09）。
2. **ホーム画面が再生中に30→26fpsへ落ちる。** overlay自体ではなく、パネル転送(`send`)とシーンが復号器・カード読み出しとCPUを取り合っている。直すならDMAの非同期化か復号タスクの優先度調整。
3. **カードの読み出しの計時が無い。** SDは25MHzで足りてはいるが余裕は未測定。`sd_media.h`の`SD_MAX_OPEN_FILES`の判断もこの測定待ち。
4. **数十分規模の連続再生が未検証。** 2曲・数曲の走行では`free`/`largest`にリークの兆候は無いが、これは「短い走行で見えない」であって「無い」の証明ではない。
5. **偽UIに対する予約キーの発見可能性。** ESCが予約キーであることをユーザーが知る手がかりは、現状overlayアプリの画面内の1行のみ。

## 日本語入力・共通テキストAPI

出所: [japanese-input.md](japanese-input.md)。

1. **入力キューのoverflowが検知されない。** `main/main.c`の`xQueueSend(keys,&k,0)`が失敗を無視するため、取りこぼしたキー入力を利用者に知らせる手段が無い。overflow表示とキー保持状態の再同期は未実装。
2. **JS用日本語アトラスのreload失敗時に再試行・上限到達表示が無い。** 現在は登録集合だけ先に進む（`jsfont.c`）。

## 手つかずのまま残っている既知の実装課題

`srcstore` と Playground の保存まわりの不具合2件（2026-09-15 にコードで再現を確認）は [platform/backlog.md](../platform/backlog.md) にある。SKK 辞書のヘッダ一致時に本文 CRC 検証を省略する挙動は、既知の制約として [japanese-input.md](japanese-input.md) に記載済み（仕様として受容、backlog 項目ではない）。
