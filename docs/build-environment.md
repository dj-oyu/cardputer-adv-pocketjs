# Windows / EIM 開発環境とビルド手順

確認日: 2026-09-06。このPCはEIMでESP-IDFを管理する。PlatformIOのIDF・Pythonを本プロジェクトのビルドに混在させない。
環境の起動と各ツールのバージョン、ファームウェアのビルド・実機書き込みを確認済み。機能の基準はcommit `2b053b7`。チュートリアル統合は進行中で、作業ツリーのビルド結果は統合後に記録する。

## このPCで確認した構成

| 項目 | 値 |
| --- | --- |
| EIM | 0.19.0、`C:\Program Files\eim\eim.exe` |
| 登録済み・選択中IDF | v6.0.1（EIM一覧は1件、状態ok） |
| IDF配置 | `C:\esp\v6.0.1\esp-idf` |
| IDF Git識別 | `v6.0.1`（describeでdirty表示なし） |
| EIM登録情報 | `C:\Espressif\tools\eim_idf.json` |
| activation script | `C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1` |
| IDF tools | `C:\Espressif\tools` |
| IDF専用Python | `C:\Espressif\tools\python\v6.0.1\venv\Scripts\python.exe`、3.11.7 |
| S3 GCC | 15.2.0、`esp-15.2.0_20251204` |
| CMake / Ninja | 4.0.3 / 1.12.1 |

PocketJS調査版のIDF要求 `>=6.0,<6.2` にv6.0.1は含まれる。このバージョンを最初のビルド基準とする。
依存取得、S3 Rustアーカイブ生成、ドライバー統合はM1でビルド・書き込み済み。[当時の結果](firmware-m1.md)を参照する。
SKKの先行sizeof調査は別のGCC 14.2.0で実施しているため、取り込み時はこのEIM環境で再確認する。

## 方法A: 現在のPowerShellを有効化（確認済み）

```powershell
Set-Location C:\devs\m5stack\cardputer-adv-pocketjs
. 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1'
idf.py --version
python --version
xtensa-esp32s3-elf-gcc --version
cmake --version
ninja --version
```

先頭のドットと空白はdot-sourceで、現在のPowerShellに関数・環境を読み込む。
このスクリプトはIDF専用venvとPATHを設定し、`idf.py`を専用Pythonで実行するエイリアスも作る。
スクリプトにはEIMの選択状態をv6.0.1へ同期する処理もある。このPCは確認前から同版が選択されていた。
別バージョンへ移るときは新しいシェルを使い、対応するEIM環境を有効化する。

## 方法B: EIMでコマンド単位に指定（確認済み）

```powershell
eim list | Out-Host
eim run 'idf.py --version' v6.0.1 | Out-Host
eim run 'python --version' v6.0.1 | Out-Host
```

このPCのPowerShellでは`| Out-Host`を付けた形式で完了まで待機し、結果を取得できた。
コマンド全体を1つの文字列として渡し、その後にIDFバージョンを渡す。
EIM起動自体の終了だけでビルド成功と判定せず、子プロセスの結果と生成物も確認する。
自動化で終了コードの厳密な扱いが必要な場合は、方法Aで有効化しidf.pyを直接実行して`$LASTEXITCODE`を確認する。

対話作業には次の形式もある（CLI help確認済み、対話シェルの起動は未実施）。

```powershell
eim shell v6.0.1
```

`eim select v6.0.1`はIDE向けの選択状態を更新する操作で、既存PowerShellのPATHを切り替える代わりにはならない。

## 依存準備と標準ビルド

リポジトリルートにCMakeLists.txtとmainがある。初回はIDF環境を有効化して `python tools/prepare_dependencies.py` を実行し、WSLで本リポジトリへ移動して `bash tools/build_native.sh` を実行する。後者はespupで用意したpocketjs-espツールチェーンと `.cache/tools/export-esp.sh` を前提とする。既存のアーカイブを使う通常の増分ビルドでは毎回Rustを構築しない。

```powershell
Set-Location C:\devs\m5stack\cardputer-adv-pocketjs
. 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1'

# 最初の構成時。毎回は不要。
idf.py -B build set-target esp32s3
if ($LASTEXITCODE -ne 0) { throw 'Target configuration failed' }

idf.py -B build build
if ($LASTEXITCODE -ne 0) { throw 'Firmware build failed' }

idf.py -B build size
idf.py -B build size-components
```

`set-target`は既存設定やビルド状態の再作成を伴うため、増分ビルドでは繰り返さない。
設定変更には`idf.py -B build menuconfig`、CMake再構成には`idf.py -B build reconfigure`を使う。
再現に必要な値はsdkconfig.defaultsに反映し、依存lockfileをGitで管理する。生成sdkconfigとbuild/は除外する。

方法Bでのビルド形式:

```powershell
eim run 'idf.py -B build build' v6.0.1 | Out-Host
```

## 書き込みとログ

有効化したIDF環境でポートを調べ、対象ADVを確認してから使用する。

```powershell
python -m serial.tools.list_ports
# COM番号は実機の結果に置き換える。
$cardputerPort = 'COM_REPLACE_ME'
idf.py -B build -p $cardputerPort flash monitor
```

monitor終了はCtrl+]。接続できない場合は公式のG0操作でダウンロードモードに入れる。
flashは現在のfirmwareを置き換える。erase-flashは通常のビルド手順に含めない。

## 辞書・フォントの別アセット

通常のidf.py flashはアプリ・bootloader・partition tableを書き込む。辞書・日本語フォントは別に生成して書き込む。最新のpartition配置を確認してから実行する。

- SKK: 移植元のskk_prep.pyでイメージ生成。`--max-size 0x200000`を指定し、skk_dictの0x310000へ配置する。辞書の版と生成結果は[日本語入力設計](japanese-input.md)を参照。
- 東雲12px: `python tools/make_jpfont.py --font shinonome --bdf-dir <BDFのディレクトリ> -o jpfont12.bin --check`。jp_fontの先頭0x510000へ配置する。
- 美咲8px: `python tools/make_jpfont.py --font misaki --bdf <misaki_gothic.bdf> -o jpfont8.bin --check`。jp_font内の相対0x40000、絶対0x550000へ配置する。

上記のパスは生成時に指定する。各フォントが割り当てた256KiB窓に収まること、辞書が2MiBに収まることを実ファイルで確認する。アセットの出典・ライセンスは生成ツールとlicensesを参照する。現行生成物を新たに測定していないため、過去のサイズを現在のファイルサイズと見なさない。

現行storageは0x590000から2496KiB。旧配置の0x710000からのデータ自動移行は実装されていないため、旧版からの更新では先に保存内容のバックアップと移行方針を決める。

## 混在を避けるための確認

有効化前、このPCのpythonは`C:\Users\core\.platformio\penv\Scripts\python.exe`を指していた。
IDF起動後は上表のEIM専用venvへ変わることを確認した。

```powershell
Get-Command python,idf.py,xtensa-esp32s3-elf-gcc
$env:IDF_PATH
$env:IDF_PYTHON_ENV_PATH
```

IDF_PATHだけを書き換えてビルドしない。Python依存、compiler、CMake、Ninjaも同じEIM環境を使う。
調査時、制限された実行環境ではEIMのログ初期化が失敗し、通常の実行環境では成功した。
同じエラーの場合はログ書き込み権限と実行環境を確認し、IDFを再インストールする前に切り分ける。

## 検証記録の位置付け

- 実行済み: EIM一覧、設定参照、dot-source、IDF/Python/GCC/CMake/Ninjaのバージョン、EIM runによるIDF/Python起動。
- M1以降に実施済み: firmwareビルド、依存解決、Rustアーカイブ構築、flash、実機ログ。[M1記録](firmware-m1.md)と[XMB記録](xmb-research.md)を参照。
- 現在の日本語入力・Playground構成、および進行中チュートリアルの性能は、各統合commitに対して別途記録する。過去のRAM/FPSを転用しない。
- インストールや既存IDFの更新は行っていない。

## 設定記号は名前どおりに効かない——測る前にビルドを指紋する

2026-09-08の1日で、設定記号が名前から期待される意味を持たなかった例が3件出た。3件とも**失敗せず、黙って何もしない**ので、測定値だけを見ても気づけない。

| 記号 | 期待 | 実際 |
| --- | --- | --- |
| `SDKCONFIG_DEFAULTS` | 指定したdefaultsが効く | ルートに`sdkconfig`が既にあると**丸ごと無視**される。`CMakeCache.txt`には正しい文字列が入ったまま |
| `CONFIG_FATFS_SECTOR_*` | FATFSのセクタ長 | `fatfsgen.py`の**イメージ生成器**の設定。実行時のバッファは1バイトも動かない |
| `-O2` の`set_source_files_properties` | 指定ファイルが`-O2` | 移動して存在しなくなったパスに付いていて、**警告なく無効** |

さらに、`MINIMAL_BUILD ON` のもとでコンポーネントが構成に入っていないと、その`CONFIG_`はKconfigごと読まれず**警告1行で捨てられる**。BLEの最初の測定が「+48バイト」になったのはこれで、`bt`がビルドに入っていなかった。`--gc-sections`も同じ形の罠で、リンクされていないモジュールのサイズを測ると0が返る。

**対策は測定値ではなくビルドを検査すること。** 順に:

1. **成果物に指紋を出す。** 比較する2つのビルドは、測定より前の行で見分けがつかなければならない。`FF_MAX_SS=… sizeof_FATFS=…` のように、**効くはずの値そのもの**をログの1行目に出す。同じ結果が出たとき「効果がない」のか「同じビルドを2回焼いた」のかを区別できるのはこれだけである。
2. **生成された`sdkconfig`を見る。** `CMakeCache.txt`ではなく`build_*/sdkconfig`（または`config/sdkconfig.json`）に記号が存在するか。無ければ何も起きていない。
3. **バイナリを確認する。** `nm`でシンボルが定義されているか、`md5sum`で2つのビルドが本当に違うか。同一チェックサムの比較は比較ではない。
4. **数字が動かないときは、まず計器と構成を疑う。** ヒープが確保をまたいで**増える**なら、それは確保の測定ではない。

この文書に置いたのは、3件とも「ビルドを見れば1分で分かり、数字を見ても何時間でも分からない」種類だからである。

## 参照

- ローカルの`eim --help`、`eim run --help`、`eim shell --help`、EIM登録情報とactivation script。
- [EIM公式CLIコマンド](https://docs.espressif.com/projects/idf-im-ui/en/latest/cli_commands.html)
- [プラットフォーム設計](architecture.md)
