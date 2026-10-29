# Character materials 縦切り実装とhost検証

2026-10-29開始。基準main843496c。実装・試験中の記録で、実機測定ではない。[utility契約](../api/character-utility.md)、[低API契約](../api/shapes.md)、[app操作](../../apps/character/README.md)。

## 実装した範囲と変更理由

既存の単色面分割では雲が硬い多面体に見え、人/キツネの圧縮伸長は1024線分を超えた。そこでshapes互換を維持し、汎用の解析的楕円体を184Bの不変packetとして候補frameへ凍結する低APIを追加した。保持費用は20entry=200B/物体。走査帯で連続法線・body UV模様・世界光・視線・光路厚み・alphaを評価し、全画面bufferや点ごとのJS drawcallを増やさない。

この方式は直交投影、方向光近似、二色band/checkerに限定する。画像texture、PBR、物理体積散乱、落影、自動depth sortは未実装。設定図と同等の髪/毛/雲の美術品質は未達で、楕円体の小さな縦切りモデルとして比較する。

utilityは配置/材質/UV変形/光/カメラのhandleを公開し、型変換・dirty依存・native提出を隠す。v0.2の全文実装ではなく、full clip/layer、morph、2D補正は未実装。WAKEのpose・呼吸・揺れ・耳flickのscheduleは軽量な名前付きperformanceへ移し、固有形状はモデル側に残した。全scene強制移行はしていない。

点描/漫画トーン/アミカケは[比較試作](../../tools/character/coverage-study.md)として分離した。毎frame乱数を使わず位相/平均被覆/移動/伸縮/停止を調べるが、nativeへ未採用。moireや知覚的ちらつき、実LCDでの効果は未検証で、速度改善も主張しない。

## 今回の計測と通過範囲

| 検査 | 結果・条件 |
| --- | --- |
| Node utility | 更新なし、局所/親/光/視点/UV/材質の依存、コンパクトbuffer共有、invalid setterと失敗後復帰を照合 |
| WAKE oracle | 基準843496cの全104frameと、モデル座標・投影行列・描画入力/順序が一致。Node mockの入力照合で、device timingではない |
| 低API | 実QuickJS ASan/UBSan、楕円体C renderer O0/O2、全frame/8行帯一致、alpha/光/視点/厚み/UV/負scale/不正packetを検証 |
| 実app/64bit host | 実QuickJS・native proc・Kasaneで3体×6mode×301frame=5418frame、例外/表示失敗0。背景必須・text capacity不備をこの試験で発見し修正 |
| 実app/m32 host | 同5418frame。初版はlive最大214096B（無制限run）。共有prototype・group省資源・backing buffer共有、再入/評価順guardと不要fallback除去、正式名/alias移行後、heap上限163840Bを課して通過、live最大162088B、評価peak98604B、独自allocator追跡peak143932B |

m32は8B JSValue/4B pointer/8byte double alignmentのhostモデル。liveはQuickJS malloc_size、評価peak/allocator peakはfixtureのTLSF丸め追跡であり、指標を足し引きしない。既定上限との差は1752Bしかなく、実機の総空き・最大連続領域やロード/GC条件の余裕を保証しない。native frame等は別領域。実機fps/heap/largest/最大単一allocは未測定。

同じm32 gateへ最大単一guest要求の追跡を加えた最終fixtureでは、load中10912B、実行中1571B。これはJS allocatorへの要求サイズで、native最大連続領域や確保済みblock総量ではない。64bit ASan側の実行中要求は2103Bであり、m32値と混ぜない。実C境界ではsource配列変更後もsnapshot画像不変、同一boundsの光/UVだけの変更で再描画、資源超過/BUSY/新beginFrame復帰、雲の停止8秒/10秒の画素一致も検査する。

追加のm32 runではframeごとのfixture強制GCを止め、上限到達時のQuickJS自身のGCに任せても同5418frameを通過し、上記peakは同じだった。全5418frameでincremental表示を退避後、同じ状態を全画面invalidateして全画素比較し一致した。新packetの送信2帯目へKSN_IOを注入し、repair中beginFrameがBUSY、同じ候補の再送完了後に全描画一致することも確認。realm解放後のguest allocator残量は0B。これらはnative malloc失敗や実DMA完了を再現する試験ではない。

別の失敗phaseでは雲のlive145416Bから上限を151000Bへ下げて人の生成をOOMにし、旧native表示の全画素保持を確認した。失敗後GCで96676B、通常の163840B上限へ戻しframeを再試行して人が表示され、live162136B。通常5418frameのpeakと失敗注入phaseの値を混ぜない。現appは失敗した旧JSモデルへ戻すのではなく、旧native画像を保持して生成を再試行する契約である。

MEGADEMO初版55cb20fの新CIでは、m32/163840Bのcandidateが2232frame中例外1848/DEGRADE3となり、baseline実行へ進めなかった。最初のログはDEGRADE tier=1 nullで、null単独から例外原因を断定しない。同じnative binary・同じ163840Bで旧843496cアプリは2232frameを通過（評価後138108/live最大151764/allocator peak146548B）。診断用に上限を2048Bだけ増やしたcandidateが通過したことも合わせ、宣言/再生の寿命と常駐費用を修正した。上限を増やす変更は採用していない。

修正後は借用keysとcompactな再生状態を残し、WAKEの宣言済みbuilder参照、および実appでは使わないprocMegademo.specs/frameのhost記録用関数を解放する。独立したシーン試験は記録用関数を保持する。実app/m32/163840Bは2232frameで例外/DEGRADE/提示失敗0、live最大154124B、評価後140352B、compile peak141404B、allocator peak148612B。初版の評価後150344Bから9992B削減した。bytes131396160、132登録・plan最大17・2set共存124frameを保ち、plan移行や演出を削っていない。これもhostのTLSFモデル値で、実機の確保余裕ではない。

正式なhone/Ayatsuri名と互換aliasを加えた後も同条件2232frameを通過。評価後140444/live最大154216/compile peak141532/allocator peak148696Bで、上段の命名前のrunと区別する。全16枚のnative合成captureが基準mainのcaptureとbyte一致した。host stubはeager namespaceのため、新hone getterのproduction lazy構築にOOMを注入した試験とはしない。

独立レビューで55cb20fの整数名channel順序問題を再現（`['time','0']`がtime=3/0=5）した。d6a1bb0のcompact再生状態では宣言names配列を直接使うためtime=5/0=3となり修正済み。numericなtrack/wave/gateと逆順numeric名の回帰テストを追加した。material setter後のevaluate省略はrender拒否を明示検査する。

追加native範囲試験は通常の160KiB/5418frameと切替OOM試験の**後**に別phaseで行う。JS確保不足と区別してnative validatorへ到達させるため、このphaseだけguest capを解除する。大きいzoom/scaleのnative上限超過2件、Float32 overflow2件、巨大checker repeats1件の全5件でINVALID_ARGUMENT、未公開candidateが旧committed表示を変えないこと、正常値へ戻して再evaluate/begin/render/commitできることを全画素で確認した。160KiBでこれらの極端な入力を生成できる保証や、新しい実機予算ではない。

初版CIには別のDERBY画素一致回帰もあった。新marker判定より前に飛ばした帯外patternのparameter entryを次のgeometryと誤認していたため、帯に関係なく必ずpairを消費するよう修正し、後続の可視primitiveを含む全描画/8行帯の回帰ケースを追加した。MEGADEMOのheap条件の失敗とは原因を分ける。

低APIのshadeは同期借用・コピーなしだが、遅延描画ellipsoidは所有権のため184Bをnativeへコピーする。utilityのcopiedBytes=0はJS内部bulk copyの範囲で、アプリ全体のコピー0ではない。更新なしのevaluateは幾何等を再計算しないが、renderは候補frameへのpacket提出を行い、帯damageが転送要否を決める。

## 既存MEGADEMO perfとの関係

[shapes導入記録](../apps/megademo-shapes.md)のdd3bfba WAKEはturn127.6〜133.6ms、841e23eは17.1〜22.9ms、3d3ea63は17.5〜19.1msという著者報告。HEAVY/broker/30frame窓で世代が異なる。3d3ea63のbytes54048〜56096は窓最終frameで、平均ではない。APIと帯damage等の寄与は分離不能。

[演出記録](../apps/megademo-story.md)のe9e7fbfはHEAVY/broker90秒/2.9周、WAKE turn15.5、CORE10〜19ms、評価後app全体free69196/largest31744/JS121760Bの著者報告。新方式の実測値ではない。今回raw receiptの独立照合はしていない。

turnにはJSから同期呼出ししたC処理も含む。session render/send、frame間隔、host CPU時間、JS heap、native free/largestを混同せず、新APIのfps保証へ流用しない。GCが旧WAKEの主因だったとも断定しない。

## 続く工程

Astraの低層レビューは、まずJSのclosure/配列重複と寿命、更新依存と被覆、C内側loop、最後に適する連続kernelのPIE化、という優先順へ反映する。現packetは10B列へfloatを直接castせず、整列済み184B構造体とのmemcpyと明示reserved/末尾zero-fillを使う。candidate/committedの交換を維持し、小packetのsnapshotコピーを削るために可変JS backingを遅延借用しない。

ユーザー指定の[esp32s3-hw-mcp](https://github.com/dj-oyu/esp32s3-hw-mcp)と[過去のPIE計測資料](https://github.com/dj-oyu/esp32s3-hw-mcp/blob/main/data/pie_measured_costs.json)は調整候補の根拠であり、このappの測定値ではない。今回PIE命令を追加していない。将来採用時は命令ごとの整列/端数/符号/飽和とSDK転送契約を確認し、packet数に加えてpixel評価数、part×band訪問、overdrawを実機で調べる。過去harnessのcycle数やTRMのSoC上限から今回のfps/LCD帯域を保証しない。

最終headでhost/CI/firmware linkを確認し、実機のロード余裕・描画時間・入力応答・質感を別工程で測る。flash/restartは今回行わない。身体だけ/光だけ/カメラだけと停止後の安定を連続frameで見て、光が模様に貼り付く問題を切り分ける。

低API設計・実装後は、同utilityをHOME background sceneから使う工程をユーザー指定の後続範囲として扱う。時計/描画先/開始停止/資源ownerを環境adapterへ分け、現Cのscene_ops_t/strip描画/背景解放と照合する。現時点でHOMEから利用できるとはしない。前景UI、scene切替、app起動/復帰時の停止再開と解放、予算を検証する。3体常駐や生活シミュレーション全体には広げない。
