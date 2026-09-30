# 面ごとの線分上限（`createSurface({maxSegments})`）

ブランチ `vm/kasane-surface-cap`（`vm/main` e15e6eb から）。依頼は「DERBY WATCH の大型画面（面1）が、1 枚約 10 KB の連続領域を 2 枚取れずに失敗する。線分の上限を面ごとに宣言できれば第2面を確保できるか」。**数値は種別を明記する**: 実機の実測、host の実測、計算。

## 結論

- **`createSurface({maxSegments: n})`（n = 1〜1,024）を足した。** 面1の候補・確定フレーム 2 枚が、それぞれ `6 + 10·n` B になる（計算。host で確保量を実測して一致）。引数なし・`{}`・`undefined` は従来どおり 1,024（10,246 B）。面0は変えていない。
- **上限は `draw()` が実行時に強制する。** 超える `draw()` は従来の 1,024 と同じ `LIMIT_EXCEEDED`（"frame drawing limit"）で、その `draw()` の線分は候補に入らずフレームは閉じる。不正な値は `INVALID_ARGUMENT` で、面の ID を発行しない。
- **実機で、DERBY WATCH の評価直後に n = 256 の面1が確保できた**（既定 1,024 は同じ image で `frame allocation failed` を再現）。数値は下の「実機の測定」。
- **ただし DERBY にそのまま足すと、plan の登録が止まる。** 面1の約 5.2 KB で、レース中の空きが DERBY 自身の登録の門（18,432 B）を下回り、1 レース目の間ずっと 6 本の plan が未登録のまま残った（実機、下の表）。適用には門か他のメモリの見直しが要る（下の「DERBY に適用するには」）。
- `capabilities` の `limits` には何も足していない（面の上限は面ごとに JS が決める値で、固定の上限は従来どおり 1,024）。

## API

```js
const proc = pocket.kasane.procedural;
const small = proc.createSurface({ maxSegments: 256 }); // 面1: フレーム 2 枚 × 2,566 B
proc.resource(small);
proc.beginFrame(0, small); // ここで確保（従来どおり最初の beginFrame）
```

| 呼び方 | 結果 |
| --- | --- |
| `createSurface()` / `createSurface(undefined)` / `createSurface({})` | 従来どおり 1,024 本 |
| `createSurface({maxSegments: n})`、n は 1〜1,024 の整数 | 面1のフレームは n 本分 |
| n が 0、1,025 以上、小数、文字列、負、`NaN`、引数が object でない（`null` を含む）、引数 2 つ | `INVALID_ARGUMENT`、ID を発行しない |
| 面1がもうある | 従来どおり `LIMIT_EXCEEDED`（"two surfaces maximum"） |
| 面1に n を超えて `draw()` | `LIMIT_EXCEEDED`（"frame drawing limit"）。VM の線分と型付き点列の線分の合計で数える |

以前は引数を 1 つでも渡すと `INVALID_ARGUMENT` だったので、動いている既存アプリ（MEGADEMO・`proc_multi_surface_probe.js` など、どれも引数なし）の挙動は変わらない。

## メモリの内訳

| 確保 | いつ | 大きさ（計算） |
| --- | --- | --- |
| 面0 の候補・確定フレーム | 面0の最初の `beginFrame` | 2 × 10,246 B |
| scratch（1 回の `draw()` の出力） | 最初の `beginFrame`（面を問わず 1 本） | 10,246 B |
| VM | 同上 | `sizeof(ksn_proc_vm)` |
| 面1 の候補・確定フレーム | 面1の最初の `beginFrame` | 2 × (6 + 10·n) B。n = 1,024 で 10,246、512 で 5,126、256 で 2,566、128 で 1,286 |

scratch と VM を縮めなかったのは、1 本の plan が 1 回の `draw()` で最大 1,024 本を出せ、面への写しの前に数を確かめる形だから（面ごとの上限は写すときに効く）。

**短いフレームが安全な理由**: フレームは `ksn_proc_frame` の先頭（`count`・`raster_steps`・`ready`、6 B）と n 本分だけを確保する。`pocket_proc.c` の中でフレームを読むのは `count` 本までの経路（`ksn_proc_render_band`、damage の計算、画像の帯）だけで、構造体を丸ごと写す・消す箇所は無い。`beginFrame` の `memset(sizeof *candidate)` は先頭 3 項目の初期化に替えた（1,024 本の面でも 10 KB の memset が無くなる）。`segments` が末尾で詰め物が無いことは `_Static_assert` で固定した。

## host の検査

`tools/kasane_contract/test_pocket_proc_turn_qjs.c` の `cap_contract`（ASan/UBSan、`run_pocket_proc_limits_qjs.py` の scalar と fake PIE の 2 通り、`run.sh` から走る）:

- 確保量: 面0は従来どおり 10,246 B × 3（候補・scratch・確定）＋VM。面1（256）は 2,566 B × 2 だけ。n = 1・1,024・`{}`・`undefined`・引数なしの境界もそれぞれの確保量と、n 本目まで通り n+1 本目で `LIMIT_EXCEEDED` になることを確かめる。
- ちょうど満杯: VM の 128 本＋型付き点列の 127 本＋VM の 1 本 = 256 で通り、次の 1 本で `LIMIT_EXCEEDED`、そのフレームは `commit()` できない（`BUSY`）。
- 満杯のフレームを `commit()` し、画像の帯で画素を読み、表示 ACK で候補と確定を入れ替えて、もう一方の短いフレームに描き直す（新しい確保なし）。
- 面1の確保失敗は `OUT_OF_MEMORY`、不正な引数 9 通りは `INVALID_ARGUMENT`。
- 面1に上限をかけても面0は 1,024 本まで描ける。
- **検査が効くことの確認**: `draw()` の上限を 1,024 に戻す変異を入れると、ASan が `heap-buffer-overflow` で止まる（host の実測）。

## 実機の測定

image は `build_surfcap`（この節の変更＋測定用コピー）。測定用コピーは [`tools/games/surfcap/surfcap_device.py`](../../tools/games/surfcap/surfcap_device.py) が `.cache/surfcap/derby_watch.js` に書き、`DERBY_BGCOST_SOURCE`（[derby-background-cost.md](derby-background-cost.md) の仕組み）で DERBY WATCH の行に埋め込んだ。`apps/derby` は変えていない。操作は `tools/games/bgcost/bgcost_device.py run`（放置でデモのレースが回る）。空き・最大連続は `pocket.memory.info()` の `internalFreeBytes`・`internalLargestBytes`（ターンの始めの標本、100 ms ごと）。

### 1 面あたりの確保量（実機の実測）

最小のアプリ（`surfcap_device.py size N`）で、8 フレームに 1 手ずつ進めて空きの減りを読んだ。面0を先に確保するので、面1の行は面1の 2 枚だけの値。

| 手 | n = 256 | n = 1,024（既定） | 計算 |
| --- | ---: | ---: | ---: |
| 面0の最初の `beginFrame`（候補・確定・scratch・VM） | 33,580 B | 33,512 B | 30,738 B＋VM |
| `createSurface`＋`resource(s)` | 292 B | 256 B | — |
| **面1の最初の `beginFrame`（候補・確定）** | **5,384 B** | **21,512 B** | 5,132 / 20,492 B |
| 面1の後の最大連続 | 90,112 → 86,016 B | 90,112 → 69,632 B | — |

空きの減りは計算より約 5% 多い。2 例とも「要求を 2 の冪の区間の 1/16 刻みに切り上げ、1 確保 4 B を足した値」とちょうど一致する（2,566 → 2,688＋4、10,246 → 10,752＋4。**推定**: ヒープの割り当ての級の丸め。仕組みはソースで確かめていない）。これが正しければ、**1,024 本のフレーム 1 枚が要る連続領域は約 10,756 B** で、[derby-watch.md](../apps/derby-watch.md) の「最大連続 10,240 B に 6 B 足りない」は実際には約 0.5 KB 足りなかった。

### DERBY WATCH の評価直後に面1を確保する（実機の実測）

測定用コピーは、評価後の最初のフレーム（`info()` が `null` でなくなる最初のターン、DERBY の plan 登録より前）に `createSurface(…)`→`resource(s)`→`beginFrame(0, s)`→`beginFrame(0)` を試し、以後 300 フレームごとに空きと plan の登録数（`reg`）と未登録の待ち行列（`q`）を出す。面1には描かない（確保とその後のメモリだけを見る）。各 1 回、150 秒（2 レース）または 30〜45 秒（1 レースの途中まで）。

| 面1 | 確保の直前の空き / 最大連続 | 結果 | 1 レース目の plan（`reg` / `q`） | レース中の最小の空き |
| --- | --- | --- | --- | ---: |
| なし（基準） | 50,632 / 10,752 B | — | 25 / **0** | 16,180 B |
| 1,024（既定、1 回目） | 56,388 / 20,480 B | **`frame allocation failed`** | 記録なし | 16,548 B |
| 1,024（既定、2 回目） | 56,296 / 16,384 B | **`frame allocation failed`** | 19 / 6 | — |
| 512 | 56,192 / 16,384 B | 確保できた | 19 / **6** | 15,936 B |
| 256 | 50,468 / 10,240 B | 確保できた | 19 / **6**（2 レース目は 25 / 8） | 15,644 B |

- **同じ最大連続 16,384 B で、1,024 は失敗し 512 は通った。** 最大連続の値は起動ごとに 10,240〜20,480 B、確保の直前の空きは 50.5〜56.4 KB と揺れた（同じ image・同じ操作。保存データの読み込み `LOADED` の中身は前の回の結果で変わる）。基準と 256 は直前の空きがほぼ同じ（差 164 B）なので、この 2 つの差が面1の分の比較になる（300 フレーム目の空き 29,600 → 23,936 B、差 5,664 B。size の測定の 5,384 B に近い）。n = 256 は、見た中で最も小さい 10,240 B でも通った。
- 既定の 1,024 が失敗したときも、候補の 1 枚（約 10.7 KB）は確保されたまま残る（`buffers()` は取れた分を返さない。従来からの挙動で、次の `beginFrame` の再試行は残りの 1 枚だけを要る）。2 回目の失敗の行の `q=6` はこの分。
- **面1を持つと、DERBY の plan 登録が止まる。** 基準では 1 レース目の前に 25 本がそろう（`q=0`）が、面1（512 でも 256 でも）があると 19 本で止まり、6 本が 1 レース目の間ずっと未登録のまま（`q=6`）。DERBY の登録の門は「空き 18,432 B 以上」（`load()`、README の根拠: 登録のターンの落ち込み 12.6 KB）。ゲートの plan を登録し始める前（パドック、300 フレーム目）の空きが、基準の 29,600 B に対し面1ありでは 23,936〜24,388 B で、数本を登録したところで門を割る。どの 6 本かは記録していない。未登録の plan は `draw` されない（`live[n]` が無い）ので例外は出ず、馬・ミニマップ・大型画面のどれかが欠けて描かれていたはず（画面は見ていない）。
- 最小の空きは「ターンの始めの標本」の最小で、ターン内の落ち込み（DERBY の README で最大 12.6 KB）は含まない。

## DERBY に適用するには（JS の変更点。今回は適用していない）

`apps/derby` は他の作業が変更中なので触っていない。大型画面を面1に戻すなら、次が要る。

1. **面1の確保**: 評価中（MEGADEMO と同じく、穴があるうちに）に `const S1 = H.createSurface({maxSegments: 256}), res1 = H.resource(S1); H.beginFrame(0, S1); H.beginFrame(0);`。評価中の確保は今回測っていない（測ったのは評価後の最初のフレーム）。以前の 1,024 の版は評価中に失敗した（derby-watch.md）。
2. **本数**: 中継は MID で 1 フレーム平均 63 本（host、derby-watch.md）。最大は測っていないので、host の全フレームで面1に描く本数の最大を出し、余裕を見て n を決める。超えた `draw()` は `LIMIT_EXCEEDED` で、そのフレームは閉じる（`commit()` できない）ので、JS が本数を数えて手前で止めるか、例外を捕まえて前の面1を残す。
3. **表示**: シーンに image ノード（`res1`）を足し、毎フレーム大型画面の矩形 `vr` へ `setRect`。`setRect` は clip を動かさないので clip はパネル全体。
4. **更新の間合い**: 表示待ちの候補は全面で 1 枚なので、面1を `commit()` したフレームは面0が止まる（derby-watch.md、MEGADEMO も同じ）。数フレームに 1 回に絞る。
5. **メモリの門**: 上の表のとおり、面1の約 5.4 KB で登録の門（18,432 B）を割る。面0の `vis`（大型画面を面0に塗る plan）など面1に移る分の plan を外すか、門を下げるか（下げた分だけターン内の余裕が減る。README の 12.6 KB の落ち込みの根拠を測り直す）を決めてから入れる。面を解放する API は無いので、面1のフレームはアプリの終わりまで残る。

## 確信の低い点

- **各条件 1 回の測定。** 最大連続は起動ごとに 10,240〜20,480 B 揺れた。「256 なら通る」は 1 回（最も悪い 10,240 B のとき）、「512 なら通る」も 1 回。1,024 の失敗は 2 回。
- 評価中に面1を確保する形（MEGADEMO・以前の DERBY の方式）では測っていない。
- 空きの減りが計算より 5% 多い理由（級の丸め）は、数字が合うことからの推定。
- 面1を実際に描いて表示した測定はしていない（確保と、その後の空き・plan 登録だけ）。描画の時間・帯キャッシュの取り替えは MEGADEMO の測定（megademo-device-limits.md）を参照。
- 面0には上限を付けられない（面0は `createSurface` を通らない）。面0を小さくしたいアプリは今のところ無い。
