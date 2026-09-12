# L1 時計コスト・ベンチ（branch `vm/l1-clockbench`）

対象: [quickjs-freertos-vm-spec.md](quickjs-freertos-vm-spec.md) §6（L1）。
[vm-L0-report.md](vm-L0-report.md) §2.2 の予算案（件数 16、時間 8 ms）を前提に、
L1 の予算判定が読む時計を決めるための実測。実機は M5Stack Cardputer ADV
（ESP32-S3、240 MHz、-Os）、COM ポート越し。

計装は `main/main.c` の `CONFIG_POCKET_VM_L1_CLOCKBENCH`（既定 n）配下に
コミットしてある。**使い捨てのベンチであって出荷コードではない** —
`sdkconfig.vmclockbench.defaults` を重ねたビルドだけがこれを有効にする。通常ビルド（`sdkconfig.defaults` のみ）は
1バイトも変わらない（後述のビルド比較参照）。

## 1. 時計を読むコスト（実測(device)）

`clockbench_run()`（`main.c`、`app_main()` の一番最初、ペリフェラルに触れる前）が
起動のたびに1回、以下を測って `CLOCKBENCH_*` 行としてログする。

手法: ループ本体なし（純粋なオーバーヘッド測定）→ `esp_cpu_get_cycle_count()`
（CCOUNT、`RSR CCOUNT`、`main/scene/flower.c` が既に使っているのと同じ命令）
連続10万回 → `esp_timer_get_time()`（systimer）連続10万回、を1セットとして
20セット繰り返す。各回の結果は `volatile` の sink 変数に書き込み、最適化による
消去を防ぐ。ループのオーバーヘッド（何も読まない同じ形のループ）を差し引いた
値を「1回あたり」として報告する。ビルドは `-Os`（ファームの通常最適化と同じ）。

実測（`build_bench_clock`、n=100,000、repeats=20、20セットとも中央値・最大値が
完全に一致 — このボードでのジッタは測れないほど小さい）:

| 項目 | 中央値 | 最大値 |
| --- | --- | --- |
| ループ自体のオーバーヘッド | 2 cycles/iter | 2 cycles/iter |
| `esp_cpu_get_cycle_count()`（CCOUNT） | 6 cycles = **25 ns** | 6 cycles = 25 ns |
| `esp_timer_get_time()`（systimer、CCOUNTで挟んで測定） | 200 cycles = **833 ns** | 200 cycles = 833 ns |
| `esp_timer_get_time()`（systimer 自身の差分で直接測定、クロスチェック） | 843 ns | 843 ns |

CCOUNT 経由の測定と systimer 自身の差分による直接測定が 833 ns / 843 ns と
1.2% 差で一致しており、どちらの時計で測ってもほぼ同じ値になる —
測定方法そのものの誤りではないという裏付け。

**CCOUNT は systimer よりおよそ33倍安い**（200/6 ≈ 833/25 ≈ 33.3）。

### L0 のジョブ単価との比較（実測(device)、vm-L0-report.md §2.1 より）

| ワークロード | 単価 | CCOUNT 1回（25 ns）の比率 | systimer 1回（833 ns）の比率 |
| --- | --- | --- | --- |
| D `promise_chain`（最も安いジョブ） | 0.07 ms/件 = 72,000 ns | 0.035% | 1.16% |
| F `async_generator`（最も高いジョブ） | 0.49 ms/件 = 490,000 ns | 0.005% | 0.17% |

どちらの時計も、1ジョブに対して読むコストは無視できる桁（1.2%未満）。
ここだけを見れば systimer でも足りるが、CCOUNT が既に33倍安く、
`flower.c` で確立済みの手段でもあるので、選ぶ理由がある（§3）。

### 32bit カウンタの折り返しと引き算の書き方

CCOUNT は32bitで、240 MHz では **2^32 / 240,000,000 ≈ 17.895 秒**で一周する。
`(uint32_t)(c1 - c0)` という**符号なし引き算**は、真の経過サイクル数が
2^32未満（＝17.9秒未満）である限り、C の mod-2^32 規則により折り返しをまたいでも
正しい差分を返す — 本ベンチも `clockbench_run()` もこの書き方をしている
（新しいコードを足す必要はない）。L1 のジョブ・ターンは ms 〜 8 ms 予算なので、
17.9 秒に対して3〜4桁の余裕があり、この前提は常に成り立つ。

## 2. CCOUNT の正しさ: ui タスクは何回コア移動するか（実測(device)）

`main/main.c:748` の `ui_task` は `xTaskCreate`（コア指定なし）で立てられている
——CCOUNT はコアごとのレジスタなので、2回の読み取りの間でタスクが移動すると
差分は無意味になる。`bench_core_tick()` を `ui_task` のループ先頭に足し、
`xPortGetCoreID()` を毎フレーム比較して移動回数を数え、900フレームごとに
`BENCH_CORE frames=... migrations=... core=...` を出す（900フレームは
30ms周期のホーム画面で約27〜30秒、遅い画面ではもっと長い——ウォールクロックの
窓ではなくフレーム数の窓なので、レートとして読む）。

実測（`build_bench_clock`、無線オフ、ホーム画面のオーバーレイ背景描画で idle、
続けて USB キー `e` で JS アプリ [hello] を起動）:

| 条件 | 標本 | 移動回数 | レート |
| --- | --- | --- | --- |
| idle（ホーム、背景の overlay ペットが動いているだけ） | 1,800 フレーム（900×2窓） | **1** | 0.056%/フレーム |
| running app（[hello] 起動後） | 3,600 フレーム（900×4窓） | **0** | 0/3,600（rule of three で95%上側 ≈0.08%/フレーム） |

**移動は起動直後の1回だけ**——ログの `shell: ui runs on core 0`（起動時、
タスク生成直後の1回きりのログ）に対し、最初の900フレーム窓の終わりには
`core=1` になっていた。つまり FreeRTOS のスケジューラが起動直後にコアへ
実際に貼り付け直した1回を数えており、**それ以降は idle でも running app でも
一度も動いていない**。頻度としては極めて低いが、ゼロではない。

Wi-Fi 条件は今回**測れていない**——このデバイスに保存済みの資格情報がなく
（vm-L0-report.md の Wi-Fi 条件は保存済み資格情報を前提にしている)、
本セッションでは接続(associate) までは行えなかった。設定画面から
スキャンだけは実行した（`SCAN_OK found=12`、無線init〜スキャン完了まで
約2.5秒、ヒープが一時的に約27KB減）。副産物として分かったこと:
**Wi-Fi ドライバタスクは `core=0` 固定で作られる**（起動ログ
`wifi driver task: ..., prio:23, stack:6656, core=0`）——ui タスクを
core 1 に pin すれば、この面では衝突しないという弱い裏付けにはなる
（実際の負荷下でのコア間競合は未測定のまま）。

### pin したビルドの静的コスト（実測(build)）

`xTaskCreatePinnedToCore(ui_task,...,1)` に変えた `build_bench_pin1` は、
`tools/memlog.py` の比較で **DIRAM +0 B**（`build_bench_clock` 比）。
pin すること自体はこのボードでは無料。

### pin したビルドの実行時コスト: 測れなかった（実測できず、要再測定）

フレーム時間・fps・空きヒープ・他コア（音声・Wi-Fi・デコーダタスク）への影響を
`build_bench_pin1` を焼いて比較するはずだったが、**`build_bench_pin1` を書き込む
前に COM ポート側の実機が USB リセットに応答しなくなった**——`build_bench_clock`
を書き込んだまま、Wi-Fi スキャンを一度動かした直後、`esptool` の
`default-reset`（RTS/DTR パルス。CLAUDE.md 記載の usbser.sys の RTS 単独書き込みが
効かない問題への対処——RTS の後に DTR を書き戻す——を含めても）に一切応答しなく
なり、COM ポート番号も COM3 → COM5 に変わった。`--trace` で見ても sync バイトへの
応答がゼロで、ROM ブートローダにも既存アプリにも見えない反応の無さで、
ファームの不具合ではなく**USBの再接続かボードの電源再投入が要る、ソフトウェアからは
直せない状態**と判断した（このマシンの PnP 一覧はこのボードを Anker の
USB-C ハブ経由と記録しており、Wi-Fi 送信時の瞬間電流でハブ側がブラウンアウトした
可能性はあるが、検証はしていない——推定）。

**残作業（解消済み、2026-09-12）:** pin 版との実行時比較は L1 が別の形で完了させた
——`docs/vm-L1-report.md` §8 が core 0 / core 1 / 無 affinity の3値を同一バイナリ族で
実測し、core 1 を既定に採用した（`CONFIG_POCKET_UI_TASK_CORE`）。専用の pin オーバーレイ
（旧 `sdkconfig.vmclockbench.pin1.defaults`）は決めるものが無くなったので削除した。
以下は当時の手順の記録。
`build_bench_clean`（このドキュメントのビルドはこちら、後述）は既に手元にあり
書き込み待ちの状態。

## 3. 結論（測定から導いたもの、好みではない）

1. **L1 の予算判定は `esp_cpu_get_cycle_count()`（CCOUNT）を読む。**
   systimer よりおよそ33倍安く（25 ns vs 833 ns、実測）、最も高くつくジョブ
   （async generator、0.49 ms/件）に対してすら0.005%のコストしかない。
   どちらの時計でも1ジョブあたりのコストは無視できる桁だが、CCOUNT は
   既に `main/scene/flower.c` で確立済みの手段でもあり、選ばない理由がない。
2. **毎ジョブ読む（N=1）。件数でまとめて読む理由がない。** 件数予算16件・
   時間予算8msの下で、CCOUNTを16回読んでも合計400ns（8msの0.005%）、
   systimerでも13.3µs（0.17%）——読むコストを理由に頻度を落とす意味がない。
   逆に N>1 にすると、asyncジェネレータの塊で最大 (N−1)×0.49ms
   だけ予算超過を遅れて検知することになり、8ms予算のマージンを直接削る。
   読むコストがほぼゼロな以上、粒度を粗くする側に倒す理由が無い。
3. **ui タスクを pin する。無料だから、measured riskの大小に関わらずやる。**
   `xTaskCreate`→`xTaskCreatePinnedToCore` は実測で DIRAM +0 B。CCOUNT の
   コア移動問題は「起動直後に1回だけ、それ以降は0/3,600フレーム」という
   低い頻度だが、ゼロではないと実測されている——pin すればこの問題自体が
   構造的に消える。無料の変更でクラス全体の不具合を消せるなら、リスクの
   大小を議論する必要はない。
4. **pin したビルドの実行時への影響（フレーム時間・fps・他コアの競合）は
   このセッションでは検証できていない。** 機材がUSBリセットに応答しなくなり
   ブロックされた（§2 参照）。静的コストがゼロで、単一の継続実行タスクを
   固定コアに置くのはFreeRTOS/ESP-IDFで枯れた操作である以上、悪化のリスクは
   低いと考えるが、**実測していないことを実測したかのように書かない**——
   ユーザーが機材を復旧させた後、上記「残作業」の手順で完成させること。
5. Wi-Fi 条件下の移動レートも同じ理由で未測定。保存済み資格情報がある環境で
   `apps/vmprobe`（`git checkout vm-L0 -- apps/vmprobe`）の wifi 条件を使い、
   `BENCH_CORE` ログと突き合わせれば埋められる。

## 4. 再現手順

```powershell
. 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1'
cd C:\devs\m5stack\cardputer-adv-pocketjs-bench

# 時計コスト + 移動カウンタ（無 pin）
idf.py -B build_bench_clock -D SDKCONFIG=build_bench_clock/sdkconfig `
  -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.vmclockbench.defaults" -p <port> flash
# 起動直後に CLOCKBENCH_* が3行、以後 900フレームごとに BENCH_CORE

# pin の比較は CONFIG_POCKET_UI_TASK_CORE（既定 1）で行う。専用オーバーレイは廃止。

# 測定が済んだら、必ず素のビルドに戻す
idf.py -B build_bench_clean -p <port> flash
```

`-D SDKCONFIG=<dir>/sdkconfig` は省略しないこと——省くと ESP-IDF は
プロジェクト直下の共有 `./sdkconfig` を使い、他の `build_*` も巻き込んで
ベンチ入りにしてしまう（`sdkconfig.vmprobe.defaults` と同じ罠、
`main/Kconfig.projbuild` 参照）。

## 5. 実機の状態

このセッションが最後に実機へ書き込めたのは `build_bench_clock`
（`CONFIG_POCKET_VM_L1_CLOCKBENCH=y`、無 pin）で、その後 USB リセットに
応答しなくなったため**素のビルドへ戻せていない**。`build_bench_clean`
（このワークツリーの `sdkconfig.defaults` のみ、ベンチ設定なし、
`tools/memlog.py` 比較で bench 有効時から `main.c.obj` が縮んでおり
ベンチコードが跡形もなく消えていることを確認済み）は手元にビルド済みで
書き込み待ち。**USB の抜き差し（またはボードの電源再投入）の後、
上の「素のビルドに戻す」コマンドを実行すること。**
