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

## 実装メモ

`pocket.pet`は`docs/api/common-api.md` §2の作法に沿う。`limits`は`main/pet/pet_hub.c`の`pet_limits[]`で公開し（`maxPets`, `maxNotifications`, `maxTimers`, `maxLabelChars`, `maxTimerIdChars`, `maxAlarmSeconds`, `maxSpeechChars`）、`capability.available`はNVSオープンの成否を返す`probe`の観測値です（決め打ちのtrueではありません）。範囲外の引数・上限超過はすべて`PocketError`（`INVALID_ARGUMENT` / `LIMIT_EXCEEDED` / `NOT_AVAILABLE`）として届き、生のQuickJS例外は`main/pet/pet_hub.c`と`pet_assets.c`からは出ません。

`pet.js`は`pocket.ui`ではなくレガシーの`ui.createNode`を直接使うため、`main/pocket/pocket_ui.c`のノード数ガード（`UI_SAFE_NODES 15`）を通ります。2026-09-07にノード数がこの上限を1つ超え（18 taffyノード）、レイアウトが要求する単一連続ブロックの段差を跨いで確保に失敗し、Rust側がエラーを返さず再起動する形で表面化しました（`CLAUDE.md`のtaffyノード段差を参照）。装飾用のバー3本を落として15ノードに戻し、実機で299フレームの描画とキー操作を確認しています。ノード数を増やす変更をするときは`tools/uibudget/`で先に確認すること。

`pet_hub_pump()`は`app_tick()`の外（`main.c`）で走るため、通知・アラーム発火からJSへ届く処理は`app_tick()`の中に置く必要があります（QuickJSの割り込み期限は`app_tick()`先頭で更新されるため）。データ変化のpush通知（`pocket_api_capability_changed()`相当）は建てていません。`rewards()`はfloatを返し確保ゼロで、`frame()`はアニメーションのため毎フレーム走る必要があるので、購読にしてもポーリングと大差が無いためです。

PC↔デバイスの双方向通信は`pocket.pet`ではなく`pocket.bridge`（`main/pocket/pocket_bridge.c`、[docs/api/common-api.md](../api/common-api.md) §13、`apps/bridge`）が担います。`pet_hub_usb()`の48バイト固定フレームは今も一方向（PC→デバイス）で、デバイス側からのレスポンス経路は`pocket.bridge`の役割です。

## 検証

`python -B tools/test_pet_companion.py` はスナップショット、未知の窓、Claude累計の扱い、重複・部分transcript、CRCを確認します。`node tools/test_pet.cjs` は育成・命名・保存復元を確認します。`idf.py -B build_pet_companion build` はフラッシュ制限内で成功しています。

### ネイティブ描画の改善ループ

まず実機で遅いフレームを特定し、計測区間に対応する実装を読みます。使われていない機能や重複処理を削れるかを最初に調べます。残る処理には、誤差を評価した近似や代数的に等価な式、ESP32-S3のPIE命令、別コアへの委譲を検討します。コアをまたぐ場合は、データ移動、同期、通信との競合も測ります。

broker MCPで同じモデルと通信条件を使い、HTTPSのストリーミングとWi-Fi再接続を実機で比較します。平均FPSに加え、LCDのDMA転送完了間隔の最大値、50 ms超の回数、最低空き内部RAMを記録します。描画を変更した場合は、基準フレームとの画素差を確認します。

### Blenderモデルとモーションの書き出し

2k bilateralモデルの書き出しには [`export_companion_track.py`](../../tools/ayatsuri/asset/export_companion_track.py) を使います。Blenderの評価済みメッシュから、ボーン・シェイプキー・表情・漫画記号を含む姿勢をサンプリングします。目の周囲の補助光と補色寄りの影を焼き、フレーム間で共有する頂点と面をまとめます。

```text
blender -b fox-2k-bilateral.blend --disable-autoexec --python-exit-code 1 --python tools/ayatsuri/asset/export_companion_track.py -- OUTPUT_DIRECTORY
```

brokerのモデル入りソースには、生成した `hone_track_asset.h` と `companion_motion_config.h` を `main/ayatsuri/private/assets/live/` に置き、CMakeの `AYATSURI_TRACK_MODEL` を有効にします。実機ではキーの間を補間し、各フレームで三角形の深度を使って各帯を描画します。生成された `asset-report.json` の元モデルとトラックのハッシュを、ELFと起動ログで照合します。

ネイティブ版は起動するとモーションを再生します。←／→でモーション内の8つの姿勢を、↑／↓で6つの表情プリセットを順に選びます。選んだ姿勢や表情は画面に保持され、Spaceで再生に戻ります。Enterを押すと描画ベンチマークを開始します。通信計測を組み込んだビルドでは、続いてWi-Fi再接続とHTTPSの負荷をかけます。

### キャラクターの切り替え（HCA）

`AYATSURI_CHARACTERS` を有効にしたビルド（`sdkconfig.variant.ay_characters`）は、`assets/characters/*.hcz` をすべてファームに埋め込み、実行中に切り替える。狐の書き出し（`AYATSURI_TRACK_MODEL`）も併用でき、その場合は先頭のキャラクターになる。

| キー | 動作 |
| --- | --- |
| ←／→ | モーション内の8つの姿勢 |
| ↑／↓ | 表情（キャラクターごとの一覧） |
| E | 漫符（なし → 各記号 → なし）。表情・モーションと同時に重ねられる |
| B | 背景（なし → 7種 → なし） |
| C | 次のキャラクター |
| M | 次のモーション（クリップ） |
| Space／Enter | 再生に戻る／ベンチマーク |

左上の4行に、キャラクターとクリップ、表情、漫符、状態を出す。右側は漫符の置き場なので空けてある。ベンチマーク中は漫符を外し、終わると表情と漫符を元に戻す。

`.hca` は [`export_character_track.py`](../../tools/ayatsuri/asset/export_character_track.py) が Blender の評価済みメッシュから作る。`pip install bpy`（Blender 5.1 でも 4.3 製の `.blend` が開ける）か `blender -b` で動く。

```text
python tools/ayatsuri/asset/export_character_track.py cloud.blend cloud OUTPUT_DIRECTORY
python tools/ayatsuri/asset/export_character_track.py fox-2k-bilateral.blend fox OUTPUT_DIRECTORY
python tools/ayatsuri/asset/export_character_track.py head-turn-seated.blend human OUTPUT_DIRECTORY ARCHIVE_ROOT/human-assets
```

- メッシュは基準姿勢で1回だけ削減する。ほかのキーは、元の頂点の変位を最近傍の逆距離重みで写すので、トポロジーは変わらない。
- 表情と漫符はキーに焼かず、グループごとの「表情マスク」と「漫符条件」で実行時に決める。どの表情でも、どのクリップでも使える。
- クリップは別の `.blend` からも取り込める。リグと頂点順が同じなら動き、そのファイルに無い物体（ポップの顔の差分など）は最近傍の頂点の動きに乗せる。
- 陰影の動きが表示の1段階以内の三角形は、色を1つにまとめる。雲（2クリップ・表情6・漫符3）で166 KB。
- 書き出しは `.hca`（展開後）と `.hcz`（deflate済み。ファームが埋め込むのはこちら）の両方を作る。雲＋人間（15クリップ）＋狐（2クリップ）で、アプリ領域には 77,133 B + 257,430 B + 28,213 B（実測、`check_flash.py` の余裕 200 KB）。
- 人間は、顔の絵（頭の160三角形にテクスチャで貼られている）を、絵のある三角形だけ細分割して表情ごとの色に焼く。漫符は元のカメラ前提の位置にあるので、頭の右上に向きと大きさをそろえて置き直す。立位のクリップでは椅子を畳む。
- 狐は、表情（普通・喜び・驚き・困り・まばたき・横目）と漫符を実行時に選ぶ。目とまぶたは、三角形ごとの表示と色を、表情ごとに元の目の材質で評価して焼く（`export_companion_track.py` の `EyeMaterial` を再利用）。クリップは旋回＋呼吸（168フレーム）と呼吸だけのループ（96フレーム）。`fox-2k-bilateral.blend` から 65,136 B。
- 動作の差分（63%）と色の時系列（29%）が大半で、表情の色は 0.5%。左右対称にしても、姿勢ごとの動きが左右非対称なので、データはほとんど減らない。

ホストでは `tools/ayatsuri/model/test_track_character.c` が、ファームと同じ帯の入口で全表情・全漫符・全フレームを描いてPPMに書く。

```text
cc -O1 -I main -I main/hone tools/ayatsuri/model/test_track_character.c main/hone/track_model.c -o t
./t cloud.hca OUTPUT_DIRECTORY
```

元の `.blend` は [cardputer-development-archive](https://github.com/dj-oyu/cardputer-development-archive) が公開している。そこから作った `.hcz`（雲・人間・狐、計363 KB）は `assets/characters/` に追跡して置くので、brokerの fetch→build でそのまま埋め込まれる。`.hca`（展開後）はホストの確認用で、リポジトリには入れない。再書き出しすると数百 KB が履歴に積まれるので、更新は確定版だけにする。

#### データ領域 `ay_chars`

展開後の `.hca` は 1 MiB に近いので、RAM にも app 領域にも置かない。ファームは `.hcz` を埋め込み、初回の起動で `ay_chars` パーティション（0x650000 から 0x1B0000。`storage` の使われていない末尾）へ展開し、以後は1体ずつ mmap で読む（`main/hone/track_store.c`）。

- `storage` は 0x270000 から 0xC0000 に縮めた。使っているのは先頭の 0xA6000 まで（ソース保存・`pocket.fs`・下書き）で、位置は変わらない。
- 展開は新しいデータを焼いた後の最初の起動で1回（数秒から十数秒。「LOADING」を出す）。ディレクトリを最後に書くので、途中で電源が切れても次の起動でやり直す。
- 先頭の 64 KiB がディレクトリで、各キャラクターは 64 KiB の境界から始まる。3体と背景で 1.69 MiB（27ページ）を使い、0x1B0000 にちょうど収まる。キャラクターを足すなら、`storage` をさらに縮めるか、パーティションを見直す。
- パーティション表の変更はbrokerの書き込みに含まれる（ブートローダ・表・アプリだけを書く経路）。データ領域そのものはbrokerでは書けないので、この方式にした。

### 背景（バックドロップ）

`assets/backgrounds/backgrounds.hcz` に、アーカイブの [stage-assets](https://github.com/dj-oyu/cardputer-development-archive/tree/main/backgrounds/stage-assets-20261120) の7種（子供部屋・ビーチ・恐竜の森・Pipe Dream 倉庫・Vaporwave Temple・初期 Mac・夕暮れの入江）を入れた。B で順に切り替える（なし → 7種 → なし）。

```text
python tools/ayatsuri/asset/pack_backgrounds.py assets/backgrounds/backgrounds.hcz "CHILD ROOM=child-room/background.png" ...
```

- 各 240×135、不透明の PNG を RGB565（64,800 B）にして、1つのコンテナ（`HBG1`）にまとめる。隣のピクセルとの差分にして deflate した（`HCZ2`）。7枚で 453,728 B が 163,896 B。
- キャラクターと同じ `ay_chars` に展開し、描画では mmap したピクセルを行ごとに合成する（RAM は使わない）。モデルの色が 0 のピクセルは「空」で、背景が見える（ラスター背景の検証と同じ規則）。
- 背景を切り替えると、画面全体を描き直す。
- **アプリ領域の余裕は 35,200 B しかない**（3体＋背景で 3,110,528 B / 3 MiB）。次に足すなら、先に何かを削る。

### ラスター背景の検証

`python tools/hone/make_companion_raster_test.py --header main/ayatsuri/private/assets/live/companion_raster_test_pixels.h` で背景画像を生成し、CMakeの `AYATSURI_RASTER_TEST` を有効にします。起動後は左側に縦線が往復します。`T` で縦線を切り替え、`Enter` で静止背景を重ねたまま性能を計測できます。アセット用リポジトリに同じテスト画像とRGB565への変換手順があります。[LCD背景テスト画像](https://github.com/dj-oyu/cardputer-development-archive/tree/main/backgrounds/lcd-test-chart)
