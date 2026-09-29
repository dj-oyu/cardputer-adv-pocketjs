# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

M5Stack Cardputer ADV（ESP32-S3FN8、PSRAMなし、240×135 LCD）向けのESP-IDFファームウェア。QuickJS版PocketJSでJSアプリを1つずつ実行する。設計ドキュメントは日本語、**コード内のコメントは英語**で、密度と「なぜ」を説明する文体に揃える。

**ドキュメントの入口は `docs/README.md`。** 主線は VM の高速化（`docs/vm/`、開発は `vm/main`）、PIE と描画の高速化（`docs/perf/`）、デザインシステム Kasane（`docs/kasane/`、開発は `feature/kasane`）の3本で、どちらのブランチも `main` へマージする。ほかは `api/` `platform/` `scenes/` `apps/` `archive/` に分けてある。新しい文書は該当ディレクトリに置いて索引に1行足す。**ファイル名は変えない** — 本文とCのコメントが名前で参照している。JSソース（`apps/**/*.js`、`tools/vmtest/**/*.js`）のコメントは、ゲストのヒープに効くので2026-09-15の移動前の古いパスのまま残してある。

## ビルドと書き込み

環境はEIM管理のESP-IDF v6.0.1。PlatformIOのIDF/Pythonを混ぜない。

```powershell
. 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1'
idf.py -B build_api build
idf.py -B build_api -p COM3 flash
idf.py -B build_api -p COM3 monitor
```

**ビルドディレクトリは作業ごとに分ける。** 複数セッションが1つのツリーを共有するため、`build/` を同時に使うと `ninja: failed recompaction: Permission denied` になる。`build_*` は `.gitignore` 済み。`sdkconfig` も追跡外で、`sdkconfig.defaults` の変更は**全ビルドディレクトリの次回ビルドに効く**。

`idf.py -B <dir> size` / `size-files` / `size-components` がサイズ計測の入口。`tools/check_flash.py` がSKK辞書・フォント領域の侵食をビルド時に止める。

初回セットアップ: `python tools/prepare_dependencies.py`（BMI270・libopus・minimp3を固定revisionで取得）。Rust UIアーカイブとPocketJS上流のcheckoutは不要（旧UI経路は削除済み）。

## 実機テスト

コマンド一覧は [`docs/platform/test-commands.md`](docs/platform/test-commands.md)。すべてUSBシリアル経由で、ESP-IDFのPython環境（pyserial）で走らせる。実機側は `smoke_device.py` / `test_settings.py` / `capture_home.py` / `test_editor_draft.py` / `benchmark_app.py` / `stress_app.py` / `test_app_resume.py`。

`test_settings.py` と `capture_home.py` は**押下回数を数えて**メニューを移動する。設定やアプリの行を増減させたら、この2つを同じ変更の中で直す。ログの大文字マーカー（`HOME_READY` / `CATEGORY %u` / `APP %u` / `SELECT %u` / `OPEN %u choice=%u` / `CHOICE %u` / `VALUE ...` / `LOADED ...` / `MODE %u %s` / `PERF ...` / `SFX %d played`）はこれらのスクリプトの契約なので、バイト単位で保つ。

ホスト側のテスト（実機不要）と `memlog.py` の呼び出しも同じ文書にある。

**DRAMは `tools/memlog.py` が記録する。** ビルドのたびに静的値を `.cache/memlog/memory.jsonl`（git管理外）へ追記し、**動いたときだけ**書くので、ログはビルドの一覧ではなく変化の一覧になる。`--port` を付けると実機の空きヒープ（アイドル時とアプリ実行中）も一緒に残る。増減はファイル別に出るので「DRAMが6KiB増えた」ではなく「`pocket_io.c.obj +1113`」が読める。

`main/` のPIEカーネルを触ったら、焼く前にこの3層を通す。詳細は `tools/pie/README.md` と `docs/perf/pie-simd.md`。

**ホスト側の検査は `main/` のパスを直書きするので、ファイルを動かすと黙って壊れる。** `main/` へディレクトリを切った `e770950` は2種類を壊した — `tools/test_solar_sail.c` の `#include "../main/solar_sail.c"`、および `tools/pie/test_kernels.py` と `tools/pie/models/accel_host_test.c` が指す `main/shell.c` / `main/render_accel.c` / `main/solar_sail.c`。前者はビルド不能、後者は5件すべてが `FileNotFoundError`。つまり**「焼く前に3層を通す」は再編以降ずっと実行できておらず、その間に焼いたものは検査されていない**。誰も走らせていない検査は、失敗しないという意味で通っているように見える。壊れていたのは他に `tools/test_solar_time.c` と `tools/pie/profile_solar.c` と `tools/pie/models/accel_host_test.c`。**`main/` の中でファイルを動かしたら、`grep -rn "main/" tools/` で参照元を洗ってから動かす。**

**`test_solar_time.c` はMinGWでは通らない。** Windowsの `struct timeval.tv_sec` は4バイトで、2038年の検査が書き込む `2147483648` が負に化ける。ESP-IDFの `time_t` は64bitなので実機は無関係。WSLで走らせること。ホストの型がファームの型と違う場所は、テストが嘘をつく。

## アーキテクチャ

**描画タスクは1つ、JSアプリは同時に1つ。** `main/main.c` の `ui_task` が全画面を回す。画面は `SCREENS[]` の記述子テーブル（`open` / `key` / `dirty` / `draw` / `wants_run` / `ended` / `frame_ms` / `takes_text`）で、ループ側が取り込み・再描画判定・フレーム配分を一度だけ行う。画面を足すときは分岐ではなく行を足す。

`main/app_session.c` がJSセッションのすべてを所有する。`app_start_test()` がゲスト生成 → `pocketjs_guest_quickjs_install_once()` で各ネイティブ面を注入 → ソース評価、の順に組み立て、`app_stop()` が逆順に壊す。`app_tick()` が毎フレーム `pocket_*_pump()` を呼んでからゲストの `frame()` を回し、描画は `pocket.kasane`（`main/ui/kasane/`）が提出したものを `present_frame()` が転送する。旧 `ui.*`（Rust UIコア・Taffy・rgb565レンダラ）はファームから削除済みで、それを呼ぶ保存済みプログラムは起動前に `APP_LEGACY_UI` で断る。**ゲストのコールバックを保持するモジュールは、ゲストが死ぬ前に `app_stop()` から reset される必要がある。**

`main/hal/board.c` がLCD・キーボード・I2Cバスを所有し、`board_present()` が唯一の転送口。描画用のストリップバッファは firmware 全体で1本（`board_strip()`）。転送は非同期で、`board_present()` がバイトスワップしながら転送用バッファ2本（`tx_buf`、1本が送信中のあいだにもう1本へ書く）へ写して queue し、前のストリップの結果を次の呼び出しで回収する。コマンドを送る前には必ず回収する（RAMWRセッションを終わらせるため）。

`main/` は役割ごとに分かれている。`hal/`（LCD・キーボード・IMU・音）、`pocket/`（`pocket.*` API と Wi-Fi）、`pet/`、`ui/`（画面とエディタ）、`text/`（フォント・字句解析・SKK・ソース保存）、`scene/`（背景と描画カーネル）、直下は `main.c` と `app_session.c` のみ。**全サブディレクトリが `INCLUDE_DIRS` に入っているので、`#include "board.h"` のような書き方は変わらない** — 移動でソースを1行も書き換えずに済ませるための構成。

**共通JS API `pocket.*`** は `docs/api/common-api.md` の実装。`main/pocket/pocket_api.c` が土台（capability登録、`PocketError`、cancelトークン、購読テーブル、Promise完了テーブル、遅延名前空間、`pocket_api_pump()`）で、`pocket_imu.c` / `pocket_av.c` / `pocket_storage.c` などが各面を載せる。**名前空間はアプリが最初に読んだときに構築される**（`pocket_api_lazy()`）。`capabilities` と `apiVersion` だけが eager で、feature-test が何も構築しないことを構造的に保証している。**新しい面は `pocket_api_register()` で capability を差し替えるだけで、`pocket_api.c` を編集しない。** 購読簿記・`settled()`/`reject()`・非同期完了は土台側にあるので、面の側で書き直さない。

`capability.supported` は「このファームが `pocket.*` の面を実装している」の意味。実装の無い面を true にしない（feature-testを通したアプリが `UNSUPPORTED` ではなく `TypeError` を食う）。`ui.basic` は実装が無いので false。`limits` に出す値は**コードで実際に強制している値だけ**。

`main/scene/solar_time.c` が天体計算の時刻源で、`solar_time_set_synchronized(true)` はSNTP成功時にのみ呼ばれる。`false` はどこからも呼ばない（一度合った時計は同期失敗後も正しい）。タイムゾーン変換はこの層に足さない。

JSアプリは `apps/<name>/<name>.js` に置き、`main/CMakeLists.txt` の `EMBED_TXTFILES` で埋め込み、`main/ui/shell.c` の `apps[]`／`app_details[]` に行を足し、`main/main.c` の `shell_app()` switch で起動する。**埋め込みシンボルはファイル名から作られるので、ファイル名を重複させない**（`main.js` は既にhelloが使用）。

## この機体で繰り返し踏む制約

- **PSRAMなし、DRAMは約334KiB。** ホーム画面の空きヒープは実測274KiB（`idle_free=280,932`、Wi-Fiリンク後）。静的DIRAMを197,847→111,383Bまで削った結果で、**この数字は削減のたびに動くので `tools/memlog.py --port --check` の実測を見ること。** JSゲストの上限は160KiB（`main/app_session.c` の `heap_limit`。128→144→160KiB と上げてきた） — 128KiBだった頃、ネイティブAPIの `.bss` が増えてアプリが解析すら通らなくなった（システムには59KiBの空きがあった）。**capabilityを足すたびにゲストの部屋が減る。** 増減の記録は `tools/memlog.py` が持つ。
- **ゲストは起動時にJSソースを解析するので、ソースのバイト数がヒープを食う。** 実測で6.5KBのアプリはゲスト107KiB、7.6KBは評価に失敗する。アプリのコメントは短く、理由は隣の `README.md` へ。
- **Rust UIコアとTaffyレイアウトはファームから削除した（2026-09-17）。** 以前ここにあった「Rust UIコアはメモリ不足を報告せずパニックする」「taffyノード数で連続ブロックが段階的に跳ねる」という制約は、その経路と一緒に無くなった。
- **`main/hal/keymap.c` は素の `` ` `` `;` `,` `.` `/` に `nav` を立てる。** テキストを受ける画面は `k->text` だけを読み `k->nav` を無視する（`codeedit.c` / `editor.c` / `wifi_ui.c` がそうしている）。
- **命令キャッシュのアラインメントで、同じカーネルがビルド間で15%動く。** それ未満の差を主張するなら同一バイナリでの比較が要る。
- **`board_capture` は byte swap と転送の前にバッファを写し、MISOは未配線。** 表示が正しいことをソフトウェアだけでは確認できない。物理確認を依頼する。
- **Kasane のシーンには上限がある: ref 32個、アプリのコマンド 80個**（cache の instance も、中の矩形の数だけコマンドを使う）。`patch` ではノードを足せない（追加は `replace` のときだけ）。`setRect` は clip を動かさないので、動かす ref には行全体の clip を渡す。LCD CATCH と DERBY WATCH は、ここで設計を変えた。
- **ソースの評価には2秒の期限があり、評価中の `pocket.memory.info().internalFreeBytes` は `null`**（ネイティブ heap の標本はターンの始めにしか採られない）。評価中に、空き heap の門で待つループを書かない（DERBY WATCH はこれで起動に失敗した。host の台本が固定値を返していたので、host では見つからなかった。host の台本は、実機で `null` になる値を、`null` で返す）。
- **評価のピークは、評価後の定常の約2倍。** QuickJS は、関数の解析用の構造を、一番外側のスクリプトが確定するまで、まとめて保つ（`js_create_function`）。クロージャ1つで約 0.3〜0.5 KB のピークを使い、コメントは効かない。DERBY WATCH の評価の余裕は、実機で 3.8〜5.6 KB。**文字列のイテレータ（`[...'abc']`、`for...of`、分割代入、`Array.from(str)`）は、修正前の実機では壊れていた**（上流 quickjs-ng の `js_string_iterator_next` が `int *` 経由で `uint32_t` を書き、Xtensa の GCC が strict aliasing で書き込みを消した。host では `uint32_t` が `unsigned int` なので再現しない。修正 `f937388`、docs/vm/spread-eval-oom.md。同種の箇所が quickjs.c にあと6つ残る）。
- **ゲームのキーは E/A/S/D と `;` `,` `.` `/` の8個。** キーボード行列のゴーストで、`f` `space` `enter` `z` は、他のキーの同時押しで押されたことになる（実機測定。docs/platform/keystate.md）。
- **Wi-Fiをリンクするだけで空きヒープが約37KiB減る。** 内訳は `.bss` だけでなく `.data` とIRAM常駐コード（S3ではDRAMと同じプール）。`esp_netif_deinit()` はIDF v6.0.1で `ESP_ERR_NOT_SUPPORTED` なので、一度無線を起動すると約4.8KiBは戻らない。自動時刻同期（設定の AUTO TIME SYNC、既定 ON）が入ったので、同期が走ったブートは毎回これを払う（docs/platform/wifi-autostart.md。ゲームのネイティブ heap への影響は未測定）。

## 並行作業（サブエージェントと実機）

- サブエージェントは、`vm/main` から切った worktree（`.claude/worktrees/<名前>`、ブランチ `vm/<題目>`）で動かす。統合は `git merge --no-ff`、終わったら worktree とブランチを消す。新しい worktree では、最初に `python tools/prepare_dependencies.py`。WSL の git は worktree の gitdir を読めないことがあり、`tools/kasane_contract/run.sh` の baseline の読み込みで止まる（スクリプトの注記どおり、baseline を `.cache` に置く）。
- 実機（COM3）を複数のエージェントが使うときは、`mkdir` で取るロックのディレクトリ（作業用の一時領域に置く）で順番を取り、1回の保持は10分まで（30分より古いロックは置き去り）。使い終わったら通常 image に戻し、変えた設定（音量など）を元へ戻す。
- **実機が要る検証を、host だけで済んだことにしない。** 実機だけで起きた不具合が、すでにある（評価中の `null`、文字列のスプレッド、面のフレームの連続領域）。

## 測定と主張

数値は**実測か推定かを必ず区別する。** このプロジェクトでは、3体のエージェントが独立に同じ結論に達して全員間違っていた例（PIEの索引ロード）、推定1.2msが実測25.6msだった例（効果音の合成）、`--gc-sections` で削除済みのモジュールを測っていた例（Wi-Fiのサイズ）がある。測ったものが本当にバイナリに入っているかを `nm` / map で確かめる。

## モデルの使い分け（ルーティング）

モデルは速さと深さで3つに象徴する。切り替えは `/model`、サブエージェントは `Agent` の `model` で指定する。**振り分け役は Sonnet が担う**ので、Sonnet はこの節を読んで、迷ったら上へ回す。

| ラベル | モデル | 得意 | 任せない |
| --- | --- | --- | --- |
| **FAST** | Sonnet 5.5 | 現状認識、症状から該当コードの絞り込み、事実の収集（file:line）、ビルドスクリプト・ブランチ管理・ホスト試験の実行と要約のような定型 | 解決策の決定、数値の決定、優先順位 |
| **STEADY** | Opus 5.5（既定） | 実装・デバッグ・実機計測など普段のコーディング全般。大きなコンテキストで文書とCを一つの文脈に載せられる | 高コストな全面レビュー |
| **SLOW** | Fable | 熟考が要る設計・仕様書・コードのレビューとブラッシュアップ。複雑な課題に対応するコードも書けるが高コストなので、普段の実装には使わない | 普段の実装 |

**このプロジェクトでは、アタリ付けの速さと解決策の確かさは別物。** 癖のあるハードウェア（PSRAMなし、命令キャッシュ、PIE、Wi-Fiが食うDRAM）に高級なことをやらせるので、不具合のアタリは速く付けられても、その先の解決策は**かなり細い「正解の道」**を引かないと通らない。FAST が速く絞り込んだ結果は、そのまま修正の根拠にしない。

**振り分けの規則**
- 症状 → 疑わしい箇所の絞り込みは **FAST**。返すのは「範囲（file:line）、理由、確認した事実と推測の区別、そのコードが実際にビルドに入り呼ばれる経路かの確認の有無」まで。解決策は書かせない。
- 修正の設計・実装、数値（上限・予算・閾値）の決定、優先順位、測定値の解釈は **STEADY 以上**。特にPIE・命令キャッシュ・DRAM/IRAM・タイミングに触れる判断は必ず STEADY。
- 設計や仕様書の見直し、STEADY が出した方針の独立レビューは **SLOW**。実装は STEADY が済ませ、その差分と文書を SLOW が読む。
- 迷ったら上へ回す。FAST が不確かなまま結論を出さない。

**根拠（2026-09-29、既知の課題を伏せた検証、各1回）:** 上限の洗い出しでは FAST は STEADY の分析とほぼ一致し、見落とした穴も1件拾った。改善方針を出させると、行番号は正確でも、ビルド条件（プローブ専用ファイルを本番経路と誤認）とテストの検査対象を読み違え、その推測を優先順位1位の根拠にした（STEADY による判定で5観点が3/3/3/3/4）。1件ずつの結果で、一般化しない。

複数セッションが1つの作業ツリーを共有するため、コミット時は `git show HEAD:<file>` に自分の変更だけを当てた blob を `git hash-object -w` + `git update-index --cacheinfo` で staging し、他セッションの未コミット変更を巻き込まない。
