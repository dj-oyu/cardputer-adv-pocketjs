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

- `replace` は `tx.background()` が無いと `INVALID_ARGUMENT` で断る。作成時は理由が出なかったが、
  Kasane側で背景要件を示すエラーメッセージを追加した。
- 角丸の半径は 8 まで（形状の幅・高さの半分以下も必要）。大きな円は描けないので、夕日は横縞の
  矩形で作っている。Kasane側で半径不正を示すエラーメッセージを追加した。
- キャッシュのコマンドは **同じ Kasane ホスト内の全テンプレート合計で 48**。インスタンスごとにテンプレートの
  コマンド数をシーンの 80 から使う。group opacity 用の追加コマンドは使わない。
- 作ったときの `bounds` が既定の `clip` になる。`setRect` で動かす図形は `clip` を明示しないと、元の位置と
  重なる部分しか描かれない（グリッドと泡は `clip: SEA`）。

80コマンドは現行FWの固定RAM予算で、Cardputerの描画ハードウェア上限と断定できない。円・線・
多数の粒子を少数コマンドで描く手段は未実装。clip既定値の変更やbatch primitive追加は既存画面の
画素・RAM・描画時間への影響を測ってから判断する。

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
  画面を確認するときは `--launch-only` を付ける。`STRESS_READY` と60フレーム進行を確認して
  ポートを閉じ、アプリは終了しない。
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

## Kasane キャッシュ配置の実機 A/B/A（2026-09-26）

同じ75コマンドのアプリ、Cardputer ADV、COM3、各段階20秒、計数OFFで、変更後→変更前
（`3b94d0f`）→変更後を独立bootで測った。`turn_ms` はJS `frame()` とそのjob処理の合計で、
Kasane API単独の時間ではない。`render_ms` は転送を除いた描画時間、`send_ms` はLCD転送時間。

| 段階 | 変更後1: fps / turn / render / send | 変更前: fps / turn / render / send | 変更後2: fps / turn / render / send |
| --- | --- | --- | --- |
| LV1 | 25.7 / 8.64 / 21.26 / 4.15 | 25.7 / 8.72 / 21.20 / 4.11 | 25.7 / 8.65 / 21.22 / 4.14 |
| LV2 | 25.7 / 8.60 / 20.98 / 4.08 | 25.7 / 8.65 / 20.94 / 4.04 | 25.7 / 8.60 / 20.98 / 4.07 |
| LV3 | 22.2 / 16.47 / 22.62 / 4.13 | 22.7 / 17.60 / 21.99 / 4.16 | 22.3 / 16.43 / 22.60 / 4.14 |

時間は各30描画窓の中央値、単位ms。全run `STRESS_APP_PASS`、75コマンド維持、LV3で
15回OOMし復帰、Kasaneエラー0。LV1/2では`turn_ms`が0.05〜0.08 ms短くなったがfpsと
描画時間の改善は検出できない。LV3はOOM周期・描画時間もずれたため速度向上を主張しない。
テンプレートextent追加により表示されたnative予約量は13,108→13,172 B（+64 B）。
このA/B/Aはキャッシュ変更だけを切り替えた同一binary試験ではない。変更後imageには成功経路の
JS診断分岐と、STRESSが使わないschema slot走査変更も入るため、0.05〜0.08 msをキャッシュ単独の
因果値とは断定しない。コード上は毎PATCH動く4 instance・計46 child commandについて、旧実装の
92回のtemplate decode、138回のchild change、4回のgroup更新が、変更後はdecode 0回、
child change 92回、group更新0回になる。実機で確認したのはこの構造的削減と、全体の非劣化・
小幅な`turn_ms`短縮である。

通常版の`render_ms`はLV1約21.2 ms、LV2約21.0 msで、`send_ms`約4.1 msは別に加わる。この全体を無駄なFW
オーバーヘッドとは呼ばない。別の計数ON診断imageで、初回全画面更新を除く30描画窓を
調べると、LV1の平均で画素合成bracket約17.7 ms、group tile約3.3 ms、text span約0.8 ms、
command view取得約0.9 msだった。bracketは入れ子で、割込みも含み、合計や通常版との差分には
使えない。約3,100回/frameのview取得bracketも実core decode回数を意味しない。現状の主な
負荷は半透明・重なりの画素処理であり、command capをハードウェア限界とする証拠はない。
ログは `.cache/kasane-stress-{baseline,preopt,postopt-repeat,profile}-20260926.log`。
