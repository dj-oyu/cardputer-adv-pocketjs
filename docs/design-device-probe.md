# 新DS 実機診断

2026-09-13、`vm/design-contracts`、コア基点`770c531`。

## 現在の結果

- COM3のESP32-S3、240 MHz、Flash 8 MBに診断ファームを書込み、Flashハッシュ照合成功。
- ユーザーが矩形の動きとホーム画面への復帰を目視確認した。
- 最初の起動時診断ではUSBログが欠落したため、コアテストのPASS、更新時間、heap差分、全画素一致は未確認。目視での成功をこれらの代わりにしない。
- USB接続安定後に`~`で起動する診断へ修正し、ESP-IDF v6.0.1ビルド成功。再書込みの自動承認レビューが一度利用上限エラーとなったが、ユーザー承認後の再試行で書込み・Flashハッシュ照合に成功した。実機にはUSB起動版が入っている。
- USB起動版では実コア回帰テストとレビュー回帰テストがともにPASS。全32,400画素の期待値一致とHOME_READYへの復帰もPASS。

### USB起動版の実測値

| 項目 | 結果 |
| --- | ---: |
| PATCH→change→end→discard、1,000回の平均 | 14 µs |
| 同最大 | 79 µs |
| 計時区間前/後のfree heap | 252,308 / 252,308 B |
| 初回の矩形合成＋全画面転送 | 12,698 µs / 64,800 B |
| 診断後UI taskのstack high-water（未使用量） | 22,028 B |
| ターゲットのsizeof(ds_frame_command) | 172 B |

1回の診断実行の値。タイミングには他タスクの割込みを含み、Wi-Fi/音声併用の条件別評価やp95ではない。
実ログと転送前画像は`.cache/ds-device-probe-usb/serial.log`、`pre-spi.png`に保存した。

## 実装範囲

`CONFIG_DS_DEVICE_PROBE`は既定で無効。`main/ui/ds/ds_device_probe.c`は実際のコアに対するホスト回帰テストをターゲットABIで実行する。
続いてAPP矩形、SYSTEM帯、APP矩形の位置変更を既存のboard stripから同期転送し、ホームへ戻る。
USB起動版はホーム画面で`~`を受け取り、UI owner taskで処理する。実行前にJSアプリを終了する。

初回の描画経路は診断用の不透明矩形・全17帯転送。後続版では位置変更と画素回収を`ds_render_rects`へ接続し、damageに基づく部分転送を検査する。native animation、QuickJS bindingの実装ではない。
1,000回のPATCH→change→end→discardを計時し、平均・最大と前後のfree heapを記録する。
これは表示を含むフレーム時間でもp95でもない。free heapが同じでも一時確保ゼロの証明にはならない。

最終状態の転送前画素を`board_capture`で回収する。ホストは32,400画素を独立した矩形の期待値と比較し、移動元消去・clip・レイヤー表示を検査する。
captureはSPIのbyte swapと物理転送より前なので、液晶の物理的な表示確認は別に行う。
回帰テストは一時的に大きなcoreをスタックに置くため、この診断のstack high-waterをproduction描画の追加スタックと解釈しない。

## サイズ確認

USB起動版のビルド時実測: app binary 2,155,072 B、Flash上限まで990,656 B。
map上の静的DIRAMは124,700 B、未接続コア版115,468 Bから9,232 B増加。
主な内訳はcore本体9,216 B、テスト結果変数4 B、共有ID8 B、USBトリガー1 Bとアラインメント。
`nm`で`probe_core`が0x2400 B、`ds_core_frame`と`ds_device_probe_run`がリンクされていることを確認した。
診断用領域を含む数値であり、新DS全体16 KiB予算の達成を意味しない。

## 再実行

通常ビルドの設定を変更せず、専用sdkconfigに`CONFIG_DS_DEVICE_PROBE=y`を設定する。
今回のworktreeでは`build_ds_contract/sdkconfig.dsprobe`を使用する。

```powershell
. 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1'
idf.py -B build_ds_contract -D SDKCONFIG=build_ds_contract/sdkconfig.dsprobe build
idf.py -B build_ds_contract -p COM3 flash
python tools/ds_contract/device_probe.py --port COM3 --out .cache/ds-device-probe-usb
```

スクリプトは`q`でHOME_READYを待ってから`~`を送る。
`DS_PROBE: PASS`とホーム復帰、135行の完全なcapture、全画素一致のすべてを要求する。
ログは出力先の`serial.log`、一致した場合のみ画像を`pre-spi.png`へ保存する。
初回の失敗ログは`.cache/ds-device-probe/serial.log`。新しい出力先を使って保持する。

## 画像登録・damage追加後の実機検証

2026-09-13。画像登録表16件をcoreの9,216 B予約内に追加。共有IDは計12 Bとなり、coreの常駐計上は9,228 B。
`nm`で`ds_core_damage`と`ds_render_rects`のリンクを確認。ファーム全体の静的DIRAMは124,700 B、app binaryは2,157,520 B。

| 検査 | 実測・結果 |
| --- | --- |
| APP矩形の横移動 | 4,453 µs、mask 0xfe0、7帯、26,880 B |
| 同じ状態の再提出 | 0帯、0 B |
| 全面再送経路の全画素回収 | 32,400画素すべて期待値一致 |
| 1,000回のPATCH→change→end→discard | 平均14 µs、最大91 µs |
| 計時区間前後のfree heap | 252,308 / 252,308 B |
| 診断後stack high-water（未使用量） | 21,964 B |
| 回帰テスト、HOME_READY | PASS |

部分転送時間はこの矩形場面1回の合成＋転送。初回の全面診断は別実装のため、時間比から一般的な高速化率を主張しない。
無変更を検査した後、ホストが転送失敗状態を注入して全面再送を要求し、実際の`ds_render_rects`から135行をcaptureする。これは物理SPI障害を発生させた試験ではない。
ホストでは別途、半透明SYSTEM重なり・画面外移動を含む150回の部分描画と独立した全面参照描画を比較し、途中転送失敗でパネルの一部が変化した状況からの復帰も検査した。
ログと画像は`.cache/ds-device-partial/serial.log`と`pre-spi.png`。実機にはこの部分転送診断版を書込み済み。

## 明示cache追加後の実機検証

2026-09-13。同じ矩形templateから2 instanceを同時表示し、片方を非表示にした。
`ds_cache`は4,096 B、共有ID8 B、template 1、instance 2、保存命令1。診断構成の静的DIRAMは128,812 Bで、cache導入前から4,112 B増加した。

| 検査 | 実測・結果 |
| --- | --- |
| 片方のinstanceを非表示 | 4,419 µs、mask 0xfe0、7帯、26,880 B |
| 同じ状態の再提出 | 0帯、0 B |
| cache操作を含む1,000回の更新 | 平均19 µs、最大201 µs |
| 計時区間前後のfree heap | 248,196 / 248,196 B |
| 診断後stack high-water（未使用量） | 21,852 B |
| 最終転送前画素、HOME_READY | 32,400画素一致、PASS |

ログと画像は`.cache/ds-device-cache/serial.log`と`pre-spi.png`。実機にはcache診断版を書込み済み。
表示されたinstanceの命令はcore bankへ展開するため、cacheの4,104 Bに加えて通常の命令quotaを消費する。

## グループ透過・modal追加後の実機検証

2026-09-14。2枚の重なる子を持つtemplateから2 instanceを表示し、片方を非表示、残りをgroup opacity=128にした。
転送前32,400画素を独立したPythonのpremultiplied合成式と比較して一致した。続いてDIM_LIVE modalを開閉し、入力scopeとfocus=42復帰をowner上で検査、HOME_READYへ戻った。

| 検査 | 実測・結果 |
| --- | --- |
| 初回表示 | 13,528 µs、64,800 B |
| 非表示＋group opacity=128 | 6,307 µs、7帯、26,880 B |
| 無変更 | 0帯、0 B |
| 2命令instanceの1,000回PATCH/discard | 平均25 µs、最大241 µs |
| 計時前後free heap | 248,300 / 248,300 B |
| 診断後stack未使用high-water | 21,756 B |
| 静的DIRAM | 128,812 B、前回比0 B |
| app binary / Flash余裕 | 2,163,024 / 982,704 B |
| core / cache / frame_command | 9,216 / 4,096 / 176 B |

`nm`でcore/cacheの実サイズと`ds_core_group`、`ds_core_poll`、`ds_render_rects`、modalのopen/close/resolve/routeのリンクを確認した。
modal_cancel/focus補正はホスト検証であり、この実機診断では未実行。物理LCD readback、実キー配送、音声/Wi-Fi併用、p95は未検証。
今回は前回の1命令instanceから2命令へ変えており、処理時間差を同一負荷での回帰と解釈しない。
ログと転送前画像は`.cache/ds-device-composition/serial.log`と`pre-spi.png`。実機にはこの診断版を書込み済み。
