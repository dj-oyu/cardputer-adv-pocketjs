# SD音声I/Oの非同期化：比較実験

2026-09-23。共通出発点は `e74d42c`。Cardputer ADV / 240×135 / PSRAMなし。
目的は「SDとLCDのバスを分離する」ことではない（すでにSPI3とSPI2で別系統）。
UIタスクの同期SD待ちを除き、音声と描画の締切を守ることが目的。
平均FPSだけでなくp99・最悪値、音声underrun、停止・抜去時の寿命を判定する。

## 比較する実装

| 案 | SDの読取方式 | 変更点・検証したい仮説 |
| --- | --- | --- |
| 基準 | UIタスクで`pocket_fs_read_at` | 現行。2,048 Bごとに解決・open・seek・read・closeする |
| S | 共通lease基盤、ただし同期producerを維持 | 基盤導入のみの影響を分離する対照 |
| A | 専用readerタスク、毎slot再open | 読取をUIから外す効果のみを測る。既存圧縮リングに直接書く |
| B | 専用readerタスク、再生中は`FILE*`保持 | Aとの差で再open/seekとディレクトリ歩行のコストを測る |
| C | Bと同じ読取、コア・SPI ISRを明示配置 | Bとの差でスケジューリング/割り込み競合の効果を測る |

A/Bのタスク・ISR配置は同一にする。CだけUI+SPI2 ISRをcore 1、
SD reader+SPI3 ISR+MP3 decoder+音声出力をcore 0に固定して比較する。
現行の実際の所属コアを先にログで確認し、推定値で比較しない。
因果比較は基準→S、S→A、A→B、B→Cの順とし、CはBから分岐して
配置変更以外を加えない。C内の個別配置要素の効果までは推論しない。
MP3用圧縮リングは既存6,144 Bを再利用し、追加のtransport copyを作らない。
これはSDからデコーダまでの全経路zero-copyを意味しない。

## 全案に共通の安全契約

- UIはpathと権限、SD世代、byte rangeを検証し、世代付きの読取leaseを発行する。
  workerはQuickJS、picker、可変`player`やFS handle表に触れない。
- path、リング、worker handle、終了理由は再生sessionが所有する。
  停止要求だけで解放せず、読取とデコーダの終了ACK後に解放する。
  timeout時はsession全体を保持・隔離し、再利用しない。
- 同一ファイルの書込み・削除・置換はlease中BUSY。別ファイルの操作は許す。
  B/Cはpause中もファイルhandleを保持し、既存のopen-file上限を増やさない。
- SDエラーでは先に論理的に失効させ、進行中のread/handleが終了してから
  物理的にunmountする。現行`sd_media_note_error()`の即時unmountは変更が必要。
  workerは通知で起床し、リング/atomic stateを真実として再確認する。
- 通常EOF、SD読取エラー、デコード不正、underrun、ユーザーcancelを区別する。
  cancelは音声faultに数えず、エラー時は公開済み音声を処理した後に一度だけ通知する。

## 実機ゲート

同一Cardputer/SD/曲/画面設定/電源で基準・A・B・Cを交互に少なくとも3回ずつ測る。
既知の`music/KAKATO/KARA OK 2nd Edition`を用い、2曲以上連続再生、
pause/resume、曲変更、overlay/home切替、アプリ終了を含む。
カード抜去・I/Oエラーは別の安全試験とし、性能分位点へ混ぜない。
COM3を使うのは1実験だけとし、各flash前後にポートが空いていることを確認する。

計測：SD open/readの回数・byte・p50/p95/p99/max、圧縮/PCMリングのlow-water、
decode active/blocked、UI work/draw/compute/sendの全件分位点、12 ms超過、
LCD bytes/帯、音声underrun/fault、pause ACKと可聴再開、free/min/largest heap、
各タスクのstack high-waterと所属core、SPI2/3 ISR所属core、終了ACK時間。
SD serviceは描画の前にあるため、draw単独は主要効果を表さない。
診断版ではUI全フレーム処理・フレーム開始間隔・AV service・
入力タスク観測からUI queue取出しまでを全件計測する。
通常再生の定常窓と、起動・次曲・pause/resume・終了の遷移は分けて集計する。
MP3DECの経過時間はプリエンプションを含むためCPU activeとは呼ばない。
画面captureや大量serial出力は時間測定と別runにする。

最低合格条件は通常再生で予期しないfault/underrun 0、画素/状態遷移一致、
100回の開始・pause・終了でUAF/リークなし。画面p99や音声deadlineを
悪化させる案は、平均FPSが良くても採用しない。採用には直前の比較対照に対し、
対象p99で反復可能な1 ms以上または10%以上の改善を目安とし、
他の主要遅延指標の5%超の悪化とメモリ上限超過がないことを確認する。
閾値は試験後に緩めない。効果が測定誤差以下なら最も単純な案を選ぶ。
採否は加点式でなく、安全性→寿命・権限→資源→主要指標の非劣化→
対象p99の改善幅→同程度なら単純さ、のゲート順とする。
未実施の抜去・障害注入等はPASSとせず、性能候補と正式採用を区別する。

## 基準版の初回実機測定（暫定）

`e74d42c`、`KASANE_P0_PROBE=ON`、アプリ領域のみ書き換えた。
診断バイナリSHA-256は
`259cc3206b4292d43729d6b9cc7fff6dda833cd0904bbac37570683246d563b9`。
書き込み後のesptool hash照合はPASS。SD 25 MHzで上記フォルダの
`01 KAKATORO.mp3`から`02 インザハウス.mp3`へ自動移行し、2曲目で
pause/resumeを1回実行した。overlay終了時、2曲目は135,728 ms、
MP3 fault 0、音声underrun 0だった。

| 項目 | 初回値 |
| --- | ---: |
| overlay draw | 7,339件、p95 15,615 µs、p99 15,999 µs、最大19,551 µs |
| draw 12 ms超 | 1,344件（18.3%） |
| LCD send | p99 11,775 µs、最大12,132 µs |
| draw compute | p99 10,879 µs、最大11,489 µs |
| heap | free 221,888 B、boot後minimum 52,716 B、largest 73,728 B |
| デコーダ | 5,200 packets、平均3,695 µs、最大21,994 µs、stack使用17,652/24,576 B |

生ログは`.cache/sd-variants/baseline-e74-run1/serial.log`。
前回測定に比べ再生時間とpause操作が異なるため、これは採用判定用の
反復測定ではない。A/B/Cでも同じ操作列・区間長で最低3回測る。
実験前の元アプリ領域先頭1 MiBを再取得し、以前に復元済みの退避データと
SHA-256一致を確認した。残り2 MiBは同じ退避データを保持している。
