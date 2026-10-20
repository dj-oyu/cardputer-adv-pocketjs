# 固定計測のON/OFF共通正常終了

J1はPR25 `dc2c5e044429a4904e91564aa7f9cb30d7d4cbbb` を親にする診断専用Draft。
実機試験中のPR25/26/27、通常main、PR19/20/24のheadは変更しない。
controller split後も詳細OFFだけが従来のstop→interruptへ戻る問題を修正する。

## 終了契約

`POCKET_DERBY_FIXED_WORKLOAD` のON時だけ、詳細ON/OFF共通でowner完了を受け入れる。
guest/frame/continuation成功、fixture DONE、exit要求、queue空、既存stopなしが必要。
例外・runaway・強制stop・Back・未drainを成功へ変換しない。通常版（FIXED OFF）は従来通り。
最終summaryとENDを別guest returnへ分け、生成controllerも両modeでreport完了をpollする。
fixture/seed/camera/120contexts/3 dependent reps/8step tail、計測scope、USB上限を変えない。

最終guest turnの250ms deadlineを再armせず確認し、owner出力に125ms以上の残量を要求する。
出力後も同じdeadlineを確認し、preemption等で超えた場合はwatchdogログとtimeout teardownで失敗にする。
180秒sessionも出力前に250msの余裕を必要とする。出力は一度だけ試行し、拒否/partialを再試行しない。
110ms成功write、240msの遅いguest return、owner write中の期限超過をホストで検査する。

wire互換のためmarker名は既存の `DERBY_DETAIL_EXIT v=1 session=<n> clean=1` を維持する。
名称は歴史的なもので、OFFでも必要なowner完了証拠。詳細stage/eventのONを意味しない。
strict v2 collectorは END→owner EXIT→APP_STOPPED→HOME_READY→quiet close と完全transportを要求する。
正しいapp/shell tagのinfo行とANSI SGRは許すが、fatal・InternalError: interrupted・LOADSTALL・stop hook失敗は
END後/HOME後も拒否する。過去に失敗した実機ログを遡及的に合格へ変更しない。

## modeと画像・flash・収集証跡

`df_fixed_exit_record` とmode定数 `df_detail_enabled` を両modeでlinkする。target proofはCMake設定、
実際のdd symbols、生成JSのELF内完全bytes、linked mode値、共通owner出力、同一IDF6 BIN/ELF pairを照合する。
自己申告ログやbuild labelだけでmodeを決めない。OFFの5主要描画Cモジュールは旧baseとのobject比較を維持する。

`tools/games/verify_derby_recorded_trial.py` は実機へ接続しないoffline gate。
local BIN/ELFから `prove()` を再実行し、ownerがexportした以下の記録を照合する。

- `broker-manifest.json` のsource provenance/full commit、artifact bytes/hash、build job。
- 正確なflash planと成功jobのplan hash、manifest canonical hash、期待app BIN/ELF hash。
- durable flash summaryの全segment readback、boot、IDF/target、ELF prefix。
- そのcaptureのv2 collection planと成功private jobのbinding、raw_base64、transport receipt。
- raw全量のhash/limit/gap/reset/disconnect/refusal、既知仕事量、owner完了、STOP/HOME、ON詳細schema/OFF詳細不在。

`flash-summary` は取得時のrequest jobを記録した `{ "requested_job_id": "<flash-job-id>", "flash_summary": <responseのflash_summary> }` exportを使う。summary単体を別jobへ後付けで結ばない。

指定head・modeが違う、boot textだけ、別job/plan/source/image、legacy collector、不完全transport、終了欠落は拒否する。
`collection-job` はowner権限で得た `fixed_trial` 付きprivate job exportを使う。最新LKGやhash一致だけから
後付けでreceiptを選ばない。このrepoはbrokerのauthority/DB/private exportを変更しない。
記録を一致させてもJSONの真正性や現在搭載imageを暗号学的に証明するわけではない。
返値は `recorded_chain_matches`、`hardware_tested:false` とし、実機受入/物理LCD/製品純性能を自動宣言しない。

```sh
python3 -B tools/games/verify_derby_recorded_trial.py \
  --build build_j1_off --expect-head <full-commit> --expect-detail OFF \
  --raw trial.log --transport transport.json --manifest broker-manifest.json \
  --flash-plan flash-plan.json --flash-job flash-job.json --flash-summary flash-summary.json \
  --collection-plan capture-plan.json --collection-job capture-private-job.json --out matched.json
```

これはinstrumentation overhead比較の基盤であり、OFFにもcoarse計測・固定fixture・同じ正常終了を残す。
通常製品の純性能には換算しない。J1にheadが変わるためON基準も再測定する。
旧J0結果は旧headの記録として保持する。18bootの自動起動、実機反映、main mergeはこの変更で行わない。
所有境界と合流手順は[合流点の設計](derby-integration-junctions.md)を参照。
