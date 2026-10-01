# MEGADEMO Act II — 上限を叩く3場面と動的 plan 読み込み

2026-09-29。対象は [apps/kasane/proc_megademo.js](../../apps/kasane/proc_megademo.js)（APPS の MEGADEMO、`local.megademo`）。**ここにある数値はすべて host のもの**（実 QuickJS、実 `pocket_proc.c`／`pocket_kasane.c`、m32 は i386 で device と同じ 8 B JSValue）。実機の描画時間・heap・LCD 転送は [megademo-device-limits.md](megademo-device-limits.md) で測り、その結果で `KN`（MID と LIMIT HEAVY）と `LOAD`（読み込み方式）を改訂した。下の表は改訂後の host 計数に直してある（改訂前の値は括弧内）。美的な評価はしない。見比べ用の画像を用意した。

## 結論

- 既存の3場面（Act I: ニュースセット、48 フレーム、C 参照と全画素一致を維持）の後に Act II として **TWIST**（ねじれ廊下、128 f）・**ZENITH**（Apple II 風ベクタ 3D、96 f）・**LIMIT**（両者を上限まで重ねる、96 f）を足した。1周 368 フレーム。
- LIMIT の HEAVY は1フレームで線分 1,010/1,024（99%、改訂前 959）、ラスタ 58,000/65,535（89%、56,352）、1 draw のラスタ最大 7,824/8,192（96%、7,294）、1 draw の VM ステップ最大 9,817/10,000（98%、9,022）、命令 63/64、レジスタ 16/16、入れ子 8/8、入力 8/8、点列 120/128 点（座標 −451〜575）。1周（368 フレーム）の登録累計 52（Act I 15＋TWIST 7＋ZENITH 16＋LIMIT 14）で 32 を超える。同時 plan は、旧・新を全部共存させると ZENITH→LIMIT で 30/32 だったが、実機ではメモリが 24〜28 本目で尽きたので、改訂後の読み込みでは実機で最大 16。
- plan は場面ごとに登録・解除する。改訂後は、次場面の plan を切替の 16 フレーム前から 1 フレーム 1 本、実機の空き heap が 22,528 B 以上の間だけ先読みし、切替で旧場面の分を解除してから残りを 1 フレーム 1 本登録する（未登録の plan の draw は飛ばす）。改訂前は 6 フレーム前から 4 本ずつで旧・新を全部共存させていた。
- 手続き IR の 15 命令はすべて使った。view 側も `pocket.kasane` の描画・画像・アニメーション系を広く使い、使わなかったものは理由を書いた（網羅表）。
- ゲストヒープ: 評価（コンパイル）時のピークが律速。device と同じ課金（TLSF 長、m32）で**評価と実行に要る最小上限は 123,125 B**（上限 163,840 B の 75%）。変更前の MEGADEMO は 74,453 B（45%）。実機では未確認。
- ネイティブ側（`main/`）は変更していない。制約として「表示待ち候補は全面で1枚」（2面を同じフレームで更新できない）を報告する（末尾）。

## 場面と設計の芯

### タイムライン

| 場面 | フレーム | plan | 1フレームの draw | 面 |
| --- | --- | --- | --- | --- |
| NEWS ×3（Act I、変更なし） | 16×3 | 5 ずつ | 5 | 面0 |
| TWIST | 128 | 7 | 7 | 面0 |
| ZENITH | 96 | 16 | 7（4 フレームに1回は面1のレーダー 2） | 面0／面1 |
| LIMIT | 96 | 14 | 11（同上） | 面0／面1 |

Enter は従来どおり全画面⇄小窓（登録済みリサイズ、bilinear）。**UP/DOWN で負荷段階**（LIGHT/MID/HEAVY、既定 HEAVY）、**LEFT/RIGHT で場面送り**。登録や描画が失敗したら段階を1つ下げて読み直す（`MEGADEMO DEGRADE`、LIGHT で失敗したら例外のまま）。ログは `MEGADEMO SCENE <名> tier=<n> plans=<live> freed=<解放数> registered=<累計>` と `MEGADEMO VIEW <名> commands=<n>`、従来の `MEGADEMO IMAGE FIXED_PIE / DYNAMIC_STRETCH`。

### ZENITH — Apple II の解釈

調べて分かったのは、*Zenith* は 1982 年の Apple II 用アクションゲーム（Nasir Gebelli、Gebelli Software）で、疑似 3D の宇宙シューティング、自機を左右に回転（ロール）できるのが特徴、建設中の都市 Zenith の上空を守る、という点まで（MobyGames と Internet Archive の記載）。画面の細部は確認していないので、作品の再現ではなく次の**解釈**で作った。

- 色は Apple II のハイレゾ6色相当に限る: 黒・白・紫 `0xfa3f`・緑 `0x17a7`・青 `0x167f`・橙 `0xfb47`（一般に引用される近似値を RGB565 に丸めたもの）。Act II の手続き面の線はすべてこの5色（＋背景の黒）。例外は view 側の枠・机・ペット（ズームアウト時のセット）と、ピクセル窓（下記、紫のビットマスクで寄せたが中間色が出る）。
- 3D は塗らずに線だけ。ロールは VM 内で地平線の基底 (u, n) を回して表す（画像回転に頼らない）。48〜63 フレームで1回転のバレルロール。
- 「走査線的な構造」は、地面を**地平線に平行な横線の束**（奥行きが幾何級数、`REPEAT_REG` で本数を入力から）として描くことで表した。緑と橙を1本ごとに交互（`LINE_COLOR_REG`）。
- 少ない資源で奥行き: 車線（消失点へ向かう線）、奥から手前へ3列の建設中のスカイライン（高さが `sin²` で決まり、場面の間に伸びる）、12 辺を1本の経路でたどる正八面体の編隊、`CUBIC` のレーザー、8 段階のロール角ごとに登録した自機の点列（1980 年代の「回転済みスプライト表」を plan で持つ）。
- 面1にレーダー（`CUBIC` 4本の円を4重、掃引線、`PLOT_COLOR_REG` の輝点）を描き、view で2か所に出す: 中央の切り抜きをネイティブの `animate` で回転、面全体を `nearest` で 60×34 に縮めたものを `setRotation` で機体のロールに合わせて回転。右上の小窓は `pocket.kasane.pixel` の munching squares（`((x ^ y) * t) & 紫マスク`、7 命令、32×24 を `scale: 2`）。

### TWIST — ねじれ廊下の構成規則

森の神殿のねじれた廊下から取ったのは、**構成は単純なのに、身体と空間のモデルが壊れる**というギャップ。規則は1つだけにした。

> 廊下の断面（矩形）の各点 P を、1 フレーム奥へ進むごとに複素数 z = r·e^{iδ} 倍する。

- r < 1 が遠近（幾何級数の奥行きで、割り算の無い IR で透視を作れる）、δ がねじれ。δ = 0 ならまっすぐな廊下、δ > 0 なら奥へ行くほど回る。場面の間に δ を 0 → 最大 → 0 と動かす（神殿のスイッチで廊下がねじれ／戻るのに対応）。
- 前進は入力側で z^{−φ}（φ はフレーム内の位相）を掛けるだけで連続になり、φ が 1 を越えた瞬間は「1つ奥の枠が手前へ来た」状態と一致する（継ぎ目がない）。
- 床（板＝橙と白の横木）・壁の段（青）・天井のアーチ（白、`CUBIC`）・四隅の手すり（緑）・枠（紫）は、どれも「断面の点を z 倍し続ける」同じ規則の別の点。ねじれると床の板が壁になり天井になる。カメラは中心から外している（手前の断面の中心 B が z 倍されて消失点へ螺旋を描く）。
- IR では複素積 `(x, y) *= z` の7命令を展開する（`X` マクロ）。枠の plan は4隅×7＋描画 15 で 1 フレーム 43 命令、合計 63 命令・16 レジスタ。
- LIMIT では同じ廊下を4面すべて横木で埋め（床・天井・右壁の rungs）、対数螺旋の点列（120 点、回転＝縮小なので2変種を交互に出すと吸い込まれ続けて見える）、ZENITH の地面と車線、8 段入れ子の格子、de Jong アトラクタを重ねる。遠近・ロール・ねじれ・渦という互いに矛盾する奥行きの手がかりを同じ画面に置く。

## 上限の使用率（LIMIT、HEAVY、host 計数）

| 上限 | 値 | 使用 | 率 | どこで |
| --- | --- | --- | --- | --- |
| 線分／フレーム | 1,024 | 1,010 | 99% | 11 draw の合計（格子 255、アーチ 192、点列 119、枠 96…）。改訂前 959 |
| ラスタ歩数／フレーム | 65,535 | 58,000 | 89% | 同上（格子 7,824、地面 7,294、枠 6,878、床・天井・壁 各 6.6k、点列 6.6k、車線 5.5k）。改訂前 56,352 |
| ラスタ歩数／draw | 8,192 | 7,824 | 96% | 格子（`lt` 5.7）。改訂前は地面の 7,294 |
| VM ステップ／draw | 10,000 | 9,817 | 98% | アトラクタ（37 本、1 本の線に内側 16 反復×15 命令）。改訂前 9,022 |
| 命令／plan | 64 | 63 | 98% | 枠 |
| レジスタ | 16 | 16 | 100% | 枠・アーチ・地面・星ほか |
| REPEAT 入れ子 | 8 | 8 | 100% | 格子（`REPEAT 2` を8段） |
| 入力／draw | 8 | 8 | 100% | 廊下・地面・車線・格子・アトラクタ |
| 点列の点数 | 128 | 120 | 94% | 対数螺旋 |
| 点列の座標 | −480〜720 | −451〜575 | 下限側 94%・上限側 80% | 螺旋の外端（画面外） |
| 同時 plan | 32 | 16（実機） | 50% | ZENITH。旧・新の全共存（30）は実機のメモリで成り立たなかった |
| 面 | 2 | 2 | 100% | 面1＝レーダー |

改訂前の目安 8〜9 割は「実機で詰める余地を残す」ためで、実機で時間が余っていた（LIMIT HEAVY 49 ms、15 fps の下限まで約 17 ms）ので、線分・1 draw のラスタ・ステップを 96〜99% まで詰めた。レジスタ・入れ子・入力は上限そのものまで使った。1 フレームあたりの VM ステップには上限がないが、LIMIT HEAVY で合計 20,437 ステップ、ZENITH 2,656、TWIST 6,571。

## 場面ごとの統計（host、全フレームの最大）

`MEGA_PLANS=1 python3 tools/kasane_contract/run_proc_megademo_scenes.py` の出力。seg/raster はフレーム（面1のフレームを含む）の最大、draw 列は1 draw の最大。

| 段階 | 場面 | フレーム | plan | draw | 線分最大 | 線分平均 | ラスタ最大 | draw ラスタ最大 | draw ステップ最大 | フレーム ステップ最大 | 命令 | レジスタ | 入れ子 | 入力 | 点数 | 点の座標 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 共通 | NEWS (s0) | 16 | 5 | 5 | 174 | 174.0 | 9919 | 3456 | 487 | 782 | 56 | 8 | 1 | 4 | 40 | 18〜213 |
| 共通 | NEWS (s1) | 16 | 5 | 5 | 166 | 166.0 | 8841 | 2400 | 487 | 762 | 56 | 8 | 1 | 4 | 40 | 19〜210 |
| 共通 | NEWS (s2) | 16 | 5 | 5 | 174 | 174.0 | 10089 | 3616 | 487 | 782 | 56 | 8 | 1 | 4 | 40 | 23〜206 |
| LIGHT | TWIST (s3) | 128 | 7 | 7 | 196 | 196.0 | 10286 | 3598 | 565 | 2697 | 63 | 16 | 1 | 8 | 0 | — |
| LIGHT | ZENITH (s4) | 96 | 16 | 7 | 108 | 92.2 | 6010 | 4168 | 420 | 1145 | 59 | 16 | 2 | 8 | 17 | 89〜142 |
| LIGHT | LIMIT (s5) | 96 | 14 | 11 | 570 | 461.2 | 26740 | 4168 | 2118 | 6431 | 63 | 16 | 8 | 8 | 120 | -451〜575 |
| MID | TWIST (s3) | 128 | 7 | 7 | 566 | 566.0 | 19190 | 7395 | 1807 | 6571 | 63 | 16 | 1 | 8 | 0 | — |
| MID | ZENITH (s4) | 96 | 16 | 7 | 266 | 201.2 | 11372 | 7294 | 1032 | 2656 | 59 | 16 | 2 | 8 | 17 | 89〜142 |
| MID | LIMIT (s5) | 96 | 14 | 11 | 766 | 608.2 | 41293 | 5731 | 4434 | 12394 | 63 | 16 | 8 | 8 | 120 | -451〜575 |
| HEAVY | TWIST (s3) | 128 | 7 | 7 | 566 | 566.0 | 19190 | 7395 | 1807 | 6571 | 63 | 16 | 1 | 8 | 0 | — |
| HEAVY | ZENITH (s4) | 96 | 16 | 7 | 266 | 201.2 | 11372 | 7294 | 1032 | 2656 | 59 | 16 | 2 | 8 | 17 | 89〜142 |
| HEAVY | LIMIT (s5) | 96 | 14 | 11 | 1010 | 791.2 | 58000 | 7824 | 9817 | 20437 | 63 | 16 | 8 | 8 | 120 | -451〜575 |

Act I（NEWS）は段階によらない。MID の TWIST・ZENITH は改訂で HEAVY と同じ値になった（実機で HEAVY の負荷でも表示レートを保つため）。

## 動的 plan 読み込み

- 起動時は Act I の最初の場面の 5 本だけを登録する（従来は 15 本を起動時に全登録）。
- 改訂後（`LOAD = [16, 1, 22528]`）: 場面の残り 16 フレームから、次の場面の plan を 1 フレーム 1 本、`pocket.memory.info().internalFreeBytes` が 22,528 B 以上の間だけ登録する。切替フレームで旧場面の全 handle を `unregister` し、次場面の残りは場面の最初のフレームから 1 フレーム 1 本ずつ登録する。未登録の plan の draw は飛ばす。プログラム配列は plan ごとの thunk で1本ずつ作る。
- UP/DOWN（段階変更）と `DEGRADE` は先読み中の集合も外してから入れ直す（`enter(scene, fresh)`）。LEFT/RIGHT は先読み中の集合が目的の場面ならそれを使う。どちらも以後は 1 本ずつ。
- 改訂前（`LOAD = [6, 4]`、切替時に残り全部を登録してから旧場面を解除）は実機で成り立たなかった: 同時 plan がメモリで 24〜28 本目に失敗し、切替フレームの JS が 208 ms に達した。1 本の解読（JS）が実機で中央値 12 ms かかるため（[megademo-device-limits.md](megademo-device-limits.md)）。
- host 実測（`run_megademo_app_host.py`、1,136 フレーム＝約 3.1 周＋ズーム・段階変更・場面送り、空き heap は実機に合わせた直線で代用）: 同時 live の最大 16、登録 204、解除 188、旧・新が同時に live なフレーム 179、1 フレームの登録最大 1。

## プリミティブ網羅表

JS から見えるものを実装（`main/pocket/pocket_proc.c`・`pocket_kasane.c`・`pocket_grid.c`・`pocket_pixel.c`・`pocket_video.c`）から拾った。A1 = Act I、TW = TWIST、ZE = ZENITH、LI = LIMIT、共 = 起動時／全場面。

| 面 | プリミティブ | 使った場面 | 使い方／使わなかった理由 |
| --- | --- | --- | --- |
| procedural | `register(program)` | 共 | 全 plan。場面ごとに登録 |
| | `register(program, affineQ14Points)` | A1, ZE, LI | A1 層3（40 点）、ZE 自機（17 点×ロール8段）、LI 対数螺旋（120 点×2） |
| | `unregister(handle)` | 共 | 場面の切替、段階変更、DEGRADE |
| | `beginFrame(color)` / `beginFrame(color, surface)` | 共 / ZE, LI | 面1はレーダー（4 フレームに1回） |
| | `draw(handle, inputs)` | 共 | 入力 0 個（レーダー円・螺旋）〜8 個（廊下・地面・車線・格子・アトラクタ） |
| | `commit()` | 共 | 毎フレーム1回（面0か面1） |
| | `resource()` / `resource(surface)` / `createSurface()` | 共 | 面0は全画面画像、面1はレーダー |
| IR | `SET` `INPUT` `ADD` `MUL` `REPEAT` `END` `MOVE` `LINE` | 共 | |
| | `SIN` | A1, ZE, LI | 正弦の走査（A1）、星の散布とスカイラインの高さ（ZE）、アトラクタ（LI） |
| | `PLOT` | ZE | 星 |
| | `REPEAT_REG` | ZE, LI | 地面の本数を入力から |
| | `BREAK_IF_GT` | LI | アトラクタの本数を入力の閾値で打ち切る |
| | `PLOT_COLOR_REG` | ZE, LI | レーダーの輝点（白と橙を交互） |
| | `LINE_COLOR_REG` | ZE, TW, LI | 地面の走査線、横木、8 段格子 |
| | `CUBIC` | TW, ZE, LI | 天井のアーチ、レーザー、レーダーの円 |
| view | `replace` / `patch` | 共 | 場面（Act I は1セット）ごとに REPLACE、ほかは PATCH |
| | `stats()` | 共 | `MEGADEMO VIEW … commands=` |
| | `resource('pets')` | Act II | 机の上のペット（ズームアウト時） |
| | `cache.create` / `tx.instantiate` / `instance.place` / `instance.setVisible` | ZE | 残機アイコン3つ、上下に揺れ、1つは途中で消える |
| | `tx.background` `tx.rect`（`opacity`） | 共 / LI | LI の統計板の2枚目が半透明 |
| | `tx.roundRect`（`radius`） | Act II, ZE, LI | モニターの筐体、ランプ |
| | `tx.strokeRect`（`width`） | Act II, ZE, LI | 画面枠、レーダー枠 |
| | `tx.gradient`（`axis` x/y、`dither`） | A1, Act II, LI | ニュースの背景（x）、部屋の壁（y、dither）、LI の負荷メーター（`setRect` で伸縮） |
| | `tx.text`（`caption` / `display`、`capacity`） | 共 / TW / ZE, LI | TW の題字は `display` |
| | `tx.image`（`clip`、`sourceX/Y/Width/Height`、`variant`、`scale`） | 共 / ZE, LI / Act II / ZE | 面0、リサイズ画像、面1の中央切り抜き、ペット、ピクセル窓（`scale: 2`） |
| | `tx.group(first, count, opacity)` | LI | 統計板2枚をまとめて不透明度 170 |
| | `ref.setRect` / `setVisible` | 共 | ズーム、HUD の出し入れ |
| | `ref.setClip` | TW | 入場時のアイリス（面0画像の clip を広げる） |
| | `ref.setColor` | ZE, LI | ランプの点滅 |
| | `ref.setText` / `setReveal` | ZE, LI / TW | スコア・統計 / 題字のタイプライター |
| | `ref.setImageFrame` | Act II | ペットのコマ送り（ズームアウト中） |
| | `ref.setRotation` | ZE | 面1の縮小画像を機体のロールに合わせて回す |
| | `ref.animate`（bounds＋rotation、`linear`、`loop`）/ `animation.stop` `finish` `poll` | ZE, LI | レーダー切り抜きのネイティブ回転。ズームアウトで `finish`、90 フレーム目で `stop`＋`poll` |
| grid | `registerResizeSource`（既定 bilinear）/ `(sampling: 'nearest')` / `resource` | 共 / ZE | 面0→112×63 のモニター、面1→60×34 |
| pixel | `open` / `stage` | 共 / ZE | 32×24 の munching squares（7 命令） |
| 使わず | `mount` / `view.set` / `bind` / `unbind` | — | REPLACE/PATCH と同じ APP lease で混ぜない契約（[architecture.md](architecture.md)）。このアプリは REPLACE 系 |
| | `createScene`（`invalidate` / `flush`） | — | 同じ replace/patch の上の制御器で、描画される内容は増えない |
| | `tx.modal.open` / `close` | — | 入力域を modal に切り替え全画面の幕を掛ける。Enter と矢印の操作を保つため使わない（overlay では UNSUPPORTED） |
| | `poll` / `cancel` / `features` / `inputScope` | — | 提出状態・機能・入力域の問い合わせで、描画ではない |
| | `cache.release` | — | テンプレートはセッション中使い続ける。解放は instance が表示から消えた後（寿命の契約） |
| | `grid.register` / `registration` / `registerResize` / `run` / `profile` / `measure` / `explain` | — | JS が持つ数値格子の縮約・畳み込み。入力をゲストの配列で渡す面で、手続き描画のストレスとは別 |
| | `grid.fold` / `index` / `view`（`gridFold`） | — | 初回呼び出しで約 6 KB の JS フロントエンドをゲストに読み込む |
| | `video.*` | — | ストリーム源（SD・ネット）が要る。JS から `push` すると 240×135 の 1 フレームで 64,800 B をゲストに持つ |
| | `pocket.overlay` | — | FLOWER 背景の overlay プロファイル専用 |
| | 画像の `rotation`（生成時の指定）、`scale: 0.5`、`easing` の他の値、`repeat: once/ping-pong` | — | 回転は `animate` と `setRotation` で、倍率は `scale: 2` で、同じ経路を通している |

## ゲストヒープ

`python3 tools/kasane_contract/run_megademo_app_host.py --m32`（i386、`-malign-double`、device と同じ JSValue 8 B／ポインタ 4 B）。確保は device の課金に合わせて TLSF 長（要求を 4 B 境界へ切り上げ、最小 12 B）で数える（`docs/vm/backlog.md` #8/#9: 実機は `heap_caps_get_allocated_size()` の実長で課金）。上限は `main/app_session.c` の 160 KiB（163,840 B）、GC の初回閾値はその半分（`pocketjs_guest_create()` と同じ）。

| | 変更前（c53f661） | 変更後 |
| --- | --- | --- |
| ソース | 10,802 B | 26,986 B |
| 評価の直前 | 30,708 | 30,708 |
| 評価中のピーク（コンパイル） | 69,812 | 115,940 |
| 評価後 | 54,952 | 97,012 |
| 実行中の live 最大（毎フレーム GC 後） | 57,328 | 109,384 |
| **評価と 1,136 フレームが通る最小の上限**（二分探索） | **74,453（45%）** | **123,125（75%）** |

QuickJS は参照カウントで即時解放するので、GC 前後のピークは同じ値になった。律速は実行中ではなく**評価（コンパイル）時**。最初の実装（33 KB、関数 104 個、plan を生成関数で書き、場面の全プログラムを一度に作る）は最小上限 ~199 KB（glibc 課金）で、実機では評価に失敗する見込みだった。次の3つで下げた（いずれもプログラムと画素のハッシュが不変なことを確認）:

1. plan のプログラムを**テキスト**（1文字＝1 opcode）にして、デコーダ `prog()` 1つで配列へ展開する。生成関数と、アセンブラの 17 個のクロージャが消えた。Act I の `program()` も同じ形式に直した（C 参照との一致試験は不変で通る）。
2. 先読み中に場面の全プログラム配列を保持していたのを、plan ごとの thunk にした（実行中のスパイクが消えた）。
3. テンプレート文字列の実行時連結をやめ、view の Act I セットをデータ配列＋ループにした。

測り方の注意: host の値は device と同じ意味の数え方をしているが、実機のゲストには他の `pocket.*` の構築状態がある一方、host にはここでの計数用 prelude（約 4 KB）がある。**実機での余裕は未確認**。

## host 検証

| 検査 | 内容 | 結果 |
| --- | --- | --- |
| [`run_proc_megademo_scenes.py`](../../tools/kasane_contract/run_proc_megademo_scenes.py) | 3段階×6場面×全 1,104 フレーム、7,440 draw。各 draw を plan（`ksn_proc_plan_run`）・デバッグ単歩 plan・生 VM の `ksn_proc_begin`＋`ksn_proc_step` で走らせて線分・ラスタ・ステップ・レジスタを比較、点列は scalar・PIE モデル・plan のディスパッチを比較。フレームを全画面と 8 行帯で描いて全画素比較。Act I は `proc_megademo.h` と全画素一致。adapter の上限（線分・ラスタ・ステップ・入力・座標・plan 数）を全フレームで検査。15 opcode すべての使用を要求。 | PASS。ハッシュ programs `0343c4b5d45b5f39` pixels `49cb7fdda82a08c2` |
| [`run_megademo_app_host.py`](../../tools/kasane_contract/run_megademo_app_host.py) | アプリ全体を実 `pocket.kasane`（view・procedural・grid・pixel）と実レンダラで 1,136 フレーム。例外 0・DEGRADE 0・表示失敗 0、live ≤ 32、累計 > 32、旧新の共存あり。ASan/UBSan（64 bit）と `--m32`。 | PASS（両方） |
| `run_proc_megademo_js.py`（更新） | Act I の programs/inputs が C 参照と一致、48 フレームの draw がその場面の plan を指す、ズームの切替、942 フレームの読み込み（live ≤ 32・累計 > 32・解除あり・死んだ handle を使わない） | PASS |
| `run_pocket_proc_qjs.py`（更新） | 実 `pocket_proc.c` で 48 フレームの全画素一致（scalar と fake PIE）、ズーム往復 | PASS（REPLACE 回数の期待値を 1→2 に。48 フレーム目で TWIST のセットに入るため） |
| `tools/kasane_contract/run.sh` 全体 | WSL | PASS（exit 0、14 分、2026-09-29。その後の `scale: 2` の変更は上の4本を再実行して PASS） |

## ファームのビルド

`idf.py -B build_mega build` は成功（ESP-IDF v6.0.1、`tools/prepare_dependencies.py` の後）。image 2,171,648 B、SHA-256 `a2f32590a690951aba03995edbec23d9fbc976d3b18f1a6b3ab30cd9f86c988e`。`tools/memlog.py` は `DIRAM +0`（diram=172,028 B）。JS は `.rodata.embedded`（flash、`_binary_proc_megademo_js_start` = 0x3c1b6265、26,987 B）に置かれ、DIRAM には入らない。C の変更はないので DIRAM の増減が無いのは期待どおり。

## プレビュー画像

`python3 tools/kasane_contract/run_proc_megademo_scenes.py --out .cache/kasane_megademo_scenes/preview`（手続き面だけ、`--sheet-tier` で段階を選ぶ）と `python3 tools/kasane_contract/run_megademo_app_host.py --ppm .cache/kasane_megademo_scenes/preview`（実レンダラで合成したパネル）。一覧は左上から右へ時間順。

| ファイル | 中身 |
| --- | --- |
| `sheet_twist_tier2.png` / `sheet_zenith_tier2.png` / `sheet_limit_tier2.png` | 各場面を 16 枚に等間隔で（面1のフレームはレーダー） |
| `sheet_twist_motion.png` / `sheet_zenith_motion.png` / `sheet_limit_motion.png` | 連続 16 フレーム（TWIST 56–71、ZENITH 40–55 はバレルロール、LIMIT 40–55） |
| `sheet_tiers.png` | 同じ瞬間を LIGHT／MID／HEAVY で横に並べた5行 |
| `sheet_act1.png` | Act I（変更なし）の 1 フレームおき |
| `sheet_app_composited.png` | view を含む合成パネル（NEWS、TWIST のアイリス、ZENITH の HUD、LIMIT、ズームアウトした Act II の机とモニター、MID 段階） |

リポジトリには [合成パネル](megademo-act2-host.png) と [TWIST の 16 枚](megademo-twist-host.png) を置いた（host の描画であり、実パネルの撮影ではない）。

## 調整ノブ

負荷の値は `KN`（段階ごとの行）と `LOAD` の2か所だけにある。`KN[t].c` が TWIST、`.z` が ZENITH、`.l` が LIMIT。

| 段階 | TWIST `c` | ZENITH `z` | LIMIT `l` | 回転 `q` |
| --- | --- | --- | --- | --- |
| LIGHT | n 12, r .7, m 2, s 4, kf 2, ka 3, D 4 | 星 24, 地面 8, 車線 7, 街 2×4, 円盤 2, レーザー 6 分割, レーダー 2 重 | 廊下 n 12 r .7 m 2 s 4、D 6、地面 8、車線 7（dn 150）、ai 6・ao 12、格子 1.5 | なし |
| MID | HEAVY と同じ（改訂前 n 18, r .76, m 3, s 6, D 5） | HEAVY と同じ（改訂前 40, 11, 11, 3×6, 3, 10, 3） | n 18 r .76 m 3 s 6、D 7、11、11（dn 200）、ai 12・ao 22、格子 2.5 | なし |
| HEAVY（既定） | n 26, r .8, m 3, s 8, kf 2, ka 3, D 6 | 60, 14, 15, 3×8, 4, 16, 4 | n 26 r .8 m 3 **s 8** ka 2、D 8、14、15（dn 285）、ai 16・**ao 37**、格子 **5.7**（改訂前 s 6、ao 34、格子 4.6） | ZENITH のレーダー |

n は廊下の奥行きの枠数、r は1枠ごとの縮小率、m は1枠あたりの横木、s はアーチの分割、kf/ka は枠・アーチを描き始める奥行き（手前の画面外の枠は描かない）、D はねじれの最大（廊下全体の回転、ラジアン）。ai はアトラクタの1本あたりの内側反復、ao は本数。`q` は [ZENITH のレーダー, 縮小画像, LIMIT のレーダー] を回すか（回す面1の画像1つにつき実機で約 23 ms）。`LOAD` は上の「動的 plan 読み込み」。改訂の根拠は [megademo-device-limits.md](megademo-device-limits.md)。

HEAVY の LIMIT は「各上限の 8〜9 割、かつ 1 draw 8,192 を超えない」を host の全フレーム検査で確かめながら詰めた値。最初の版は線分 951・ラスタ 36,020 だった。側面の手すり・段（ラスタが 1,000 前後と軽い）を天井・壁の横木（6.6k）へ替え、車線を画面外まで延ばし（`dn` 150→285、300 では座標が範囲外になり検査が止めた）、格子を広げ（`lt` 2→4.6）、アトラクタを点から線にして 56,352 にした。

## 実機で測るべき項目

7 項目とも [megademo-device-limits.md](megademo-device-limits.md) で測った（2026-09-29）。パネル上の見え方（色、ねじれの錯視、面0の止まり）は人が見る項目として同文書に残した。

## 迷った点と判断

- **面1の更新頻度**: 表示待ち候補は全面で1枚（D2 の契約）なので、2 面を同じフレームで更新できない。面1（レーダー）は 4 フレームに1回だけ更新し、そのフレームは面0が止まる。ネイティブの変更（候補を面ごとに持つ）は、実機でこの止まりが見えるかを確かめてから判断すべきで、今回は変えていない（実機計測後も同じ判断。人が見る項目に残した）。
- **既定の段階**: ストレステストの目的から HEAVY にし、失敗時は自動で段階を下げる。実機計測でも HEAVY は安定した（megademo-device-limits.md）。ただし `DEGRADE` は負荷を下げるだけでメモリは減らさないので、メモリの失敗は救えない（統合直後の ZENITH がそうだった）。
- **Act I を残した**: 既存の 48 フレームの C 参照・実機ハッシュとの比較を壊さないため、Act I の programs/inputs は同一のまま（書き方だけテキストに変えた）。
- **Zenith の解釈**: 作品の画面を確認できなかったので、確かめられた事実（ロールする疑似 3D シューター、建設中の都市）と Apple II ハイレゾの一般的な性質から組んだ。
- **使わなかった API**: 網羅表のとおり。特に `mount`/`view.set` は REPLACE 系と同じ APP lease で混ぜない契約、`video` は JS からフレームを渡すとゲストヒープに 64,800 B 要る、`grid.fold/index/view` は 6 KB の JS をゲストに読み込む。
