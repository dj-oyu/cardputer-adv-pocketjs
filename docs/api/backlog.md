# pocket.* の未解決事項

`docs/api/common-api.md` と `docs/api/filesystem-api.md` からの、現時点で本当に開いている作業だけを集める。完了した項目・陳腐化した項目はここに置かない（元の記述から削除する）。各行に出典と現在の状態を書く。

## BLE（common-api.md 12節）

- **BLE Centralが未実装。** `main/pocket/pocket_ble.c` は名前空間とメソッドだけを持ち、全メソッドが `UNSUPPORTED` を返す（意図的な設計。`ble.central` の `supported=false` は builtins テーブルが既にstage Aから返している）。NimBLE採用時のRAM実測が未実施。
  出典: common-api.md 12節、`main/pocket/pocket_ble.c`。
- **BLE Peripheral** は仕様のみで着手前。`open({name,services})`、GATT定義、notify、HID、Mesh、長距離PHY、Central/Peripheral同時運用は未検証。
  出典: common-api.md 12節。

## 音声（common-api.md 9節）

- **Opusの採否は未決。** ストリーミング実装によりバッファ制約は解消したが、復号タスクのCPU取り分（実機18.6〜20.0%、20msフレーム中）とレンダラーの現状（33.3ms予算に対し39.9ms使用）を突き合わせた採否判断がまだ無い。
  出典: common-api.md 9.1「未完了として残っているもの」、[opus-feasibility.md](../apps/opus-feasibility.md)。
- **`sd:` 上のストリーミング再生が未成立。** 400kHzバス帯域（約50,000B/s）に対しPCM16は48,000B/s必要で実時間に間に合わない。出口は「SDクロックを上げる」（`sd_media.c` の `max_freq_khz`）か「供給を優先度4の専用タスクへ移す」（fs面のスレッド安全化が先）の2つで、どちらも着手前にカード読み出しの実測が必要。
  出典: common-api.md 9.1.1。
- **WAVヘッダの範囲走査（`wav_parse` / `player_at`）にホストテストが無い。** `tools/test_stream.c` はリング・スロット走査のみを検査。`JUNK` チャンクや `data` が離れた位置にあるファイルは実機でしか通っていない。
  出典: common-api.md 9.1.1。
- **音そのものを人が聴いて確認する作業が残っている。** `board_capture` はフレームバッファしか見えず、音声の正しさはソフトウェアだけでは確認できない（WAV/ADPCM/Opus/MP3いずれも）。
  出典: common-api.md 9.1.1, 9.1.2, 9.1.3。
- **MP3のギャップレス（encoder delay/padding除去）、free-format、途中レート変更、破損フレーム、APEタグは未対応。**
  出典: common-api.md 9.1.3。

## PC bridge（common-api.md 13節）

- **wire形式の詳細仕様が未確定。** フレーム種別・length・sessionId・requestId・CRC・応答の具体形式は別仕様として残っている。Wi-Fi transport（認証・暗号化）も未実装（現行はUSBのみ）。
  出典: common-api.md 13節、18節。

## 外部I/O・その他（common-api.md 10, 18節）

- 外部ADC/PWM、UARTパリティ、SPI複数セグメントは能力として未設計（IR送信は実装済み）。
- 画像／スプライトAPIは未設計。
- native回復用RAM余裕、ポート一覧の電気的制約の文書化、証明書・時刻の供給方式、Wi-Fi/BLE profileの組合せ試験は未固定。
  出典: common-api.md 10節, 18節。

## ファイルシステム（filesystem-api.md）

- **SDを含むcopyが未実装。** 1ターンで400kHz全量転送すると入力が止まるため、将来のjob APIへ回す設計。現状 `UNSUPPORTED`。
  出典: filesystem-api.md 10節「進捗（2026-09-08）」。
- **`crashSafeReplace` は `sd:` で常に `false`。** 電源断試験を通していないため。試験実施か、試験しない方針の明文化が必要。
  出典: filesystem-api.md 6節、10節。
- **`sd:` の帯域は算術のみで実機計時者がいない。** 400kHzでの実転送速度、`pocket_fs_read_at()` のディレクトリ走査コストは未計測。
  出典: filesystem-api.md 3節「この機体のSD」。
- **未採用と明記されている機能**（着手条件なし、将来アプリが現れたら検討）: 再帰削除、任意format、ファイルwatch、書込seek、メモリマップのJS公開、POSIX互換、圧縮アーカイブ展開、別volumeの原子的move。
  出典: filesystem-api.md 10節。
