# STRESS LV3 のヒープ枯渇を QEMU で追う（2026-11-12）

[VMの文書](README.md)

実機で STRESS TEST を LV3 にすると、約 30 秒で EXECUTION FAILED になった。ログの最後は `InternalError: out of memory at wrap.js:1:114`（`frame()` を呼ぶラッパーの中で、STRESS の `try` に入る前の OOM）。ゲストは 160 KiB の上限の手前（約 130〜140 KB）で、システムヒープの空きは約 4〜5 KB、最大ブロック 224〜400 B だった（実測、実機のログ）。

問い: このメモリ不足は、実際に使われている（生きている）メモリか、回収されないごみ（GC が追いつかないスラッシング）か、断片化か。

## 1. 道具

`tools/r5sim`（QEMU の ESP32-S3 像。STRESS のターンを実機と同じゲスト部品で回す）に、測定用の選択肢を足した。

- `-DR5SIM_GC_TRACE=1`: ターンごとの行 `R5SIM GC t= used= thr= gc=(+) freed= last= oom= q= a= req= sys_free= sys_largest= pend= needs= sub=`。GC の回数と解放量は quickjs.c の `POCKET_GC_STATS`（測定用、ファームでは無効）。`q`/`a` は OOM の内訳（ゲストの上限で断った数／アロケータが断った数）。`pend` は Shiki のフレームが提示待ちか、`needs`/`sub` は Kasane が提示を要るか・提出があるか。
- `-DR5SIM_FREE_KIB=89`: 評価の後のシステムヒープの空きを実機と同じ約 91 KB にする（QEMU には無線もパネルも無いので、そのままでは空きがずっと多い）。4 KB の塊で埋める。
- `-DR5SIM_GC_ON_OOM=1`: アロケータが断ったターンの後で GC する実験（今回は使わなかった。アロケータの拒否が起きなかったため）。
- 実行: `FAST_WAIT_TICKS=2400 tools/r5sim/fast.sh .cache/r5gc -DR5SIM_DEVICE=1 -DR5SIM_APP=shiki -DR5SIM_LEVEL_UP=1 -DR5SIM_FRAMES=400 -DR5SIM_GC_TRACE=1 -DR5SIM_FREE_KIB=89`（LV3 はフレーム 80 から）。

## 2. わかったこと（QEMU、命令列。時間は含まない）

- **GC はほとんど何も解放しない。** 1 回あたり 0〜3 KB。LV3 のメモリは生きたデータ（STRESS の `pool`）で、STRESS が `pool` を捨てると参照カウントで即座に戻る（163 KB → 41 KB）。循環ごみの取り残しではない。
- 上限の手前では GC の回数は増える（上限に当たるたび 3〜8 回）が、そのたびに解放はほぼ 0。`gc-cap-backoff.md` の天井の後退が効いていて、毎オブジェクトの GC にはなっていない。
- **断片化が周回ごとに進む。** `pool` を捨てた直後の空きは毎回 87〜89 KB に戻るのに、最大ブロックは 25 KB → 13 KB → 11.5 KB → 8 KB と縮む。
- QEMU では断るのはいつもゲストの上限（`q`）で、アロケータは断らなかった（`a=0`）。実機ではアロケータが 1,059 B の要求を断っている（最大ブロックが 224〜400 B）。実機のほうが断片化がずっと進んでいる（無線・表示・入力のタスクが同じヒープから確保するため、と推定。未確認）。

## 3. 見つけた不具合（修正済み）

QEMU の 1 回目は、フレーム 295 から毎フレーム `STRESS_FAIL frame Error: previous frame awaits presentation or repair` になり、二度と描かなかった。

- その直前のターンは OOM で、HUD の `V.patch` も失敗した。そのターンで commit した Shiki のフレームは前と同じで、損傷が空だった。
- Shiki の `commit` は、image モードでは変わったバンドだけを無効化する（`pocket_kasane_proc_publish_at`）。損傷が空なら何も無効化しない。ふだんは HUD の提出があるので提示が走り、そこで `pending` が下りる。HUD も失敗したターンでは提示が走らず、`pending` が立ったまま残った（トレース: `pend=1 needs=0 sub=0`）。以後の `beginFrame` はすべて BUSY。
- 修正: `commit` の後で Kasane に提示するものが無ければ、そのフレームはその場で提示済みにする（`pocket_shiki_frame.c`、`pocket_proc_present_result(KSN_OK)`）。見せるものが無いので、見せたことにしてよい。
- 修正後: 400 フレームで `fail=0`。フレーム 295 は `pend=0`。

## 4. 実機の EXECUTION FAILED について（未解決）

- 実機では、ゲストが上限に届く前にシステムヒープ（最大ブロック）が尽き、`frame()` の呼び出しそのものが OOM になる。STRESS は自分の `try` の中の OOM は捕まえて回復するが、`frame()` に入る前の OOM は捕まえられない。ラッパーはそれを投げ直し、セッションが終わる。
- 本質は「ゲストの上限 160 KiB が、実機のシステムヒープの実際の余裕より大きい」こと。LV3 は上限まで積むアプリなので、上限より先にシステムヒープが尽きる。
- 次の候補（未実施）: (1) ゲストの上限を、起動時のシステムヒープの空きに合わせて下げる、(2) 断片化を減らす（ゲストのアリーナを大きくする、大きめの確保をアリーナに寄せる）、(3) `frame()` の入口で OOM になったターンはセッションを終わらせずに次のフレームへ進む。
