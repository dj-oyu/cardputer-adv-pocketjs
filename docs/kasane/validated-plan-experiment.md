# 登録済みprocedural planと実行状態の分離（実験）

正本の[P1候補](https://chatgpt.com/space/page_0ff002295eb08191ac548083270a6316)を単独で試す。
基準は通常main `e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267`、tree
`9e80c14dfe141dcdec8bdf7ca2121c5b5e7bae66`。PR19/20の派生ではなく、両headを変更しない。
QuickJS-NGの実装・JSソース・近似数学・第2コア・DMA・raster・segment appendの契約は対象外。

`KSN_PROC_VALIDATED_PLAN` は既定OFF。通常採用は実機比較後とし、Draftを維持する。
`KSN_PROC_VALIDATED_REFERENCE=ON` は同じcacheを保持しつつ既存snapshot begin/runを使う比較用。
reference指定にはplan指定も必要。新しいJS APIは追加しない。
他のDERBY描画実験との同時指定は未受入のため拒否する。

## 所有と寿命

内部handleの登録時に、引数binding済みのコード、検証済みloop情報、既存のfusion印を
独立所有のopaque planへ保存する。ROM entryや元配列、引数への借用pointerを残さない。
毎drawにはreg/input/pc/loop stack/pen/steps/statusと参照を持つ実行状態だけを初期化する。
入力・stateの有限性、step/segment/raster上限、失敗位置と丸め順は引き続き検査する。
命令を実行するcoreは参照経路と共有する。公開generic begin/bound/stateと従来plan APIは
引き続きコードをsnapshotする。

実行状態はzero初期化して使用し、activeな状態はcopyしない。begin成功時にpinし、
使用中のprepare/freeは元planを変更せず拒否する。beginの重複も既存leaseを保って拒否する。
step/runはleaseと世代・program/metadataの同一性を確認する。generationはwrapしない。
失敗prepareはplanを無効化する。endはpinを放しpointerを消す。二重endは何もしない。
途中endは中断扱いでINVALIDとなり、成功stateのcaptureはend前に限る。
generationだけで解放済みpointerを安全にする設計ではない。owner taskとpinが生存を守る。

native drawはbegin→run→endを同期的に終えてから、error生成・typed points・appendへ進む。
既存のJS再入guard・stale handle検査を維持し、使用中reset/ROM table変更も拒否する。
slot公開前の追加cache OOMは登録全体を失敗させ、既存slotと次handle番号を保持する。
unregister/reset時にはcacheも解放する。

ROM table切替では内部cacheを失効させる。既存handleは元entryを指す既存契約を保ち、
次drawでstill-liveな元entryから再prepareする。table/entry/code/patchは全sessionより長寿命
でなければならず、table切替は旧table解放の許可ではない。typed pointsの係数はcacheに
借用せず、従来の所有plan側で管理する。JSから公開後に変更するAPIはない。

## 比較と測定の範囲

```sh
python tools/test_proc_validated.py --out artifacts/contracts
python tools/test_proc_validated_rom.py --out artifacts/native-rom
python tools/games/run_proc_validated.py --out artifacts/game
```

最初の検査は、OFFの3moduleが基準とobject全byteで一致することを確認する。
generic APIをOFF/ON、最適化版とASan/UBSan版で検査し、新APIは毎stepのpause/resume、
read-before-write zero、source/binding mutation、pin中prepare/free拒否、世代、失敗snapshot、
capture、全実行上限、解放後操作を参照VMと比較する。

1命令と64命令のprobeはregistration prepare、draw begin、execute、append、lease releaseを
別々に記録する。参照には既存sized planのfusion実行を使う。appendはcompiler barrierと
測定区間外のvolatile checksumで実copyを残す。clock呼出し等を含むホストphase観測であり、
実機速度・whole-work差・採用根拠にはしない。

実QuickJSのadapter検査は20planのsynthetic境界入力、引数拒否、OOM atomicity、slot churn、
ROM切替、active resetをscalar/fake PIEで検査する。ゲームcaptureとは別の検査である。
全ゲームはOFF/ON-reference/ON候補を3tierと強制oval MIDで比較し、draw入力列・順、
frame CSV/work、pixel hash、race結果を一致させる。実際の経路呼出し数と、reset後cache要求量0も確認する。

## メモリと未受入事項

ROMにもbinding済みコードを保持するため、元の40-byte程度のROM planに追加heapが必要。
既存plan/pointsは参照・typed points用に保持し、slotにも追加pointerを持つ。cacheは最大32個で
各コード数分を確保し、上限を超える無制限cacheはない。allocation失敗はOOMを返す。

初回64bitホストの全ゲームでは追加cacheのピーク要求payloadが15,668 bytes、reset後0。
これはtarget allocator metadataや実DRAM、差引増分ではない。小さいexecutionは192 bytes、
同ON buildのsnapshot VMは1,040 bytesだった。target ABI・static DIRAM・allocator peak・
guest heap・評価余裕・stack・独立bootのwhole-work/表示受入を別途比較する必要がある。
OFF/reference/candidateのtarget CIはlinked symbol・設定・source head・BIN/ELF pairを証明し、
size-filesを保存する。実機性能・heap/stack受入には代用しない。

詳細計測版`90b9f686`はEND後HOME前のinterruptにより厳格parserがFAILとの報告がある。
この別firmwareの計測値をP1採用根拠には含めない。PR19の通常smokeとは分けて扱う。
