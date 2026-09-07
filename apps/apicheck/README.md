# apicheck / pickcheck

共通APIの新しい面が実機で本当に立つかを確かめる2本。表示は持たず、`console.log` の
行が結果そのもの。`tools/pocket_bridge.py demo --source apps/apicheck/<name>.js` で
Playground に打ち込んで走らせる。

## apicheck.js

一度に3つの面を検査する。

- `pocket.ble` が **存在すること**。`ble.central` は `supported=false` だが、名前空間が
  無いと feature-test を飛ばしたアプリが `UNSUPPORTED` ではなく `TypeError` を食う。
  これは §2 が防ぐために書かれている失敗そのものなので、`typeof` と「呼べば Promise が
  返る」を別々に見る。
- `audio.playback` と `workspace` の capability が立つこと。
- `pocket.app.info()` がアプリ同一性を返すこと。以前は「ソースのポインタが pet と同じか」
  で判定していた。
- `workspace.create` → `read` の往復。

実機での結果（2026-09-07、commit ba81afd + 各面）:

```
CAP ble.central false/false NOT_IMPLEMENTED
CAP audio.playback true/true null
CAP workspace true/true null
TYPEOF ble=object ws=object player=object
REACH ble.scan promise
APP local.playground pocket-app works=pick
LAUNCH state=null result=null
WS created / WS read rev=1 title=APICHK n=9
```

停止後の `MEM free=195596 js=0` まで見ること。セッションが持っていたものを返している。

## pickcheck.js

`pocket.workspace.pick()` だけを見る。これは**生きているゲストの上にホスト画面を描き、
キーをゲストから取り上げる**ので、他のどの呼び出しとも壊れ方が違う。

```
PICK 2 works → PICKED 0 → CHOSE → READ rev=22 title=PLAYGROUND n=729
```

**USBからは上下キーを送れない。** `main/main.c` の `usb_stroke` はテキストを受ける画面に
対して left(0x02)・right(0x06)・del(0x04) は持つが up/down を持たない。ピッカーの移動を
ホストから検査するにはそこに割り当てを足す必要がある。今は Enter で先頭を選ぶところまで
しか自動で確かめられない。
