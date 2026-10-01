# DERBY WATCH: `ser()` を C に写す（S1b = N2 + N12）

2026-10-01、ブランチ `vm/ser-native`（`vm/main` fac3552 から）。[derby-native-survey.md](derby-native-survey.md) のシナリオ S1b を実装する。目標は楕円コーナーの首振り WIDE 2 を 20 fps 以上にすること（今 実測 16.1 fps、MID）。**画素と結果は変えない。**

**結論（実測）: 楕円コーナーの首振り WIDE 2 は MID 15.9 → 24.6 fps（JS 44.11 → 22.00 ms）、HEAVY 16.0 → 24.4 fps。w=−3 でも 13.3 → 23.0 fps（MID）。** 着順・時計・種・デモ・R 再生・オッズ・保存と全画素ハッシュは vm/main fac3552 と全行一致（host）、JS の `ser()` と C の `draw` の入力はビット単位で一致（host の oracle、0 件の不一致）。ゲストの最大は 138.4〜138.6 KB → 133.2〜133.3 KB（実機 MID）、評価の余裕は 22,477 → 30,638 B（実機）。 §1〜§7 は実装の前に書いた設計で、§7 の数値は推定。試験の方法は §8、結果は §9 以降（実測と推定を書き分ける）。

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

## 9. 結果（host、WSL）

- `run_derby.py`（ASan、16 回 + oracle 3 回）・`--m32`（16 回）・`--course oval`（ASan、16 回 + oracle 3 回）が全回 PASS。
- **基準（vm/main fac3552 を別の木に展開して同じ手順で走らせた）と、全 16 構成で着順・完走時刻（全桁）・種・デモ = 手のレース・R 再生・保存の行と全画素ハッシュが一致**（ASan と m32 の両方。楕円だけの構成も）。Newton の誤差の行も同じ（最大 0.0749 px）。`tune_derby.mjs`（直線・楕円）と `derby_corner.mjs app` は PASS。
- **oracle**（§8）: ゲーム中の呼び出し 19,935 回（種の混在）・27,310 回（楕円のみ）、ランダムな 32,000 呼び出し × 6 回（draw 103,315 本ずつ）で、**不一致 0**。ASan/UBSan のエラーなし。
- **m32 では oracle を回さない**: i386 は倍精度を x87 の 80 ビットで計算し、C（式を registers に保つ）とインタプリタ（演算ごとに格納）で末尾 1 ビットがずれる（最初の試行で 27,721 件、どれも 1〜2 ulp）。実機（soft-float の IEEE）と 64bit host（SSE2）には無い性質なので、判定は 64bit の build で行う。m32 でも画素ハッシュは全構成一致（入力は float に丸めて VM に渡るため）。
- N12（数値引数の `draw`）単独: 全行・全画素一致。`run_pocket_proc_qjs.py`（O2・O0）PASS（配列と数値で同じフレーム、誤りの場合）。

### ゲスト（host、TLSF モデル）

| | 基準 fac3552 | N12 のみ | 今回（N12 + N2） | 差（今回 − 基準） |
| --- | ---: | ---: | ---: | ---: |
| m32 評価後 | 116,996 | 117,136 | 111,524 | **−5,472** |
| m32 評価のピーク | 131,660 | 131,800 | 126,008 | **−5,652** |
| m32 GC 後の最大（構成ごと） | 140,240〜142,564 | 140,808〜142,828 | 134,820〜138,044 | **−4,520〜−5,544** |
| 64bit 評価後 / 評価のピーク | 159,940 / 173,680 | — | 154,172 / 167,664 | −5,768 / −6,016 |

N12 の列は harness の包み（数値引数を読む分）が評価前に +64 B 増えた値を含む。アプリ分は評価後 +76 B、GC 後の最大 +200 B（m32）。

## 10. 実機（COM3、2026-10-02）

- image: 同じ firmware（この枝の C、`-DKASANE_MEGADEMO_TRACE=ON -DKASANE_BGCOST_TRACE=ON -DPOCKET_KEYTEST=ON`、`-DSDKCONFIG` 分離）に、`apps/derby` だけを差し替えた 4 構成（基準 = fac3552 の JS / 今回、それぞれ WIDE 2 の楕円用 w = −100 / −3）。**デモの種は `HW = 0x2545f492` に固定**（`vm/trig-device` の derby-trig-cull-device.md と同じ値。保存は race=18 で、デモは直線 1・楕円 4、今回の構成は 420 秒で 6 本目の直線まで進む）。bin に入った JS（`S.view(`・`function ser(`・w・`HW`）を確かめた。
- 手順は [`tools/games/ovalcost/ser_device.py`](../../tools/games/ovalcost/ser_device.py)（書き込み → 6 秒後に DERBY → キーを押さずにデモ 420 秒。HEAVY はパドックで tab を 1 回押して 330 秒）。集計は切り替わりの後 3 ターンを捨てた中央値。**MID は 3 回の中央値（3 回の差は 0.1 fps・0.2 ms 以内）、HEAVY は 1 回。**
- 全 run で `LOADSTALL`・`FRAMEFAIL`・`LOADFAIL`・`DEMOFAIL`・OOM なし。**レースごとの `FINISH`（着順・完走時刻の全桁）は 4 構成・全 15 run で同じ**（race 1000001〜1000005）。

### fps / JS ms / VM draw ms（本数）

| 場面 | 基準 w=−100 | **今回 w=−100** | 基準 w=−3 | **今回 w=−3** |
| --- | --- | --- | --- | --- |
| **MID 楕円 首振り WIDE 2（コーナー）** | 15.9 / 44.11 / 4.69（31） | **24.6 / 22.00 / 3.04（31）** | 13.3 / 55.68 / 6.19（40） | **23.0 / 24.15 / 3.84（40）** |
| **HEAVY 楕円 首振り WIDE 2** | 16.0 / 43.50 / 4.82 | **24.4 / 22.02 / 3.20** | 13.5 / 54.40 / 6.43 | **22.8 / 24.12 / 4.11** |
| MID 楕円 首振り WIDE 3 | 19.5 / 31.70 | 25.8 / 19.42 | 16.1 / 42.90 | 24.7 / 21.36 |
| HEAVY 楕円 首振り WIDE 3 | 19.1 / 32.05 | 25.4 / 19.61 | 15.9 / 43.10 | 24.3 / 21.61 |
| MID 直線 首振り WIDE 2 | 24.9 / 21.00 | 27.6 / 17.00 | 24.9 / 21.00 | 27.6 / 17.09 |
| HEAVY 直線 首振り WIDE 2 | 24.5 / 21.38 | 27.2 / 17.33 | 24.5 / 21.35 | 27.2 / 17.37 |
| MID 横見 WIDE（直線） | 29.9 / 14.40 | 29.5 / 13.31 | 29.8 / 14.40 | 29.4 / 13.27 |
| MID 横見 FIELD（直線） | 29.9 / 14.60 | 30.2 / 13.35 | 29.9 / 14.61 | 30.3 / 13.35 |
| MID 楕円 VISION | 25.7 / 18.60 | 27.0 / 16.75 | 25.7 / 18.60 | 27.0 / 16.75 |

- 受け入れ基準（楕円コーナーの WIDE 2 が 20 fps 以上、MID・HEAVY）: **w=−100・w=−3 とも満たす。** survey の見立て（推定）23.4〜25.9 fps（w=−100）、22.0〜24.9 fps（w=−3）の中。
- 楕円 WIDE 2 のフレームの非 JS 分（間隔 − JS）は基準 18.8 ms、今回 18.7 ms で変わらない。今の上限は JS 22 ms + 非 JS 18.7 ms。27 fps（間隔 37 ms）には JS を約 18.3 ms まで下げる必要がある（推定）。
- VM draw の時間が減ったのは、計時に入っていた配列の読み取りが無くなったため（native の draw は `float` を直接渡す）。plan と線分は同じ（本数も同じ）。
- 横見の WIDE は JS が 1.1 ms 減ったのに 0.4〜0.5 fps 低い。今回の構成は 6 本目のレース（直線）まで進み、横見 WIDE のフレームが倍（169 対 84）で、同じフレームの比較になっていない。切り分けていない（確信の低い点）。

### メモリ（実機）

| 項目 | 基準 w=−100 | 今回 w=−100 | 基準 w=−3 | 今回 w=−3 | 線 |
| --- | ---: | ---: | ---: | ---: | --- |
| ゲストの最大 `gu` MID（3 回） | 138,432〜138,556 | **133,192〜133,276** | 138,520〜138,556 | 133,200 | ≤ 140,000 |
| 同 HEAVY | 138,624 | 133,292 | 138,708 | 133,360 | |
| 評価の余裕（heapprobe `derby`、16 B 刻み） | 22,477（141,363 で評価） | **30,638**（133,202、2 回同じ） | — | — | ≥ 20 KB |
| ターン内の最小 `mn` MID / HEAVY | 26,124 / 29,492 | **33,672〜33,796 / 34,064** | 26,124 / 28,884 | 33,672 / 33,856 | ≥ 11,208 |
| ゲートのターンの最小 MID / HEAVY | 31,876〜31,956 / 31,820 | **34,844〜35,152 / 35,028** | 31,876〜31,904 / 31,752 | 35,100〜35,164 / 35,084 | ≥ 23,552 |
| 1 レースごとのゲストの最大 MID（1 回目、S 直線・O 楕円） | 137,676 S・138,392 O・138,432 O・138,424 O・138,424 O | 132,376 S・133,060 O・133,120 O・133,276 O・133,172 O・133,012 S | | | 増え続けない |

- 基準の `mn` の最小は最初のターン（評価の直後）。最初のターンを除くと基準 29,044〜29,468、今回は最初のターンも含めて 33,672 以上。
- 6 レース連続（直線と楕円）でゲストの最大は増え続けない（1 → 2 レースで約 700 B 上がり、その後 ±150 B）。

### firmware（`idf.py size`、通常 image、基準 fac3552 との差）

| | 差 |
| --- | ---: |
| DIRAM（`.bss`） | **+96 B**（`view` の状態とコースのポインタ） |
| IRAM | 0 |
| flash `.text` | +9,624 B（`pocket_derby.c` の `.text` 11,254 B のうち `js_ser` 5,027 B。`-O2`） |
| flash `.rodata` | −3,488 B（埋め込みの JS が縮んだ） |
| image 全体 | +6,136 B |
| DERBY の実行中の heap | コースのブロック約 1.0 KB（楕円）/ 約 0.1 KB（直線）。推定（構造体と double の数から） |

### 回帰（実機）

- 一巡（今回の診断 image、キー注入）: 起動で `LOADED` → `1` でレース → 結果 → `R` の再生（`FINISH` の全桁が一致）→ `1` で次のパドック → 放置でデモ → キーで `DEMO END` → Back（`SAVE`）→ 起動し直して `LOADED`。KEYTEST（`r`）の起動と Back。
- 通常 image（今回）: `smoke_device.py --cycles 20`（`SMOKE_OK 20`）、`stress_app.py`（`STRESS_APP_PASS`）、`test_app_resume.py`（`TEST_APP_RESUME_OK`）、MEGADEMO（40 秒）・LCD CATCH・BIG WAVE・DERBY の起動と Back（異常の印なし）。
- 一巡と再生の確認で DERBY の保存が進んだ: `points=6070 race=18` → `points=5870 race=19`（手で走らせた 2 レース。設定は変えていない）。
- 実機は最後に通常 image（vm/main fac3552 のビルド）を書き戻し、`HOME_READY` を確かめた。

## 11. 確信の低い点

- 実機で画素が同じことは見ていない（MISO が未配線で読めない）。根拠は host の画素ハッシュ・oracle と、実機の `FINISH` の全桁一致・同じ VM draw の本数。実機の C と JS が同じ倍精度を出すことは、soft-float と newlib の libm を両方が通ることに依る（host の SSE2 と同じく IEEE の丸め）。
- 横見 WIDE の −0.4〜0.5 fps（上の表）は切り分けていない。命令キャッシュの差（CLAUDE.md: 同じカーネルでビルド間 15%）か、場面の違いか。
- HEAVY は各 1 回。
- flash の `.text` +9.6 KB は見積もり（2〜3 KB）より大きい。`-O2` の展開による。`-Os` にした場合の速さとの引き換えは測っていない。
- N12 単独ではゲストの GC 後の最大が +200 B（m32）。中身（引数 9 個の `dr` の呼び出しで残るもの）は調べていない。今回の全体では −4.5〜−5.5 KB なので受け入れ基準には効かない。
- 固定種の image は出荷の JS と `HW` の行だけ違う。
