# CLOUD PROBE 有限動的診断

状態: 実装・host検証済み。db4fbb82の通常smokeと区別し、外乱/pause/replayの実機検証は未完。

## 実行操作

登録app `local.cloudprobe`（CLOUD PROBE）を既存app sessionで開き、通常frameが表示された後にtyped tapの`accept`（画面のEnter、buttons 0x4000）を送る。Enter開始frameは他のL/R/U/D入力を併用せず、fixtureがBODY/呼吸OFF/seed42で実行される。実装でも開始入力を排他的に扱う。typed tapの`up`（画面のU、buttons 0x10）で中断して通常BODY表示へ戻る。物理文字キーのuを送る操作ではない。完了時も通常表示へ戻る。停止/CLOSEは既存app sessionの所有者が行う。

CLOUD ACTの6モードとCHARACTER LABは従来のアプリとして維持する。CLOUD PROBEは同じ完全な雲モデル・材質・運動核を共有し、固定世界光/カメラの通常BODY表示と有限診断に絞った別のapp-owned controllerである。形状や部品数を減らして容量へ合わせてはいない。

fixtureは必要時だけ追加chunkを読み込み、brokerの新機能、権限、sleepによる時刻合わせを要しない。時計は既存appの描画tickと固定60Hz simulation step。モニタで最大297回の成功描画（30Hz換算約9.9秒だが実機の壁時計保証ではない）を待つ。

## 入力と結果

- seed42、最初は目標0。
- simulation step60のaccepted描画後、body目標[.18,-.09]へretarget。q/vは保持。
- step120のaccepted描画後、角力積J=[.35,-.22]をbodyへ注入。速度変化はJ/I。
- step150で15回同じ描画tickを提出。q/v、目標、3oscillatorの位相が不変。
- step280の状態を記録しreset。同じretarget/impulseをreplayして30値を厳密比較。

snapshotはq/v/goal24個のF32と、3つのuint32位相をlow/high16へ分割した6個のF32。位相をF32一個へ丸めない。render retryはイベントを消費しない。イベント処理はH.commit成功境界に結び付け、LCD ACKと同一視しない。

`CLOUD_FIXTURE START`、`EVENT RETARGET/IMPULSE/PAUSE/REPLAY`、`COMPLETE result=PASS digest=...`、`RETURN reason=complete|cancel|failure`を記録する。digestは同一実行環境の回帰証拠で、JS/C一般bit一致や画質を保証しない。実行中の失敗はfailurePendingを設定してそのframeを終了し、次frameの入力処理前に診断を解除して通常の再構築へ戻す。例外とVM frameの寿命が終わる前に再構築せず、最後のcommitted描画を勝手に成功と扱わない。

## 検証境界

Node実adapterの297回・pause15・retarget q/v保持・replay30値一致、通常301frameのCLOUD ACT BODY描画packet一致、load中を含む再入拒否はPASS（基準digest e12e1890）。既存native APIの不正入力検査は別のfollow/utility回帰で維持する。CIと最終commitの製品linkはChecksと引渡し結果を参照。新機能の実機検証は候補完成後に担当へ引き渡す。

開始前にactorと時計を基準へ戻す。chunk load/attach失敗でも旧時刻から長時間replayを要求しない。native loaderでtop-level実行後に失敗したchunkはrealm内で再実行できないため、fixture再挑戦にCLOSE後のfresh STARTが必要な場合がある。通常表示への復帰とfixture再試行成功は別契約。


## ローカル最終検証

m32/SSE2、163840 B cap、自動GCあり/毎frame強制GCなし、incremental/full-reference全描画一致。独立2プロセスとも297回の本試験、再開始、commit失敗注入と旧画像保持、次turn通常復帰PASS。各presented=308（注入失敗frameは旧画像）、motion digest=e12e1890、panel hash=3797bb18。最終heap sample158812 B、requested allocator peak146240 B、最大単一guest要求8496 B、realm解放後0 B。sampleは最大値ではなく、requested peakはQuickJS管理費込みpeakでもnative heapでもない。上限163840 Bは全操作中に強制した。

通常CLOUD ACTは2169frame PASS、sampled live_max157148 B、requested peak144048 B。CHARACTER LABは5418frame PASS、sampled live_max144664 B、requested peak132196 B。各解放後0 B。別枠のnative幾何範囲検査6件はunlimited guest条件。3体切替の強制OOM試験は初期値151000 Bでは省メモリ化後にOOMしなくなったため、直前使用量+4096 B（今回138284 B）へfault capを変更し、frame内throw/caught、旧画像保持、通常cap復帰を継続確認した。主frame列は160KiB、補助の強制OOM/復帰試験に限り明示GCを使う。

Astraの独立read-only比較48ケースで、従来版と264 B storage/packed span/legacy引数が完全一致。private material/UV view各1本を省き、native snapshot copyは残した。CLOUD ACTの旧sampled live_max160144 Bから157148 Bへの差は、同条件アプリ全体の変化でありviewだけの単独寄与とは断定しない。実機fps/native free/largest/stackは今回未計測。

内部証拠は `.cache/character-fixture-m32/fixture-result.json`、`.cache/cloud-probe-final-reentry-cli160.log`、`.cache/cloud-act-final-regression160.log`、`.cache/character-final-forcedcap-regression160.log`。プロトタイプbin loaderは製品へ含めず、[不採用記録](cloud-static-asset-study.md)と `.cache/cloud-bin-rejected-db4/` に保存した。
