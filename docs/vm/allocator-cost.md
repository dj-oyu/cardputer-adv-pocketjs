# ゲストのアロケータの値段（2026-09-27、`vm/runtime-speed`）

計装は `CONFIG_POCKET_VM_ALLOCPROBE`（既定 n）: `components/pocketjs_guest/src/guest.c` のアロケータの入口
（malloc/calloc・free・realloc・ブロック長の読み戻し）の中で使ったサイクルと回数を数え、`KASANE_PAINT` と同じ
30 描画フレームの区切りで `ALLOCPROBE ...` として JS のターン時間と並べて出す。USB の `)` は `heap_caps` の
マイクロベンチ（`ALLOCBENCH ...`）。

## 1. 実装で確認したこと（ESP-IDF v6.0.1）

| QuickJS 側 | 実機で走るもの | 置き場所 |
| --- | --- | --- |
| `js_malloc_rt` | `heap_caps_malloc` → 優先度 3 段 × 登録ヒープを走査して caps が合うヒープを探す → `multi_heap_malloc`（クリティカルセクション）→ `tlsf_malloc` | IRAM |
| 確保の直後 | `heap_caps_get_allocated_size`（上限の計上に実ブロック長を使う）→ `find_containing_heap`（ヒープ一覧の線形探索）→ `tlsf_block_size` | **flash** |
| `js_free_rt` | 読み戻し → `heap_caps_free` → もう一度 `find_containing_heap` → クリティカルセクション → `tlsf_free` | IRAM |

毒化・トレース・タスク追跡は無効、`CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH` も無効、ゲストは PSRAM を先に試さない
（`prefer_psram=false`）。設定の取り違えは無い。

## 2. 実測（実機、同じバイナリ）

`)` のマイクロベンチ（内部 RAM、200 個ずつ、2 回とも同じ値）:

| 大きさ | malloc | ブロック長の読み戻し | free | malloc→free の組 |
| --- | --- | --- | --- | --- |
| 16〜48 B | 約 1,180 サイクル | 約 110 | 約 680 | 約 1,800（7.5 µs） |
| 64〜256 B | 約 1,225 | 約 110 | 約 680 | 約 1,850 |

見積もり（1 組 500 サイクル）の 3 倍以上。大きさにはほとんど依らない。

STRESS（`stress_app.py`、各段階 20 秒、30 フレームの区切りごとの中央値）:

| 段階 | JS のターン | うちアロケータ | 割合 | 1 フレームの呼び出し |
| --- | --- | --- | --- | --- |
| LV1 | 7.63 ms | **1.56 ms** | **20.6%** | malloc 166・free 165・realloc 11・読み戻し 389 |
| LV2 | 7.59 ms | 1.57 ms | 20.7% | ほぼ同じ |
| LV3 | 8.14 ms | 1.66 ms | 20.5% | ほぼ同じ |

1 回あたり: malloc 約 1,270 サイクル、free 約 620、realloc 約 1,100、読み戻し約 110（全体の 1 割強）。
アロケータの外に出るほど遅い経路は無く、**呼び出しの回数そのもの**が値段になっている。

## 3. 次の手（backlog R3）

- 同じ大きさの確保と解放が大半（JSObject、小さな配列・プロパティ配列）。解放したブロックを大きさごとに少数
  手元に置いて使い回せば、その分の `heap_caps` の走査とクリティカルセクションが消える。上限の計上（手元の
  ブロックも「使用中」のまま）、GC とメモリ逼迫時の返却、断片化への影響を設計してから入れる。
- ブロック長の読み戻しを TLSF のブロックヘッダの直読みにすれば、flash の関数とヒープ一覧の探索を省ける
  （最大でアロケータ時間の 1 割）。
