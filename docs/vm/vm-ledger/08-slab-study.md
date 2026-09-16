# 08 スラブ方式と最大空きブロック

対象: JS オブジェクトのヒープ全体に所有者を置く案。L2a のセグメント層（[07](07-segment-g6.md)）は VM フレーム専用で、`js_malloc_rt` → `guest_malloc` → IDF TLSF（Rust UI・taffy と共有）の上に載っている。JS オブジェクトのヒープには所有者がおらず、`guest.c` は要求サイズヘッダを付けて `heap_caps_malloc` へ流しているだけ（[05](05-allocation.md) §1）。台帳06 §5 では、実行中に最大空きブロックを 59,296B（taffy 34ノード段）以上保てたトレースが tlsf・estalloc とも 0/16 だった。

**問い: 区分スラブは、同じプールで tlsf より最大空きブロックを保てるか。**

2026-09-16、ブランチ `vm/l2-slab-study`。**すべて実測(host)であり、実機の値ではない。** トレースは64bitホスト（JSValue 16B・ポインタ 8B）で採ったもの（§5）。

## 0. 結論

> **32bit（実機と同じ JSValue 8B・ポインタ 4B・TLSF 4B 境界）で取り直した結果は §9。** 12区分が負けるのは変わらないが、区分ゼロの優位は消えて tlsf とほぼ同等になり、59,296B 段は tlsf・区分ゼロとも 17/36 で保てた。以下 §0〜§8 は 64bit ホストの結果。

**No。** 実トレースの確保サイズ分布から決めた12区分のスラブは、試した6通りの配置方針のどれでも、tlsf より最大空きブロックを保てなかった。

- 160 KiB プールに収まったトレースは 16〜17/36（tlsf 23/36）。29,648B 段を実行中ずっと保てたトレースは **0/36**（tlsf 14/36）。
- tlsf とスラブの両方が収まった16本では、最小の空き extent が**16本すべてで tlsf より小さく**、中央値は 15,104B（tlsf 44,520B、差 −27,484B）。
- 効かなかったのは配置方針ではない。carve=low と carve=best は要約のどの列も同じ値、carve=split は収まる本数が1本増えただけ、pick=recent は悪化した。**原因は区分ごとの「半端なセグメント」**で、丸め余白ではない（区分に入る要求の丸めは平均 0.9B。ピーク時の生存バイトに対して、36本合計で 1.0%、160 KiB に収まる23本の合計で 3.5%）。error_toplevel で、生きているセグメント内の空き（使われていない枠）が 37,152B。区分なしの方式では同じトレースで 10,560B。12区分それぞれが 4 KiB の途中まで埋まったセグメントを1本ずつ抱えるので、区分数×セグメントの端数がそのまま外に出る。

区分数を減らすほど良くなり（§4.3）、**区分ゼロ**、つまり 4 KiB セグメントを 8B 粒度の「ブロック開始」「使用中」ビット列だけで管理する形（ブロックヘッダなし、free は台帳の二分探索）では tlsf を上回った。

- 29,648B 段を保てたトレース 17/36（tlsf 14/36）。tlsf が収まる23本のうち22本で最小空き extent が tlsf より大きく、中央値 +6,560B。
- 59,296B を常に空けておくのに要るプールは、tlsf が収まる23本の中央値で **175.5 KiB**（tlsf 183.2 KiB、12区分スラブ 209.2 KiB）。
- 配置方針は pick=low（低位のセグメントから詰める）がよい。pick=recent は 17→15 本に下がる。

ただし **59,296B 段は全方式・全変種で 0/36。** ホスト幅では、最も近いトレースでも区分ゼロで 3,552B、tlsf で 9,056B 足りない（§4.1）。区分ゼロの代償は malloc の探索コストで、平均 115 steps、最大 1,091 steps（var セグメントの線形走査。tlsf は構造的に 1）。

## 1. 作ったもの

| ファイル | 役割 |
| --- | --- |
| `tools/vmalloc/adapter_slab.c` | スラブ・アロケータ。`vmalloc.h` の既存インタフェース（`owner` / `segment_at` / `events` / `configure` / `check`）に加えて `usable_size` を実装 |
| `tools/vmalloc/vmalloc.h` | `usable_size()` フック（任意）。tlsf（`multi_heap_get_allocated_size`）・estalloc（`est_usable_size`）・segment にも付けた |
| `tools/vmalloc/replay.c` | `--allocator slab`、汎用の `--cfg KEY=VALUE`。全バックエンドの行末に free の steps、realloc の据え置き回数、`usable_size` による grow の被覆数、`usable_size` の丸め余白、アプリ区間の `largest_free_block` 最小値を追加（既存の列は変えていない） |
| `tools/vmalloc/size_histogram.py` | トレースの要求サイズ分布と、ピーク時に生きているサイズ構成。`--classes` で区分集合の丸め余白 |
| `tools/vmalloc/slab_study.py` | 比較ドライバ。`compare_fragmentation.py` と同じ方式（同一 o2 バイナリ・同一トレース・同一プール・全走行 `--verify`、SHA256 を JSON に保存）を変種×3モードに広げたもの。`--tables` で本書の表を出す |
| `tools/vmalloc/verify_all.sh` | G6 関所に slab を追加（陽性 44 トレース＋故障注入5種）。`--only segment|slab` |
| `tools/vmalloc/test_fragmentation.py` / `compare_fragmentation.py` | slab を対象に追加 |

## 2. 設計

```
pool（replay が渡す arena）
  └─ 台帳: 全セグメントの [start,end) を start 順の配列で。ブロックヘッダはどこにも無い
       ├─ スラブ     4096B、1区分、枠ごとに使用ビット1つ
       ├─ var        4096B、8B 粒度、粒ごとに「ブロック開始」「使用中」ビット（中サイズ）
       ├─ 専用       var_max を超えるもの。サイズ = end − start
       └─ キャッシュ 空になった標準セグメントを cache 本まで保持（既定2、segment と同じ）
```

- **台帳**: free / realloc / usable_size は台帳を二分探索して所有セグメントを得る（free の steps は最大 10、§4.4）。台帳とクラス別リストはホスト側の構造で、実機なら「セグメントあたり 8B（`uint32_t` 2つ）」の表になる想定。その 8B×本数は `used_bytes` に足しているが、プールの中には置いていない（固定長の `.bss` 表を想定。§7）。
- **セグメント内ヘッダ**: 先頭 8B（区分番号・種別）＋ビット列、8B 境界に切り上げ。ポインタを持たないので**ホストと実機で同じ大きさ**。スラブは `8 + ceil(cap/8)` を 8B 境界へ、残りを `cap × 区分サイズ`（端数は使わない）。var は 4096B で 495 粒・ヘッダ 136B。
- **usable_size**: スラブは区分サイズ、var は開始ビットから次の開始ビットまで、専用は `end − start`。要求サイズより大きい値を返す（slack が出る）。
- **realloc**: スラブは新サイズが同じ区分に入り、かつ枠の半分を超えるなら据え置き。var は後ろの空きブロックへその場で伸ばす／末尾を返す。専用は、直後のプール extent が空いていればその場で伸ばし、縮むときは末尾をプールへ返す。
- **区分（既定）**: 8, 16, 24, 32, 48, 56, 64, 72, 96, 104, 120, 144。`size_histogram.py`（bench_\* を除く36トレース、`# teardown` 前の要求 1,069,332 件）で要求数の多いサイズ（8B 切り上げ）をそのまま区分にした: 48 が 281,715 件、72 が 170,639、32 が 148,450、16 が 124,108、96 が 104,206、64 が 67,037。145〜3,879B は要求の 2.6%。この12区分に入る要求は 97.4%、丸めは平均 0.9B。**区分の選び方は分布どおりで、丸めでは負けていない** — 負けた理由は §0 のとおり区分の数そのもの。
- **中サイズ**: 既定 `var_max=1024`（var セグメント）、超えたら専用。§4.3 で 0 / 256 / 4000 と比べた。区分ゼロの変種は `classes=none:var_max=4000`（var の上限 3,960B で頭打ち）。
- **配置方針**（`--cfg`）:
  - `carve=low` 最も低位の、足りる extent の先頭から切る／`best` 足りる中で最小の extent（同点は低位）／`split` 標準セグメントは low、専用は足りる中で最も高位の extent の**末尾**から切る（寿命の短い大物を小物の間に挟まない）。
  - `pick=low` 同じ区分で空きのある最も低位のセグメントから取る（高位が空になって返せる）／`recent` 最後に触ったセグメントから取る（LIFO。普通のスラブの選び方）。pick=low ではキャッシュも低位を残し、より低位の空きセグメントが出たら最も高位のキャッシュを返す。
- **`check()` の不変条件**: 台帳が start 順・互いに素・16B 境界で、空き extent と合わせてプールをちょうど敷き詰める。extent は整列・非隣接（結合済み）。スラブ: ヘッダの区分番号・種別が台帳と一致、使用ビットの popcount = live、容量外のビットが立っていない、used = live×区分、空スラブが残っていない、部分リストへの所属 ⇔ live < cap、ヒントより下に空き枠が無い。var: 粒0に開始ビット、使用ビット ⊆ 開始ビット、隣接する空きブロックが無い、live・used・最大空き連長が一致。専用: live=1。キャッシュ: 本数と上限。リスト: 種別・区分が一致し台帳から引けて、pick=low なら番地順。
- **ASan**: 空き枠・空き粒・返却セグメント・キャッシュを poison（すべて 8B 境界・8B 倍数長）。

## 3. 比較の方法

`slab_study.py`、`-O2`、bench_\* を除く36トレース（`.cache/vmtest/traces/`、メインのチェックアウトから写したもの、各 SHA256 は JSON）。1走行＝1プロセス。

| モード | プール | 何を見るか |
| --- | --- | --- |
| fixed | 163,840B（`main/app_session.c` の JS ヒープ上限） | 収まるか。毎 op 標本、`--verify` |
| wide | 16 MiB | 容量の制約を外したときの `pool − min(最大空き extent)`（＝その方式が必要とした番地の幅）。20万行を超える3本だけ標本を間引く（g5_gaps 7 op、memory_device 3 op、promise_chain 2 op ごと） |
| bisect | 二分探索（64B 分解能、上限 4 MiB） | 最小プール。生存ピーク 9 MiB の g5_gaps は上限を超えて求まらない |

指標（すべて `# teardown` より前＝アプリ区間）:

- **最大空き extent**（`app_min_pool_largest`）: どのブロックにもセグメントにも属さない連続領域の最大値の、アプリ区間での最小値。tlsf は空き物理ブロック＋ブロックヘッダ、slab / segment はセグメントの外側。taffy が要る「単一の連続ブロック」に当たる量。
- **外部断片化**（`app_ext_frag`）: 空き extent の合計 − 最大空き extent の最大値。
- **内部の空き**（`app_slack_inside`）: 生きているセグメント内の未使用枠・未使用粒の最大値（セグメント方式のみ）。
- **丸め余白**（`app_usable_waste`）: 生存ブロックの Σusable_size − Σ要求サイズ の最大値（usable_size を持つ全方式）。
- **steps**: malloc は（リスト先頭 1）＋（見たセグメント数）＋（ビット列を 32bit 語単位で見た数）＋（var で見たブロック数）＋（プール extent を見た数）。free は台帳の二分探索回数。tlsf は台帳06 §4 のとおり構造的に 1、estalloc は vendored のカウンタ。
- **grow の被覆**（`realloc_grow_covered`）: 要求が前回より大きい realloc のうち、新サイズが**呼び出し前の** `usable_size` 以下だったもの。QuickJS の `js_realloc2` は `usable − 要求` を容量に足すので（[05](05-allocation.md) §6）、ゲストが usable を正直に返していれば、この呼び出しは起きなかった。呼び出し側が実際に要った長さは要求以下なので、**おおむね下限**。ただし要素サイズ（JSValue 16B）での切り捨てと、呼び出しが消えた後のサイズ系列の変化は無視している。

## 4. 結果

### 4.1 変種ごとの要約（fixed = 160 KiB、wide = 16 MiB）

| 変種 | 160KiB に収まる | 最小の最大空き extent（収まった走行） | ≥59,296 | ≥29,648 | 外部断片化 最大 | 内部の空き 最大 | 丸め余白 最大 | 59,296 を常時空けるのに要るプール（tlsf が収まる23本の中央値） |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| tlsf | 23/36 | 7,376 | 0 | 14 | 39,376 | – | 6,798 | 187,552（183.2 KiB） |
| estalloc | 23/36 | 7,688 | 0 | 15 | 40,576 | – | 8,238 | 184,144（179.8 KiB） |
| segment（07） | 21/36 | 5,120 | 0 | 14 | 18,848 | 40,720 | 2,878 | 187,904（183.5 KiB） |
| slab（12区分、low/low） | 16/36 | 1,472 | 0 | **0** | 11,712 | 47,432 | 3,664 | 214,176（209.2 KiB） |
| slab pick=recent | 16/36 | 1,040 | 0 | 0 | 11,840 | 50,064 | 3,664 | 214,176 |
| slab carve=split | 17/36 | 1,280 | 0 | 0 | 7,888 | 47,432 | 4,450 | 210,736 |
| slab classes=16,32 | 23/36 | 3,584 | 0 | 12 | 20,032 | 43,024 | 3,742 | 189,600 |
| **slab 区分ゼロ**（classes=none, var_max=4000） | 23/36 | **13,280** | 0 | **17** | 16,384 | 36,296 | 2,800 | **179,680（175.5 KiB）** |
| 区分ゼロ pick=recent | 23/36 | 8,672 | 0 | 15 | 12,288 | 35,720 | 2,800 | 183,776 |
| 区分ゼロ carve=split | 22/36 | 18,176 | 0 | 17 | 16,384 | 36,296 | 2,800 | 179,680 |

最終列 = wide の `16 MiB − 最小の最大空き extent`（その方式が必要とした番地の幅）の中央値 + 59,296。carve=best は要約の値が全変種で carve=low と同じなので行を省いた（`.cache/slab/sweep3.json`）。

「59,296 段が 0/36」の中身: tlsf が収まる23本で、必要な番地の幅の**最小**は tlsf 113,600B、区分ゼロ 108,096B。160 KiB から 59,296B を引いた 104,544B を下回るトレースが1本も無い。**ホスト幅の生存バイトそのものが大きすぎる**ので、方式で届く段ではない（実機幅は §5）。

### 4.2 トレースごとの最小空き extent（160 KiB、FAIL = 収まらない）

tlsf が収まる23本。収まらない13本（budget_frame_boundary、budget_honest_long_chain、budget_reject_far_catch、budget_teardown_live、g5_gaps、gc_threshold_device、promise_chain、seg_add_deep、seg_boundary_bigframe、seg_oom_boundary、seg_return_reuse、special_calls、stop_with_queue）は全方式で FAIL。

| trace | tlsf | estalloc | segment | slab | slab 16,32 | 区分ゼロ | 区分ゼロ split |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| budget_boundary_exact | 12,656 | 12,008 | 14,752 | FAIL | 8,960 | 27,072 | 27,072 |
| budget_completions | 9,688 | 8,936 | 10,656 | FAIL | 8,960 | 22,976 | 22,976 |
| budget_exit_midchain | 46,152 | 46,712 | 47,520 | 17,152 | 41,728 | 51,648 | 51,648 |
| budget_starve | 22,168 | 21,688 | 22,944 | FAIL | 21,248 | 35,264 | 35,264 |
| builtin_reentry | 7,376 | 7,688 | FAIL | FAIL | 3,584 | 17,104 | 18,176 |
| closures | 26,896 | 27,576 | 22,944 | 4,864 | 29,440 | 35,264 | 35,264 |
| deep_recursion | 47,592 | 48,680 | 39,328 | 19,872 | 44,448 | 51,648 | 51,648 |
| deep_recursion_device | 51,368 | 52,224 | 47,520 | 21,248 | 45,824 | 55,744 | 55,744 |
| error_toplevel | 52,592 | 54,184 | 51,616 | 25,344 | 49,920 | 55,744 | 55,744 |
| fair_fifo_tail | 47,344 | 47,976 | 47,520 | 17,152 | 41,728 | 55,744 | 55,744 |
| fair_midchain | 31,664 | 31,736 | 35,232 | 8,960 | 29,440 | 43,456 | 43,456 |
| fair_reject_point | 40,992 | 41,392 | 43,424 | 13,056 | 37,632 | 47,552 | 47,552 |
| generators | 18,936 | 20,024 | 5,120 | FAIL | 17,600 | 21,584 | 21,584 |
| job_throw | 52,592 | 54,184 | 51,616 | 25,344 | 49,920 | 55,744 | 55,744 |
| memory_device | 11,688 | 13,248 | FAIL | FAIL | 7,456 | 13,280 | FAIL |
| microtask_order | 11,944 | 11,472 | 14,752 | FAIL | 13,056 | 22,976 | 22,976 |
| rejections | 42,888 | 43,728 | 43,424 | 13,056 | 37,632 | 51,648 | 51,648 |
| runaway_jobs | 52,592 | 54,184 | 51,616 | 29,440 | 49,920 | 55,744 | 55,744 |
| seg_closure_survives | 38,024 | 38,992 | 39,328 | 13,056 | 33,536 | 47,552 | 47,552 |
| seg_generator_frames | 46,624 | 47,472 | 47,520 | 21,248 | 45,824 | 51,648 | 51,648 |
| sync_loop | 39,504 | 40,048 | 34,240 | 8,144 | 35,808 | 38,416 | 38,416 |
| try_finally | 29,472 | 30,368 | 22,768 | 8,944 | 29,424 | 31,024 | 31,024 |
| yield_async_from_sync | 31,728 | 31,888 | 31,136 | 1,472 | 29,440 | 39,360 | 39,360 |

tlsf との対比（この23本）: 区分ゼロは 22本で大きく1本（sync_loop）で小さい、中央値 +6,560B。carve=split は memory_device で収まらなくなる代わりに中央値 +7,096B。12区分スラブは収まった16本すべてで小さく、中央値 −27,484B。estalloc は19本で大きい（中央値 +680B）。

同じトレースの内訳（fixed、アプリ区間の最大値。独立な瞬間の最大値なので足さない）:

| trace | 方式 | 最小の最大空き extent | 内部の空き | 外部断片化 | 専用セグメント（ピーク本数） | 丸め余白 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| error_toplevel | tlsf | 52,592 | – | 6,008 | – | 3,373 |
| | slab 12区分 | 25,344 | 37,152 | 3,648 | 6 | 2,474 |
| | slab 16,32 | 49,920 | 11,648 | 3,648 | 6 | 2,044 |
| | 区分ゼロ | 55,744 | 10,560 | 0 | 1 | 1,978 |
| closures | tlsf | 26,896 | – | 14,272 | – | 5,106 |
| | slab 12区分 | 4,864 | 42,368 | 3,648 | 8 | 3,277 |
| | 区分ゼロ | 35,264 | 16,752 | 0 | 1 | 2,445 |

12区分と区分ゼロの差（error_toplevel で 30,400B）は、内部の空きの差（26,592B）でほぼ説明できる。丸め余白の差は 500B。

### 4.3 何を変えると効くか（fixed / wide、36本）

区分の数と中サイズの境（すべて carve=low, pick=low、`.cache/slab/sweep2.json`）:

| 区分 | var_max | 160KiB に収まる | ≥29,648 | 最小の最大空き extent | wide 必要幅の中央値（36本） | malloc steps 平均 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 12区分 | 0（すべて専用） | 19 | 3 | 1,024 | 162,824 | 2.08 |
| 12区分 | 256 | 16 | 1 | 3,264 | 166,288 | 2.63 |
| 12区分 | 1024（既定） | 16 | 0 | 1,472 | 167,168 | 3.47 |
| 12区分 | 4000 | 17 | 1 | 1,104 | 165,440 | 3.54 |
| 16,32,48,72,96 | 1024 | 19 | 6 | 2,608 | 162,824 | 4.90 |
| 16,32,48,72 | 1024 | 21 | 9 | 1,168 | 158,752 | 9.59 |
| 16,32 | 1024 | 23 | 12 | 3,584 | 148,512 | 56.63 |
| 16 | 1024 | 23 | 16 | 6,944 | 145,816 | 73.16 |
| 16 | 4000 | 23 | 16 | 9,184 | 142,912 | 70.77 |
| なし | 1024 | 23 | 16 | 11,040 | 141,720 | 79.17 |
| なし | 4000 | 23 | 17 | 13,280 | 138,816 | 75.64 |
| （tlsf） | | 23 | 14 | 7,376 | 150,380 | 1.00 |

区分を1つ足すごとに段を保てる本数が減り、探索コストだけが下がる。区分の数とセグメントの端数のトレードオフで、この36本の範囲では**端数の側が常に重い**。

セグメントサイズとキャッシュ（`.cache/slab/sweep3.json`）:

| 変種 | 160KiB に収まる | ≥29,648 | 最小の最大空き extent |
| --- | ---: | ---: | ---: |
| 12区分 seg 2048 | 21 | 10 | 4,240 |
| 12区分 seg 4096（既定） | 16 | 0 | 1,472 |
| 12区分 seg 8192 | **0** | – | – |
| 12区分 cache=0 | 16 | 0 | 1,472 |
| 区分ゼロ seg 8192 | 24 | 17 | 2,496 |
| 区分ゼロ cache=0 | 23 | 17 | 13,280 |

12区分はセグメントを小さくすると端数が減って持ち直し（2048 で 29,648 段 10本）、8192 では1本も収まらない。キャッシュ（空セグメント2本の保持）は段に効いていない（cache=0 で同値）。区分ゼロは 8192 で収まる本数が1本増えるが最小値が落ち、探索が平均 122 steps に増える。

### 4.4 最小プール・探索・realloc

最小プール（bisect、g5_gaps を除く35本、tlsf 比）:

| 変種 | 160KiB 以下 | tlsf 比 中央値 | 最小 | 最大 | tlsf の最小プールが 400KiB 未満の本の合計 |
| --- | ---: | ---: | ---: | ---: | ---: |
| tlsf | 23/35 | 1.000 | | | 4,211,264 |
| estalloc | 23/35 | 0.996 | 0.983 | 1.037 | 4,196,032 |
| segment | 21/35 | 0.997 | 0.959 | 1.282 | 4,353,728 |
| slab 12区分 | 16/35 | 1.161 | 0.831 | 1.265 | 4,900,864 |
| slab 16,32 | 23/35 | 1.016 | 0.920 | 1.049 | 4,276,160 |
| 区分ゼロ | 23/35 | **0.944** | 0.836 | 1.019 | **4,000,640** |
| 区分ゼロ split | 22/35 | 0.944 | 0.836 | 1.185 | 4,046,016 |

12区分で 1 を切るのは生存ピークが 700 KiB を超える5本（promise_chain 0.831、budget_teardown_live 0.837、budget_honest_long_chain 0.844、stop_with_queue 0.870、seg_boundary_bigframe 0.990）で、端数が固定量なので大きいトレースほど比率が薄まる。tlsf の最小プールが 160 KiB 以下の23本では 1.10〜1.27 倍。

探索と realloc（wide、36本の合計・最大）:

| 変種 | malloc steps 平均 | malloc steps 最大 | free steps 最大 | その場で済んだ realloc / 全 realloc | grow の被覆 / grow |
| --- | ---: | ---: | ---: | ---: | ---: |
| tlsf | 1.00 | 1 | 1 | 13,148 / 81,218 | 6,695 / 79,989（8.4%） |
| estalloc | 2.54 | 4 | 0 | 34,531 / 81,218 | 8,146（10.2%） |
| segment | 19.37 | 16,507 | 0 | 35,778 / 81,218 | 6,073（7.6%） |
| slab 12区分 | 3.06 | 95 | 10 | 4,841 / 81,218 | 4,057（5.1%） |
| slab 16,32 | 76.40 | 843 | 10 | 8,558 / 81,218 | 5,970（7.5%） |
| 区分ゼロ | 115.33 | 1,091 | 10 | 12,767 / 81,218 | 4,034（5.0%） |
| 区分ゼロ pick=recent | 67.09 | 1,064 | 10 | 34,999 / 81,218 | 4,034（5.0%） |

- **free の steps**: 台帳の二分探索で最大 10。ヘッダ方式（tlsf 1、segment 0）より重いが上限は対数。
- **malloc の steps**: 12区分は平均 3（リスト先頭＋ビット列）。区分ゼロは var セグメントを番地順に線形に見るので平均 115、最大 1,091。**区分ゼロを実機に持っていくなら、var セグメントを最大空き連長で引ける索引が要る**（未実装、§6）。
- **slack**: grow の 5〜10% が「前回の usable に収まっていた」。tlsf（8.4%）や estalloc（10.2%）の方が slab（5%）より多い。TLSF / estalloc が分割しきれない端数をブロックに含めて返すためと推定している（未確認）。QuickJS の realloc の大半は 1.5 倍成長の要求で、区分の丸め（平均 1B）はそれを覆わない。**usable_size を正直に返すこと自体は、どの方式でも数%の realloc を消しうる**（下限の目安。§3 の注記つき）。今のゲストは要求サイズを返すので 0。

## 5. 64bit ホストとの差

- **生存バイト**: トレースは JSValue 16B・ポインタ 8B。実機（8B / 4B）では同じワークロードの生存バイトが小さい（台帳06 §5 の注記、推定・未計測）。§4.1 の「59,296 段 0/36」はこの幅のせいが大きく、**実機で到達不能とは言っていない**。
- **区分**: 区分はホストのサイズ分布から決めた。実機では 72→40 前後、48→28 前後と縮むはずで（推定）、区分の集合はそのままでは合わない。一方で**負けた原因（区分数×セグメント端数）はオブジェクトの大きさに依らず絶対量で効く**ので、生存バイトが小さい実機では相対的にもっと重くなる、というのが推論（未計測）。
- **ヘッダ**: slab のセグメント内ヘッダはポインタを含まずホストと実機で同じ大きさ。台帳は実機幅（8B/本）で計上。tlsf はホストでは 8B 境界パッチ版で、ブロックヘッダが実機より大きい（台帳06 §1）。これは tlsf に不利な側に効いており、それでも12区分スラブは tlsf に負けた。区分ゼロの tlsf に対する優位（中央値 +6,560B）のうち、どれだけがホストのヘッダ幅によるものかは分けていない。
- **アラインメント**: slab のブロックは 8B 境界（セグメント 16B、ヘッダ 8B 倍数、区分と粒は 8B 倍数）。replay の 4B 検査を満たし、JSValue / double の 8B 要求（台帳06 §1）も満たす。

## 6. 検査（G6 関所）

`bash tools/vmalloc/verify_all.sh`（ASan+UBSan、96 MiB プール、2026-09-16）: segment・slab（既定の12区分）とも **44/44 トレースが `verify=OK check=1`**、故障注入は両方とも5種すべて検出。`G6: all traces verify clean; all faults caught`、13分45秒（大半は slab の g5_gaps を毎 op 標本で走らせる時間）。

| slab の `--fault` | 何を壊すか | 検出 |
| --- | --- | --- |
| `early-return` | 生存ブロックが1つ残ったセグメントを空扱い | ASan use-after-poison（replay がパターンを読んだ瞬間） |
| `overlap` | 7回に1回、枠の使用ビットを立て忘れる | `check()`: used bits != live count |
| `pool-overlap` | 7回に1回、切り出した extent を消費し忘れる | `check()`: gap or overlap before trailing extent |
| `ledger-order` | 5本に1本、台帳へ番地順でなく末尾に挿す | `check()`（op 1611） |
| `bitmap` | free で隣の枠のビットを消す | verify: 生存ブロックのパターン破損・2ブロックの重なり |

既定以外の変種（pick=recent、carve=split、classes=16,32、区分ゼロ×3）は `slab_study.py --binary asan --modes fixed,wide` で36本を ASan+UBSan 下で `--verify` 再生した（§8）。432走行すべて rc=0・`verify=OK`・`check=1` で、§4.1 の数値は o2 と同じ値になった（9分6秒）。

`test_fragmentation.py`（o2・asan）の陰性・陽性対照は slab を含む5方式で成功（slab の fragmented は 4,096B: 4096B 要求は専用セグメントでヘッダ無し）。

## 7. 未解決・限界

- **実物の VM はこのアロケータの上で動いていない。** 確保履歴の再生であって、`guest.c` を差し替えた実行ではない。
- **taffy の確保は含まない。** 「最大空き extent」は JS 側だけで見た量。L2 で JS ヒープを専用プールに切り出すなら、taffy は TLSF 側に残るので、この数字が効くのは JS と taffy が同じプールを分け合う設計（台帳06 §5 の注記と同じ条件）に限られる。
- **区分ゼロの malloc 探索は線形**（最大 1,091 steps）。実機に載せるなら索引が要り、その索引のメモリと、索引を持ったときの配置（低位優先を保てるか）は測っていない。
- **台帳の置き場所**: 8B/本を `used_bytes` に足しただけで、プールの外に置いた。プール内に置けば台帳自体の成長が連続領域を要求する。160 KiB / 4 KiB = 40 本＋専用の上限で固定長にできる見込みだが、専用セグメントの本数上限は決めていない。
- **grow の被覆は下限の目安**で、「消える realloc の回数」を実際に数えたものではない（要素サイズの切り捨てと系列の変化を無視）。実数は `guest_usable_size` を差し替えて vmrun でトレースを取り直さないと出ない。
- **区分は64bitホストの分布**。実機の確保サイズ分布は未取得（07 §6 と同じ）。
- 59,296B 段が実機幅で届くかは、この台帳では答えていない。

## 8. 再現コマンド

```bash
# WSL、ワークツリーのルートで。トレースは .cache/vmtest/traces/（tools/vmtest/run.sh --trace）
bash tools/vmalloc/build.sh
python3 tools/vmalloc/size_histogram.py $(ls .cache/vmtest/traces/*.trace | grep -v bench_)
python3 tools/vmalloc/size_histogram.py --classes 8,16,24,32,48,56,64,72,96,104,120,144 $(ls .cache/vmtest/traces/*.trace | grep -v bench_)

V=tlsf,estalloc,segment,slab,slab:pick=recent,slab:carve=split,slab:classes=16+32,slab:classes=none:var_max=4000,slab:classes=none:var_max=4000:pick=recent,slab:classes=none:var_max=4000:carve=split
python3 tools/vmalloc/slab_study.py --modes fixed,wide,bisect --jobs 8 --output .cache/slab/study.json \
  --variants "$V" .cache/vmtest/traces/*.trace          # §4.1（約30分）
python3 tools/vmalloc/slab_study.py --tables .cache/slab/study.json tlsf estalloc segment slab slab:classes=16+32 slab:classes=none:var_max=4000   # §4.2・§4.4

bash tools/vmalloc/verify_all.sh                         # §6（約14分。--quick で bench_* を飛ばす、--only slab）
python3 tools/vmalloc/slab_study.py --binary asan --modes fixed,wide --jobs 8 --output .cache/slab/asan.json \
  --variants slab:pick=recent,slab:carve=split,slab:classes=16+32,slab:classes=none:var_max=4000,slab:classes=none:var_max=4000:pick=recent,slab:classes=none:var_max=4000:carve=split \
  .cache/vmtest/traces/*.trace

# 1本だけ
.cache/vmalloc/vmalloc_replay-o2 --allocator slab --cfg classes=none --cfg var_max=4000 --pool 163840 --verify .cache/vmtest/traces/closures.trace
```

## 9. 32bit で取り直す（2026-09-16 追記）

§5 の「64bit ホストとの差が結論に効くか」を、トレース採取側（vmrun）と再生側（vmalloc_replay）の両方を `-m32` でビルドして確かめた。**すべて実測(host, i386)であり、実機の値ではない。**

### 9.1 結論

**No。区分ゼロは 32bit では tlsf に勝たない（ほぼ同等）。** 64bit で見えた優位はほとんど消えた。12区分スラブは 32bit でも明確に負ける。

| 32bit、36トレース、160 KiB | tlsf | estalloc | slab 12区分（32bit 分布） | slab 12区分（64bit の区分） | 区分ゼロ |
| --- | ---: | ---: | ---: | ---: | ---: |
| 160 KiB に収まる | 25 | 24 | 21 | 21 | 25 |
| 59,296B を保てた | **17** | 11 | 0 | 0 | **17** |
| 29,648B を保てた | **22** | 19 | 17 | 14 | 21 |
| 最小の最大空き extent の中央値（収まった走行） | **67,080** | 58,620 | 43,120 | 31,184 | 65,536 |
| tlsf が収まる25本で tlsf と比べて（大きい / 小さい、差の中央値） | – | 0 / 24、−8,410 | 0 / 21、−27,620 | 0 / 21、−37,580 | 15 / 10、+1,184 |
| 59,296 を常時空けるのに要るプール（tlsf が収まる25本の中央値） | **156,896（153.2 KiB）** | 165,152 | 182,512 | 192,304 | 157,600（153.9 KiB） |
| malloc steps 平均 | 1.00 | 2.52 | 3.06 | 2.68 | 96.54 |

- 区分ゼロと tlsf で段の判定が分かれたのは special_calls の1本だけ（tlsf 32,484B、区分ゼロ 28,272B で 29,648 段を割る）。59,296 段は同じ17本。
- 対の差（+1,184B、15勝10敗）と必要プール（+704B）は向きが逆で、どちらも数 KiB の1%未満。**「勝つ」と言える差ではない。** そのため指示どおり、線形探索の索引案は試していない。
- 12区分は 32bit の分布（上位: 16 が 252,584 件、48 が 188,045、32 が 183,128、40 が 136,761、80 が 104,441、8 が 90,096）から区分を取り直しても、59,296 段は 0/36 のまま。§0 の原因（区分数×セグメントの端数）は 32bit でも変わらない。

### 9.2 64bit との差

同じ worktree の同じソースから 64bit の vmrun も作り直し、44本を両方で採り直した（§4 の表は main のチェックアウトから写した古いトレースで、tlsf が収まる本数が 23 → 20 と少し違う）。

| 指標 | tlsf 64 → 32 | 区分ゼロ 64 → 32 | 12区分 64 → 32 |
| --- | --- | --- | --- |
| 160 KiB に収まる | 20 → 25 | 20 → 25 | 12 → 21 |
| 59,296B を保てた | 0 → 17 | 0 → 17 | 0 → 0 |
| 29,648B を保てた | 10 → 22 | 14 → 21 | 0 → 14 |
| 最小の最大空き extent の中央値 | 30,064 → 67,080 | 37,312 → 65,536 | 13,056 → 31,184 |
| tlsf との差の中央値（tlsf が収まる本） | – | +7,484（20勝0敗）→ +1,184（15勝10敗） | −27,100 → −37,580 |
| 必要プールの中央値 | 190.8 → 153.2 KiB | 181.5 → 153.9 KiB | 211.2 → 187.8 KiB |

読み方:

- **59,296B 段は、32bit では tlsf でも17本で保てる。** §4.1 の「全方式 0/36」は 64bit の生存バイトのせいだった（§5 の推定が当たった）。
- **区分ゼロの 64bit での優位は、主にホスト幅の TLSF ヘッダによるものと見ている（推定。要因を分けて測ってはいない）。** 64bit ホストの tlsf は 8B 境界パッチでブロックヘッダが大きく、ポインタが 8B の空きブロックの最小サイズも大きい（台帳06 §1）。32bit で `ALIGN_SIZE_LOG2=2`（実機と同じ既定）に戻すと、この差が消える。§5 で「どれだけがヘッダ幅によるかは分けていない」と書いた点への答え。
- estalloc は 32bit でも 8B 境界のままなので、32bit では tlsf に負ける側に回った（64bit では 16勝4敗、32bit では 0勝24敗）。
- 丸め余白は 32bit では tlsf 8,135B、区分ゼロ 3,193B、estalloc 17,393B（収まった走行での最大）。

### 9.3 32bit ビルドの条件と、揃っていない点

- **ツールチェーン**: WSL に gcc-multilib が無く、sudo にはパスワードが要る。`tools/vmtest/m32_sysroot.sh` が `apt-get download`（root 不要）で i386 の libc・libgcc・libasan を落として `/tmp/m32sys` に展開し、libc.so のリンカスクリプトのパスを書き換え、その ld-linux.so.2 と DT_RPATH でリンクするフラグを出す。ASan+UBSan も 32bit で動く。
- **JSValue 8B**: quickjs-ng は `INTPTR_MAX < INT64_MAX` で `JS_NAN_BOXING` を既定で有効にする（`quickjs.h`）。実機（Xtensa）も同じ条件で、firmware の `components/quickjs-ng/CMakeLists.txt` はこれを上書きしていない。32bit のトレース見出しは `sizeof_JSValue=8 sizeof_ptr=4`。
- **構造体の中の double / int64 の境界**: i386 の ABI は構造体内で 4B、Xtensa は 8B。`-malign-double` で 8B に揃えた（`offsetof(struct{char; double;})` が 4 → 8 になることを確認）。glibc のヘッダはこのフラグを想定していないので、64bit のフィールドを持つ構造体を libc と受け渡す箇所で配置が食い違いうる。vmrun の 44本の出力は `expected/` とバイト一致した（44 passed）。
- **`JS_VM_FRAME_ALIGN`**: `quickjs-vmstack.h` は `ESP_PLATFORM` のときだけ 4 で、ほかは 8。32bit ホストでは 8 だとフレームリンク（ポインタ1個、4B）の `_Static_assert` が通らないので、`UINTPTR_MAX == 0xFFFFFFFF` でも 4 にした（実機と同じ値。64bit ホストと実機のビルドは何も変わらない）。
- **TLSF**: `VMALLOC_TLSF_ALIGN_LOG2=2`（IDF の既定）。台帳06 §1 の 64bit 用パッチはマクロで既定に戻る。
- **揃っていない点**: `max_align_t` は i386 でも 16B で、実機（16B、`_Alignof` 8）と同じだが、`long double` は i386 が 12B、Xtensa は 8B（QuickJS の確保サイズには出ないと見ているが未確認）。ヒープ上のブロックの配置と確保サイズは実機の QuickJS と同じになるはずだが、実機の確保サイズ分布とは突き合わせていない。
- 同じ条件で 64bit の o2 は seg_oom_boundary の出力が `expected/` と食い違った（43 passed, 1 failed）。32bit 化とは無関係の既存の差（64bit ビルドのソースは変えていない）で、トレースはそのまま使った。
- 32bit の再生は ASan+UBSan でも fixed の 144走行（4方式×36本）が rc=0・`verify=OK` で、o2 と同じ値になった。

### 9.4 再現コマンド

```bash
F=$(bash tools/vmtest/m32_sysroot.sh)
VMTEST_OUT=$PWD/.cache/vmtest64 bash tools/vmtest/build.sh o2
VMTEST_OUT=$PWD/.cache/vmtest32 VMTEST_CFLAGS="$F" bash tools/vmtest/build.sh o2
VMALLOC_OUT=../../.cache/vmalloc32 VMALLOC_CFLAGS="$F" VMALLOC_TLSF_ALIGN_LOG2=2 bash tools/vmalloc/build.sh
N=$(ls .cache/vmtest/traces/*.trace | xargs -n1 basename | sed 's/\.trace$//')   # §4 と同じ44本
VMTEST_OUT=$PWD/.cache/vmtest64 bash tools/vmtest/run.sh --variant o2 --trace $N
VMTEST_OUT=$PWD/.cache/vmtest32 bash tools/vmtest/run.sh --variant o2 --trace $N
V=tlsf,estalloc,slab:classes=8+16+24+32+48+56+64+72+96+104+120+144,slab:classes=8+16+24+32+40+48+56+64+80+88+96+120,slab:classes=none:var_max=4000
python3 tools/vmalloc/slab_study.py --binary-dir .cache/vmalloc --output .cache/slab/s64.json --variants "$V" .cache/vmtest64/traces/*.trace
python3 tools/vmalloc/slab_study.py --binary-dir .cache/vmalloc32 --output .cache/slab/s32.json --variants "$V" .cache/vmtest32/traces/*.trace
```
