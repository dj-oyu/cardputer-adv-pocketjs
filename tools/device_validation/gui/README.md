# Cardputer local validation GUI

Windows 11 用の **ユーザー自身が起動する**ローカル GUI です。統合版の保持済み
build run と復旧用 app を別々に選び、実行プランを確認してから既存の
`Flash-Integrated.ps1` / `Test-Integrated.ps1` を実行します。
ファームウェアの再ビルドは不要です。GUI の Python 環境は IDF の環境とは別です。

## 起動

1. 既存の **ESP-IDF v6.0.1 PowerShell プロファイル**を有効にします。
   `IDF_PATH` と `IDF_PYTHON_ENV_PATH` が必要です。後者の
   `Scripts/python.exe` を明示的に使います。GUI の Python へフォールバックしません。
2. 新しい tooling checkout の `tools/device_validation/gui` に移動します。
   保持済み build run の `wt-integrated` にツールをコピー・追加しないでください。
3. インストール済みの [uv](https://docs.astral.sh/uv/getting-started/installation/) を使い起動します。
   Python 3.11–3.13 対応です。初回は lockfile に固定された依存をこの GUI の `.venv` に用意します。

```powershell
cd C:\work\pocketjs\tools\device_validation\gui
uv run --frozen --no-dev python launch.py --project C:\work\pocketjs

# IDF_PATH だけを明示する場合（IDF の Python 環境は有効化済みのものを利用）
uv run --frozen --no-dev python launch.py --project C:\work\pocketjs `
  --idf-path C:\tools\esp-idf-v6.0.1
```

`--project` は必須、`--idf-path` は省略可能です。空白を含むパスは引用符で囲みます。
`--idf-path` だけで EIM の環境を作成・変更したり Python を推測したりはしません。
環境が不足している場合は GUI に原因が表示され、実機操作は無効になります。
`UV_PROJECT_ENVIRONMENT` を別の環境へ設定している場合は、この GUI の `.venv`
を指すようにこの起動時だけ指定してください。IDF の仮想環境を指定しないでください。

既定ブラウザーが開きます。自動で開かない場合はターミナルの URL を使用してください。
`--no-browser` で自動起動を抑止できます。ターミナルを開いたまま使用してください。
ページの再読み込み後は、ターミナルの URL を開き直します。

## 操作の流れ

1. 起動時に project と `git worktree list` に登録された worktree を自動探索します。
   「候補を探す」で再探索できます。各ルートの `.cache/flash_backup` も対象です。
   その他の `.cache` は探索しません。
   パス・サイズ・更新日時を見て復旧用 app を自分で選びます。最新のファイルを
   自動採用しません。bootloader、partition table、フルフラッシュ、サイズ超過、
   ESP32-S3 app descriptor のない `.bin` は選択できません。ファイル名に
   `backup` が含まれるだけでは除外せず、サイズと image/app descriptor で判定します。
   一覧にない app は「復旧用 .bin を直接追加」に絶対パスを貼り付けて追加できます。
   ファイルのアップロード・自動選択はありません。追加だけでは実機に触れません。
   直接追加した候補は再探索でも保持して再検査し、消失・形式変更時は選択不可として表示します。
2. 統合ビルドの run を選びます。一覧にない外部の保持済み run は
   `manifest.json` のあるフォルダーの絶対パスを貼り付けて追加できます。
   元の絶対パスのまま保持してください。ファイルをブラウザーへアップロードしません。
3. COM を列挙して選びます。列挙だけではポートを開いたり reset したりしません。
4. 操作を選び、「実行プランを確認」を押します。この段階も実機を開きません。
   既存の厳密な build manifest / 六つの artifact / H・F OFF 証跡を再検証し、
   IDF Python の esptool image-info でチップ情報を確認します。
5. 正確な source SHA、app/recovery SHA-256、COM の識別情報、書込領域と確認事項を
   読んで実行します。プランは 10 分限り・一度きりです。入力の変更、再探索後の
   ファイル変更、COM の交換、期限切れでは再確認が必要です。
6. 書込終了後は自分で通常再起動してホーム画面を待ち、Test のプランを作成します。
   Test は同じ run / app / COM の成功した書込記録を要求します。
   HELLO は 1–20 cycle、必要なら通常の GRID LAB を追加できます。
   Test のために復旧用 app を再選択する必要はありません。

復旧用ファイルの既定ラベルは **未検証の候補**です。チップ・サイズ・hash が通っても
正常起動の証明にはならず、検証済み rollback の保証はありません。「確認済み」は
同じ正確なバイナリで正常起動を確認したユーザーの自己申告です。
復旧用 app と書込対象が同一パスまたは同じ bytes の場合は選択をやり直します。
復旧用 app の自動書込・自動復旧はありません。

## 操作範囲と安全上の限界

- factory app の `0x10000`、最大 `0x300000` bytes のみ。現在の app は置き換わります
- 既存 bootloader / partition table の互換性はユーザーの確認が必要です。
  自動読出し、repair、erase-all、reset、reboot は行いません
- 診断、OOM、fault injection、stress、任意コマンド、任意 serial API はありません
- 単一 broker の worker と、既存の run lock / `%TEMP%` の COM lock を使います。
  GUI の無効ボタンに頼らず server でも同時実行・再送を拒否します
- GUI 承認時の image / manifest / recovery hash を既存スクリプトへ渡し、lock 下でも照合します。
  COM 情報も書込・verify・test の直前に、lock 下で列挙して照合します
- COM 情報は暗号学的な機体識別ではありません。列挙から open までの race を完全には
  消せず、外部アプリの利用を OS レベルで禁止しません。他の monitor を閉じ、
  正しい機体を自分で管理してください
- 実行中の flash/test を強制中断するボタンはありません。ブラウザーを閉じても処理は
  継続します。機体・ケーブル・ターミナルに触れず、完了を待ってください。
  通常の server 停止も child の終了を待ちます。OS による強制終了は防げません
- serial marker 成功は実機の LCD / 物理キー / audio の確認や完全な VM conformance を代替しません

詳細は [INTEGRATED-DEVICE.md](../INTEGRATED-DEVICE.md) を参照してください。

## ローカル限定・ログ・探索

`127.0.0.1` の OS が選んだ空きポートにだけ bind します。外部公開、トンネル、
autostart、永続 credential、MCP server は作りません。起動ごとの一時 token を URL
fragment で渡し、画面はすぐ fragment を削除します。token は JavaScript メモリーのみで、
Cookie / localStorage / sessionStorage には保存しません。Host、Origin、JSON、CSRF、
同一 origin、token を server 側で検証し、CSP で外部 script / frame を許可しません。
起動 URL は他人へ共有しないでください。同じ OS ユーザーの悪意あるプロセスに対する
隔離境界ではありません。

探索は最大 64 worktree、30,000 entry、深さ 10、300 `.bin`、30 秒で打ち切ります。
symlink / junction / UNC はたどらず、全ドライブ探索はしません。
探索は background job で中止できます。打切り・欠落は画面で通知されます。
明示的に追加した run / app はその一つを読むだけで、親フォルダーを探索しません。
直接追加は session あたり最大 64 app、探索結果と合わせて最大 300 candidate です。
元の app が追加後に変わっても、プラン作成・実行時の再検証と hash pin は省略されません。

GUI の session log は OS ユーザーの一時フォルダーに保存され、場所は起動時に表示されます。
1 job 約 1 MiB / 300 表示行、最大 40 job を保持します。終了後も調査用に残ります。
token は log に含めません。既存スクリプトの artifact / serial log / result.json は元の
run 配下に従来どおり保存されます。これらの詳細 log は GUI の session 上限とは別です。
個人のパス、実機 log、token、実 firmware はリポジトリへ commit しないでください。

## 開発・オフライン検証

```powershell
# GUI のディレクトリで。実機・esptool・PowerShell subprocess はすべて mock
uv run --frozen python -m unittest -v test_gui
# 簡易 DOM harness（Node が既存の場合。追加 npm 依存なし）
node test_ui.cjs

# 既存の全オフラインテスト。Windows PowerShell 自身の parse/native fixture を含む
..\Test-Offline.ps1
```

`core.py` は HTTP に依存しない探索・plan・job broker、`server.py` は loopback HTTP
adapter です。将来別 UI/MCP adapter を設ける場合も、同じ broker と downstream lock
を使い、別の device owner や確認バイパスを作らない設計です。
