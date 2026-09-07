# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

M5Stack Cardputer ADV（ESP32-S3FN8、PSRAMなし、240×135 LCD）向けのESP-IDFファームウェア。QuickJS版PocketJSでJSアプリを1つずつ実行する。設計ドキュメントは日本語、**コード内のコメントは英語**で、密度と「なぜ」を説明する文体に揃える。

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

初回セットアップ: `python tools/prepare_dependencies.py`（PocketJS上流を固定revisionで取得）、`tools/build_native.sh`（WSLでS3用Rustアーカイブをビルド）。

## 実機テスト

すべてUSBシリアル経由。ESP-IDFのPython環境（pyserial）で走らせる。

```powershell
python tools\smoke_device.py --port COM3 --cycles 20   # 起動/停止のライフサイクルとリーク
python tools\test_settings.py --port COM3              # XMB設定・ミュート順序・画面遷移
python tools\capture_home.py --port COM3               # 実ピクセル取得と30fps確認
python tools\benchmark_app.py --port COM3              # JSアプリのPAINT内訳
```

`test_settings.py` と `capture_home.py` は**押下回数を数えて**メニューを移動する。設定やアプリの行を増減させたら、この2つを同じ変更の中で直す。ログの大文字マーカー（`HOME_READY` / `CATEGORY %u` / `APP %u` / `SELECT %u` / `OPEN %u choice=%u` / `CHOICE %u` / `VALUE ...` / `LOADED ...` / `MODE %u %s` / `PERF ...` / `SFX %d played`）はこれらのスクリプトの契約なので、バイト単位で保つ。

ホスト側のテスト（実機不要）:

```bash
python tools/test_flash_budget.py       # パーティション予約ガード
python tools/pie/stalls.py              # PIEインラインasmの静的パイプライン解析
python tools/pie/test_kernels.py        # PIEカーネルを命令レベルで模擬実行しスカラーと全画素比較
python tools/pie/run_models.py          # カーネルが使う式の全域ビット一致証明
wsl -e bash -lc "cd tools/uibudget && cargo run --release --bin sweep"    # レイアウト確保の段差
wsl -e bash -lc "cd tools/uibudget && cargo run --release --bin screen 9 3"  # この画面は載るか
python tools/memlog.py --map build_api/cardputer_pocketjs.map            # DRAMの増減とファイル別内訳
python tools/memlog.py --map build_api/cardputer_pocketjs.map --port COM3 --check   # 実機の空きも記録し予算を検査
```

**DRAMは `tools/memlog.py` が記録する。** ビルドのたびに静的値を `.cache/memlog/memory.jsonl`（git管理外）へ追記し、**動いたときだけ**書くので、ログはビルドの一覧ではなく変化の一覧になる。`--port` を付けると実機の空きヒープ（アイドル時とアプリ実行中）も一緒に残る。増減はファイル別に出るので「DRAMが6KiB増えた」ではなく「`pocket_io.c.obj +1113`」が読める。

`main/` のPIEカーネルを触ったら、焼く前にこの3層を通す。詳細は `tools/pie/README.md` と `docs/pie-simd.md`。

## アーキテクチャ

**描画タスクは1つ、JSアプリは同時に1つ。** `main/main.c` の `ui_task` が全画面を回す。画面は `SCREENS[]` の記述子テーブル（`open` / `key` / `dirty` / `draw` / `wants_run` / `ended` / `frame_ms` / `takes_text`）で、ループ側が取り込み・再描画判定・フレーム配分を一度だけ行う。画面を足すときは分岐ではなく行を足す。

`main/app_session.c` がJSセッションのすべてを所有する。`app_start_test()` がゲスト生成 → `pocketjs_guest_quickjs_install_once()` で各ネイティブ面を注入 → UIコア・バインディング・レンダラ生成、の順に組み立て、`app_stop()` が逆順に壊す。`app_tick()` が毎フレーム `pocket_*_pump()` を呼んでからゲストの `frame()` を回す。**ゲストのコールバックを保持するモジュールは、ゲストが死ぬ前に `app_stop()` から reset される必要がある。**

`main/board.c` がLCD・キーボード・I2Cバスを所有し、`board_present()` が唯一の転送口。ストリップバッファは firmware 全体で1本（`board_strip()`）で、同期転送だから成立している。非同期DMA化するならここを2本に割る必要がある。

**共通JS API `pocket.*`** は `docs/common-api.md` の実装。`main/pocket_api.c` が土台（capability登録、`PocketError`、cancelトークン、購読テーブル、Promise完了テーブル、`pocket_api_pump()`）で、`pocket_imu.c` / `pocket_av.c` / `pocket_storage.c` が各面を載せる。**新しい面は `pocket_api_register()` で capability を差し替えるだけで、`pocket_api.c` を編集しない。** 購読簿記・`settled()`/`reject()`・非同期完了は土台側にあるので、面の側で書き直さない。

`capability.supported` は「このファームが `pocket.*` の面を実装している」の意味。レガシーの `ui.createNode` があることを理由に true にしない（feature-testを通したアプリが `UNSUPPORTED` ではなく `TypeError` を食う）。`limits` に出す値は**コードで実際に強制している値だけ**。

`main/solar_time.c` が天体計算の時刻源で、`solar_time_set_synchronized(true)` はSNTP成功時にのみ呼ばれる。`false` はどこからも呼ばない（一度合った時計は同期失敗後も正しい）。タイムゾーン変換はこの層に足さない。

JSアプリは `apps/<name>/<name>.js` に置き、`main/CMakeLists.txt` の `EMBED_TXTFILES` で埋め込み、`main/shell.c` の `apps[]`／`app_details[]` に行を足し、`main/main.c` の `shell_app()` switch で起動する。**埋め込みシンボルはファイル名から作られるので、ファイル名を重複させない**（`main.js` は既にhelloが使用）。

## この機体で繰り返し踏む制約

- **PSRAMなし、DRAMは約334KiB。** ホーム画面の空きヒープは実測228KiB（Wi-Fiリンク後）。JSゲストの上限は144KiB — 128KiBだった頃、ネイティブAPIの `.bss` が増えてアプリが解析すら通らなくなった（システムには59KiBの空きがあった）。**capabilityを足すたびにゲストの部屋が減る。** 増減の記録は `tools/memlog.py` が持つ。
- **ゲストは起動時にJSソースを解析するので、ソースのバイト数がヒープを食う。** 実測で6.5KBのアプリはゲスト107KiB、7.6KBは評価に失敗する。アプリのコメントは短く、理由は隣の `README.md` へ。
- **Rust UIコアはメモリ不足を報告せずパニックして再起動する。** `ui.setText` が引き起こすフォントアトラス再構築が小さなアプリの最大の単発確保。
- **レイアウトが要求する単一連続ブロックは taffy ノード数で段階的に跳ねる。** taffyノード = ルート + 「木に繋がっていて本文が空でない」ノード（空のテキストランは `layout.rs` の `build()` が `None` を返し木に入らない）。**16以下 → 2,048B / 17〜33 → 29,648B / 34以上 → 59,296B。** アプリ実行中の最大連続空きは実測23.5KiB程度なので、**その段差を跨ぐ画面は確保に失敗し、Rust側がabortする**。`ui.createNode` で組んでも同じで、34ノード以上はこの機体では到達不能。`pocket_ui.c` は空ランを区別せず多めに数えるので、崖の手前で断る側に倒れている。`tools/uibudget/` で焼く前に確認できる（`cargo run --release --bin screen 9 3` が実機の失敗をそのまま再現する）。
- **`main/keymap.c` は素の `` ` `` `;` `,` `.` `/` に `nav` を立てる。** テキストを受ける画面は `k->text` だけを読み `k->nav` を無視する（`codeedit.c` / `editor.c` / `wifi_ui.c` がそうしている）。
- **命令キャッシュのアラインメントで、同じカーネルがビルド間で15%動く。** それ未満の差を主張するなら同一バイナリでの比較が要る。
- **`board_capture` は byte swap と転送の前にバッファを写し、MISOは未配線。** 表示が正しいことをソフトウェアだけでは確認できない。物理確認を依頼する。
- **Wi-Fiをリンクするだけで空きヒープが約37KiB減る。** 内訳は `.bss` だけでなく `.data` とIRAM常駐コード（S3ではDRAMと同じプール）。`esp_netif_deinit()` はIDF v6.0.1で `ESP_ERR_NOT_SUPPORTED` なので、一度無線を起動すると約4.8KiBは戻らない。

## 測定と主張

数値は**実測か推定かを必ず区別する。** このプロジェクトでは、3体のエージェントが独立に同じ結論に達して全員間違っていた例（PIEの索引ロード）、推定1.2msが実測25.6msだった例（効果音の合成）、`--gc-sections` で削除済みのモジュールを測っていた例（Wi-Fiのサイズ）がある。測ったものが本当にバイナリに入っているかを `nm` / map で確かめる。

複数セッションが1つの作業ツリーを共有するため、コミット時は `git show HEAD:<file>` に自分の変更だけを当てた blob を `git hash-object -w` + `git update-index --cacheinfo` で staging し、他セッションの未コミット変更を巻き込まない。
