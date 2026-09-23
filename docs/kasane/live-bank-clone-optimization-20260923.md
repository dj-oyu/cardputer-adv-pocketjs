# Kasane bank複製の使用済み領域化（2026-09-23）

## 変更

`ksn_core` の `PATCH` は従来、command 3,072 Bとtext 1,024 Bの
bank全体を毎回複製していた。`REPLACE`も保持側layerの容量全体を複製していた。
候補bankへは、確定bankの各layerで `count` 個のcommandと `text_used` Bの
textだけを複製する。`add`は新しいcommandを全フィールド代入し、text確保部を
初期化するため、未使用末尾は公開read・描画の対象外。animationは停止済みIDも
照会可能なため、track blockの全複製を維持する。API、bank容量、heap確保は不変。

## 同条件の実機比較

Cardputer ADV、COM3、`KASANE_P0_PROBE=ON`。前回の診断ログを変更前、
今回のSHA-256 `12ebb9e3e07ec155680e726c5564c7d7581439d9407d47038681a8a373ebb46c`
を変更後とする。診断器自体が約5.3 KiBのDIRAMを使うため出荷ビルドの
絶対値ではない。音声再生なし。

| 指標 | 変更前 | 変更後 |
| --- | ---: | ---: |
| hello bank command+text clone、180描画 | 733,824 B | 46,361 B（93.7%減） |
| hello render p95 / p99 | 1,031 / 1,142 us | 1,050 / 1,194 us |
| hello render 30描画平均×6窓 | 0.88 ms | 0.88 ms |
| deskclock overlay draw p95 / p99 | 8,424 / 8,449 us | 8,383 / 8,406 us |
| music overlay draw p95 / p99 | 9,046 / 9,089 us | 8,984 / 9,034 us |

変更後のhello cloneはcommand 28,640 B / 179回、text 17,721 B / 179回。
初回REPLACEで保持側が空なのでcloneがない。両overlayのこの測定では
clone counterが0だった。各測定区間の12 ms超過は0。helloのrender p95/p99は
微増なので「全面的に描画が速くなった」とは判定しない。平均は同じで、
overlayの値は改善方向だが、短期試行の測定揺らぎは残る。

music help画面のPNG SHA-256は変更前後で一致。
deskclockとmusic通常画面のPNGは時刻等の可変内容を含むため、hash一致を
判定条件にできない。host契約テストではPATCH/REPLACE、両layer、文字、
画像、animation、IO失敗→repairの画素・状態比較が通過した。
特に未使用bank末尾をpoisonしても両layerの使用済み範囲が保持される回帰テストを追加。
QuickJS/Kasane統合テストは失敗0件。

併せて画像回転anchorのpacked keyをunsigned演算へ変更し、既知の
signed-left-shift UBSan警告を解消。該当テスト11,833構成・23,665 panel hashを
`UBSAN_OPTIONS=halt_on_error=1`で通過した。ただし全contract suiteをこの
修正後に即停止モードで再実行したわけではない。

実機のHOME OVERLAY設定は元の2へ戻し、測定前アプリ領域3 MiBを復元。
`esptool --no-stub verify-flash`で両分割のdigest一致、COM3は閉じた。
新規フォルダ許可を伴う再生測定と長時間音声共存は未実施。

生ログ：`.cache/kasane-p0-20260923/hello-diagnostic-final.log`、
`.cache/kasane-p0-20260923/hello-live-clone.log`、
`.cache/kasane-p0-20260923/overlay-diagnostic/serial.log`、
`.cache/kasane-p0-20260923/overlay-live-clone/serial.log`。
最適化診断バイナリ：`.cache/kasane-p0-20260923/diagnostic/optimized-live-clone.bin`。
