# Kasaneフルシステム受入試験

入口は`tools/system_full_test.py`。System managerとKasaneを、Taffyなしのfirmwareとして
hostから実機まで検証する。通常のlegacy混在buildはこのsuiteに含めない。

## 実行

リポジトリrootでEIMのESP-IDF環境を読み込んで実行する。
シリアルポートは排他的に使用し、`--flash`は生成したfirmwareを実機へ書き込む。
userdata/NVSはeraseしない。通知probeは専用ownerだけを生成・回収する。

```powershell
. 'C:/Espressif/tools/Microsoft.v6.0.1.PowerShell_profile.ps1'
python tools/system_full_test.py --out .cache/system-full-run `
  --nm C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin/xtensa-esp32s3-elf-nm.exe `
  --port COM3 --flash
```

既定buildは専用`build_system_full`、起動終了は100回。`--cycles`は2以上へ変更でき、
実施回数をreportへ記録する。出力先は新規/空ディレクトリに限定し、前回の証拠を上書きしない。
ホスト環境はWindowsではWSLのgcc/python/bash、他ではnative bashを使用する。

実機なしの部分試験は`--host-only --out .cache/system-host-run`。
この場合は`HOST_PASS_DEVICE_NOT_RUN`であり、`FULL_PASS`にはならない。

## 合格条件

| 段階 | 検証内容 |
| --- | --- |
| System host | clock/power、独立dirty/poll、通知8+1、timer満杯再試行、壁時計/snooze/鳴動期限、時刻巻戻し、tick換算、JS powerの寿命。ASan/UBSanとO2 |
| Kasane native | 合成・透明・gradient・画像・cache・modal・animation・SYSTEM重なり・IO失敗/修復・quota・PIE/scalar一致・ヘッダ互換 |
| Kasane JS | 実QuickJS経由のAPI、確保失敗、APP/SYSTEM寿命、解放。ASan/UBSanとO2 |
| firmware | `KSN_ONLY=ON`、legacy sourceを存在しない場所に指定してbuild。PSRAMなし・native probeあり |
| Taffy排除 | component graph、map、ninja、compile commands、ELF symbolsにTaffy/旧UIがないこと。失敗したらflashしない |
| 実機animation | 300 tickの変化、画像変形、modal→APP復帰、power初期配送、複数計測窓 |
| 実機System | 動くAPP上の通知、満杯snooze拒否、ACK後timer再試行、snooze、消去、SYSTEM命令数とcapture |
| 実機lifetime | 未移行APPの拒否、Kasane共通サービス、入力release、既定100回の終了時JS=0とheap/最大連続領域の非劣化（許容256 B） |

全段階成功時だけ`FULL_PASS`。途中失敗は即終了し、後続段階を成功として埋めない。
flash後は失敗時もprobe回収とHOME復帰を試み、cleanup失敗も失敗として記録する。
実行器自身は`python tools/test_system_full_runner.py`で検査する。host部分成功、host失敗で停止、
Taffy監査失敗時のflash禁止、cleanup失敗時の終了コード、全成功時のartifact記録を、実機に触れず確認する。
component/source/symbolへTaffyを混入させる拒否試験も含む。Pythonの`-O`/`PYTHONOPTIMIZE`は
assertを無効化するため受入試験では拒否する。IDF起動はredirect時の出力回収停止を避ける`--no-hints`経路を使う。

`report.json`にはgit commit、開始時差分、各command/exit code/所要秒、firmware SHA-256、
全cycleのmemoryを保存する。段階ごとの`.log`、device serial log、通知PNGも同じ出力配下に残す。
未実施項目はreportの`not_measured`に明示する。

## この合格が意味しないこと

対象は現在実装されたSystem managerとKasaneの統合経路。全周辺機器・全アプリの網羅、
実LCD/スピーカーの目視・聴取、SNTPの実時刻変更、sleep消費電流、filesystemの全故障条件は
このsuiteでは確認しない。壁時計境界や音の発行条件はhostで確認し、実機時計やNVS設定を
試験のために変更しない。各未移行機能の実装時に対応する実機試験を追加する。
