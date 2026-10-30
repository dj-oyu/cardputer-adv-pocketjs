# ayatsuri 動的追従核の最小実装

これは[動的演技の設計案](ayatsuri-dynamic-motion-draft.md)の一部を実装した縦切り。一般IK、接触、制約グラフ、顔輪郭morph、HOME adapterは未実装。製品への実機適用は別工程。

## 状態・目標・更新

`main/ayatsuri/ayatsuri_follow.h` は2自由度の相互連成を固定1/60秒で評価する。状態は一般化座標q[2]、速度v[2]。pair内では同じ単位Uを使い、回転はrad、雲の彩雲変位はモデル距離、速度はU/s。目標g[2]は次のretargetまで保持し、retargetでq/vを変更しない。周回角の最短経路処理はこの最小核に含めず、連続した非周回座標を入力する。

慣性M=diag(I0,I1)、対角の駆動剛性k0/k1と結合kc、減衰c0/c1と結合ccからK/Cを作る。結合誤差は `(q1-g1)-(q0-g0)`。各値は正規化したモデル単位であり、実物の質量推定ではない。forceは一般化力（慣性×U/s²）、impulseは一般化力積J（慣性×U/s）で、速度変化はJ/I。回転の場合は角力積。変位を直接加算する操作とは分ける。

完全後退Euler:

`A=M+hC+h²K; v'=A⁻¹Mv−A⁻¹hK(q−g)+A⁻¹hf; q'=q+hv'`。

prepare時にB=A⁻¹M、F=A⁻¹hK、G=A⁻¹hを計算し、更新中の逆行列を避ける。これは反力のある2DOF核。雲の複数pair間の根元→末端駆動は一方向の視覚近似であり、全身の物理的相互連成ではない。

## CとJSの境界

```c
ayatsuri_follow_prepare(const ayatsuri_follow_config *, ayatsuri_follow_coeff *);
ayatsuri_follow_step(const ayatsuri_follow_coeff *, const ayatsuri_follow_goal *,
                    const float force[2], ayatsuri_follow_state *);
ayatsuri_follow_impulse(const ayatsuri_follow_coeff *, const float J[2],
                       ayatsuri_follow_state *);
ayatsuri_follow_retarget(const float target[2], ayatsuri_follow_goal *);
ayatsuri_follow_drive(uint32_t phase[3], const uint32_t increment[3], float *out);
```

Cはcaller所有メモリを同期借用し、ポインタを保持しない。状態F32、演算中間F64、結果をF32へ一度格納する。非finite/範囲外を検証してから全結果を公開するため失敗時stateは不変。`-fno-fast-math -ffp-contract=off`、binary64中間を要求する。prepare成功後の係数は変更せず使用する。

JSの通常利用は `apps/lib/ayatsuri_follow.js` の `AyatsuriFollow.create(description)`。`retarget(goal2)`、`impulse(J2)`、`step(force2)`、caller提供出力への `sample(out)`、明示 `reset()` を持つ。各instanceは固有状態を持つ。戻り値をimmutable snapshotとみなさず、必要な保存先をcallerが所有する。JS wrapperはdescriptorを初期packetへ変換し、native `followCreate` / `driveCreate` が状態と共有メソッドを持つobjectを返す。状態確保はQuickJSのjs_mallocでguest上限に算入し、finalizerで解放。guest bufferを保持せず、realm解放で回収する。C経路は同じ核であり、独立した製品用数値実装を二重維持しない。Node参照backendとpacket adapterは試験内だけに置く。

低層JSは `pocket.ayatsuri.followPrepare(configF32[8],preparedF64[14])`、`followStep(prepared,stateF32[4],goalF32[2],forceF32[2])`、`followImpulse(prepared,state,J)`、`followDrive(phaseU32[3],incrementU32[3],outF32[1])`。configはI0,I1,k0,k1,c0,c1,kc,cc、preparedはB[4],F[4],G[4],I[2]、stateはq0,q1,v0,v1。高層利用者がこのoffsetを操作する必要はない。橋渡しは短いstack snapshotを取り、成功時だけ書き戻す。可変guest bufferを後の描画まで借用しない。無効入力はINVALID_ARGUMENT、数値範囲超過はLIMIT_EXCEEDED。

## 共通駆動と時計

64×int16の共有sin LUTは[-32767,32767]、線形補間。3つのuint32位相と増分を持ち、現在位相をsampleしてからunsigned wrapで進める。重み.55/.30/.15。seedは初期位相だけを決め、毎stepの独立乱数ではない。これは軌跡の再生ではなく外力入力であり、応答は現在q/vと目標で変わる。LUT方式がPRNGより高速という実測はまだない。

核自身は時計を持たない。同tickでstepを二度呼べば二度進む。app側がstep数、pause、再開を所有し、描画帯数や描画失敗で重複更新しない。雲adapterは同時刻を再評価するとstep0、後退seekは上限付き再実行。複数pairの途中失敗はprivate状態をinvalid化して次回replayする方式であり、全actorのtransaction rollbackではない。最後のcommitted画像の保持は描画側の責務。

## 検証の区別

机上試験の厳しい20秒/1e-8 radと16秒/1e-4 rad/sのFAILは[記録](../perf/dynamic-motion-host-study.md)に残す。新しい実用probeは試験前にq誤差と速度各1e-3以内を60step継続、20秒以内と定義。数値安定・参照一致・この収束基準・目視の演技受入は別々に判定する。描画上の補償を物理IKと呼ばない。

無確保の対象はC核とnative typed-packet呼出しのwarm loop。JSの表情計算、モデル更新、sampleへの数値boxing、ログ、初期化、描画まで無確保とは主張しない。固定stepなので描画FPS変更は同step状態に影響しないが、実機の処理時間・最大同時actor数は未計測。

managed sampleの出力はposition/velocity/goal各Float32Array(2)、drive.sampleはFloat32Array(1)、drive.snapshotのphases/incrementsはUint32Array(3)を要求する。通常Array出力を保証しない。sampleはcaller提供bufferへ同期コピーし、保持しない。
