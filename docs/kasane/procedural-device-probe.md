# 手続き型描画の実機診断

2026-09-27 に `vm/main` (`6430ad6`) を取り込んでから実機で測定した。`KASANE_PROC_DEVICE_PROBE=ON` の ESP-IDF 6.0.1 / ESP32-S3 image `build_api/cardputer_pocketjs.bin` は 2,051,456 B、SHA-256 は `e930a3098fb3786eaa417cd298f578e9a1a34dda00a7b2b4be7a9dfa6e7e400`。

## 実機結果

Cardputer ADV (ESP32-S3 rev 0.2、8 MB flash、PSRAM なし) の COM3 へアプリ領域のみを書き込み、ホーム画面で `|` を送った。60 フレーム完走して `KSN_PROC: PASS` と `END` を確認。2面の surface を確保できた。元のアプリ領域 3 MiB は事前に保存し、測定後に全域を復元して書き込みハッシュを検証した。復元後のホーム画面 capture でも 135 行すべてを取得した。

| 指標 | 結果 |
| --- | ---: |
| native wave 生成 | 95線分、678 VM step / frame |
| `prep_us` | 平均 641 µs、最大 719 µs |
| `stage_us` | 平均 89 µs |
| `compose_us` | 平均 677 µs |
| `lcd_us` | 平均 2,924 µs |
| `present_us` | 平均 3,601 µs、最大 5,313 µs |
| 内部 RAM | 起動時 free 173,104 B、2面確保後 109,516 B、解放後 173,104 B |
| 最大連続空き | 起動時 126,976 B、2面確保後 65,536 B、解放後 126,976 B |
| UI task stack high water mark | 23,708 B |

最後の SPI 転送前 capture は 240×135 の 135 行すべてを取得した。RGB565 SHA-256 は `230179e52f6561ae6c3292bdca0ab82d12e2fa39054cf8998bee7a38c1e6888f`。青帯は y=0..8 の 2,160 画素、赤い曲線は 368 画素、水色の点格子は 60 画素で、復元画像も確認した。capture は LCD GRAM の読戻しではないため、パネル側の最終表示を直接検証したものではない。フレーム間に 33 ms の待機があり、音声、JS、FLOWER は同時に動かしていない。

## 起動

通常のファームウェアには含めない。専用ビルドで CMake オプション `KASANE_PROC_DEVICE_PROBE=ON` を指定する。

```text
idf.py -B build_proc -DKASANE_PROC_DEVICE_PROBE=ON build
```

実機へ書き込んだ後、ホーム画面で USB シリアルから `|` を1文字送る。診断は UI owner task 上で起動し、完了後にホーム画面へ戻る。実行中はホームの通常更新を止める。通常ビルドでこの文字は診断を起動しない。

## 表示と記録

[`ksn_procedural_device_probe.c`](../../main/ui/kasane/ksn_procedural_device_probe.c) は Kasane core の SYSTEM 色帯、格子下地、手続き型の波形と点格子を表示する。波形は 96 sample の native 反復で95線分を生成し、基準高さを変えて60フレーム描く。2面目を確保できなければ波形1面で継続する。最後に全画面を再描画し、既存の `PIX` capture を出す。目視では上端の青い帯、背景格子、赤い動く曲線、2面時の水色の点格子を確認する。

`KSN_PROC START/ALLOC/FRAME/PASS/END` のログから次を読む。

- 内部 RAM の free、最大連続空き、起動以来の minimum free、UI task stack high water mark。領域は実行時に確保し、診断終了時に解放する。確保に失敗した場合は `ALLOC_FAIL` を出し、端末を再起動させない。
- `prep_us`: VM による命令実行と線分生成。`stage_us`: 候補フレームの検証・所有コピー。`compose_us`: Kasane の描画と周辺処理を含む `present_us - lcd_us`。`lcd_us`: `board_present_sync` の呼出しに費やした時間。`present_us`: 合計。
- 更新した8行帯の mask、転送バイト数、生成線分数、VM step。33 ms の待機とログ・capture は計測区間の外に置く。`compose_us` は純粋なラスタ時間ではなく、core の準備なども含む。

この診断は表示負荷と RAM 可否を見るためのもの。JS からの登録、透明、3D depth、動画 decode、FLOWER との同時表示は扱わない。PSRAM のない Cardputer ADV では、最大容量の2面のフレームスロットだけで 40,984 B 必要になる。`surfaces=1` なら2面目の内部 RAM 確保に失敗したことを示す。

## 追加した D3a コンパイラ診断

同じ `KASANE_PROC_DEVICE_PROBE=ON` と `|` で表示診断の終了後に `KSN_COMPILER` 診断を実行するコードを追加した。VM と登録済み scalar plan の step・状態・線分を照合し、Q14 点列の scalar と PIE を 5 係数×14 長さ×4 alias 形態の280ケースで比較する。末尾ガードと積和の相殺も含む。値が一致した場合だけ、8・16・40点のまとまった呼出し時間と内部 RAM・stack をログに出す。初回の時点では PIE の選択は診断内に限り、製品側の閾値は未設定だった。

ESP-IDF 6.0.1 の `KASANE_PROC_DEVICE_PROBE=ON` 全体ビルドは通過した。追加部分を含む `build_api/cardputer_pocketjs.bin` は 2,061,408 B、SHA-256 `b81af569615056ebc2fcf493a97b0d4af6388bec48a65eddfe975ab10233a5d9`、DIRAM 159,836 B。COM3 の app 領域 3 MiB を `.cache/kasane-proc-compiler-20260927/app-before.bin`（SHA-256 `303e6324489bb745c8224766005a8b0645e277e94d7fb5ff04c701606493de54`）へ保存し、診断 image の app 領域だけを書き込んで esptool のハッシュ検証を通した。しかし USB `|` は開始マーカーが出ず、PIE 診断は実行できなかった。その直後、別の試験プロセスが COM3 を占有し、解放後の app 先頭 4 KiB は診断 image と保存した元 image の両方と不一致だった。他の試験を上書きしないよう、こちらのバックアップは復元せず、以後の COM3 操作を止めた。現在の app 内容の管理は並行する試験側に移っている。PIE 命令の値一致、速度、task 切替時のレジスタ保持は未検証である。上記の実機数値と SHA は追加前の image の結果である。

その後、COM3 の現行 app 全域が保存済み元 image と一致することを `verify-flash` で確認し、単独の診断セッションとして再実行した。`KSN_COMPILER: POINT PASS cases=280 diagnostic_plan=PIE`、`KSN_COMPILER: PASS`、`KSN_PROC: ALL PASS` が出た。2048回の集計時間（scalar/PIE）は8点で6,392/2,342 µs、16点で12,478/2,834 µs、40点で30,706/4,301 µs。7点は scalar fallback、40点は PIE を選択し、出力は scalar と一致した。この値は点列 kernel の時間であり、JS・線分化・LCDを含まない。task 切替を挟む検証や音声との同時負荷は未実施である。

続いて JS megademo の型付き Q14 点列へ8点以上の PIE policy を接続し、ESP-IDF 6.0.1、`KASANE_PROC_DEVICE_PROBE=ON`、`KASANE_PROC_JS_DIAGNOSTIC=ON` の image（2,072,976 B、SHA-256 `7c0a1bdb25752baebfb5c19dcc82deec3da03f5a92f287e14d6c1aafca780789`）で3場面を表示した。各場面の連続2フレームの送信 RGB565 は host と全画素一致し、各表示フレームで PIE が1回選ばれた。元の app 領域3 MiBは保存値と再照合して復元し、`HOME_READY` を確認した。再実行用スクリプトと詳細は[ロードマップ](dynamic-rendering-roadmap.md)に記録する。先の COM3 競合と不一致は当時の失敗として上記に残す。
