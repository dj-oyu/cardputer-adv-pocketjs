# P1 固定容量source snapshot pool（2026-09-24）

`ksn_source_pool`は別taskのproducerが値を直接構築し、owner turnのreaderが
その値を**コピーせずにpinして借用する**ためのアプリ非依存部品。
3つのpayloadスロットを呼出側が事前確保する。1 writer・複数readerで、
publish/acquireにheap確保、FreeRTOS通知、UI待機を入れない。
producerは空きslotへ完全な値を書いてから公開し、公開後は保持した可変pointerを
使わない。readerのlease中は古い世代も上書きしない。

`begin`は現行slotを避け、freeまたはpin数0のretired slotを1回のCASで確保する。
3 slotとも保持中なら`KSN_BUSY`とskip数を返す。producerはその時点の更新を
coalesce/skipでき、UIの解放を待たない。`publish`はrelease/acquireで
最新tokenを切り替え、旧slotのretireは単一atomic RMWで済ませる。
readerはtokenを読んでpinし、tokenを再確認するため、切替との競合で
古い世代を「最新」と誤認しない。revisionは32-bit token内で単調増加し、
上限で`KSN_LIMIT`を返してwrapしない。

このpoolは**寿命・公開のprimitiveのみ**で、現時点ではclock/musicの
`ksn_source_provider`へ接続していない。source registryの権限・field型・
consumer cursor、UI transactionのPRESENTED ack、期限schedulerは別層の責務。
pool単体が全経路zero-copyを保証するわけでもない。producerが別の場所で
値を作ってからslotへ移せばそのcopyは残り、core bank・render scratchも
従来どおり。P1固定snapshotの完成認定にはprovider統合、pool枯渇時の
音声deadline、detach/reset、複数consumer、実機heap/p99のgateが必要。

`test_source_pool.c`で、cancel、double release拒否、世代交代中の
旧値不変、3 slot枯渇時の即時`BUSY`、255 reader pin上限、
1 writer＋2 readerの100,000回並行試行を確認した。
`-O2 -fstrict-aliasing`とASan/UBSanの両方でPASS。
Kasane contract suite全体も両構成でPASS。
ThreadSanitizerはこのWSL環境で`unexpected memory mapping`により
開始できず、race不存在の証拠には用いない。ESP-IDF 6.0.1の
診断OFF build・容量検査はPASS。まだ実利用者がいないのでlinkerは
未参照poolを除去し、今回のimage容量は増えなかった。実機gateは
provider統合後に行う。
