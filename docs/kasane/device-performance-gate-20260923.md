# Kasane 汎用 mount 実機ゲート（2026-09-23）

Cardputer ESP32-S3 / COM3、115200 baud。旧版は
`build_presenter/cardputer_pocketjs.bin`（SHA-256 `485836e0272e21e3ed65aefd105d53ef5a2501484746d57a7dfffacf25774e55`）、
新・整理後は `build_app_mount/cardputer_pocketjs.bin`
（SHA-256 `cb01dffee46d6c4e3f329870583b9a3db838ff8a6bc67a499c292defb1edbf64`）。
両方ともアプリ領域 `0x10000` のみ flash し、同じ実機で旧→新→旧→新の順に確認した。
SDは書き換えず、pet は Back で保存せずリセットした。HOME OVERLAYのNVS設定は
計測のため一時的に切り替え、元の設定2へ復元した。

| 測定 | 旧版 | 新・整理後 | 判定 |
|---|---:|---:|---|
| hello 30描画窓×6、JS turn平均 | 0.17 ms | 0.05 ms | 改善 |
| hello 同、render平均 | 0.92 ms | 0.89 ms | 劣化なし |
| hello 同、LCD send平均 | 0.68 ms | 0.70 ms | 0.02 ms差。帯3・768 Bは同一。単独で改善/劣化と判定しない |
| deskclock overlay、背景draw安定窓 | 8.28 ms | 8.27–8.28 ms | 同等 |
| music overlay、背景draw安定窓 | 8.89–8.90 ms | 8.87–8.88 ms | 同等 |
| deskclock overlay、起動直後free | 139,888 B | 139,492 B | 396 B減。ただしlargestは86,016→94,208 B |
| music overlay、起動直後free | 129,664 B | 129,500 B | 164 B減。largestは双方86,016 B |

hello の両画面は同一 PNG。music help も同一 PNG。IMU・pet・clock・背景を含む
画面はセンサー値・時刻・アニメーション位相が変わるため、異なる PNG hash を
回帰とはみなさず目視確認した。旧→新の起動直後freeは hello +1,728 B、
imucal +2,720 B、companion +1,516 B、pet +3,300 B。4アプリとも
`KASANE_FRAME_PRESENTED` を確認し、panic/開始失敗はなかった。

この結果に基づき、旧 presenter のうち mount 済み6アプリ用の未使用 kind と
描画分岐を削除した。音楽は現在も旧 presenter の reactive playback/status/help
を使用するため、その互換経路は維持した。固定容量を実利用上限に縮め、
music overlay の旧版に対する追加確保量を整理前の584 Bから164 Bへ減らした。
ESP-IDF build、QuickJS統合テスト（0 failures）、Kasane契約テスト
（ASan/UBSanとO2、presenter/schemaを含む）はPASS。
契約テスト中、`ksn_render.c` の既存のsigned left shift UBSan警告は残る。
後続の2026-09-24変更で負方向gradientの該当shiftと参照テストを修正し、
専用tileテストおよびKasane契約テストをASan/UBSan診断0件で再実行した。
この段落の2026-09-23時点の性能値は再測定していない。

未完了のゲート：music を汎用 descriptor/native source へ移した同一条件比較、
foreground 各アプリの更新p95/p99/max・12 ms超過、長時間再生中のunderrun。
平均値と短い起動試験だけで旧音楽 presenter全体や `createScene` escape hatch
を削除しない。

## music 専用モジュールへの分離

Kasane の `mount` はアプリ名に依存しない `pocket_app_view_provider` を参照する。
登録表は `app_view_provider.c`、music の状態・JS API・reactive 更新は
`app_music_view.c`、音楽固有の描画計画は `app_legacy_presenter.c` に隔離した。
Kasane 本体は music の識別子や描画仕様を持たない。旧描画アルゴリズムは
性能比較のため維持しており、汎用 descriptor への移行完了を意味しない。

分離後の `build_app_mount` を同じ Cardputer のアプリ領域だけに flash。
分離後バイナリの SHA-256 は
`be474aba13c07135bb1a85dc25efcd0193a22574519fad02726462175b8cb806`。
分離直前の整理後版と同じ `overlay_device_test.py` で比較した。
deskclock の背景 draw は 8.27–8.28→8.30–8.31 ms、music は
8.87–8.88→8.89–8.91 ms。music 起動直後 free は 129,500→129,496 B、
overlay 追加確保は 100,728→100,684 B。music help PNG は SHA-256 一致。
再試行でも music draw は 8.89 ms、起動直後 free は 129,452 B で、
短期計測では有意な描画性能劣化は見られない。最悪ターンは
分離直前 5,872 us、分離後 6,068 us / 6,296 us であり、長期・分布評価は残る。
`OVERLAY_HEALTHY` は起動時一回のログなので、music の観測窓で
`healthy=0` となるのは分離前後共通。`OVERLAY_STOPPED` / panic は
観測中に発生しなかった。
通常アプリ代表の hello は30描画窓×6で JS turn平均 0.06 ms、render平均
0.89 ms、LCD send平均 0.70 ms（分離直前 0.05/0.89/0.70 ms）。

QuickJS 統合テストは 0 failures、ESP-IDF build と Kasane 契約テスト
（ASan/UBSan、O2）は PASS。音楽再生を伴う
実機確認は、新たなフォルダ許可が必要だったためスキップした。
HOME OVERLAY の設定は元の2へ復元し、COM3を閉じた。

生ログと画面は `.cache/kasane-legacy-device-20260923/`、
`.cache/kasane-mount-device-20260923/`、
`.cache/kasane-legacy-overlay-device-20260923/`、
`.cache/kasane-overlay-trimmed-20260923/`、
`.cache/kasane-overlay-provider-20260923/`、
`.cache/kasane-overlay-provider-playback-20260923/` に保存（git管理外）。
