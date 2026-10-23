# patternを含むframeの通常線：Y判定をY更新時へ移す実験

baseはmain `e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267`。製品変更は `ksn_procedural.c::plain_band()` のみ。粗いY重なり判定とband_entryが最初の画素をband内に置く正の高さでは、Yはyy+=syの時だけ変わるため、帯の終了判定をその直後へ移す。x clip・Bresenhamの厳密不等号・色・端点順は維持する。水平線fastpath等の別候補やPR42/44の転送・workspace/cache寿命は含めない。

元案は公開APIが受理する高さ0で画素を書いた。線(-480,-480)→(0,0)、y=0,height=0が反例。製品版には冒頭のif(!height)returnを必ず含め、これを外す負例が独立whole-walk oracleで失敗することを確認する。通常線だけのframe.ext=0経路とpattern線自体は変更しない。

native64 O2の40組を2回、raceのJS呼出し→present比は0.98464 [0.96564,0.99286] / 0.98597 [0.97774,0.99563]。pad利益は再現せず、demo全体も未確定。JS単体にも明確な改善なし。区間には観測のsample保存・pending job・end_turnを含む。band/pixel内時計なし、同一binaryのband入口selectorで比較する。2つのraster cloneとselectorは観測コードのみで、製品binaryそのものとは区別する。製品scheduler/LCD/FPS・ESP32性能は未測定。

独立oracleは元端点から全長Bresenhamを歩き、height0・画面外・全方向・tie・重ね描きを含め120352比較。ASan/UBSanと通常O2を通過。MID全14564最終panelを全byte直接比較し、CSVとイベントも一致。数値・heap・JS bytecodeを変える機能は追加しないが、XtensaのC stack差は未確認。独立コードレビューは初期Y不変条件と端点/tie保持を確認した。

```sh
python tools/experiments/plain-y-step/boundary.py
python tools/experiments/plain-y-step/boundary.py --sanitize
python tools/experiments/plain-y-step/boundary.py --negative
python tools/experiments/plain-y-step/verify.py
python tools/experiments/plain-y-step/bench.py --pairs 40
```

計時中に他のbuildや検証を並行しない。各trialは新context、40warm+300測定frame、AB/BA交互、先頭2組破棄。m32は同じscriptに--m32を付け、-malign-double・heap上限163840Bを使う。nativeは524288B。QuickJS/header/compiler/flags fingerprintで古いobjectの混入を防ぐ。

専用CIはnative/m32境界・負例・最終panel・40組A/Bを行い、通常製品とoval/straight診断の各baseline/candidateをESP-IDF6.0.1でbuildする。診断は既存の凍結済み `5bf332368d6e793cd40a8a18a48b4da4ac270622` の生成器を再利用する。warm40後、既存owner callbackあたり最大8stepで390mへ進みsettle10、120frame測定する。24秒期限・INFO起動ログ・partition/security・watchdog/heap/stack予算は不変更。診断は別checkoutのみで通常製品に常駐しない。

実機担当はartifactのexperiment.json/source head/ELF/SHA256SUMSと通常bootのApplication informationを照合する。workloadはderby-app-oval-120-v2またはderby-app-straight-120-v2。fullhead・pointer_bytes=4・120frame・END complete:true・EXIT running=0を確認し、timeout/app_errorは無効。flash offsetは同artifactのflasher_args.jsonを正とし、古いPRの値を転記しない。CI成功は実機完走ではなく、USB/flash/mergeは行わない。
