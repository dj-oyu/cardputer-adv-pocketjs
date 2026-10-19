# USB送信とHOME復帰の統合修正

main `5455a5993df55ef3eb1e1acc0f145f119456ac61` に、PR5の補間位置に基づく
横揺れとPR6のPAN投影深度順を統合する。未回収の `da5b133` / `0d5fdbd`
を含めたものではなく、現在のソースから再構成した修正。

## 再現した送信経路の欠陥

ESP-IDF **6.0.1** の `usb_serial_jtag_vfs.c` は、既定の
`usb_serial_jtag_tx_char_no_driver()` がFIFO timeoutで文字を捨てても、
外側の `usb_serial_jtag_write()` が要求したsizeを返す。
本ファームはdriverをinstallするだけでstdioの経路を変更していなかった。
driver版の標準VFSもtx_charの失敗をwriteに伝えないため、切替だけでは足りない。

`tools/test_usb_console.py --idf <IDF 6.0.1 checkout>` は実際の上記2関数を
抜き出してコンパイルする。`partial\n` の8 bytesを受理したと返してLFを失い、
次の `HOME_READY\n` と `partialHOME_READY\n` に連結することを再現する。
これは送信欠落のコード上の再現であり、既存の実機記録の唯一の原因と断定しない。

## 修正と限界

`hal/usb_console.c` がdriverのブロック送信を使うVFSへstdout/stderrを接続し、
bridgeも同じロックを使う。stdioは256-byte単位、最大995-byteのbridgeは
従来どおり1回のall-or-nothing送信。LFは変換しない。拒否したbytesを成功と
返さず、自動再送もしない。中断した行の後、または未完の行から別streamへ移る前に
LFを送り、次のマーカーを以前のログへ連結しない。同じstreamの正常な分割出力は保つ。

予算は1呼出し50ms。IDF driver内のmutex/ring各待機が残り時間を使うため、
最悪は合計100ms＋tick丸め。無限待機・USB再接続・resetは追加しない。
ホストが読まなければマーカー自体が拒否され得る。リングへの受理はホスト受信やLCDの
表示完了を証明しない。bootloader/アプリ開始前のROMログはこのVFSの対象外。

q/Esc、画面所有者、`APP_STOPPED` / `HOME_READY` の意味は変更しない。
回帰は実際の `end_run()` をコンパイルし、HOME所有時の停止→HOMEと、editor所有時に
HOMEを偽って通知しないことも検査する。stdout/stderr切替、LF欠落後の復旧、部分的な
長文拒否、全体deadline、ロック競合、bridgeの単一フレームと拒否を検査する。

## 検証と引継ぎ

```sh
python tools/test_usb_console.py --idf "$IDF_PATH"
python tools/test_session_dispatch.py
python tools/games/run_derby.py
python tools/games/run_derby.py --m32
```

DERBY runnerはlane、photo、PANのNode回帰3本も実行する。同一head CI
`derby-home.yml` がASan/UBSan、m32、HOME、IDF buildを行い、実際のHEAD/tree、
Gitlink、SDK版、生成物hashをreceiptへ記録する。m32未実行を成功扱いしない。
個別PRの既存binのhashをこの統合版へ転用しない。

flashは別の明示操作。実機では新しいHOME応答、DERBYの横移動・PANの遮蔽、
WIDE 2 fps、guest heap、評価余裕、LOADSTALL、LCDを確認する必要がある。
この変更のhost試験やビルドは、実機確認の代わりにはならない。
