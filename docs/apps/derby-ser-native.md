# DERBY WATCH: `ser()` を C に写す（S1b = N2 + N12）

2026-10-01、ブランチ `vm/ser-native`（`vm/main` fac3552 から）。[derby-native-survey.md](derby-native-survey.md) のシナリオ S1b を実装する。目標は楕円コーナーの首振り WIDE 2 を 20 fps 以上にすること（今 実測 16.1 fps、MID）。**画素と結果は変えない。** この節（§1〜§7）は実装の前に書いた設計で、数値はすべて推定。結果と実測は §8 以降。

## 1. 境目（JS に残す分 / native に下ろす分）

| | JS に残す | native に下ろす（`main/pocket/pocket_derby.c`） |
| --- | --- | --- |
| コース | `OC`・`CH`・`pose()`（`wide`・`shot`・距離標・走者・大型画面が使う） | 同じ表の写し（レースの最初に 1 回渡す）と、その上の `pose()` の C |
| 1 フレーム | カメラ `pc` の決定（`shot()`）、`zf`・`L` の決定、系列の呼び出し 5 回、大型画面・距離標・走者・HUD | 弦の端のカメラ座標 `VE` と視野に入る弦 `VC`（今の `pan()` の 82〜88 行） |
| 系列 | 呼び出し（plan のハンドルと数 6〜7 個） | `ser()`・`lim()`・`ou()`・`inr()` の全部と、そこから出る `draw`（`prail`・`t0`・`t1`・`hl`・`crowd`） |
| plan・VM | 触らない | 既存の `draw` の経路をそのまま通す（§4） |

`ser()` の C は JS の**直訳**にする: 倍精度、同じ演算の順、同じ判定・LOD・格子・丸め。`Math.round`・`Math.min`・`Math.max` は QuickJS の実装（`js_math_round`・`js_fmin`・`js_fmax`。符号付きのゼロと NaN の扱いを含む）を写す。`floor`・`ceil`・`sqrt`・`sin`・`cos`・`fmod`（JS の `%`）は QuickJS と同じ libm の関数を呼ぶ。コンパイラが式を変えないよう、このファイルは `-ffp-contract=off`（倍精度は soft-float なので Xtensa では元から融合しないが、host を揃える）。

## 2. JS から見える入口

DERBY のセッションにだけ注入する面（`pocket.pet` と同じ作り。[common-api.md](../api/common-api.md) §2「アプリ固有の面は共通 API に含めない」）。名前空間 `pocket.derby` は最初に読まれたときに作る（`pocket_api_lazy()`）。

```js
// レースのコースが決まったとき（enter('pad')）に 1 回。
// ends: 弦の端の g（CH）。sections: 区間の表（OC）、直線は 0（pose は [g, w, 1, 0]）。dnr: DNR。
pocket.derby.course(ends, sections, dnr)
// 首振りのフレームごとに 1 回、系列の前に。pc = [x, z, 向き x, 向き z, f, 先頭までの距離]。
// t: シーンのフレーム数（L = 0 の系列だけが使う）。hl・crowd: スタンドの段と観客の plan のハンドル
// （未登録は偽、描かない）。hlColor: KN[tier][0]、cells: KN[3][5]。
pocket.derby.view(pc, zf, t, hl, crowd, hlColor, cells)
// 系列 1 本 = 今の ser(n, w, s, g0, L, zf, a, b)。h: plan のハンドル（偽なら本体の draw を飛ばす。
// hl・crowd は描く。今の dr() と同じ）。b を省くと芝（t0・t1）の式。
pocket.derby.ser(h, w, s, g0, L, a, b)
```

- 1 フレームの境目の呼び出しは `view` 1 回 + `ser` 5 回（スタンド・外柵・芝 2・内柵）。描画の順（奥から手前、間に大型画面・距離標・走者）は今と同じで、JS が呼ぶ順で決まる。
- `capability`: `derby.series`。**この面が入った firmware の DERBY のセッションでだけ** `pocket_api_register()` で登録し `supported: true`。ほかのセッションでは登録しないので `supported: false`。manifest（`app_registry.c`）の `local.derby` の optional に名前を足す（`pet.companion` と同じく、名前を挙げたセッションにだけ注入）。
- `limits`（コードで強制する値だけ）: `maxEnds`（`course` の弦の端の数、32）、`maxSections`（区間の数、4）。DERBY は 19 と 3。

## 3. native が持つ状態

- `course` で heap に 1 ブロック確保: 弦の端 n 個（double）、区間 m 行 × 7（double）、`dnr`、フレームごとの `VE`（4n double）と `VC`（n−1 バイト）。DERBY の楕円で約 0.95 KB、直線で約 0.1 KB（推定）。`course` の呼び直しで作り直す。
- `view` の値（`pc` の 6 個、`zf`、`t`、2 つのハンドル、2 つの数）は静的な構造体（約 80 B の `.bss`。heap にまとめる案もあるが、`course` の前の `view` を BUSY にするのと同じ判定で足りる）。
- **解放**: `pocket_derby_reset()` を `app_stop()` から `pocket_kasane_reset()` の直後に呼ぶ（heap のブロックを返し、状態を 0 に）。ゲストのコールバックも JS の値も持たないので、順序の条件は増えない（ハンドルは数として持つだけ）。

## 4. 既存の `draw` の経路への接続

`pocket_proc.c` の `draw_impl` を 2 つに分ける: 入力を読む前半（配列 / 数値引数）と、`float` の入力 8 個で VM を回して候補のフレームに足す後半。native 用に `pocket_proc_draw_numbers(ctx, handle, const double *in, n)` を足す。中身は JS の `H.draw(h, [...])` と同じ順の検査:

1. `beginFrame` の中か（外なら `BUSY`「beginFrame required」）、再入でないか（`js_call_active`）。
2. ハンドルの slot（無ければ `CLOSED`）。
3. 各入力が有限かつ `float` にして有限か（違えば `INVALID_ARGUMENT`「non-finite input」）。
4. VM（ステップ上限 10,000）、線分の上限（面の `maxSegments`、既定 1,024）、ラスタの上限。失敗は `LIMIT_EXCEEDED` などで、そのフレームを捨てる（`building = false`）。

`ser` は最初に失敗した `draw` の例外をそのまま返して止まる（JS の `ser()` が `dr()` の例外で止まるのと同じ）。診断の計時（`KASANE_MEGADEMO_TRACE` の `draw_us`・`draw_n`、`KASANE_BGCOST_TRACE` の plan 別）にも native の `draw` を数える。

## 5. N12: 数値引数の `draw`

`H.draw(h, a0, ..., a7)`: 2 つ目の引数が数のとき、続く数を入力にする（配列を作らない）。**末尾の `undefined` は渡さなかったのと同じ**（短い配列と同じく 0 で埋める）。途中の `undefined` や数でない値は `INVALID_ARGUMENT`。配列の形 `H.draw(h, [..])` はそのまま。DERBY の `dr` は `(n, a, b, c, d, e, f, g, h) => { if (live[n]) H.draw(live[n], a, b, c, d, e, f, g, h); }` にして、呼び出しの配列リテラルを外す（`dr(extra[0], extra[1])` のような配列の呼び出しは、末尾の `undefined` を無視するのでそのまま通る）。

## 6. フレームの流れ（首振り）

```
paint → H.beginFrame → course() → pan(xs):
  zf, L を決める
  S.view(pc, zf, t, live.hl, live.crowd, k[0], KN[3][5])   // VE・VC（楕円のみ）
  S.ser(live.prail, 40, 12, 0, L, 31727, -7.5)            // スタンド（hl・crowd も）
  大型画面（hl 2 本、feed）                                 // JS
  S.ser(live.prail, DFR, k[4], 0, L, 0xad55, 4.9)         // 外柵
  S.ser(live.t0, DNR, 2 * k[5], 0, L, 11.6)               // 芝
  S.ser(live.t1, DNR, 2 * k[5], k[5], L, 11.6)
  距離標・走者                                              // JS（N12 の数値引数）
  S.ser(live.prail, DNR, k[4], 0, L, 0xffff, 4.9)         // 内柵
→ H.commit
```

## 7. 見積もり（推定）

- **ゲスト**: `ser`・`ou`・`lim`・`inr` と `pan()` の `VE`・`VC` の JS、トップレベルの名前 8 個（`ser`・`ou`・`lim`・`inr`・`Lo`・`Hi`・`VC`・`VE`）が消え、`S`（`pocket.derby`）1 個と関数オブジェクト 3 個が増える。survey の −3〜−6 KB の範囲を見込む。評価のピークも下がる側。
- **native**: flash +2〜3 KB（C の `ser` と入口）、`.bss` 約 0.1 KB、DERBY の実行中の heap 約 1 KB（§3）。ほかのアプリは `.bss` 分だけ。
- **時間**（w=−100、実機の尺度）: 系列 27.6 ms が、呼び出し 6 回（0.2〜0.4 ms）+ C の倍精度の算術（4〜8 ms の幅、survey）+ VM と線分（今と同じ約 1.8 ms）に。N12 で −0.3 ms。ターン 20.8〜24.8 ms → 23.4〜25.9 fps（survey）。

## 8. 試験の方法（host）

- **oracle**（最強の検査）: `DERBY_SER_ORACLE=1` の harness は、今の JS の `ser()`・`ou()`・`lim()`・`inr()` と `VE`・`VC` の計算の写し（`tools/games/derby_ser_oracle.js`、vm/main fac3552 の JS から機械的に置き換えたもの）を、`pocket.derby.view`・`ser` の呼び出しのたびに先に回し、JS が出す `draw` の入力の列（ハンドルと倍精度の 8 個）を、native が `draw` に渡した列と**ビット単位で**比べる。あわせて、ランダムなカメラ・コース・w・s・L・色の組み合わせ（総当たりに近い数千ケース）を `draw` を走らせない検査モードで比べる。
- **端から端**: `run_derby.py`（ASan と `--m32`）の全構成で、着順・時計・種・デモ・R 再生・オッズ・保存の行と、全画素ハッシュが vm/main fac3552 と一致すること。
