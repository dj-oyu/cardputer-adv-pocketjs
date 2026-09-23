# P1 汎用source契約とclock移行の途中結果（2026-09-24）

`ksn_source`はKasane側がアプリ名・domain名を知らない型付きsourceの
内部C契約である。固定4登録・各24 field/slotの範囲で、service所有の
`acquire(cursor)`/`release` lease、世代付きhandle、consumer認可、
slotとfieldの型照合、完全な現在値、revision/changed/valid mask、
期限切れ時のbase値復帰を扱う。文字列payloadはlease中に借用し、
`effective`へはdescriptorだけを写す。producerはpin中のsnapshotを
書き換えない責任を持つ。更新cursorは検証・`ksn_schema_session_step`成功後に
進め、表示cursorはPRESENTED後だけ進める。登録世代はregistryの再利用後も
重複しないようにプロセス内のatomic counterを用いる。

最初のconsumerはclock assetである。wall-clock値をsource所有の短い
文字列に構築し、Kasaneのbase slotは別に保つ。clock以外の通常アプリと
music providerは既存経路のまま。`view.bind`の公開APIや複数sourceの合成、
expiryを起床させるscheduler、producer pool枯渇時の並行task保証は
**未実装**で、P1全体は未完了。clockの取得は同一owner turn内なので
並行pinを必要としないが、一般の別task producerには別途固定容量の
snapshot所有実装が必要である。

## Host gate

- `tools/kasane_contract/test_source.c`: 認可、型・重複slot拒否、pin中の解除拒否、
  複数reader独立cursor、preflight相当の失敗時cursor維持、revision gap、
  有効期限、base復帰、不正snapshot、世代使い回しをASan/UBSanとO2で試験。
- 実QuickJS統合: clockの分更新にJS `frame()`なしで描画、同じ分ならsubmitなし、
  JS base変更はnative有効中に表示・転送を変えない。全テスト0失敗。
- 普通のpet presenterは従来のstatic allocation 1280 Bを維持。

## 実機gate（Cardputer ADV、COM3、ESP-IDF 6.0.1）

P0固定条件は[時間ゲート](p0-timing-gates-20260924.md)を参照。
clockは画面キャプチャで表示と約6秒の連続稼働を確認した。
背景が動的かつ現在時刻が異なるため、過去PNGとの画素一致は主張しない。
clock overlayの起動差分は過去の90,508 Bに対し90,772 B（+264 B）。
これは同一バイナリA/Bではないため参考値とする。

| 試験 | binary SHA-256先頭 | app描画p99 | overlay描画p99 | 音声異常 |
| --- | --- | ---: | ---: | --- |
| P0旧・再測定 | `07DCC123B72B` | — | 9,343 µs | 0 |
| P1初回 | `D62ACAB110DC` | 1,151 µs | **16,767 µs 不合格** | 0 |
| P1世代修正版 | `9ADAD6EE3BF4` | — | 9,343 µs | 0 |
| 同一実行payload・追試 | `28C943FC813E` | 1,279 µs | **16,127 µs 不合格** | 0 |

P1初回helloは180更新を2回実施し、両回181描画、LCD 210,720 B/557帯、
render p99 1,151 µs、12 ms超え0、heap/stackは固定下限以上だった。
音声試験は各45秒で同じSDの01→02曲、一時停止2秒→再開。
旧再測定と世代修正版はsend p99 5,119 µs、work p99 3,711 µs、
ui_frame p99 12,287 µs、underrun/decoder fault/IO ERROR 0。
初回P1だけsend p99 12,031 µs、work p99 7,423 µs、
ui_frame p99 18,431 µsに悪化した。追試の不合格版はsend p99
11,775 µs、work p99 6,783 µs、ui_frame p99 17,407 µs。
`9ADAD6...`と`28C943...`のimageをバイト比較すると相違は65 B、
offset 176から32 Bと末尾33 Bのみで、音声・描画の実行payloadと主要関数の
配置アドレスは同じだった。したがって追試の合否反転はコード配置によるものではない。
現在のsourceを再ビルドした`C4C7D114...`も`9ADAD6...`と実行payloadは同一
（相違64 Bは記述子・末尾digestのみ）。
不合格の時間帯はLCD `board_present`を含むsendが平均約4.8→6.0 msに増え、
scene kernelは約0.61 msのままだった。SDカードは常に25 MHz、
LCDとSDは別SPI hostだがDMA/メモリ競合の詳細は未測定。
musicは新sourceを購読しないため、汎用source処理時間を原因とは断定できない。
同じ実行コードで固定音声描画gateが揺れるため、P1実機gateは**未合格**。
閾値を緩めず、送信reap/queueとSD readの同時性を計測して切り分ける。

生ログとclock PNGは`.cache/kasane-p1-20260924/`に保存。
試験後に元の3 MiBアプリ領域を復元し、2領域とも`verify-flash`で
digest一致を確認してCOM3を解放した。
