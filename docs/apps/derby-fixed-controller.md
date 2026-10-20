# 固定DERBY診断controllerの分割

batch詳細計測の実機起動で、9,683Bのplay chunkがcompile OOMになった。source/build/readback/boot成功とLKGはアプリ起動成功を意味しない。旧headの失敗証拠とbaseline/nativeの受入結果は保持し、実機追加操作を停止したまま修正する。

`derby_fixed_workload_chunks.py`は生成済みplayの固定controllerだけを`derby_fixed.js`へ分け、元の位置へ同期`pocket.app.load('fixed')`を置く。新chunkは明示的なstrict global script。同じrealmのlexical bindingsを使い、frame wrapperを設置してから元playの`enter('pad')`、READYへ進む。既存entryのprog/view/scene/pan/play呼出順は変更しない。fixedはplay内のnested loadなので、APP_LOAD完了ログではfixedがplayより先になる。

比較をそろえるためbaseline/native/batch、詳細ON/OFFのすべてへ同じ分割を適用する。fixture/seed/camera/390→510/120contexts/3 dependent reps/250ms watchdog/180秒session/8step tail、描画/VM/同期I/O、heap/stack予算は変更しない。OFF生成playへcontrollerを元位置で戻すと元比較版の生成bytesへ完全一致する。通常アプリとnative CのOFF経路は変更しない。

controllerが無い・syntax error・runtime throw・strict違反ならplayから起動失敗を伝播させ、READY/計測開始へ進まない。fallbackや再試行は足さない。分割前のplay compile failureと、新しいfixed nested failureではfilename/causeやloaderのcompile/run分類が異なる。特に外側playは実行中失敗となり部分初期化済み/再実行不可になるため、例外文言・retry分類まで完全同一とは主張しない。既存loaderのfatal interruptの伝播は変更しない。新規load分の起動費用とstackは未測定である。

ホストharnessは同じ生成controllerとchunk登録を使用する。proofはwatch/view/playに加えてcontrollerの生成fileとELF内完全bytesを要求し、欠落・改変・strict欠落を拒否する。固定ON/OFF実行でwork/draw/race/pixel hash、tail/終了parserを検査し、新しいstartup負例を通す。通常host回帰と同headのCI/target ON/OFF proofも確認する。

前段のsequential候補の64bitホストlive payload peak低下は、今回のnested load実装や実機RAMの数値根拠に流用しない。m32/hostモデルもESPの共有内部DRAM・fragmentation・namespace常駐量を再現しない。実機OOM解消と実機性能は未検証。旧実機batchを保持し、新flash/reset/recovery/試験は別の手順と承認で行う。
