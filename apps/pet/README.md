# Pocket Pet

Cardputer ADV向けの同梱JSペットアプリ。ホームのAPPS末尾にあります。

- 猫: 茶トラ・三毛・黒猫・サイバーパンク。
- ウーパールーパー: ピンク・アイボリー・ミント・サイバーパンク。
- オカメインコ: グレー・イエロー・ブルー・サイバーパンク。

初回は左右で色、上下で動物を選んでEnter。育成画面では左右で
FEED（ごはん）・PLAY（遊ぶ）・SLEEP（寝る／起こす）・NAME（名前）・PETS（変更）を選んでEnter。
NAMEは左右で桁、上下で文字を変え、Enterで保存します。名前編集は英数字・空白の8文字まで。
共通テキスト入力APIが未実装のため、日本語命名は今後の対応です。
Escで保存してホームへ戻ります。

12体それぞれの名前・満腹度・楽しさ・元気・睡眠状態を保存します。
育成が進むのは表示中の1体だけ。終了中や別のペットを表示中は停止し、死亡・ロストはありません。
ごはんは満腹度+20、遊びは楽しさ+20／元気-8。元気が10未満なら休憩が必要です。
睡眠中は毎秒0.8ずつ元気が回復します。状態は各操作、60秒ごと、Esc終了時に保存します。
電源断時には最後の保存以降の最大60秒分を失う可能性があります。
保存失敗は画面に表示し、読み込み失敗時には既存データを上書きしません。

## 実装

`pet.js`が育成ロジックとUIを所有します。共通の方向ボタン（PSPビット配置）と
`pocket.storage`を利用します。ホストは`local.pet`名前空間を割り当てます。
Esc時にはキャンセル前に0x2000の終了フレームを1度配送します。
描画時刻はフレーム数ではなく単調時計を使用し、入力でフレーム数が増えても育成速度を変えません。

2026-09-17、`pocket.kasane`（Kasane）へ移植しました。旧`ui.createNode`/`setProp`/
`insertBefore`/`replaceText`と旧`pocket.pet.place`/`say`/`show`オーバーレイは呼びません。
`view.createScene({build,patch})`を1個所有し、`build`が固定トポロジ（タイトル・バッジ・
パネル・地面・種名・状態・バー3本・フッタ・ヒント）を1回作ります。ペット画像と吹き出しは
ロード成功後の`scene.invalidate(true)`による再`build`でトポロジへ加わります
（未ロード中は旧UIと同様に何も出ません）。以降は`patch`が値だけ更新します。

ペット画像は`view.petImage()`で借りるopaque handleと`tx.image({resource,bounds})`、
色・表情の更新は`ref.setImageFrame(tx,variant,frame)`です。variantが0–11の配色番号、
frameが表情0–5で、旧`pocket.pet.place`の第1引数・第4引数とそれぞれ対応します。
吹き出しは旧`pocket.pet.say`のネイティブオーバーレイに依存できないため
（Kasaneが最初のsubmitで旧RGB565 rendererを解放し、以後そのオーバーレイは描画されない）、
`tx.rect`の吹き出し本体1枚と`tx.text`＋`setReveal`で文字送りをJS側から再実装しました。
枠と内側の二色塗りだった旧デザインは単色1枚に簡略化しています（`KSN-MISSING`参照）。
英数字UIは`font:'caption'`で日本語フォントアトラスの増加を避けます。

ブラウザプレビュー`preview.html`は旧`ui`/`pocket.pet.place/say`をモックしており、
この移植後の`pet.js`とは非互換です（`pocket.kasane`を呼ぶため即エラーになります）。
`pocket.kasane`のブラウザ側モックは未着手で、修正はこのコミットに含めていません。

### Kasane missing

移植時点でKasaneに無く、JS側で代替した／簡略化した点です。

- `KSN-MISSING(text.animate)`: 文字列のreveal（文字送り）を進める native track が無く、
  `patch`が呼ばれるたびにJSで経過時間から表示文字数を計算しています。旧`pocket.pet.say`は
  ネイティブ側が70ms刻みで進めていました。
- 吹き出しの二色塗り（外枠+内側ハイライト）は単色矩形1枚に簡略化しました。Kasane側の
  制約ではなく、ゲストソースのバイト数予算（移植元以下を維持）を優先した判断です。

旧`ui.createNode`で作っていたノード数上限（taffy段差）の節はKasane移植で意味を失った
ため削除しました。Kasaneのcommand/text予算は`docs/kasane/design-api.md`の
`KSN_APP_COMMANDS`(80)・`KSN_APP_TEXT_BYTES`(896)で、pet.jsの使用量はどちらも
1/3未満です（命令17、文字予約252 B）。

`assets/concepts.png`はこの会話で内蔵画像生成ツールを使って作成・承認された下絵です。
最終指示: 「3行4列。猫は茶トラ・三毛・黒猫・濃紺にシアン／マゼンタのサイバーパンク。
ウーパールーパーとオカメインコは既存の通常3色とサイバーパンクを維持。
姿・ポーズ・ドット絵の雰囲気を保つ」。
`tools/pack_pet_assets.py`で下絵を中間データ`pets.bin`に変換し、
`tools/pack_pet_compact.py`で`pets-compact.bin`と確認用`compact-preview.png`を生成します。
実機に埋め込むのは`pets-compact.bin`だけです。下絵と中間データは制作・再生成用です。

## 確認

2026-09-08のアニメーション修正: 定期まばたきは両目を閉じる表情1を使い、5秒ごとに200 ms閉じます。ウインク5は独立した表情として残します。選択中は表示しているペットの睡眠・空腹状態を参照し、育成中の一時的な反応はまばたきより優先します。上下の動きは1秒ごとに1ドット、表情更新は最大10 Hz、ステータス更新は2 Hz＋キー操作時です。ノードや画像データは増やしていません。

ネイティブ描画のソースは再編後の`main/pet/pet_pixels.c`、ヘッダー検索パスは`-Imain/pet`です。

`node tools/test_pet.cjs`は旧`ui`/`pocket.pet.place`APIをモックしており、Kasane移植後の
`pet.js`とは非互換で失敗します（2026-09-17時点で未更新、要`pocket.kasane`モック書き直し）。
`node tools/test_companion.cjs`（companion、未移植）は引き続き旧APIのままです。
`tools/test_pet_pixels.c`はホストCコンパイラーで`main/pet/pet_pixels.c`とリンクし、
引数に`apps/pet/assets/pets-compact.bin`を指定すると全画素・表情・クリップ境界を確認できます。
`idf.py -B build_pet_compact build`で専用ディレクトリにビルドします。
実機での表示、ヒープ、書き込み後の再起動確認は別途必要です。
