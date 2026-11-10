# Native diagnostic D1 契約

状態: firmware の host 検証・全体 compile/link 対象。実機の配送、動的stack、DMA、20fpsは未確認。認証・brokerのlease権限を追加する仕様ではない。

## Frame

host→firmware は `GS D1 REQUEST <id> <boot> LF`、`STATUS`、`CANCEL` のみ。GS は1byte `0x1d`、その他はASCII、空白は各1個、LF終端、CRは禁止。全体64bytes以下。idは32桁、bootは16桁のlowercase hex。idはbrokerがjobへ固定し、REQUESTを再送しない。

応答は `GS ND1 <id> <state> <flags> <current-boot> <enabled> LF`。enabledは0/1、flagsは2桁lowercase hex、最長77bytes。起動時bootは非zeroで、そのboot中は不変。bootは認証ではない。

zero id／zero boot のSTATUSだけをbootstrapに使い、`REFUSED 00` と現在boot／enabledを読む。通常のunknown id、boot不一致、zero idへのREQUEST/CANCELも`REFUSED 00`、記録・実行状態は変えない。正常な非zero idへのdisabled REQUESTは`REFUSED 20`。disabledでもD frameを全体消費し、キーへ漏らさない。

D frameの不正／oversizeな末尾はLFまで捨て、応答しない。途中の新GSは旧frameを破棄して新namespaceへ同期する。未完frameを通常キーへ戻すtimeoutはない。B/Cの既存CRC/lease通信は変更しない。q/Escは既存キーのままで診断cancelに変えない。

## Identityと状態

boot中8件、eviction・容量回復なし。admission拒否とbusy拒否も記録する。満杯の新REQUESTは`REFUSED 20`で、そのboot中後から実行されない。同じidのREQUESTはreceiptを返すだけで再実行しない。STATUS/CANCELは同じboot/idだけが対象。terminal後のCANCELは元receiptを返す。リセット後の旧bootはunknownで、再送しない。

状態はPENDING、RUNNING、CANCEL_REQUESTED、COMPLETED、CANCELLED、REFUSED、QUARANTINED。入力taskは小型のsnapshot応答だけを行い、UIが同期work中・quarantine中でもUI callbackを呼ばずSTATUS/CANCELを処理する。応答は各有効frameに1件。自発通知は送らず、terminalはSTATUSで確認する。送信失敗はwork rollbackや再実行の理由にならない。

flagsはそのrequestの処理で確認できた事実であり、その後もHOMEが続くという保証ではない。

| bit | 意味 |
| --- | --- |
| 01 | work callbackが戻った。falseで失敗して戻った場合も含み、成功を意味しない |
| 02 | output drainを確認 |
| 04 | checked HOME復元を確認 |
| 08 | borrowを解放 |
| 10 | ownerを解放 |
| 20 | work未開始。01と排他 |

成功は`COMPLETED 1f`だけ。安全な取消終端は開始後`CANCELLED 1f`、開始前`CANCELLED 3e`だけ。cancel受理はCANCEL_REQUESTEDで、I/O abortやreleaseではない。start CASより前ならworkを抑止するが、acquire→drain→checked HOME→final drain→releaseを実行してから3eを返す。admission不成立なら`REFUSED 20`であり安全なHOME終端とは扱わない。work／drain／restore失敗はQUARANTINED、owner保持、再開なし。

UIのadmissionはHOME、app非実行、modalなし、overlay非実行／非開始、guestなし、dormantなし。manual `~` とtyped ownerは競合させない。物理force_stopはactive receiptへcancelを記録するが、過去idや別idをcancelしない。

## Broker側の責務

bootstrap後nonce固定。boot変化、timeout、失権後はREQUEST再送・自動reopenをしない。失権後は追加write禁止。serial close確認とfirmware終端は別記録であり、不明終端／close失敗／QUARANTINEDはreview。REFUSED 00は完了証拠ではない。

## 検証と制約

`python tools/run_diagnostic_control.py` はcontroller／wireをO2とASan/UBSanで検証。`python tools/test_app_control.py` は実際のB/C/D共有parser、UI work中のSTATUS/CANCEL、disabled・不正frameを検証。`python tools/run_diagnostic_session.py --sanitize` はmain.cから抽出したtyped/manual dispatchと実callbackのadmission、最終barrier、失敗quarantineを検証する。

通常版はCONFIG_KSN_DEVICE_PROBE=n、診断版だけy。defaults、partition、task stack、watchdog、guest budgetを変更しない。8件receiptとparserは固定静的領域でheap割当なし。応答は80byteの局所bufferを使い、既存の約1KiB encoderを通さない。USB送信待ちは既存有限budgetのまま。compile/静的frame分析は実機high-waterの代わりではない。
