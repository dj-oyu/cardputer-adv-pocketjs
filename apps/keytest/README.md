# KEY TEST（`apps/keytest/keytest.js`）

`pocket.input.keys` の診断アプリ。押されている物理キーの名前、同時押しの数と最大値、押下・解放の総数を画面とログに出す。**実機の同時押しの上限（ゴースト・取りこぼし・FIFO あふれ）を測るための道具**で、測定手順は [docs/platform/keystate.md](../../docs/platform/keystate.md) の「実機で測る項目」。

## 入れ方と起動

メニューの行ではない（`tools/test_settings.py` と `capture_home.py` はメニューを押下回数で数えるので、行を足すと壊れる）。診断ビルドだけに入る。

```powershell
idf.py -B build_keytest -DPOCKET_KEYTEST=ON build
idf.py -B build_keytest -p COM3 flash monitor
```

ホーム画面で USB から `r` を1バイト送ると起動する（`idf.py monitor` なら `r` を打つ）。`` ` ``（Back）で終わる。`` ` `` は main.c がアプリの終了に使うので、このアプリでは測れない。

同じビルドで、USB からキーの押下・解放を注入できる（`US(0x1f) 'K' op hex hex LF`、op は `+` 押下 / `-` 解放、hex はキー番号 row*14+col）。注入は実キーボードと同じ `keymap_poll()` を通るので、翻訳（シェルへの打鍵）と `keystate` の両方に効く。

## 画面とログ

| 行 | 内容 |
| --- | --- |
| 1 | いま押されているキー（`keys.down()`、正規名） |
| 2 | `NOW` 同時押しの数、`MAX` その最大 |
| 3 | `PRESS` / `RELEASE` の総数（`keys.pressed()` / `keys.released()` を56キーぶん毎フレーム数える） |
| 4 | そのフレームの押下 `+名前` と解放 `-名前` |
| 5 | フレーム番号 |

ログは変化があったフレームだけ `KEYTEST f=… +e -s down=… n=… max=… p=… r=…`。起動時に `KEYTEST_READY cap=true/true keys=56`。

`KEYTEST_SKEW` は「アプリが見た押下数 − 解放数 ≠ いま押されている数」。辺は交互にしか数えないので、通常は出ない。出る正当な場合は、アプリの外（通知、テキスト欄）がキーボードを取ったターンで押されていたキーが隠されたとき（keystate.md「ライフサイクル」）。それ以外で出たら不具合。

Enter は押すたびにクリック音が鳴り、素の `;` `,` `.` `/` は矢印として `frame(buttons)` にも届く（既存の動作で、このアプリは読まない）。
