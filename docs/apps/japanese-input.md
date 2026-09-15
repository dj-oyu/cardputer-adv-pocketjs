# 日本語入力: SKK移植の設計と現状

対象: ネイティブSKK変換（かな→漢字）と、それをJSアプリへつなぐ経路。
確定済み実装（commit `2b053b7` 以降）: skk_core/ime_core移植、SKK Practice、Playgroundへのネイティブ日本語入力、JSノードへの日本語表示、および汎用の`pocket.input.text`（[common-api.md](../api/common-api.md) §6、`main/pocket/pocket_text.c`）。
本書のメモリ・辞書サイズ表は調査時・実装時の測定であり、`tools/memlog.py`が記録する最新実測を優先する。

## 目的と役割分担

Editor、SKK Practice、Playgroundは同じ変換エンジン（skk_core/ime_core）を共有する。
目標は、同梱アプリもユーザーJSも変換エンジンを持たず確定UTF-8を受け取ることで、これは2系統で達成されている。

| 経路 | 用途 | 実装 |
| --- | --- | --- |
| ネイティブ編集画面 | Editor / SKK Practice / Playgroundの自前バッファ | `main/ui/editor.c`, `main/ui/codeedit.c`が確定結果を直接編集バッファへ挿入 |
| `pocket.input.text` | 任意のJSアプリの入力欄（Wi-Fiパスワード欄を含む） | `main/pocket/pocket_text.c`（JS面）+ `main/text/textfield.c`（編集ロジック、ホストテスト可能） |

どちらも同じSKKセッション（`skk_session.c`）とキー配列を使うが、バッファの所有者と描画経路は別。
ネイティブ編集画面は専用の全画面レイアウトを持ち、`pocket.input.text`は呼び出したアプリの画面に1つのフィールドを合成する（`pocket_text_overlay()`）。

## 再利用元と範囲

参照: `dj-oyu/esp32p4-mqjs`、調査commit `a901d0e685dbc4cf48c505dabeaeb46532cc8679`。

| 再利用候補 | 役割 | 扱い |
| --- | --- | --- |
| skk_core/skk_kana.c | ローマ字変換、送り仮名、候補選択 | 純Cの状態機械として移植済み |
| skk_core/skk_dict.c | 辞書イメージ検証・検索 | Flash直接参照の構造を維持 |
| skk_core/include/skk_core.h | 固定容量とデータ寿命 | 契約を維持する |
| ime_core | IMEの有効化、入力の消費・通過・確定 | アプリ共通の入力ポリシーとして使用 |
| tools/skk_prep.py | 辞書の事前生成 | PC側で実行。デバイスでは索引を生成しない |
| tools/tests/test_skk_*、test_ime_core.c | 変換と入力順序の回帰テスト | 移植時に引き継いで実行 |

MicroQuickJSバインディング、Tab5のLVGL描画、Tab5固有キーマップ、P4のpartition設定は流用していない。
既存skk_coreとime_coreはI/O、LVGL、ESP-IDF、動的確保を含まない。
`#include`は標準ヘッダのみで、ファイルスコープのstaticはすべてconstであり可変グローバルを持たない。
差し替えたのは辞書のありかを決める`skk_builtin.c`と`skk_dict_image.S`で、ADVでは`skk_dict` partitionをmmapしたblobを渡す。
`kbd_core`の`esp_timer_get_time()`一箇所は現在時刻を引数で受け取る形へ変え、純Cのまま保った。

## メモリ

S3向けGCC 14.2.0（esp-14.2.0_20241119）で既存ヘッダーのsizeofを確認した（実測、2026-09-06）。

| 構造 | byte |
| --- | ---: |
| skk_t | 1,272 |
| ime_t（skk_tを内包） | 1,296 |
| skk_dict_t | 240 |
| 1セッション＋共有辞書ハンドル | 1,536 |

これは全体のRAM消費ではない。スタック、辞書マッピング管理、表示キャッシュ、入力キュー、QuickJS文字列は別に測る。
固定容量は読み96byte、候補64件、preedit192byte、commit256byte。UTF-8の文字数ではなくバイト数。
候補は辞書内のoffset/lengthで保持し、64件をまとめてJS文字列へ展開しない。
表示中の候補と確定文字列だけを必要時にコピーする。

## 辞書

`skk_prep.py`でSKK-JISYOから生成したイメージのサイズを実測した（2026-09-06、`skk_dict` partitionは`0x310000`に2MiB）。

| 辞書 | 見出し | image | 2MiB枠の残り |
| --- | ---: | ---: | ---: |
| S | 3,379 | 116,991 | — |
| M | 8,346 | 303,297 | 1.79MB |
| ML | 48,750 | 1,949,758 | 147,394 |
| L | — | 8.07MB | 収まらない |

MLは2MiBに収まる。まずMで経路を通し、確定後にMLへ差し替える方針だった。`--max-size 0x200000`を必ず付け、超過時はビルドを失敗させる。
生成ツール、入力辞書revision、生成オプション、生成サイズ、payload CRC32を記録する。
`alphabet_crc32`はS/M/MLで同一の3840893823であり、辞書を替えてもイメージは無効にならない。

Flash内の専用領域をmmapし、headerの長さと境界を検証して必要範囲のみ参照する。
移植元のヘッダはこの経路を想定しており、`image_len`が`blob.len`より短くてよいと定めている。
mmap窓がページ丸めされる前提が既に入っているため、partition先頭をそのまま渡せる。
Flash辞書は使用中に差し替えない。初回にCRC検証し、ヘッダの長さとCRC値をNVSへ記録する。同じ組なら次回の本文検証を省略するため、**ヘッダが変わらない本文破損の検出は保証しない**（`main/text/skk_session.c`）。これは既知の制約で、再検証を強制する操作は無い。
SDは現実装のbase pointer参照に直接接続できない。全辞書をRAMへ読む回避策は採らず、必要なら別途ページ読み込み方式を設計する。
Tab5の8.5MB領域は8MB FlashのADVに移植しない。

## フォント

PocketJSのフォント口はDCFAアトラス一つで、字形は1ピクセル1byteのアルファ、
cmapのコードポイントはu32のためCJKも形式上は載る。
ただし`load_font_atlas`はRustコア側でVecへ全コピーするため、載せた分だけSRAMを消費する。
JIS第1水準を12pxで丸ごと積むと490KB必要で、アプリ終了後の空き・最大連続空きの実測値（`tools/memlog.py --port --check`）では成立しない。

そこでフォントの消費者を二つに分ける。

| 消費者 | 経路 | 字形の置き場 |
| --- | --- | --- |
| ネイティブのIMEオーバーレイ、Shell、テキスト入力欄 | strip合成時に自前で1bpp→RGB565展開 | Flashをmmapして直接参照 |
| JSノード内の日本語表示 | ui.setTextを包み、実行中に出現した文字を累積してDCFAを再構築 | SRAM、上限160字。未使用文字の追い出しはない |

編集画面は独立したネイティブ描画で、`pocket.input.text`のオーバーレイ（`pocket_text_overlay()`）はrender_stripとboard_presentの間で合成し、閉じた領域の再描画を管理する。

新しい文字を含む`ui.setText`呼び出しごとにアトラスを再ロードする。layoutとrasterの更新による再描画負荷がある。
1グリフあたりcoverage 144byteとcmap 8byteで152byte。
`Atlas::parse`は新しいVecを確保してから旧アトラスを捨てるため、
再ロードの瞬間はホスト側blob・新アトラス・旧アトラスが同時に存在し、**定常の最大3倍**になる。
128字なら定常19.5KB・ピーク58KB、256字なら定常38.9KB・ピーク117KB、512字はピークが最大連続空きを超え得る（実測値は`memlog.py --port --check`を都度確認する）。
現在のホスト側blobは再構築ごとに動的確保・解放する。文字集合をソート保持し、cmapと字形を再構築する。**reload失敗時の再試行と上限到達時の表示は未実装**（[backlog](backlog.md)）。

字形は東雲12pxを採用した。実機で14pxと並べて確認済み。

| 候補 | セル | JIS X 0208全6,879字 | ライセンス |
| --- | --- | ---: | --- |
| 東雲12 | 12×12 | 161KB | Public Domain |
| 東雲14 | 14×14 | 188KB | Public Domain |
| 東雲16 | 16×16 | 215KB | Public Domain |
| 美咲ゴシック | 8×8 | 54KB | 独自フリー |
| k8x12 | 8×12 | 81KB | 独自フリー |

東雲はPublic Domainで通知義務がなく、元がビットマップのため1bpp設計と字形が一致する。
Noto Sans JPはアンチエイリアス前提で1bpp化すると字形が崩れるため採らなかった。
M辞書の候補漢字3,031字、MLの4,587字はいずれもJIS X 0208に収まり、東雲なら欠字は出ない。
240×135では12pxで全角20桁×11行が幾何上の上限。本文に東雲12px、狭いコンソール領域に美咲8pxを使用する。UIの余白やヘッダー分は表示可能行数から差し引く。

`jp_font` partitionは512KiB（`0x510000`、`0x80000`）。東雲12pxを領域先頭、美咲8pxを相対0x40000へ配置する。12/14/16pxを全部積む構成ではない。各256KiB窓への収まりはヘッダ・cmapを含む生成イメージのサイズで確認する。

Flash側の形式は辞書と同じ流儀に揃える。固定長ヘッダ、コードポイント昇順のcmap、固定長1bppセル、CRC。
12pxなら1字24byteで、S3のFlashキャッシュが64byteライン単位で吸収するためSRAMキャッシュを別に持たない。
欠字はDCFAの規約に合わせgid 0の字形を描き、ネイティブ側も同じ字形を使う。

移植元のLVGLフォントはDCFAとも自前1bpp形式とも別物であり、そのまま持ち込んでいない。

## 入力経路

TCA8418キーイベント（押下/解放と修飾キーを保持）
  → 専用ForceStopの検出（`main/main.c`の入力タスク、モード分岐より前で処理。IME/JSの状態に依存しない）
  → フォーカス先を確定してIME所有タスクへ渡す
  → `ime_feed()`
      IME_TAKEN: preedit/候補表示を更新、アプリへキーを渡さない
      IME_TEXT: 確定UTF-8を所有バッファへコピー、TextCommitを送る
      IME_PASS: エディタ/JSの通常キー、未処理のBackをShellへ渡す

変換中のEscは取消、Enterは確定、候補操作はIMEを優先する。変換確定Enterを改行・送信・アプリ終了へ再利用しない。
通常Backと専用ForceStopは異なる論理操作。IMEが消費しなかったEscはBackとして扱う。

board_key_event()が押下・解放を返し、keymap.cが修飾状態を保持して文字・ナビゲーションへ変換する（公式デモと同じ4×14の対応表）。

| 操作 | 割当 | 理由 |
| --- | --- | --- |
| 英数/かな切替 | Ctrl+J、補助でopt+Space | SKK標準。ADVは物理ctrlを持つ |
| 変換取消 | EscとC-g | Escは変換中のみIMEが消費する |
| Back | IMEが消費しなかったEsc | Fn+`` ` `` |
| ForceStop | Ctrl+Alt+Del | 三キー同時で誤爆せず、取消と重ならない |

移植元はCtrl+JがEnterと同じバイトへ潰れるためSKK標準の切替を諦めているが、ADVはキー変換を自前で書くので潰す前に識別できる。修飾状態を落としてからIME切替を判断しない。
編集時のEscはFn+`` ` ``へ移した。ホームでは従来の修飾なし`` ` ``も戻るとして扱う。
IME切替操作は押下エッジのみ、文字と候補送りには意図したrepeatを適用する。

## セッション・スレッド・フォーカス

アクティブな入力欄に1つのime_tを割り当て、1タスクのみが操作する。辞書は読み取り専用で共有できる。
QuickJS contextはアプリタスクのみが触る。制御タスクからJSコールバックを直接呼ばない。
ime_text/preedit/candの借用ポインタをタスク間キューへ入れない。次のime_feed等で無効になるため、その場で長さ付きコピーを作る。

`pocket.input.text`のセッションは`textfield.c`の`generation`カウンタでフォーカス世代を持ち、`tf_refocus()`が古い世代のcommitを捨てる。ネイティブ編集画面（editor.c/codeedit.c）は画面ごとに専用バッファを持つため世代は不要。
フォーカス喪失・アプリ終了では未確定文字を破棄し、勝手に確定しない。確定済みの編集内容は保持する。

キュー満杯時のoverflow表示・キー保持状態の再同期は未実装。`main/main.c`の`xQueueSend(keys,&k,0)`は失敗を無視するため、入力欠落が検知されない（[backlog](backlog.md)）。
辞書未搭載・検証失敗時は英数入力を維持し、IME利用不可を表示する。全体を起動失敗にしない。

## 表示とJS API

`pocket.input.text`は[common-api.md](../api/common-api.md) §6の実装で、以下を提供する（`main/pocket/pocket_text.c`）。

- 入力欄がフォーカス取得時に`pocket.input.text.open()`でセッションを開始し、終了時に閉じる。
- アプリはonEdit/onSubmit/onCancelでUTF-8確定文字列を受け取り、キーストローク・preedit・候補は受け取らない。
- preeditや候補選択はプラットフォーム（ネイティブIMEオーバーレイ）が表示し、アプリの文字列にはまだ入れない。
- 編集はUTF-8境界で行う。全角文字を1byte/1pixelとして扱わない。

現行実装は変換学習・ユーザー辞書を持たない。辞書の候補順を維持する。
注釈表示、concat形式、一部の送り仮名別候補グループなど既存の制限は、移植後も対応済みと宣言しない。

## 実装順序の履歴と検証

実装は失敗時の手戻りが大きい順に進めた: (1) 辞書partitionの実機検証（mmap、`skk_dict_open` VERIFY、検索速度）、(2) skk_core/ime_coreのホストテスト（WSL側gcc）、(3) キーボードの全マトリクス化、(4) 東雲BDFから1bpp形式生成、(5) IMEオーバーレイのネイティブ描画、(6) 確定UTF-8のJS配送、(7) 計測。全段階が完了している。

以下は着手前に潰したリスクと最小検証で、いずれも実施済み。

| リスク | 最小検証 |
| --- | --- |
| 動的アトラスがRAMを圧迫する | 12×12×256字のダミーDCFAを足し、空きと最大連続の変化を測る |
| 辞書とフォントの二領域同時mmap | 命令キャッシュとの競合で打鍵遅延が出ないか測る |
| アトラス再ロードの全面再描画 | 毎フレーム再ロードする最悪ケースでフレーム時間を測る |
| 修飾キーの同時押し復元 | 全イベントをrow/col/stateでログし実機で確認する |
| 12pxの可読性 | 12と14を並べる |

## ライセンスと移植手順

移植元リポジトリはGPL-3.0-or-laterを宣言しているが、その理由はwolfSSL/wolfSSHの同梱であり、
skk_core・ime_core・kbd_coreの全コミットは本プロジェクトと同一著者による。
各ファイルにSPDX表記・著作権表示はなく、外部からの移植・派生を示す記述もない。
ローマ字表も出典注記のない自前記述で、移植元のTHIRD_PARTY_NOTICESのVendored表にも載っていない。
著作権者自身の判断としてMITで再提供し、取り込む各ファイルの先頭にSPDX-License-Identifierと著作権表示を付ける。

辞書はコードと別の配布物として扱う。SKK-JISYOはGPL-2.0-or-laterであり、
配布イメージへ`skk_dict`を含めると配布物全体にGPLの義務がかかる。
コードと辞書を別ファイル・別手順で配布し、辞書は利用者がskk-dev/dictから生成する構成とする。
ライセンス、出典、元データ、生成手順を記録する。

東雲フォントはPublic Domainで通知義務がないが、出典と取得元をlicensesへ記録する。

## 参照

- [再利用元](https://github.com/dj-oyu/esp32p4-mqjs)
- [ime_coreの契約](https://github.com/dj-oyu/esp32p4-mqjs/blob/a901d0e685dbc4cf48c505dabeaeb46532cc8679/components/ime_core/include/ime_core.h)
- [skk_coreの契約](https://github.com/dj-oyu/esp32p4-mqjs/blob/a901d0e685dbc4cf48c505dabeaeb46532cc8679/components/skk_core/include/skk_core.h)
- [再利用元のライセンス記録](https://github.com/dj-oyu/esp32p4-mqjs/blob/a901d0e685dbc4cf48c505dabeaeb46532cc8679/THIRD_PARTY_NOTICES.md)
- [`pocket.input.text`の共通API契約](../api/common-api.md) §6
