# plan を命令数ぶんだけ確保する

2026-09-30、ブランチ `vm/plan-alloc`（`vm/main` b692f0c から）。[js-to-ir.md](js-to-ir.md) §1 の「native の plan は命令数によらず 872 B」を受けて、`pocket.kasane.procedural.register()` の plan をその命令数ぶんだけ確保するように変えた。API とアプリは変えていない。

## 結論

- **DERBY WATCH（今のアプリ）の 25 本の plan が、ネイティブの heap で 22,500 → 9,696 B になった**（実測、登録ごとの `PLANSZ` の合計、−12.8 KB）。レース中の空きの最小は 23,056 → 35,580〜36,208 B、ターン内の最小（`mn`）は 14,220 → 25,648〜25,964 B で、どちらも約 +11.5〜12.5 KB（実測）。
- **首振りカメラの段階 3（30 本、`vm/pan-camera` 55956e9 のアプリ）で、毎レース出ていた `GO LOADSTALL` が消えた。** 基準は 3 レースとも LOADSTALL、新しいファームは 2 回の起動の 6 レースすべて `GO`（実測）。plan が全部載った状態でのターン内の最小は 9,052〜9,068 B（基準は plan が欠けた状態で 7,140 B）。`vm/pan-camera` の docs/apps/derby-pan-memory.mdの受け入れ条件「ターン内の最小 ≥ 11,208 B」には、まだ約 2.2 KB 足りない。
- fps・JS のターン・帯描画・登録の時間は、測定の揺れの範囲で変わらない（実測、下の表）。
- 出力はすべての桁で同じ。新しい plan と固定長の plan は同じ関数（`mark_fused` と `run_core`）を通る。host の `tools/kasane_contract/run.sh`（`GAMES_M32=0`）は全部通過し、MEGADEMO・DERBY・BIG WAVE・LCD CATCH の台本も基準と一致した。

## 1. 設計

**固定長の `ksn_proc_plan` は残し、ヒープ用に `ksn_proc_sized_plan` を足した**（[ksn_proc_plan.h](../../main/ui/kasane/ksn_proc_plan.h)）。固定長の plan は、host の試験・device probe・`proc_megademo_preview.c` が、スタック・静的配列・構造体のコピーで使っている。そこで固定長の plan の末尾を可変長にすると、これらが全部壊れる。新しい型は、ヘッダの後ろに可変長配列 `code[]` を持つ。

| 項目 | 固定長 `ksn_proc_plan` | `ksn_proc_sized_plan` |
| --- | --- | --- |
| 命令 | `code[64]`（768 B） | `code[]`（12 B × n） |
| 融合の印 | `fused_at[64]`（64 B）→ **`uint32_t fused[2]`（8 B）に変更** | `uint32_t fused[2]` |
| 命令数 | `program`（ビュー 8 B） | `count`、`capacity`（各 1 B） |
| 点列の係数・方針 | 16 + 8 B | 同じ |
| 大きさ（Xtensa、コンパイルで確認） | 872 → **816 B** | **40 + 12n B** |

- `_Static_assert(sizeof(ksn_proc_sized_plan)==offsetof(ksn_proc_sized_plan,code))` で後ろに詰め物が無いことを固定し、`ksn_proc_sized_plan_bytes(n)` をちょうどの大きさにした。`KSN_PROC_CODE <= 64` も静的に検査する（融合の印が 64 ビットのため）。
- `ksn_proc_sized_plan_prepare(plan, capacity, program)` は `program->count <= capacity` を検査し、`capacity` の外には書かない。失敗の経路の `memset` も `capacity` ぶんだけ。
- 融合の判定（`mark_fused`）と実行（`run_core`）は、2 つの型で同じ関数を使う。挙動の差は、どこに置くか（配置）だけになる。
- `pocket_proc.c` は `calloc(1, ksn_proc_sized_plan_bytes(count))` で確保する（`count` は 1..64 を先に検査済み）。上限（1..64 命令、レジスタ 16、`REPEAT_REG` 0..255、32 本、step・線分・ラスタ）の検査は、どれも元の場所のまま。

## 2. plan 1 本の大きさ

要求は `40 + 12n` B（Xtensa）。heap のブロックは、実機では要求を TLSF の区分に切り上げた大きさになる（`heap_caps_get_allocated_size`）。実際に減る空き（`took`）は、それにヘッダの 4 B を足した値。`took` は、`calloc` の前後の `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` の差で、診断 image の `PLANSZ` ログから採った。

| 命令 n | 要求 | ブロック | 実際に減る空き | 固定長（前）との差 |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 52 | 52〜64（実測） | 56〜68（実測） | −832〜−844 |
| 8 | 136 | 136（推定） | 140（推定） | −760 |
| 16 | 232 | 232（推定） | 236（推定） | −664 |
| 17 | 244 | 248（実測） | 252（実測） | −648 |
| 32 | 424 | 432（推定） | 436（推定） | −464 |
| 40 | 520 | 544（実測） | 548（実測） | −352 |
| 56 | 712 | 736（実測） | 740（実測） | −160 |
| 60 | 760 | 768（実測） | 772（実測） | −128 |
| 64 | 808 | 832（推定） | 836（推定） | −64 |
| 前（固定長） | 872 | 896（実測） | **900（実測）** | — |

推定の行は、測った行と同じ規則（128〜255 B は 8 B、256〜511 B は 16 B、512〜1,023 B は 32 B 単位に切り上げ）で出した。実機で出た要求の大きさ（n = 1、17、20、22、26、29、30、35、37、38、40、49、54、56、60 と固定長の 872）は、すべてこの規則どおりになった。1 命令の plan のブロックが 52〜64 B でばらつくのは、少し大きい空きの塊を分けずにそのまま使ったため（残りが最小ブロックに満たない）。

## 3. 実機（COM3、診断 image `-DKASANE_MEGADEMO_TRACE=ON -DKASANE_BGCOST_TRACE=ON -DPOCKET_KEYTEST=ON`）

DERBY は `tools/games/bgcost/bgcost_device.py run --seconds 190〜200` で、放置からデモ 3 レース。基準は b692f0c に同じ `PLANSZ` の計測だけを足したビルド。

### DERBY WATCH（今のアプリ、25 本）

| | 基準（1 回） | 新（2 回） |
| --- | ---: | ---: |
| 25 本の合計（`took`） | 22,500 | **9,696** |
| レース: 空きの最小 / 中央値 | 23,056 / 23,936 | 35,580〜36,208 / 36,152〜36,764 |
| ゲート: 空きの最小 | 23,184 | 36,180〜36,568 |
| ターン内の最小 `mn`（全体） | 14,220（ゲート） | **25,648〜25,964**（ゲート） |
| 最大の連続ブロック `lg`（レース、最小） | 7,680 | **17,408** |
| 最大の連続ブロック `lg`（パドック、中央値） | 9,216 | 26,624 |
| ゲストの最大 | 128,688 | 128,580〜128,676 |
| レース fps / JS 中央値 / 帯 | 29.5 / 14.25 / 1.56 ms | 29.5 / 14.22〜14.30 / 1.58 ms |
| 写真判定 fps | 24.0 | 24.0〜24.1 |
| `GO` | 3/3 | 3/3、3/3 |

### 段階 3（首振り、30 本、`vm/pan-camera` 55956e9 の `apps/derby` を載せたビルド）

| | 基準（1 回） | 新（2 回） |
| --- | ---: | ---: |
| `GO LOADSTALL` | **3/3 レース** | **0/6** |
| ゲートの長さ（ターン数） | 903（門で待つ） | 123 |
| レース: 空きの最小 | 17,020 | 19,292〜19,328 |
| ゲート: 空きの最小（門 18,432 B） | 16,940 | 19,496〜19,628 |
| ターン内の最小 `mn` | 7,140（plan が欠けた状態） | **9,052〜9,068**（全部載った状態） |
| `lg`（最小） | 7,168 | 7,680 |
| ゲストの最大 | 143,048 | 142,288〜142,352 |
| レース fps / JS 中央値 | 28.8 / 16.06 ms | 29.0 / 15.56〜15.63 ms |

新しいファームでは、登録された 30 本（70 回の登録）の plan がすべて載る。門（18,432 B）に対する余裕は、ゲートの最小の時点で約 1.1 KB（19,496 B）しかない。

### MEGADEMO（170 秒、全場面を 6 周以上、DEGRADE なし）

| 場面 | fps 基準 → 新 | JS 中央値 | ターン内の最小 `mn` |
| --- | --- | --- | --- |
| NEWS | 29.4 → 29.4 | 4.62 → 5.04 ms | 13,696 → 13,816 |
| TWIST | 29.4 → 29.5 | 10.10 → 10.06 ms | 10,512 → 11,368 |
| ZENITH | 17.6 → 17.7 | 8.37 → 8.34 ms | 6,272 → **9,880** |
| LIMIT | 20.0 → 19.9 | 19.73 → 19.65 ms | 8,348 → 8,064 |

MEGADEMO は場面ごとに plan の登録と解除を繰り返すので、`mn` は、同じ場面でも周によって揺れる。LIMIT の −284 B は、この揺れの範囲とみなす（1 回ずつの比較）。

### その他（新しいファーム）

- 通常 image（`build_api`）: `smoke_device.py --cycles 20` SMOKE_OK 20、`stress_app.py` STRESS_APP_PASS、`test_app_resume.py` TEST_APP_RESUME_OK。DERBY（一巡して `GO`）、LCD CATCH、BIG WAVE、MEGADEMO の起動と Back は、エラーの印なしで HOME_READY に戻った。
- 診断 image: KEYTEST（`r`）が `KEYTEST_READY` まで起動し、LCD CATCH・BIG WAVE も起動した。
- 段階 3 の 1 回目のログの先頭に、書き込み直後の起動で `board_init()` が `ESP_ERR_NOT_FOUND` で abort した記録がある。アプリが動く前の段階で、自動の再起動の後は正常だった。plan とは関係ない（1 回だけ、原因は未調査）。

## 4. 断片化

- **大きさの区分（16/32/48/64 命令の 4 段）は入れなかった。** 理由は 2 つ。(1) ゲストの QuickJS も同じ内部 DRAM の heap から確保する（`prefer_psram=false`）。解放された plan の穴は、次の plan より先にゲストの小さな確保で埋まりうる。穴の大きさを揃えても、plan 同士で再利用される保証にはならない（推定）。(2) DERBY が実行中に解除して登録し直す plan（ゲート、観客・スタンドの段の切り替え）は、同じ命令列を登録し直すので、同じ大きさを要求する。区分で丸めると、平均で約 6 命令ぶん（約 70 B）が 1 本ごとに無駄になる（推定）。
- 実測の指標は最大の連続ブロック `lg`。今の DERBY はレース中の最小が 7,680 → 17,408 B、パドックの中央値が 9,216 → 26,624 B と、むしろ大きくなった。小さい塊が減って、連続した空きが残るため。段階 3 は 7,680 B のまま変わらない（その時点の最大の塊は、plan 以外の確保で決まっている）。起動ごとの揺れは、新しいファームの 2 回で `mn` が 316 B、レースの空きの最小が 628 B（実測）。
- host では、32 スロットに 1..64 命令をでたらめに 600 回登録・解除する churn を ASan の下で回した（`test_pocket_proc_turn_qjs.c` の `plan_size_contract`）。これで分かるのはメモリの安全だけで、TLSF の断片化は host では再現しない。

## 5. 検証

- `tools/kasane_contract/test_proc_sized_plan.c`（新規、`run.sh` に追加。ASan/UBSan と `-O2 -fstrict-aliasing` の 2 通り）: 固定長の plan と比べて、prepare の成否・融合の印・VM の状態・フレームが全桁一致することを確かめる。容量 1・2・63・64 と乱数の n で 4,000 本超（完走 3,700 本）、debug step の有無の両方を回す。1 本ずつ、ちょうどの大きさの `malloc` に置く。拒否の経路（容量 0/65、`count > capacity`、NULL、自分の code の部分範囲からの再 prepare、点列の登録と、再 prepare による解除）も含む。
- `test_pocket_proc_turn_qjs.c` に `plan_size_contract` を追加した。実際の QuickJS と `pocket_proc.c` で、n = 1/2/3/8/16/32/63/64 の `calloc` の大きさが `ksn_proc_sized_plan_bytes(n)` と一致すること、描画の画素、65 命令の INVALID_ARGUMENT、32 本の churn、33 本目の LIMIT_EXCEEDED を確かめる。既存の OOM の経路（`fail_alloc`）もそのまま通る。
- 変異（どちらも手で入れて確認し、元に戻した）: `prepare` の `program->count > capacity` の検査を外すと、ASan が `memmove` の heap-buffer-overflow で止まる。`pocket_proc.c` の確保を 1 命令ぶん少なくすると、`run_pocket_proc_limits_qjs.py` が ASan で止まる。
- `run.sh`（`GAMES_M32=0`）は全部通過。WSL の git が worktree を読めないため、MEGADEMO の baseline を `.cache/kasane_megademo_app/proc_megademo_baseline.js` に置いた。

## 6. 確信の低い点

- 基準は各構成 1 回ずつ（DERBY、段階 3、MEGADEMO）。新しいファームは DERBY と段階 3 が 2 回ずつ。
- 段階 3 は「plan が全部載った」状態の初めての実測で、ターン内の最小 9.0 KB は、受け入れ条件（11,208 B）より約 2.2 KB 低い。門の余裕（ゲートの最小 19,496 B に対して 18,432 B）は約 1 KB。
- `PLANSZ` の `took` は、`calloc` の前後で他のタスクが確保しない前提で測っている。測った 170 回以上の値は、いずれもブロック + 4 B で一定だった。

## 7. 続き: flash の plan を id で登録する（js-to-ir.md の優先 2）

- plan の本体が可変長になったので、flash に置いた命令列を指す形（ヘッダ 40 B + ポインタ）は、同じ `ksn_proc_sized_plan` の変種として足せる。`begin` は、毎回 `vm->owned_code` へ写してから検証するので、ソースが flash でも実行経路は変わらない（写す 12n B の読みが flash のキャッシュを通る分だけ遅い、推定）。
- 見込み（推定）: DERBY の native は、9.7 KB から約 1.2 KB（25 本 × 約 48 B）へ、さらに約 −8.5 KB。ゲスト側は、js-to-ir.md の測定で評価後 −7.1 KB（段階 3 では −8.1 KB）。段階 3 のターン内の最小の不足（約 2.2 KB）は、native 側の分だけで埋まる計算になる。ただし、登録の時間（今は 1 本あたり約 1.1〜1.2 ms、行の配列の読みが大半）の変化と、flash に置く表の管理（アプリと firmware の版を合わせる）は、未検討。
