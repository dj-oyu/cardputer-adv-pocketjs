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

## 半透明・すりガラス比較診断

2026-09-14。`ds_frost`の1/8縮小・分離box blur・bilinear拡大・tintを実機で検証した。
左パネルはtintのみ、右はblur＋tint。線・ボタン・枠は後から描く。両側tintのみ3秒、右の半径1を4秒、半径2を12秒表示する。
ホーム画面で本体から`~`を入力すると再実行できる。USBの`~`も従来通り。物理キーボード入口はビルド済みだが、今回の自動実行はUSB経由。

| 検査 | 実測・結果 |
| --- | --- |
| 半径1: 背景生成＋縮小＋blur | 29,011 µs |
| 半径2: 背景生成＋縮小＋blur | 30,210 µs |
| 半径1/2: 比較画面全体の生成・SPI転送 | 47,079 / 47,154 µs |
| 専用保持領域 | 2,048 B |
| 静的DIRAM | 130,860 B、前回から+2,048 B |
| app binary / Flash余裕 | 2,165,552 / 980,176 B |
| 半透明部品の既存画素検証 | 32,400画素一致 |
| 半径2の比較画面 | 32,400画素一致、HOME_READYへ復帰 |

`nm`で`probe_frost=0x800`および`ds_frost_feed/blur/span`のリンクを確認。ホストでもASan/UBSanと最適化の両方で、半径1/2の計64,800画素が独立Python参照式と一致した。
準備時間には市松模様の背景生成も含む。全画面比較表示時間には模様の再生成、左側tint、右側補間、装飾と転送を含み、blur単体や毎フレームの性能ではない。
転送前画像は`.cache/ds-device-glass/glass-pre-spi.png`、実ログは同ディレクトリの`serial.log`。物理LCDのreadbackではない。
この診断は既知の背景からの画素処理検証で、実アプリのcapture handle/attach、期限付き逐次実行、frosted modalへの接続は未実装。

## 経時変化・毎フレーム再生成の負荷試験

2026-09-14。600フレーム、約60秒（回収除外）の連続負荷。背景スクロール、2枚の半透明矩形の移動/alpha変化、すりガラスパネルの移動/tint alpha変化を同時に実行した。
毎回2 KiBのsnapshotを作り直し、blur半径1/2を60フレームごとに切り替え、全画面をSPI転送する。30 fps目標の周期へ追従できない場合は飛ばした周期を計上する。

| 項目 | 実測 |
| --- | ---: |
| フレーム処理時間 平均 / p95 / 最大 | 88,196 / 89,388 / 89,986 µs |
| 背景生成＋縮小＋blur 平均 | 44,207 µs |
| 再合成＋補間＋tint＋SPI 平均 | 43,988 µs |
| 回収除外の経過時間 | 59,988,406 µs |
| 実効表示速度（待機・通常ログ含む） | 約10.00 fps |
| 33,333 µs期限超過 | 600 / 600フレーム |
| 飛ばした目標周期数 | 1,200 |
| 通常転送量（回収再描画を除く） | 38,880,000 B |
| free heap 開始 / 採取最小 / 終了 | 246,148 / 246,148 / 246,148 B |
| 最大連続空き 開始 / 採取最小 / 終了 | 73,728 / 73,728 / 73,728 B |
| UI task stack未使用high-water | 21,692 B |

600件の時間を保存する診断スタック2,400 Bを追加。静的DIRAMは130,860 Bで増加なし。app binaryは2,169,040 B、Flash余裕976,688 B。`nm`で`ds_stress_probe_run`のリンクを確認した。
この条件では30 fpsを達成していない。約10 fpsには目標周期への待機も含み、処理時間の逆数は約11.34 fps。
画素回収の合計2,999,242 µsは時間とアニメーションから除外。空き領域は60フレーム間隔で採取した最小値で、瞬間最低値ではない。
0/299/599フレーム（tick=0/29,899/59,899 ms）の計97,200画素が独立Python式と一致し、HOME_READYへの復帰も確認した。記録は`.cache/ds-device-stress/serial.log`と`stress-report.json`、画素は`stress-000.png`、`stress-299.png`、`stress-599.png`。
PASSは負荷処理の完走を示し、性能目標達成を意味しない。QuickJS/DS PATCH/音声/Wi-Fi併用を含むアプリ全体の試験とは分ける。
