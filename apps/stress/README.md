# STRESS TEST（`apps/stress/stress.js`）

ゲストのヒープと Kasane の描画に同時に負荷をかける試験用アプリ。見た目は vaporwave の海
（縞の夕日、ピンクの透視グリッド、魚群、跳ねるイルカ、泡）。メニューの最後の行（index 7）。

## 何をするか

- **メモリ**: 毎フレーム 2 個の塊を足す（オブジェクト 24 個の配列、長い文字列、`Float32Array`、
  `DataView`+`ArrayBuffer`、`Int16Array`。5 回に 1 回は循環参照のごみも作り、循環回収を働かせる）。
  塊の数が段階の上限を超えたら古い半分を捨てる。
  - Enter で段階を回す: LV1 15 個 → LV2 35 個 → LV3 上限なし。
  - LV3 は実際に確保失敗まで積み、捕まえてすべて捨て、また積む（`STRESS_OOM`）。確保失敗は
    `app_session.c` がログに残すだけで、アプリは続く。途中で失敗した `replace` は参照をすべて古くするので、
    失敗したフレームの次はシーンを作り直す。
- **ネイティブの `Uint8Array`**（F3b の実機確認、`docs/vm/builtin-floor-plan.md` §17.3）: 2 フレームに 1 回
  `assets:/hello.js` を 1 KB ずつ `pocket.fs` で読む。読んだ塊もメモリの負荷に入る。最初の塊で、JS がまだ
  グローバルの `Uint8Array` を読んでいない状態から、ネイティブが作ったものの `prototype`・`constructor` が
  グローバルと一致するかを確かめる（`STRESS_NATIVE ok|NG`）。
- **描画**: 75 コマンド（上限 80）。魚群 2 群（キャッシュの 1 テンプレート × 2 インスタンス、1 群 9 匹）、
  イルカ 2 頭、泡 8 個、透視グリッド 4 本、縞の夕日、文字。1 回の patch で参照 20 個を更新する（上限 32）。
  120 フレームごとに `replace` でシーンを作り直す。

## Kasane の制約（作りながら踏んだもの）

- `replace` は `tx.background()` が無いと `INVALID_ARGUMENT` で断る（理由は出ない）。
- 角丸の半径は 8 まで。大きな円は描けないので、夕日は横縞の矩形で作っている。
- キャッシュのコマンドは **ビューの全キャッシュ合計で 48**。インスタンスはテンプレートのコマンド数 +1 を
  シーンの 80 から使う。
- 作ったときの `bounds` が既定の `clip` になる。`setRect` で動かす図形は `clip` を明示しないと、元の位置と
  重なる部分しか描かれない（グリッドと泡は `clip: SEA`）。

## ログ（USB）

| 行 | 意味 |
| --- | --- |
| `STRESS_READY` | 評価が終わった |
| `STRESS_NATIVE ok len=N` | ネイティブ生成の `Uint8Array` がグローバルと一致（`NG` なら不一致） |
| `STRESS f= lvl= pool= peak= oom= cyc= nat= natOk= err= cmds= native=` | 60 フレームごと |
| `STRESS_LEVEL n` | Enter で段階が変わった |
| `STRESS_OOM n= at=` | 確保失敗を捕まえた |
| `STRESS_FAIL where ...` | 確保失敗以外の例外（数は `err=`） |

描画の時間はファームの `KASANE_PAINT turn_ms= render_ms= ...`（30 フレームごと）で見る。

## 回し方

- 実機: `python tools/stress_app.py --port COM3 [--seconds 20] [--log FILE]` が起動・3 段階・終了を通しで回し、
  段階ごとの fps・`turn_ms`/`render_ms` の中央値・OOM 回数を JSON で出して `STRESS_APP_PASS|FAIL` を判定する。
- ホスト: `bash tools/build_stress_app_test.sh && /tmp/test-stress-app`（WSL）。実物の QuickJS と
  `pocket.kasane` で 900 フレーム回し、Kasane の検証に通らないシーンや例外を焼く前に見つける。
  `STRESS_PPM=<prefix>` で 90・240・420 フレーム目の画面を PPM に書く。

## 実測（実機、2026-09-26、vm/main + F3）

| 段階 | fps | `turn_ms` 中央値 | `render_ms` 中央値 | OOM |
| --- | --- | --- | --- | --- |
| LV1 | 26.6 | 7.8 | 21.2 | 0 |
| LV2 | 26.5 | 7.8 | 20.9 | 0 |
| LV3 | 22.2 | 12.2 | 22.3 | 16（すべて回復） |

ソース評価後の `js=` 61,704。
