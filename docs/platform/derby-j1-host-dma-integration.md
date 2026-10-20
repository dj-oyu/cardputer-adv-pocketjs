# J1 host r2 と DMA の専用合流点

通常 main とは分けた検証候補。測定済み PR29 `b561aea1c0798feb28055a00d36681fae81ac58a`
を変更せず、PR31 → PR32 → PR33 の DMA/contract/proof 成果へ、PR35 の host r2 を合成する。
PR34 は PR33 と同じ commit の CI 起動用であり、追加実装として重ねない。
PR30 の共通 capture 修正は PR32 に既に含まれるため再追加しない。

PR35 は同じ b561 を起点にした `integration/derby-j1-host-r2-20261020` へ移し、
`c7b0a84581a4af56cea365e3ced167e4c714f22f` で merge。tree は PR35 head と同一。
専用 DMA 候補は PR33 `ac7cce7d20eb34fdd54659e6678d672d23e5d2d1` とこの host merge を親に持つ。

## 合成する契約

consumer は公開 MCP59 merge `c85412b2e10e4919ad7d8d8e0e2d7f95a39f2406`
（`dj-oyu/esp-device-broker-mcp`）へ固定する。手書き binding で代用せず、合成 fixture に対して
実 `prepare_collector` / `resolve_collector` → schema / live 検査 → host r2 を通す。
entry 追加は offline test process 内のみで、production registry は変更しない。

| lane | 必須の期待値と照合 |
| --- | --- |
| legacy J1 | 既存の詳細 ON/OFF と全 receipt。DMA 軸は省略のみ許可、明示 null は拒否 |
| entry-bound J1 | 上記と明示 `--expect-entry-sha256`。manifest/planned/captured の entry が一致 |
| DMA | 明示 `--expect-dma ON/OFF` と entry digest。詳細との二軸、外側 DMA proof 全体の hash を一致 |

DMA CLI は actual local BIN/ELF から `prove_dma` を再計算する。nested J1 proof の代用、
entry 欠落/null、別 source/owner/device/current receipt、cache の BOOL と STRING/無型の混在、
DMA 関数欠落・data symbol の代用、BIN/ELF 改変を拒否する。

## 新しい head ごとの検証

- 既存 r2 434 tests と producer tests を実行する。
- `tools/test_derby_mcp59_composition.py` と consumer 自身の DMA 負例を frozen 環境で実行する。
- 詳細 OFF/ON × DMA OFF/ON の4構成をそれぞれ新 head から build する。
  CI artifact の source stamp、actual BIN/ELF/cache/typed proof を
  `tools/games/verify_derby_dma_consumer.py` で実 consumer に渡し再検証する。
- full contracts は J1 b561 基準と合成 head の両方で `GAMES_M32=1` を実行する。
  共通 test overlay、source/head/tree/hash の照合を保持する。
- 独立 review と CI は候補の最終 head で確認する。以前の head の成功は流用しない。

## 未実施の受入段階

production source registry は J1 b561 のみ。候補 source の実受理・登録・配置や実機操作、
通常 main への統合はこの候補作成に含まない。host/CI の成功は実機性能、LCD、heap/stack の受入ではない。

実際の owner-exported records を使う段階では、digest-bound retained manifest の正式な export 契約が必要。
MCP の public `read_build_logs` manifest は raw cache を `cache_evidence` に置換するため、
内部 flash manifest と digest が異なる。public 応答を内部 manifest とみなして検査を緩めない。
この合成検査では完全に合成した内部 manifest だけを使い、private cache・実測 raw・実 receipt を公開しない。
