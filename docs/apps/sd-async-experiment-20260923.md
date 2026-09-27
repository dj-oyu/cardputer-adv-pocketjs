# SD音声I/Oの非同期化：比較実験

2026-09-23。共通出発点は `e74d42c`。Cardputer ADV / 240×135 / PSRAMなし。
目的は「SDとLCDのバスを分離する」ことではない（すでにSPI3とSPI2で別系統）。
UIタスクの同期SD待ちを除き、音声と描画の締切を守ることが目的。
平均FPSだけでなくp99・最悪値、音声underrun、停止・抜去時の寿命を判定する。

## コンセプトと責務

問題はSPI2（LCD）とSPI3（SD）の物理バス競合ではなく、UIタスクが
`read_at`の完了を待つ間、別バスの描画も進められないことである。
KasaneのUI状態・描画契約やJSのプレーヤー制御は変えない。UIタスクは
権限確認と再生sessionの作成・終了を担当し、SD専用workerだけが
lease経由でSDを読み、既存の圧縮リングの未公開slotへ直接書く。
リングの所有権を公開する前にSD読み込みを完了し、停止・抜去では
worker ACK前にring/lease/マウントを解放しない。通知は起床のヒント、
リングとatomic stateが真の状態である。これが全案共通の契約であり、
A/B/Cの違いはleaseの開き方とCPU/ISRの配置だけに限定する。

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

## 同一シナリオの基準と同期対照S（暫定）

以降の診断は`ui_frame`（SD serviceを含む）、`av_service`、
`ui_interval`、`input_queue`を加え、各指標に適したヒストグラム幅で
tailなしのp99を得た。SDの先頭2曲、2曲目を90秒再生、30秒目に
2秒pause、resume、終了という同一スクリプトを各3回実行した。
音声fault/underrunは6回とも0。値は各runのp99（µs）であり、
セッション全体には選曲・遷移も含まれる。定常窓のみのp99ではない。

| 版 | `ui_frame` p99 | `av_service` p99 | `ui_interval` p99 | `overlay_draw` p99 |
| --- | --- | --- | --- | --- |
| 基準＋全指標 | 39,935 / 39,935 / 39,935 | 29,439 / 29,439 / 29,311 | 40,959 / 40,959 / 40,959 | 16,127 / 16,127 / 16,127 |
| 同期対照S | 39,935 / 39,935 / 39,935 | 29,311 / 29,439 / 28,927 | 40,959 / 40,959 / 40,959 | 16,127 / 15,999 / 16,127 |

Sは共通lease基盤だけを追加し、MP3 producerは従来どおりUI上の同期read。
主要p99にはヒストグラム分解能を超える変化がない。Sの診断バイナリは
`3212e78`、SHA-256
`1a971f3556022fefdfef7b56fda67a53b03fb316ee58e6ec6d6aa0abd16d7bf2`。
生ログは`.cache/sd-variants/baseline-full-range-run{1,2,3}`と
`.cache/sd-variants/sync-control-run{1,2,3}`の`serial.log`。
この6 runは実装準備中に基準3回→S3回と測ったため、完全な順番交替ではない。
最終判定ではA/B/Cと対照を再度交互に実施する。

## A/B/C実機比較と選定

診断バイナリはA `d828858`（SHA-256 `c36a262d2024a52fd8cc43ec6e7f197187236ee2a22f940d370c29b920cef5a6`）、
B `7cb4d8f`（`5b878a3e5d55e2884ce24eadba926906f3ff7a09706e2998a288338d1ecb233`）、
C統合 `db4b89f`＋診断条件修正 `efac303`
（`b2a2ac37095b6de16f168398e2f131b4aeb3f52ab4a51f0010c1448a1d2da6f1`）。
Cの最初の試行は`POCKET_PROBES=ON`で起動時SDプローブがSPI3を占有したため
無効とし、以後は全案`KASANE_P0_PROBE=ON, POCKET_PROBES=OFF`に統一した。
同じ先頭2曲・2曲目90秒・30秒で2秒pause/resumeを各3回、Bは後で1回追試。
別時点のS追試も元の結果を再現した。いずれも通常再生のMP3 fault/underrunは0。

| 案 | `ui_frame` p99 µs | `av_service` p99 µs | `overlay_draw` p99 µs | SD open/2曲目 | heap最低 B |
| --- | --- | --- | --- | ---: | ---: |
| S | 39,935 / 39,935 / 39,935 | 29,311 / 29,439 / 28,927 | 16,127 / 15,999 / 16,127 | 同期反復open | 48,476 |
| A | 12,287 / 12,287 / 12,287 | 127 / 127 / 127 | 9,343 / 9,343 / 9,343 | 約1,765 | 43,468～43,572 |
| B | 12,287 / 12,287 / 18,431 | 127 / 127 / 127 | 9,343 / 9,343 / 16,383 | 1 | 43,240～43,564 |
| C | 12,287 / 12,287 / 12,287 | 127 / 127 / 127 | 9,343 / 9,343 / 9,343 | 1 | 42,572～43,564 |

Bの4回目は`ui_frame` 12,287 µsへ戻り、3回目のLCD送信p99
12,159 µsは再現しなかった。B/CはAに対するUI p99の反復可能な改善がない。
B/Cの最大SD read時間は概ね12 ms、Aは概ね17 msだったが、これは
readのp99でも画面p99でもない。B/Cは再生・pause中に既存上限2本の
VFSファイルhandleの1本を占有する。CのSPI ISR coreログは設定要求の証拠で、
ISR内の実行coreを直接観測した値ではない。

選定はA（毎slot再open）とした。S→Aで`ui_frame` p99が27,648 µs
（69.2%）改善し、`av_service`は計測バケット上限127 µsになった。
B/Cの複雑さと常時handle占有を正当化する追加効果は確認できない。
これは本Cardputer/SD/曲での選定であり、B/Cの別条件での優位性を否定しない。

## A選定後の安全・持久試験

Aの初版には外部lease失効をユーザーcancelと同一視し、圧縮リングEOFを
公開しない欠陥が見つかった。`ae555b8`で、明示的stopだけをcancel、
外部失効・読取失敗をsource error＋EOFとし、一時停止中のsource errorも
所有者側で`P_ERROR`へ進めた。ホストのASan/UBSanテストと診断ON/OFFビルドはPASS。
修正版の90秒実機試験は`ui_frame` p99 12,287 µs、fault/underrun 0。
この修正はB `7f12ebd`、C `b562d98`にも反映して各案のホストテストと
診断ON/OFFビルドを通した。ただしB/C修正版を再度実機測定していないため、
上表の性能値は修正前の通常再生試験値である。通常経路の実装差分はない。

- 初版Aの5分試験：2曲目から3曲目へ進み、合計約12.6 MBを読み、
  IO ERROR/MP3 fault/underrun 0。セッション全体の`ui_frame` p99は
  17,407 µsで、短い同一条件試験との直接比較には使わない。
- 修正版Aの100回連続曲送り：起点2曲を含む102 sessionのopen/解放が一致。
  fault/underrun/隔離警告0、stop ACK最大9,055 µs、p99 8,785 µs。
  最低heap 41,312 B、終了後free 217,652 Bへ回復した。
- 再生中に人がSDを抜き、数秒後に再挿入：SD read失敗から公開
  `PLAYBACK IO_ERROR`まで269 ms、`source_fault=1`、underrun 0。
  途中MP3 frameの不足に伴うdecode faultも1と記録されたが、公開エラーは1回。
  packet/PCM EOF後もUIは応答し、終了時のfree heapは217,652 B。
- pause中の抜去・再挿入：停止中はSD I/Oをしないうえ検出ピンもないため、
  失効は観測されなかった。異常処理のPASSとは数えない。
- 以前の問題曲`04 リズム.mp3`を修正版Aで180秒再生し、60秒で
  pause/resume。IO ERROR/MP3 fault/underrun 0、7.2 MBを読んだ。

生ログは`.cache/sd-variants/`の`a-reopen-run{1,2,3}`、
`b-persistent-run{1,2,3}`、`b-persistent-post-c-run4`、
`c-affinity-valid-run{1,2,3}`、`sync-control-post-b-run1`、
`a-reopen-long-run1`、`a-reopen-fixed-*`以下にある。実機の診断バイナリ、
操作列、カードは同一である。未完のゲートはWi-Fi負荷、高ビットレート/VBR等の
別曲群、可聴の再開遅延、100回規模のpause/resumeと終了操作、抜去中の
再マウントと稀なaudio stop timeoutの寿命確認である。入力遅延は今回の
サンプル数ではp99判定できない。これらを確認せずに全条件での製品保証とは呼ばない。

選定したAは本ブランチの`c198692`、`6519d65`、`81e75c1`に統合した。
診断OFFのESP-IDFフルビルド、SD reader/leaseのホストASan/UBSanテストもPASS。
実機試験後は試験前に退避した元のアプリ領域（`0x10000`と`0x110000`からの
2領域）をCardputerに書き戻し、両領域を`verify-flash`で照合した。
現在の端末は実験用Aバイナリではなく、元のファームウェアである。
