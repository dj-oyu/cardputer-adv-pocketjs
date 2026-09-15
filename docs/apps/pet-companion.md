# Pet Companion

ペットを端末内だけの育成キャラから、作業環境のコンパニオンへ広げる機能です。

## できること

`PET COMPANION` はホームのAPPS末尾にあります。

- **CODEX / CLAUDE**: 最新スナップショットの使用率、次回リセットまで、累計トークンを表示します。上下で対象ペット、左右でページを切り替えます。
- **TOKEN SNACK**: Codexの `account/usage/read` の累計トークンとClaude Codeの応答使用量を、前回との差分だけスナックとして現在のペットへ与えます。初回取得は基準値になり、過去の大量使用分を一度に与えません。
- **リセット通知**: Codexの primary/secondary、Claude Codeの5時間/7日ウィンドウがリセット時刻を過ぎると、全画面の上部にペットと通知を出し、音を鳴らします。Enterで閉じ、右キーで5分後に再通知します。
- **目覚まし**: `ALARM`ページで上下5分、Enterで設定／解除します。毎日ローカル時刻のその時刻に鳴ります。PC同期がない場合は時計が未同期と表示されます。
- **タイマー**: `TIMER`ページで1〜120分を設定し、Enterで開始／停止します。通知は同じペット表示を使います。

端末のSound設定がOFFなら、通知状態は表示されますが音は鳴りません。通知は8件までキューに入り、タイマーは4件までです。期限切れの通知やタイマーは再起動後には復元しません。育成状態・目覚まし時刻・使用量の基準値はNVSに保存します。

## PC側アダプター

`tools/pet_companion.py` はアカウント認証を端末へ渡さないPC側の読み取りアダプターです。USBには検証済みの小さなバイナリスナップショットだけを送り、プロンプト、応答本文、APIキー、メールアドレスは保存も送信もしません。

CodexはApp ServerのJSON-RPCで `account/rateLimits/read` と `account/usage/read` を読みます。`account/read` が利用できない環境でも匿名のローカルストリームとして続行し、アカウント識別子を端末へ送らない設計です。レート制限のパーセントとリセット時刻はサーバー値を使い、トークン数が返らない場合は不明のまま表示します。

```powershell
python tools/pet_companion.py codex --once
python tools/pet_companion.py claude-statusline < statusline.json
python tools/pet_companion.py send --port COM3
```

Codexの収集は最低30秒間隔で実行できます。継続実行は`codex`の`--once`を外します。Claude Codeはstatus line commandに次を設定し、標準入力JSONをそのまま渡します。

```json
{
  "statusLine": {
    "type": "command",
    "command": "python C:/devs/m5stack/cardputer-adv-pocketjs/tools/pet_companion.py claude-statusline"
  }
}
```

Claude Codeのstatus lineに来る`context_window.total_input_tokens`と`total_output_tokens`は現在のコンテキスト／直近応答の値で、長期累計ではありません。そのためアダプターは各応答の`message.id`と`usage`をtranscriptで重複排除し、`rate_limits.*.resets_at`はstatus lineから使います。Claude側のレート制限は利用可能なPro/Maxまたはgateway環境でだけ現れ、欠けている窓は不明として扱います。

時計だけを同期する場合は次のコマンドです。

```powershell
python tools/pet_companion.py send --port COM3 --clock-only
```

USB形式は`RS`（0x1e）で始まり、`P`、48バイトの16進データ、改行です。バージョン、プロバイダ、シーケンス、時刻、CRCを検証し、同じストリームの古いシーケンスや時計の3分を超えるずれは無視します。送信側は`PET_ACK`を確認して最大3回再送します。

## API

ほかの同梱JSアプリは`pocket.pet`を使えます。

```js
pocket.pet.select(0);                 // 0..11、戻り値は現在の選択
pocket.pet.rewards(0);                // PCから与えられた累計スナック
pocket.pet.usage(0);                  // 0=Codex, 1=Claude
pocket.pet.notify('BUILD COMPLETE');  // 24 ASCII文字まで、8件まで
pocket.pet.alarm('build', 300, 'BUILD'); // 秒後、0で解除
pocket.pet.timer('build');            // 残り秒、なければnull
pocket.pet.wake(7, 30);               // 毎日07:30、wake(-1)で解除
pocket.pet.clock();                   // UTC、ローカル分、目覚まし設定
```

認証・アカウント接続・リセットクレジットの消費はこの機能から行いません。Codexのリセットクレジット消費は別の明示的な操作であり、通知はリセット時刻を知らせるだけです。Claude Codeの使用量を取得できない環境でも、目覚ましとローカルタイマーは動作します。

## 検証

`python -B tools/test_pet_companion.py` はスナップショット、未知の窓、Claude累計の扱い、重複・部分transcript、CRCを確認します。`node tools/test_pet.cjs` は育成・命名・保存復元を確認します。`idf.py -B build_pet_companion build` はフラッシュ制限内で成功しています。
