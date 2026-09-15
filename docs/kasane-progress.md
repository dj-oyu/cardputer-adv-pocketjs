# Kasane実装記録

実機確認方針（2026-09-15ユーザー指示）: シリアルポートは別タスクで使用中。
書込み・シリアル診断は後でまとめて実施し、各checkpointはhost試験とESP-IDFビルド後に
commit・pushして進める。実機未確認は各記録に残す。

## checkpoint 0 — JS失敗の原子性（2026-09-15）

- 有効な所有transactionで起きた引数検証・getter・確保失敗は、JSでcatchしても更新全体をabortする。
- 古いtransactionは現在のbuilderを取消しない。callback後処理まで再入buildを防ぐ。
- ticket/template/instance wrapperをnative公開前に確保し、返却OOMによる部分成功やquota漏れを防ぐ。
- poll/features/statsのプロパティ生成失敗を検出し、部分オブジェクトを破棄する。
- 破棄済みDrawRefのnative枠を即時回収し、resetをまたいで識別IDを再利用しない。
- QuickJSが遅延生成時に参照するcacheメソッド定義を関数内`static const`へ変更する。
  自動記憶域の定義は初回`cache.create`で最適化構成のabortを引き起こしていた。

検証:

- `bash tools/build_kasane_test.sh && /tmp/test-pocket-kasane`: PASS。
- `CFLAGS="-O2 -fstrict-aliasing" OUT=/tmp/test-pocket-kasane-o2 bash tools/build_kasane_test.sh`
  と生成実行ファイル: PASS。
- 上記はadapter/core/test側のASan/UBSan・最適化構成。リンクするQuickJS本体は既存host cacheのO1 object。
- 各構成で129箇所のJS確保失敗、native calloc失敗と再試行、100回のabort後の枠回収を確認。
  allocator注入はメソッドを解決してから行い、QuickJS自身の遅延メソッド生成失敗は対象外。
- Astraが`tools/kasane_contract/run.sh`のASan/UBSan・O2、frost/PIE、C++ヘッダ試験成功を確認。
- ESP-IDF 6.0.1 `-B build_ds_contract build`: PASS。S3、PSRAMなし、KSN_DEVICE_PROBE無効。
  app 2,167,648 B、partition空き978,080 B。既存GNU-stackリンカ警告のみ。
- 実機K診断: 未確認。COM3への書込みを試行したが、portが存在せずopenに失敗。
  新firmwareは未書込み。USB再接続後に300ターン診断を実行する。

commit: `0227ac0`。`origin/vm/design-contracts`へpush済み。

## checkpoint 1 — committed stateの再描画・修復（2026-09-15）

- guest submissionがなくても、host invalidateから確定済みbankを再描画する。
- private repairはbankをswapせず、APP/SYSTEMのpoll・cache・ref・modalの確定状態を変更しない。
- 転送中に届いたinvalidateはack後も保持する。途中IO失敗は同じ画面を保持して再試行する。
- 転送前OOM/unsupportedではprivate repairの占有だけ解除し、修復要求を残す。
- `app_force_redraw`を接続。hostのみの修復成功後はJSへ進み、連続するrecording更新で
  guestが実行されなくなるのを防ぐ。Backの最終保存turnはIO修復待ちでも配送する。
- board側pet/recording overlayはSYSTEM移植まで既存合成を使用する。

検証:

- H: `tools/kasane_contract/run.sh`、ASan/UBSan・O2ともPASS。
- Q: `tools/build_kasane_test.sh`と生成exe、ASan/UBSan・pure O2ともPASS。
- 全17帯の転送失敗→cancel→owner repair失敗→成功をJS更新なしで確認。
  画素一致、poll/cache/ref/modal保持、callback中のinvalidate、OOM回復、reset、初回cancelも確認。
- 実sessionの連続invalidate時dispatchはコードレビューとESP-IDFコンパイルで確認。
  実機のrecording併用・picker終了・capture試験はUSB未接続のため未確認。
- ESP-IDF 6.0.1 `-B build_ds_contract build`: PASS。S3、PSRAMなし。
  app 2,168,048 B、partition空き977,680 B、DIRAM 115,548 B。既存GNU-stack警告のみ。

次のcheckpoint 2はgroup内gradientのdither規則。
実機利用可能後に0/1の確認（K診断、静止app再描画、recording、picker/capture）をまとめて行う。
