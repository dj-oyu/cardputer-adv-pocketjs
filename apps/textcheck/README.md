# textcheck — `pocket.input.text` を実機で確かめる

`docs/common-api.md` §6 の TextOptions 段落が**実際に強制されているか**を1行1主張で出す。
表示は持たない（編集欄はホストが描く）。出力の各行は `TXT ...`。

```
tools/pocket_bridge.py demo --source apps/textcheck/textcheck.js
```

`main/shell.c` の一覧には入れていない。Playground に貼るか、`main/CMakeLists.txt` の
`EMBED_TXTFILES` に足して起動する。

## 走らせ方（USB）

編集欄が開いている間、**ファームはキーボードをゲストから取り上げる**。
`main/main.c` はその間 `text_screen` を立てるので、USBで送ったバイトは
メニュー操作ではなく文字として届く。ホストから打つときのバイトは既存の対応表と同じ:

| 送るバイト | 意味 |
| --- | --- |
| 印字可能ASCII | そのまま文字 |
| `\r` / `\n` | Enter |
| `0x08` / `0x7f` | Backspace |
| `0x1b` | Escape |
| `0x02` / `0x06` | ←／→ |
| `0x04` | Del（後方削除） |
| `0x0b` | IMEトグル（C-J相当） |

実機のキーボードなら Fn+` が Escape、Fn+←→ がカーソル、C-J がIMEトグル。

## 期待する出力

### phase A — capability・引数拒否・単行欄

```
TXT CAP true/true null
TXT LIMITS maxBytes=256 maxSessions=1 multiline=true ime=skk
TXT REFUSE no-rect -> INVALID_ARGUMENT
TXT REFUSE ime-maybe -> INVALID_ARGUMENT
TXT REFUSE long-initial -> INVALID_ARGUMENT
TXT REFUSE onEdit-not-fn -> INVALID_ARGUMENT
TXT REFUSE offscreen -> INVALID_ARGUMENT
TXT OPEN-A getText=[ねこ]
TXT SECOND -> BUSY
TXT TYPE into A, then ENTER to submit (or ESC to cancel)
```

`CAP true/true` が **この作業の目的**。以前は `false/false NOT_IMPLEMENTED` だった。
`LIMITS` に出る4つは全部コードが強制している値で、`maxBytes` は `js_open` と毎打鍵の
`tf_insert` の両方で効く。

ここで文字を打つと1打鍵ごとに1行:

```
TXT EDIT 8 [ねこabc]
```

**`EDIT` は確定済みテキストだけ。** IMEで `neko` と打っている最中の `▽ねこ` は
画面には出るが `EDIT` にも `getText()` にも出ない。出たら実装の負け。

Enter で:

```
TXT SUBMIT [ねこabc]
```

**変換確定のEnterでは `SUBMIT` が出てはいけない。** `▽ねこ` の状態で Enter を押すと
確定して `EDIT` が1行出るだけ。もう一度 Enter を押して初めて `SUBMIT`。
これが「確定EnterはonSubmitへ二重配送しない」の実機での見え方。

### phase B — 閉じたセッション、複数行

```
TXT STALE getText -> CLOSED
TXT STALE close -> ok
TXT OPEN-B multiline: ENTER must NOT submit; ESC ends it
```

B で Enter を押すと `EDIT-B` の長さが1増えるだけで `SUBMIT-B` は出ない。
出たら単行／複数行の判定が壊れている。Escape で:

```
TXT CANCEL-B
```

**変換中の Escape は `CANCEL` を出さない。** B は `ime:'off'` なので常に取消側だが、
A で `▽ねこ` の最中に Escape を押すと変換だけが消えて `CANCEL-A` は出ない。

### phase C — 遅延commitの拒否

```
TXT OPEN-C: the FIRST character swaps the session underneath the callback
TXT EDIT-C [a]
TXT OPEN-D (the replacement). Type, then ENTER.
```

C の `onEdit` が自分を `close()` して D を開く。ホスト側はこのコールバックから
戻った時点で「セッションが入れ替わった」ことを世代番号で知り、C のために計算していた
残りを D に書かない。**シリアルログに `pocket.text: TEXT_STALE` が出たら、それは
拒否が働いた証拠**であって失敗ではない（出ないのが普通）。

D で打った文字は `EDIT-D`、Enter で `SUBMIT-D`。ここまで出れば全段通し。

## 実装のどこを見ているか

| 主張 | 実装 |
| --- | --- |
| ホストが欄を所有 | `main/main.c` `tick_run()` がキーをゲストへ渡さない |
| 画面へ合成 | `main/app_session.c` のストリップループ内 `pocket_text_overlay()` |
| 未確定文字を渡さない | `pocket_text.c` が `ime_text()`（IME_TEXT）だけを写す |
| Enterの二重配送なし | `textfield.c` `tf_key()` の `ime_took` |
| Escapeの二義 | 同上。engineが食えば `IME_TAKEN`、食わなければ `TF_CANCEL` |
| 自動close | `finish_submit()` / `finish_cancel()` が callback より先に detach |
| 画面変更でclose | `app_stop()` の `pocket_text_reset()` |
| 同時1セッション | `live` ポインタ1本、2本目は `BUSY` |
| 遅延commit拒否 | `tf_commit()` の世代照合。host側の証明は `tools/test_textfield.c` |
| maxBytesはバイト数 | `tf_insert()` が丸ごと拒否する（途中で切らない） |

ホストだけで確かめられる分は焼かずに走る:

```
wsl -e bash -lc "cd /mnt/c/devs/m5stack/cardputer-adv-pocketjs && gcc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined -I main/text tools/test_textfield.c main/text/textfield.c -o /tmp/t && /tmp/t"
```
