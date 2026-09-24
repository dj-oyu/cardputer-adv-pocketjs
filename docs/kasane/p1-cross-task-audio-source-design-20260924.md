# P1 別task source：音声出力の数値telemetry

2026-09-24。これは実装前の接続設計であり、P1完了や実機gate合格を宣言しない。

実装追記：最初の製品service接続は、描画に直接使える1 Hzの8 byte text
`HH:MM:SS`を選び、[実機結果](p1-audio-output-source-device-20260924.md)に記録した。
その後、[U32拡張と実機確認](p1-audio-source-u32-device-20260924.md)で
frame・starved block・stream IDを同じsnapshotへ追加した。
約33 msの更新周期は未採用で、現行は約1 Hz。短時間observer OFF/ON ABBAは
[別記録](p1-audio-source-abba-20260924.md)で実施したが、描画workloadは
OFF/ONで異なるため性能出口には用いない。
元案の「callback内で文字列生成しない」は、固定8文字の時刻を音声serviceの
空きpool slotへ直接書く実装へ変更した。動的確保・中間コピー・JS処理はなく、
1秒に1回の有界な整数演算だけである。毎回owner taskへ数値を渡してから
別のtext snapshotを作る二段階より、コピー/同期と表示遅延を抑えるためである。

## 接続点

既存の`pocket_av_ui_read()`はUI owner taskで`player`の状態と
`sound_stream_position()`のatomic値を組み立てる。したがってこの値を
UI taskからpoolへ写すだけでは「別task producer」の検証にならない。
`wifi_time`はイベントcallbackと同期taskの複数writerで、単一writer poolに
直結すると契約を破る。音声出力taskの`play_stream()`は既に単一writerで、
実際に消費したframe数とstarvation block数を持つため、この境界を採用する。

音声HALからKasaneをincludeしない。`sound_stream_start()`へ任意の
非blockingな数値observerを渡し、出力taskが`emit()`に成功した後、
進捗を`(stream id, output frames, starved blocks)`として通知する。
observerは`pocket_av`が所有し、事前確保した3 slotの`ksn_source_pool`へ
小さな完全snapshotを直接書く。callback内で文字列生成、JS、malloc、
SD access、task通知、mutex、待機をしない。poolがBUSYならその版をskipし、
次回に全fieldの最新値を再公開する。`changed_fields`は全fieldを立て、
revisionを飛ばしたreaderが差分を落とさないようにする。

音声出力は128 frame ≒5.33 ms/block。全blockでpoolを触らず、
6～7 blockごと（約32～37 ms）だけ試みる。判定はblock counterの
整数比較だけで、starvation中のunderrun変化も取りこぼさない。
pause中は公開しない。開始時の0、終了時の最終値も
試みるが、pool枯渇によるskipは許容し、UI側の最終stateは従来の
owner-task hand-backを正とする。33 msは設計初期値であり、
描画・音声の同一バイナリA/Bに通らなければ増やすか、この接続を棄却する。

payloadは数値だけ（出力frame、starved block、stream id）。
曲名、総時間、再生/一時停止状態、文言は含めない。これらは
`pocket_av` owner taskの別の事実であり、音声taskにコピーして
整合性のない「全player snapshot」を擬装しない。別source間の
原子的同時性はP1 v1の契約どおり保証しない。時刻表示は
`pocket_av`側でframe→msを変換する。現行Kasane schemaの数値slotは
`U16`だけなので、32-bitのframe数を公開bindする前には、汎用の
`U32` slotをschema/JS検証/契約試験に追加する。16-bitへの飽和・折返しで
長い曲の値を偽らない。内部poolの実機A/BはこのAPI拡張より先に行える。
新sourceを消費しない
アプリは現行のatomic読取りを使い続けられる。

## 寿命と退避

poolのstorageとadapterはplayer serviceの静的領域とし、音声taskが
参照する間はreset/reinitしない。stream idは再生・seekごとに変わる。
新streamの最初のsnapshotはidを含む完全値で、旧streamの進捗を
新streamへ誤適用しない。`sound_stream_stop()`が音声taskの離脱を
確認してから旧streamのcallback参照を無効化する。離脱を確認できない
異常系ではstorageを維持し、解放・再初期化しない。

registryからの解除はKasane側のleaseが解放された後に行う。
現状の`app_stop()`は`pocket_av_reset()`が`pocket_kasane_reset()`より先なので、
sourceを公開する実装では順序の見直し、またはserviceの静的寿命と
明示的なgeneration失効が必要。reset直後の同一app epochで古い
capabilityが再利用できないことをhost試験に含める。

## 実装・採用gate

1. callbackなしの`request_t`サイズとqueueの増分を記録し、
   音声task側でcallback有無の分岐を1 blockにつき高々1回にする。
   可能ならobserverなしの通常再生pathは追加のpool accessを0にする。
2. producerの完全snapshot、3 slot枯渇、複数reader、seek後のid変更、
   stop/resetとの競合をhost ASan/UBSanとO2で確認する。
3. 同一診断バイナリ・同一SD曲でobserver OFF/ONをABBA比較する。
   既定の短時間gateに加え、長時間再生、pause/resume、
   `draw/send p95/p99/max`、12 ms超過、audio underrun/fault、
   free/largest/min heap、stack high-water、pool skippedを採る。
   基準閾値はP0の固定値を使い、後から緩めない。
4. 数値sourceをKasaneへbindするviewで、JS `frame()`を呼ばずに
   進捗表示が更新されること、stream idの切替時に旧値を描かないこと、
   不正snapshot・BUSY・DISCARDEDで表示cursorを進めないことを確認する。
5. 描画性能または音声安定性が悪化すればobserverを既定有効にせず、
   従来のatomic読取りを維持して失敗を記録する。P1の別task source
   gateは未達のままとする。

Kasane coreは音声、曲名、状態文言を知らない。callbackとpoolは
system service側の実装であり、Kasaneへの境界は既存の型付き
`ksn_source_provider`だけとする。
