# CLOUD PROBE

CLOUD ACTと同じ雲モデル・材質・ayatsuri運動核を使う、固定世界光/カメラの検証アプリ。通常はBODYを表示し、有限fixtureだけ必要時にロードする。CLOUD ACTの6モードは別アプリとして維持する。

- 登録ID: `local.cloudprobe`。既存app sessionのSTARTを使う。
- typed tap `accept`（Enter）: seed42で途中retarget→外乱→pause→replayを開始。
- typed tap `up`（画面のU）: 中断して通常BODYへ戻る。文字キーuではない。
- 完了は`CLOUD_FIXTURE COMPLETE result=PASS`、通常復帰は`CLOUD_FIXTURE RETURN`。

ホスト: `python3 tools/kasane_contract/run_character_app_host.py --cloud --fixture --m32`。これは実機確認ではない。操作、時間境界、容量門、失敗時の次frame cleanupと未検証事項は[検証記録](../../docs/perf/cloud-finite-fixture.md)を参照。
