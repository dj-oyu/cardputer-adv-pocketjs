# 60 fps への道のり（2026-11-04〜05 のまとめ）

[性能と描画の文書](README.md)

表示の周期を 16 ms にして 4 アプリを回すと、今どこまで 60 fps に近いか。そこへ至る LCD 転送・描画・JS の仕事を 1 か所に並べ、詳細は各文書へ送る。
この文書は**要約**で、数字はすべて元の文書からの転記。区分は元の文書のままで、**実測**＝実機、**host**＝ホストの計数・画素一致、**推定／模型**＝計算・QEMU の模型。新しい測定はしていない。

## 0. 結論

**60 fps には届いていない。** 表示の周期 16 ms の測定版（`VM_DISPLAY_PERIOD_MS` 33 → 16、測定後に revert）の fps（実測、`KASANE_PAINT` 30 フレームごとの時刻から）:

| アプリ | 11-04（ffbba5c） | 11-05（0d7bff2） | 11-05 の内訳（turn / render / send、ms） | どこで 60 に張り付くか |
| --- | --- | --- | --- | --- |
| BIG WAVE | 55.6 | **58.5**（窓 57.3〜60.7） | 7.2 / 3.56 / 4.28 | 一部の窓 |
| STRESS | 44.3 | **50.5**（窓 49.6〜51.9） | 6.72 / 6.80 / 2.93 | 張り付かない |
| MEGADEMO | 50.5 | **52.2**（窓 36〜61） | WAKE 38〜41（turn 11.5〜15）、NEWS 50〜58、TWIST 50〜59、ZENITH 58〜61、CORE 40〜59 | ZENITH |
| DERBY | 55.5（レース） | **53.1**（窓 17〜61） | パドック 57〜60、レース 54〜60、終盤の WIDE 2 で 41〜49（turn 13〜14.5） | パドック・結果 |

- 出典: [apps-60fps-trial-20261104-ffbba5c.txt](../kasane/logs/apps-60fps-trial-20261104-ffbba5c.txt)、[apps-60fps-recheck-20261105.txt](../kasane/logs/apps-60fps-recheck-20261105.txt)。33 ms の出荷設定では全アプリ 30 fps を保つ。
- **DERBY の 55.5 → 53.1 は退行ではない。** 記録は窓のレース区間が違うと明記している（11-05 は重い終盤の WIDE 2 を含む。[lcd-dma-driver.md](../platform/lcd-dma-driver.md) §9.13）。同じ区間の比較ではない。新しいのは 11-05 の値。
- **残りの壁は描画でも送信でもなく JS の turn と、その中の重い場面**（元の文書の読み）。STRESS は render 6.8 ＋ turn 6.7 ＋ send 2.9 ＝ 16.4 ms で、他の処理の前に 16.7 ms を使い切る。MEGADEMO の WAKE は turn 11〜15 ms（tier-2 の plan の登録中）、DERBY の終盤は馬群が広がって SHIKI の draw が 31 → 53 に増え turn 13〜14.5 ms。
- 60 fps が 1 フレーム 16.7 ms という予算に対し、11-04 の時点で「turn・render・send の和」と 1 フレームの実時間に 1.5〜5 ms の差があった（BIG WAVE 16.2 対 18.0 ms、STRESS 17.4 対 22.6 ms）。**この差（pump、キー、音、GC など三つの計時の外）は今も分けていない**（§4）。
- アプリの動きがフレーム数で進むものは、16 ms にすると速く動く。既定は 33 ms のまま。

## 1. 時系列（何をして何が効いたか）

前史（2026-09〜10）: flash を QIO にして JS の 1 フレーム 10.78 → 7.25 ms（実測、[turn-cpi.md](../vm/turn-cpi.md)）。DMA と次の帯の描画の重ね合わせで MEGADEMO の send が約 5 ms/frame 減った（実測、[kasane-dma-overlap.md](kasane-dma-overlap.md)、既定 ON）。

### 1.1 LCD 転送（2026-11-04、[lcd-dma-driver.md](../platform/lcd-dma-driver.md) §9）

目標は「描画と送信の合計 `render_ms` 7.38 ＋ `send_ms` 2.90 ＝ 10.34 ms（STRESS LV1、実測）を 8 ms 前後に」だった（§0、推定の目標）。

| 項目 | 何をしたか | 効果（区分） | 採否 |
| --- | --- | --- | --- |
| 待ち行列の深さ 2（§9.1） | `spi_master` のまま `queue_size=2`、+3,840 B。同じバイナリの A/B | `send_ms` 2.90 → 2.32、render+send 10.25 → 9.73（実測、中央値）。設計時の見込み −2.5〜2.8 ms は外れ、縮むのは 1 フレーム約 0.5〜0.6 ms | 割に合わず、深さを増やす案は採らない |
| LSB 先（LE）（§9.2〜9.3） | RAMCTRL の ENDIAN を LSB 先にして CPU のバイト入れ替えをやめる。目視で ST7789 が受け付けると確認（持ち主の目視） | send 2.90 → 2.99（窓のばらつきの中、実測）。**速くならない** | 描いたまま送れる形にするため既定にした（速さのためではない） |
| 自作の送信経路（§9.3、`lcd_spi.c`） | GP-SPI2 を直接駆動（`BOARD_LCD_DIRECT`） | send 3.00 → 2.68、render+send 10.32 → 10.07（実測、同じバイナリの A/B）。約 0.3 ms は帯ごとのキュー・割り込み・タスク起床の分（推定） | 採用 |
| 写しなしの交互使い（1'）と 12 行の輪（2）（§9.4） | DERBY 実機 A/B | 送信 ms/10 KB: 今 0.73、1' 0.76、2 0.66（実測）。1' は速くならない、2 は約 1 割（1 フレーム約 0.4 ms、推定）で送信用メモリ +1,920 B | 1' は設計 0 の土台になった。出荷は設計 0 + B |
| 設計 0（帯 1 本を前後半で使う）と B（帯ごとの範囲だけをたどる）（§9.5〜9.7） | `tx_buf` 3,840 B を外し、4 行の半分を交互に描いて送る。`band_signature` が帯ごとの entry の範囲を覚える（272 B） | B は 8 行の描き方も速くする: 描画 −0.35 ms（実測、DERBY）。0 は B と合わせて今の B なしとほぼ同じ（+0.2 ms、ばらつき大）。空きヒープは起動直後 +3.5〜4.4 KB、定常 BIG WAVE 約 +3.9 KB、DERBY レース中 約 +4.9 KB（実測、別バイナリの比較） | 既定（`BOARD_STRIP_HALF` 1、B も既定有効）。エディタ・wifi などは同期送信で、そのぶん遅くなる（§9.6） |
| 4 行の帯の描画を速くする（§9.9） | (1) 途中から入る線の 64 bit 除算を 32 bit に、(2) 前半の帯が後半に掛かる entry を覚える（carry、144 B） | (1) 描画 −0.18 ms（中央値）、(2) Shiki の画像 24,843 → 22,393 サイクル（−9.9%）、描画 8.19 → 8.00 ms（実測）。STRESS の描画は二つで約 0.4 ms 減（8.35 → 約 7.95、推定） | 採用。画素は同じ（host の digest 一致） |
| `commit` の高速化（§9.10〜9.11） | 帯のハッシュを点検（32 bit の衝突 0、下位ビットの偏りあり）。ハッシュを entry ごと 1 回にし、`pocket_shiki_frame.c` の二つの歩きを `-O2` に | STRESS commit 2.15〜2.30 → 0.97〜1.00 ms、BIG WAVE 0.95〜0.98 → 0.46〜0.48 ms（実測、同じバイナリの A/B）。ハッシュだけでは −0.12 ms で、効いたのは `-O2` | 採用。時間を食っていたのは計算量ではなくサイズ最適化のコードだった |
| 毎フレームの道を `-O2`（§9.12） | `ksn_font.c`・`ksn_render.c`・`ksn_core.c`・`pocket_kasane.c`・`pocket_shiki_frame.c` を `-O2`（`-ffp-contract=off`）、`render_group` だけ `-Os` | render_ms: STRESS 7.94 → 6.78、MEGADEMO 5.32 → 4.62、BIG WAVE 4.21 → 3.77（出荷版 3.55）、DERBY パドック 6.07 → 5.35（出荷版 4.83）（実測、ビルド間の比較。命令キャッシュの配置で約 15% 揺れる）。代償: flash +12.6 KB、静的 DIRAM +44 B | 採用（C 案） |

### 1.2 なぜ `-O2` が効いたか（[o2-machine-code.md](o2-machine-code.md)、[pie-simd.md](pie-simd.md) §3.9）

- 規則（実測、XTPERF）: 整数コードのサイクル ≈ 退役命令 ＋ 2 × 成立した制御移行 ＋ レジスタ依存 ＋ 命令フェッチ待ち。分岐予測は無く、飛ぶと 3 サイクル。(バブル − レジスタ依存) ÷ 成立分岐は 4 条件で 1.99〜2.00。
- 描画区間のサイクルは STRESS 55.45 M → 47.48 M（−14.4%）、DERBY 40.80 M → 33.10 M（−18.9%）。減りの約 2/3 は命令数、約 1/3 は成立分岐 × 2。**IPC は 0.65 付近で最適化レベルによらず不変**（実測）。
- §9.12 の B（`render_group` も展開）が CORE の場面で逆に遅くなったのは、`render_rects` のフレームが 1,312 B になり、1,020 B より奥の読み書きが 3 命令になったため（推定、B に計器は付けていない）。C は 896 B に収めた。以後 `tools/frame_lint.py` がビルドのたびに数える（[o2-machine-code.md](o2-machine-code.md) §5）。

### 1.3 JS の turn（2026-11-05、[docs/vm/](../vm/README.md)）

描画が縮んだ後に残る壁は turn なので、JS のターンを調べた。結論はどれも「小さい、または効かない」。

| 項目 | 結果 | 区分 | 文書 |
| --- | --- | --- | --- |
| 分岐 | サイクルの 13〜15%。うち命令の振り分け（`jx`）は約 1%（1 ターン STRESS 約 4,300、DERBY レース約 11,000 オペコード）。flash キャッシュ待ちが 34〜43%、IPC 0.445 / 0.38 | 実測（XTPERF） | [turn-jumps-pie.md](../vm/turn-jumps-pie.md) |
| 振り分けの複製（`JS_CallInternal` を `-O2`） | 得は STRESS 0.5%、DERBY 1.2%（推定）、+1.2 KB。採らない | 推定 | 同上 |
| PIE（SIMD） | QuickJS のターンにほぼ無い。データ並列は `memcpy`/`memset` 程度で host の命令数の 2% 未満 | host の計数と推定 | 同上 §4 |
| 作業集合 | 1 ターンが触るコード 42.5 → 41.8 KiB（試作 3 つ入りでも）、インタプリタ（core ＋ helper）21.3 KiB は予算 14 KiB の約 1.5 倍 | 模型の推定 | [turn-icache.md](../vm/turn-icache.md) §2 |
| 行の使用率 | 取り込む 40.8 KiB のうち実行は 23.4 KiB（57.5%）。走らないバイトの多くはどのターンでも走らない冷たいコード。上限の模型で 27.7 KiB に収まり warm ミス 940 → 73 | 模型の推定 | [turn-icache.md](../vm/turn-icache.md) §3 |
| 命令キャッシュの先読み・固定（2026-11-09） | 自動の先読みは STRESS の 1 フレーム 16.20 → 16.16〜16.88 ms、Shiki の実行は +1〜4%、DERBY の同じ場面で JS +12%。固定・手動の先読み・関数の手での分割は模型で得が小さい | 実測と推定 | [turn-icache.md](../vm/turn-icache.md) §4 |
| hot/cold 分割（`CONFIG_POCKET_VM_QJS_HOTCOLD`、既定 OFF、2026-11-13） | GCC の出すアセンブリの上で `quickjs.c` の基本ブロックを並べ替える。模型でインタプリタの触る行 STRESS 17.9 → 13.2 KiB、DERBY 19.1 → 13.7 KiB、warm ミス −37% / −19%。flash +3.4 KB、DRAM 0。**実機は未測定** | 模型の推定 | [qjs-hotcold.md](../vm/qjs-hotcold.md) |
| PGO（`CONFIG_POCKET_VM_QJS_PGO`、既定 OFF） | 模型は触る行 −2.8%、warm ミス −4.9%（推定）。**実機は差なし**: BIG WAVE 58.5 / 58.5、STRESS 50.5 / 50.7、MEGADEMO 52.2 / 52.1、DERBY 53.1 / 53.8 fps（実測、DERBY は窓のレースが違う）、STRESS の turn 6.72 / 6.73 ms | 推定と実測 | [qjs-pgo.md](../vm/qjs-pgo.md) |

つまり「JS の turn を速くする」決め手はまだ無い。最大の項は flash キャッシュ待ち（34〜43%）で、行の使用率の 57.5% がその一因と模型は言うが、効く打ち手と見込んだ hot/cold 分割は Xtensa の GCC が無効にする（[qjs-pgo.md](../vm/qjs-pgo.md) §0）。

## 2. 効かなかったもの・捨てたもの

- **待ち行列を深くする／送信用を増やす**: 深さ 2 で 0.5〜0.6 ms、+3,840 B に見合わない（[lcd-dma-driver.md](../platform/lcd-dma-driver.md) §9.1）。
- **LSB 先を速さのために入れる**: 速くならない（§9.3）。PIE の入れ替えは写しと同じ速さで、どちらもメモリの速さで決まると見られる（推測）。
- **写しなしの交互使い 1'**: 同じ速さ（§9.4）。ただし設計 0 の土台。12 行の輪（2）は約 1 割縮むが、出荷は設計 0 + B。
- **確定時に帯ごとの線分を保存する案（§9.5 の A）**: DERBY で最大約 1.8 KB。持ち主の判断で試していない（§9.9）。代わりに carry（144 B）。
- **ブレンド表を最初の画素で遅延構築**: STRESS では作る回数が変わらず（31.4M 命令のまま）戻した。枠を増やすにはメモリが要るので見送り（§9.9）。
- **`-O2` の B 案（`render_group` も展開）**: CORE の場面で `render_group` 16.4k → 20.8k サイクル（+27%、実測）。C 案で解決（§9.12）。
- **ハッシュの混ぜ直しだけでの高速化**: `commit` −0.12 ms。効いたのは `-O2`（§9.11）。
- **インタプリタの振り分けの複製、PIE 化**: §1.3。
- **`-O2` ＋ PGO**: `JS_CallInternal` が 22 KB → 31.8 KB、r5sim の flash +116 KB で捨てた（[qjs-pgo.md](../vm/qjs-pgo.md) §0）。
- **PGO を既定にする**: 実機で差が出ず、既定 OFF のまま（#120）。

## 3. 測り方

- **16 ms 測定版**: `VM_DISPLAY_PERIOD_MS` を 33 → 16 の 1 行だけ変えたビルド（11-04 は ffbba5c = 116a3eb ＋ 1 行、11-05 は 0d7bff2 = ad8a964 ＋ 1 行）。測定後に revert する。出荷はしない。
- **fps は `KASANE_PAINT` から**: 30 フレームごとのログの時刻の間隔（実機のログのタイムスタンプ、ms）から 30 ÷ 間隔。**何も描かないフレームは数えない**ので「新しい絵の率」。11-05 は最初の窓を捨てる。ログの上限で先頭が落ちることがある（DERBY の最初の約 124 行）。turn・render・send は同じ行の窓の平均。
- **同一バイナリの A/B**: 同じバイナリの中で切り替えて 30 フレームの窓を交互に回し、中央値を比べる（§9.1、9.3、9.9、9.11）。**ビルド間の比較は命令キャッシュのアラインメントで同じカーネルが約 15% 動く**（CLAUDE.md）ので、それ未満の差は同一バイナリでしか言えない。§9.12 の A/B/C はビルド間の比較で、その旨が本文にある。
- **実機の取り合い**: 実機は USB device broker 越しにジョブ（`t60-*`、`r60a-*`、`r60b-*`）を投げて測る。ジョブの記録は `docs/kasane/logs/`。
- **サイクルの内訳**: XTPERF（PM0 = cycles、PM1 を 1 窓に 1 イベントで巡回）。ターンは `CONFIG_POCKET_VM_TURNPERF`（[turn-cpi.md](../vm/turn-cpi.md)）。描画の機械語は [o2-machine-code.md](o2-machine-code.md)。
- **host の画素一致**: STRESS の digest `de4a66c5`、DERBY の先頭 `f2d667e3` が、帯の高さ（4/8 行）・範囲あり／なし・最適化の前後で不変。ただし host の検査は CMake のフラグを使わないので、`-O2` のコード生成の確認にはならない（§9.12）。
- **実機でしか分からないこと**: 画面が正しいかはログでは分からず、持ち主の目視を使っている（DERBY と STRESS は §9.4〜9.5 で済み、`-O2` 版の画素は目視していない）。

## 4. 残りの課題

詳細は GitHub の issue（この文書は番号だけを参照する）。

| issue | 内容 |
| --- | --- |
| [#119](https://github.com/dj-oyu/cardputer-adv-pocketjs/issues/119) | 残りの壁は JS の turn。STRESS（render 6.8 ＋ turn 6.7 ＋ send 2.9 = 16.4 ms）、MEGADEMO WAKE（tier-2 の plan の登録中、turn 11〜15 ms）、CORE 序盤（40〜47 fps）、DERBY 終盤 WIDE 2（SHIKI の draws 31 → 53、turn 13〜14.5 ms）、シーン切り替えの一時停止（DERBY photo → res 1.7 s、gate → race 26〜27 fps の窓）、三つの計時の外の時間の分解 |
| [#120](https://github.com/dj-oyu/cardputer-adv-pocketjs/issues/120) | QuickJS PGO: 模型（触る行 −2.8%、推定）が実機に出ない理由を、`TURNPERF` の `I_STALL_BUSY` で PGO なし／ありの 2 ビルドで比べて確かめる。効く見込みが立てば学習に DERBY を足し、立たなければ仕組みは残して閉じる（[qjs-pgo.md](../vm/qjs-pgo.md) §5） |
| [#121](https://github.com/dj-oyu/cardputer-adv-pocketjs/issues/121) | `frame_lint` を報告のみからビルドを止める `--check` に切り替え、基準の更新手順を `docs/platform/test-commands.md` に書く（道具は [o2-machine-code.md](o2-machine-code.md) §5） |

issue に無い、元の文書に「未」と書かれているもの:

- 三つの計時の外の 1.5〜5 ms の内訳（[lcd-dma-driver.md](../platform/lcd-dma-driver.md) §9.8）。**2026-11-09 に分けた**: 外は約 0.85 ms で、残りは周期の刻み・超過時の 1 tick・ログを書くフレーム（[frame-breakdown.md](frame-breakdown.md)、実測と推定）。
- シーン切り替えの落ち込み: DERBY の結果画面 17 fps、ゲートからレースへ 26〜27 fps はシーンの構築の時間（§9.13）。
- `-O2` の B 案の XTPERF、MEGADEMO の CORE の場面の内訳、BIG WAVE（[o2-machine-code.md](o2-machine-code.md) §4）。
- 窓例外（`entry` の窓あふれ）の 1 回の値段（STRESS で 30 ターン 11.4 万回、[turn-jumps-pie.md](../vm/turn-jumps-pie.md) §5）。

## 5. 文書の地図

| 知りたいこと | 文書 |
| --- | --- |
| LCD 転送の仕様・監査・各 A/B（§9.1〜9.13） | [lcd-dma-driver.md](../platform/lcd-dma-driver.md) |
| 生の記録 | [apps-60fps-trial-20261104-ffbba5c.txt](../kasane/logs/apps-60fps-trial-20261104-ffbba5c.txt)、[apps-60fps-recheck-20261105.txt](../kasane/logs/apps-60fps-recheck-20261105.txt)、[o2-xtperf-20261105.txt](logs/o2-xtperf-20261105.txt) |
| `-O2` の機械語と性能カウンタ、`frame_lint` | [o2-machine-code.md](o2-machine-code.md) |
| スカラー整数コードのサイクルの式 | [pie-simd.md](pie-simd.md) §3.9 |
| JS のターン: 分岐・PIE | [turn-jumps-pie.md](../vm/turn-jumps-pie.md) |
| JS のターン: 作業集合、行の使用率 | [turn-icache.md](../vm/turn-icache.md) §2・§3 |
| QuickJS の PGO | [qjs-pgo.md](../vm/qjs-pgo.md) |
| JS のターンの CPI、flash を QIO に | [turn-cpi.md](../vm/turn-cpi.md) |
| DMA の重ね合わせ | [kasane-dma-overlap.md](kasane-dma-overlap.md) |
