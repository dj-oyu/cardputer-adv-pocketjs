# VM L0 報告（phase 0 / `vm/p0-foundation`）

対象: [quickjs-freertos-vm-spec.md](quickjs-freertos-vm-spec.md) §5（L0）と、L1/L2 の判断材料。2026-09-12 時点。

数値は、実機の値（実測(device)、§2.1）、ビルド成果物の静的な値（実測(build)）、WSL 上のホスト実行の値（実測(host)）、コード読解の結果、推定、のいずれかで、それぞれ明記する。

要点:

- quickjs-ng とゲストのリポジトリへの取り込みは完了し、コミット済み。ファームの DIRAM はバイト単位で変わらない。
- 計測プローブ（既定で無効）、ホストの判定基盤、アロケータ比較、台帳 6 本が揃い、実機で **6 ワークロード × 5 条件（base / ui / audio / wifi / all）× 3 反復 = 90 マス**を同一バイナリで採取した（§2.1）。中央値・p95・最大値は**全標本に対する厳密値**。§5 の完了条件の点検表は §2.3、L1 の予算案（提案）は §2.2。
- 実機の採取の途中で、`main` にもある起動直後のクラッシュ 2 件を見つけて直した（`40f8261`。§2.1）。
- L2 について最も重要な事実: **既存の async/generator 機構はフレームのデータをヒープへ移すだけで、C の再帰は取り除かない。** 「C スタックを JS の深さから切り離す」には、call 系 opcode と `done:` の構造を新しく作る必要がある。

## 1. できたこと

| 項目 | 状態 | ファイル |
| --- | --- | --- |
| quickjs-ng 0.14.0 とゲストの取り込み | コミット済み（`10f5185` 無改変の取り込み → `6de51f4` immutable-buffer パッチ → `d9ef1f9` リンク先の切り替え） | `components/quickjs-ng/`、`components/pocketjs_guest/` |
| L0 プローブ（`CONFIG_POCKET_VM_PROBE`、既定 n） | コミット済み | [main/Kconfig.projbuild](../main/Kconfig.projbuild)、[sdkconfig.vmprobe.defaults](../sdkconfig.vmprobe.defaults)、[main/pocket/vmprobe.c](../main/pocket/vmprobe.c) / `.h`、`quickjs.c` の `VM_PROBE` ブロック 4 つ + [quickjs-vmprobe.h](../components/quickjs-ng/quickjs-ng/quickjs-vmprobe.h)、`app_session.c` / `main.c` / `pocket_api.c` / `main/CMakeLists.txt` の `#ifdef` 部分 |
| 実機用ワークロード 6 本（USB の `A`〜`F` だけで起動し、ホームの一覧には出ない） | L0完了後に削除（タグ `vm-L0` から `git checkout vm-L0 -- apps/vmprobe` で復元可） | 旧 `apps/vmprobe/` |
| 競合条件 5 種（USB の `P`〜`W`。UI・音声・Wi-Fi を実物の面で動かす） | L0完了後に削除（同上） | 旧 `apps/vmprobe/condition.js`・README、`main.c` / `app_session.c` / `vmprobe.h` の該当部分 |
| 実機の採取スクリプト（行列・再起動・厳密な統計） | L0完了後に削除（`git checkout vm-L0 -- tools/vm_l0_capture.py` で復元可） | 旧 `tools/vm_l0_capture.py` |
| ホストの判定基盤（guest.c を写した vmrun、コーパス 23 本、Test262 部分集合、時間の基準、確保トレース、`--force-yield` の入口） | コミット済み | [tools/vmtest/](../tools/vmtest/)（手順は [README](../tools/vmtest/README.md)） |
| アロケータ比較（IDF の tlsf 実物 / estalloc / naive） | コミット済み | [tools/vmalloc/](../tools/vmalloc/) |
| 台帳 | コミット済み | [vm-ledger/01〜06](vm-ledger/) |

取り込みで挙動が変わっていないことの根拠:

- 取り込み前後で `idf.py size` の DIRAM は 115,292 B、IRAM は 16,384 B で同じ（実測(build)）。
- `.bin` は 64 B 小さくなった（2,539,504 → 2,539,440 B）。原因は、`__FILE__` から作られる assert の文字列に入るソースのパスが短くなったこと。DIRAM には影響しない。
- 取り込んだパッチ済みの `quickjs.c` は、旧来のビルド時生成物（`prepare_quickjs.py` が作ったもの）と `diff -q` で一致した。

レビューで直した欠陥は 9 件。主なものは次の 3 件。

- `sdkconfig.h` を無条件に include していたため、ホストテスト 4 本がビルドできなくなっていた。
- `deep_recursion.js` は固定の 400 段で、実機では毎フレーム例外になる内容だった。
- 台帳 06 の bench 表は、`replay.c` の計測の不具合から出た値だった。

別のレビュー（Fable）が挙げた台帳の欠落 13 件は、すべて `quickjs.c` と照合して事実と確認し、台帳に反映した。

ホストで再現するコマンド（WSL、`/mnt/c/devs/m5stack/cardputer-adv-pocketjs-vm`）:

```bash
bash tools/vmtest/build.sh all && bash tools/vmtest/run.sh && bash tools/vmtest/run.sh --variant o2
python3 tools/vmtest/test262.py -j 8        # 初回は --fetch
python3 tools/vmtest/timing.py
bash tools/vmalloc/build.sh && bash tools/vmalloc/run_all.sh   # トレース採取は台帳06 §4
```

## 2. ホストで得た基準値

特記のないものは**実測(host)**（x86-64、WSL、gcc 13.3）。ホストでは JSValue が 16 B、ポインタが 8 B で、実機（8 B / 4 B）より多く確保する。**バイト数の絶対値を実機の予算判定に使わないこと。**

| 項目 | 値 |
| --- | --- |
| プローブ無効ビルドの DIRAM | 115,372 B（実測(build)、`build_vm_l0_off`）。**プローブと競合条件を足した本作業の前後で 1 バイトも動かない**。flash も 1,550,840 B で同一 |
| プローブ有効ビルドの DIRAM | 119,692 B、+4,320 B（実測(build)。生標本の配列と書き出しの buffer の分だけ、旧版の +1,760 B から増えた） |
| コーパス | 23/23 一致（ASan、-O2、`--force-yield`）。L0 には yield の入口が無いので、`--force-yield` はまだ何もしない |
| Test262 部分集合 | 7,501 pass / 194 fail / 0 skip。失敗はすべて未対応の機能: `Promise.allKeyed` 系 174、末尾呼び出し 9、`using` 7、`Promise.try` 4。[test262-baseline.txt](../tools/vmtest/test262-baseline.txt) |
| 時間の基準 | 8 本のベンチ、各 30 回の中央値・p95・最大。[timing-baseline.txt](../tools/vmtest/timing-baseline.txt) |
| 20 KiB スタック上限での再帰の深さ | ホスト o2 で 29 段、`map` のコールバック経由で 11 段。実機は約 50 段と**推定**（ELF 上の `JS_CallInternal` のフレームが `entry a1, 0x150` = 336 B + alloca）。実機の段数は未計測 |
| 空のランタイム | `qjs_malloc_size` 98,680 B。160 KiB の 6 割にあたる |

コードから確定している性質（実測ではない）:

- **循環ゴミの自動回収が起きない。** GC 閾値の初期値は 256 KiB で、ゲストの上限 160 KiB より大きい。firmware はどこでも `JS_SetGCThreshold` / `JS_RunGC` を呼ばない。ホストでは `gc_threshold_device.js` が、2 オブジェクトの循環 265 個で OOM になる。
- **上流の不具合。** バックトレースを組み立てる途中の OOM で use-after-free が起きる（ASan で検出。[known/oom_backtrace_uaf.js](../tools/vmtest/known/oom_backtrace_uaf.js)）。
- **8 バイト境界が保証されない。** 実機の tlsf は 4 バイト境界で、JSValue や double が 8 バイト境界に乗る保証はアロケータ層のどこにも無い。実害（Xtensa で落ちるのか、遅くなるだけか）は未確認。
- **ゲストの上限の記述が食い違っている。** ソース上は 160 KiB（`app_session.c`）だが、`CLAUDE.md` は 144 KiB と書いている。

アロケータ比較（[台帳06](vm-ledger/06-allocator-baseline.md)、実測(host)）:

- 最小プールの大きさで、tlsf と estalloc の差はほぼ 1 KiB 未満。どちらが小さいかはトレースによって入れ替わる。naive はすべてのトレースで明確に悪い。
- estalloc の探索は全トレースで最大 3 段で、線形の fallback には一度も落ちなかった。
- estalloc の `est_calloc` には乗算オーバーフローの検査が無い。サイズ上限の検査は `assert` 頼み。
- 「160 KiB プールの最大空きブロック」は、今日の taffy の確保可否とは別の量。ゲストは専用プールを持たず、共有ヒープから確保している。

## 2.1 実機の基準値（実測(device)、2026-09-12）

**L0完了（本タグ `vm-L0`）後、この手順を動かしていたワークロード本体と採取スクリプトは削除した**
（`apps/vmprobe/` の6ワークロード・`condition.js`・README、`tools/vm_l0_capture.py`）。
以下の数値は削除前に採取したもので、そのまま残す。再測定したい場合は
`git checkout vm-L0 -- apps/vmprobe tools/vm_l0_capture.py` で両方とも復元できる。プローブ本体
（`main/pocket/vmprobe.c` のサンプリングと `VMPROBE STATIC` / `VMPROBE WINDOW` の報告）は残っていて、
`CONFIG_POCKET_VM_PROBE` を有効にしたビルドで動く任意のアプリを測る。

`build_vm_l0`（プローブ有効）を COM3 の実機に 1 度だけ書き込み、`tools/vm_l0_capture.py` で
**6 ワークロード × 5 条件 × 3 反復 = 90 マス**を各 15 秒採取した。生データは
`.cache/vm/l0-matrix.jsonl`（git 管理外、1 窓 1 行で 1,265 行）。**90 マスすべて同一バイナリ**で、
条件は実行時に USB のバイトで選ぶだけなので、配置（命令キャッシュのアラインメント）の差は
条件間の比較に入らない。

**中央値・p95・最大値は全標本に対する厳密値。** 実機は要約しない — フレームごとに turn 時間・
`frame()` 時間・drain 時間・ジョブ件数を、完了ごとに遅延を、生の値のまま吐く（窓は 1 秒か
64 標本の早い方で閉じるので、間引きも上書きも起きない）。p95 は nearest-rank
（`sorted[ceil(0.95n)-1]`）。**実測の内訳: フレーム標本 28,127、完了遅延標本 7,295、取りこぼし
（`lat_drop`）は 90 マス通して 0。** 厳密でないのは 4 項目だけで、`heap_free` / `heap_largest` /
`js_used` / `stack_hw` は 8 フレームに 1 回（約 4 Hz）の標本の窓内極値。

`VMPROBE STATIC`: `sizeof(JSValue)`=8、`sizeof(JSStackFrame)`=48、`sizeof(JSVarRef)`=32、
`-Os`、gcc 15.2.0。仕様 §11 の仮置き（JSValue 8 B、フレーム 32〜64 B）と合う。

DIRAM（実測(build)）: **プローブ無効 115,372 B（本作業の前後で 1 バイトも動かない）**、
有効 119,692 B（+4,320 B）。flash も無効側は 1,550,840 B で同一（`tools/memlog.py`）。

### 行列（実測(device)。時間は ms、バイトは B）

| ワークロード | 条件 | frame 中央値 / p95 / 最大 (ms) | jobs 中央値 / p95 / 最大 | lat 中央値 / p95 / 最大 (ms) | 空きヒープ最小 | 最大空きブロック最小 | js_used 最大 | stack_hw 最小 | 標本数 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| sync_loop | base | 117.91 / 124.45 / 125.26 | 0 / 0 / 0 | — | 107,484 | 65,536 | 85,748 | 23,788 | 320 |
| sync_loop | ui | 119.30 / 125.96 / 127.34 | 0 / 0 / 0 | — | 99,836 | 46,080 | 88,895 | 21,404 | 296 |
| sync_loop | audio | 118.87 / 119.89 / 120.82 | 0 / 1 / 1 | 98.58 / 109.59 / 110.25 | 99,572 | 50,176 | 90,298 | 21,404 | 324 |
| sync_loop | wifi | 119.24 / 127.19 / 134.04 | 0 / 2 / 5 | 3.04 / 98.15 / 101.16 | 25,408 | 16,384 | 104,777 | 21,404 | 319 |
| sync_loop | all | 130.81 / 142.08 / 151.10 | 0 / 3 / 5 | 50.28 / 141.26 / 150.03 | 18,632 | 7,680 | 108,768 | 21,404 | 278 |
| deep_recursion | base | 0.68 / 0.88 / 0.94 | 0 / 0 / 0 | — | 106,696 | 64,512 | 86,348 | 8,332 | 1209 |
| deep_recursion | ui | 2.16 / 2.53 / 3.29 | 0 / 0 / 0 | — | 99,016 | 45,056 | 89,457 | 8,284 | 1183 |
| deep_recursion | audio | 0.73 / 0.99 / 3.59 | 0 / 0 / 1 | 15.40 / 28.20 / 33.10 | 98,824 | 48,128 | 90,860 | 8,252 | 1209 |
| deep_recursion | wifi | 0.73 / 1.07 / 15.21 | 0 / 0 / 5 | 24.89 / 32.39 / 32.80 | 24,704 | 15,872 | 105,299 | 8,332 | 1207 |
| deep_recursion | all | 2.21 / 4.24 / 16.17 | 0 / 1 / 5 | 19.05 / 31.21 / 32.88 | 17,220 | 7,680 | 109,926 | 8,332 | 1183 |
| closures | base | 50.11 / 50.69 / 51.42 | 0 / 0 / 0 | — | 106,664 | 64,512 | 86,407 | 23,788 | 780 |
| closures | ui | 50.76 / 51.47 / 52.33 | 0 / 0 / 0 | — | 99,128 | 45,056 | 89,478 | 21,404 | 624 |
| closures | audio | 50.88 / 51.98 / 53.05 | 0 / 1 / 1 | 14.51 / 38.88 / 43.59 | 98,896 | 48,128 | 90,881 | 21,404 | 751 |
| closures | wifi | 51.57 / 59.50 / 76.47 | 0 / 2 / 5 | 3.11 / 35.69 / 51.89 | 25,676 | 16,384 | 104,592 | 21,404 | 732 |
| closures | all | 56.45 / 65.49 / 81.62 | 0 / 2 / 6 | 19.64 / 67.84 / 76.34 | 16,144 | 7,680 | 110,755 | 21,404 | 559 |
| promise_chain | base | 11.93 / 12.43 / 12.86 | 41 / 41 / 41 | — | 106,712 | 40,960 | 86,320 | 23,692 | 1179 |
| promise_chain | ui | 13.56 / 14.22 / 14.62 | 41 / 41 / 41 | — | 99,052 | 32,768 | 89,467 | 21,404 | 1171 |
| promise_chain | audio | 12.35 / 12.98 / 14.93 | 41 / 41 / 42 | 24.51 / 32.77 / 32.86 | 98,892 | 31,744 | 90,790 | 21,404 | 1176 |
| promise_chain | wifi | 12.78 / 15.08 / 27.75 | 41 / 41 / 47 | 19.49 / 24.63 / 24.63 | 33,832 | 6,144 | 97,661 | 21,404 | 1188 |
| promise_chain | all | 15.06 / 17.50 / 35.93 | 41 / 41 / 47 | 5.54 / 32.49 / 33.49 | 37,764 | 5,632 | 104,129 | 21,404 | 1169 |
| io_wait | base | 1.53 / 1.91 / 2.22 | 2 / 2 / 2 | 0.07 / 0.07 / 0.12 | 104,528 | 62,464 | 87,941 | 23,788 | 1174 |
| io_wait | ui | 3.16 / 3.71 / 4.40 | 2 / 2 / 2 | 0.07 / 0.07 / 0.13 | 96,504 | 43,008 | 91,344 | 21,404 | 1188 |
| io_wait | audio | 1.84 / 2.29 / 4.69 | 2 / 2 / 3 | 0.07 / 0.07 / 27.49 | 96,760 | 47,104 | 92,323 | 21,404 | 1170 |
| io_wait | wifi | 1.84 / 2.88 / 15.03 | 2 / 2 / 7 | 0.07 / 0.14 / 31.62 | 22,944 | 12,288 | 105,527 | 21,404 | 1560 |
| io_wait | all | 3.38 / 5.88 / 21.43 | 2 / 3 / 7 | 0.07 / 9.55 / 33.08 | 14,768 | 7,680 | 111,690 | 21,244 | 1183 |
| async_generator | base | 32.34 / 33.06 / 33.64 | 62 / 62 / 62 | — | 105,976 | 63,488 | 86,965 | 23,788 | 1169 |
| async_generator | ui | 33.74 / 34.39 / 34.79 | 62 / 62 / 62 | — | 98,080 | 45,056 | 90,330 | 21,404 | 819 |
| async_generator | audio | 34.01 / 35.00 / 38.00 | 62 / 62 / 63 | 7.51 / 32.48 / 32.48 | 97,840 | 49,152 | 91,733 | 21,404 | 1092 |
| async_generator | wifi | 32.69 / 38.92 / 69.46 | 62 / 62 / 67 | 33.71 / 40.33 / 41.61 | 23,976 | 12,800 | 105,956 | 21,388 | 1108 |
| async_generator | all | 37.99 / 44.39 / 72.22 | 62 / 64 / 68 | 25.29 / 54.15 / 55.15 | 15,396 | 7,680 | 111,351 | 21,404 | 747 |
### 内訳（turn / `frame()` / drain / UI の tick と draw。すべて中央値、ms）と反復間のばらつき

| ワークロード | 条件 | turn | frame() | drain | UI tick+draw | 反復ごとの中央値 |
| --- | --- | --- | --- | --- | --- | --- |
| sync_loop | base | 117.91 | 117.56 | 0.01 | 0.34 | 117.85 / 117.87 / 124.42 |
| sync_loop | ui | 119.30 | 118.36 | 0.01 | 0.93 | 119.22 / 119.22 / 125.76 |
| sync_loop | audio | 118.87 | 118.47 | 0.01 | 0.39 | 118.86 / 118.87 / 118.89 |
| sync_loop | wifi | 119.24 | 118.67 | 0.01 | 0.56 | 119.17 / 119.21 / 119.38 |
| sync_loop | all | 130.81 | 129.44 | 0.01 | 1.36 | 121.85 / 132.29 / 132.74 |
| deep_recursion | base | 0.68 | 0.33 | 0.01 | 0.33 | 0.67 / 0.68 / 0.68 |
| deep_recursion | ui | 2.16 | 1.01 | 0.01 | 1.14 | 1.91 / 2.17 / 2.19 |
| deep_recursion | audio | 0.73 | 0.37 | 0.01 | 0.34 | 0.73 / 0.73 / 0.73 |
| deep_recursion | wifi | 0.73 | 0.37 | 0.01 | 0.34 | 0.73 / 0.73 / 0.73 |
| deep_recursion | all | 2.21 | 1.02 | 0.01 | 1.18 | 2.20 / 2.21 / 2.21 |
| closures | base | 50.11 | 49.75 | 0.01 | 0.35 | 50.10 / 50.11 / 50.11 |
| closures | ui | 50.76 | 49.56 | 0.01 | 1.19 | 50.53 / 50.55 / 51.03 |
| closures | audio | 50.88 | 50.45 | 0.01 | 0.42 | 50.83 / 50.91 / 50.92 |
| closures | wifi | 51.57 | 50.98 | 0.01 | 0.57 | 51.45 / 51.57 / 51.57 |
| closures | all | 56.45 | 54.95 | 0.01 | 1.49 | 55.10 / 55.53 / 59.25 |
| promise_chain | base | 11.93 | 8.66 | 2.95 | 0.32 | 11.93 / 11.94 / 11.94 |
| promise_chain | ui | 13.56 | 9.42 | 2.93 | 1.21 | 13.51 / 13.57 / 13.57 |
| promise_chain | audio | 12.35 | 9.05 | 2.92 | 0.38 | 12.35 / 12.35 / 12.36 |
| promise_chain | wifi | 12.78 | 9.45 | 2.92 | 0.41 | 12.77 / 12.78 / 12.79 |
| promise_chain | all | 15.06 | 11.09 | 2.92 | 1.05 | 15.03 / 15.07 / 15.09 |
| io_wait | base | 1.53 | 0.04 | 1.14 | 0.35 | 1.52 / 1.53 / 1.53 |
| io_wait | ui | 3.16 | 0.71 | 1.20 | 1.25 | 3.13 / 3.14 / 3.41 |
| io_wait | audio | 1.84 | 0.10 | 1.29 | 0.44 | 1.83 / 1.84 / 1.84 |
| io_wait | wifi | 1.84 | 0.10 | 1.24 | 0.49 | 1.81 / 1.82 / 1.85 |
| io_wait | all | 3.38 | 0.71 | 1.35 | 1.32 | 3.33 / 3.34 / 3.57 |
| async_generator | base | 32.34 | 1.55 | 30.27 | 0.51 | 32.33 / 32.33 / 32.34 |
| async_generator | ui | 33.74 | 2.26 | 30.31 | 1.18 | 33.73 / 33.73 / 33.77 |
| async_generator | audio | 34.01 | 1.73 | 31.85 | 0.42 | 33.98 / 34.02 / 34.02 |
| async_generator | wifi | 32.69 | 1.62 | 30.53 | 0.54 | 32.66 / 32.70 / 32.76 |
| async_generator | all | 37.99 | 2.58 | 34.29 | 1.12 | 37.82 / 38.04 / 38.26 |

読み方:

- **UI は turn に +0.6〜1.2 ms。** `frame` − `frame()` − `drain` が UI コアの tick と draw で、
  base の 0.32〜0.51 ms が ui 条件で 0.93〜1.49 ms になる。UI を足しても JS の時間は変わらない。
- **音声はフレーム時間をほとんど動かさないが、完了遅延を作る。** audio 条件の `lat` は
  ワークロードのターン長そのもので、A（turn 118 ms）で中央値 98.58 ms、B（turn 0.73 ms）で
  15.40 ms。**完了は次の `pocket_api_pump()` まで待つので、遅延の下限はターン長**という関係が
  そのまま出ている。L1 の時間予算がそのまま応答遅延の上限になる、という根拠。
- **Wi-Fi はヒープを削り、稀に長いフレームを作る。** 空きヒープ最小は base の約 105 KiB から
  wifi で 23〜34 KiB、all で 14.8〜37.8 KiB まで落ち、最大空きブロック最小は **5,632 B**
  （D、all）。**taffy の 2 段目（29,648 B）すら、無線を上げた状態では入らない。**
  一方フレーム時間の中央値はほぼ動かず、効くのは p95 と最大（C: 50.11 → 51.57 → 56.45、
  最大 51.42 → 76.47 → 81.62）。
- **qpeak は全 90 マスで最大 3。** ジョブは積まれた側から消費されており、キューは溜まらない。
  1 回の drain の件数は D=41、F=62（all 条件の最大 68）で、これが L1 の件数予算の元。
- **drain の単価には桁の差がある。** F は 62 件で 30.27 ms（約 0.49 ms/件）、D は 41 件で
  2.95 ms（約 0.07 ms/件）。**件数だけの予算では F と D が同じ扱いになる**ので、件数と時間の
  両方が要る（仕様 §6 の「件数・時間によってジョブ間でホストへ戻る」と整合）。
- **反復間のばらつきはほとんど 0.1% 未満だが、A だけ 2 回目まで 117.85/117.87 で 3 回目が
  124.42（+5.6%）。** 同一バイナリ・同一入力なので、これはこの機体の実行環境の揺れ
  （CLAUDE.md が言う 15% の命令キャッシュ差と同じ種類の話）。**5% 未満の差を主張するときは、
  この列を根拠に使えない。**
- **B の `stack_hw` は 8,252〜8,332 B**（他は 21,244〜23,788 B）。深い再帰が ui タスクの C
  スタックを約 15 KiB 使う（23,788 − 8,332 B）。ホーム画面の描画より深いので、この値は JS 側が
  出したもの。
- **プローブ自身の費用**: 窓ごとの書き出しが最大 6,628 µs（実測、`flush_us`）。turn の計測の
  外側だが、1 秒に 1 回そのフレームの後ろを伸ばす。フレーム時間そのものには入らない。


### 2.1.0 固定した競合条件（何が動いていたか）

5 条件。USB の 1 バイト（`P` + マスク）で選び、`apps/vmprobe/condition.js` がワークロードの
`frame()` を包んで実物の面を動かす。**90 マスすべて同一バイナリ**で、条件の切り替えに
再ビルドを挟まない（仕様 §5 の「可能なら同一バイナリで比較する」）。

| 条件 | 字 | 実際に動いていたもの | 実測での確認 |
| --- | --- | --- | --- |
| base | `P` | ワークロードだけ | — |
| ui | `Q` | `pocket.ui` の画面（rect 1 + text 1）を毎フレーム `setText` | `VMCOND ui on` |
| audio | `R` | `pocket.audio.tone` 440 Hz 1 秒を完了から張り直す（合成も I2S も本物、音声タスクは優先度 7） | `VMCOND tone 5/10/15`（15 秒で 14〜15 音） |
| wifi | `T` | リンクを走行中ずっと保持し、3 秒ごとに LAN ゲートウェイへ平文 GET、本文を EOF まで読む | `VMCOND wifi connected 192.168.1.42 -> http://192.168.1.1/`、`VMCOND http 200 56 …ms` |
| all | `W` | 上の 3 つ同時 | 上記すべて |

条件の側が失敗した回も走り切り、`VMCOND` の行が JSONL に残る。**Wi-Fi の要求は
ワークロードによっては断られる**: `NET_PLAIN_MIN_FREE`（12 KiB）の門にかかると
`VMCOND http OUT_OF_MEMORY not enough memory for a request: 10,860 free, 7,680 largest block`
が残る（D promise_chain は毎フレーム 40 段の連鎖を作るので、要求の瞬間の空きが最も少ない）。

### 2.1.1 行列の採取で分かった 2 件（ファームの不具合ではない）

1. **HTTPS はこの構成では成立しない。** 宛先を `https://example.com/` にした最初の版では、ゲストが 95 KiB を持ち無線が 37 KiB を取った状態で握手が始まり、要求の直前の空きヒープは実測 6,024 B だった。握手が通ったあと QuickJS 側の確保が落ち、**投げられた例外の値が `null` / `[uninitialized]`** になってセッションが終わる。これは上流の設計どおりの見え方で（Error オブジェクトを作る確保まで失敗したときに QuickJS が投げる値）、ファームの論理の誤りではない。条件は平文 HTTP（LAN のゲートウェイ）に変え、理由を [apps/vmprobe/README.md](../apps/vmprobe/README.md) に残した。
2. **未処理 rejection は 1 件でセッションを終わらせる。** `drain_jobs()` が未処理 rejection を frame error として返し、`main.c` がアプリを止める。仕様どおりの動作だが、条件スクリプト側は連鎖の末尾に `catch` を置く必要があった（置くまで、上の OOM が毎回セッションを殺していた）。

**採取ツール側の不具合も 1 件。** Windows の `usbser.sys` では RTS だけを変えても device には届かず、**セッション中 2 回目以降のハードリセットが黙って効いていなかった**（実測: リセット 1 回目だけ再起動し、2・3 回目は前の起動のまま続いた）。esptool と同じく RTS を書いたあとに DTR を書き戻して直した。直す前は、1 本目以外の `stack_hw` と空きヒープが前の回の履歴になる。

## 2.2 L1 の予算案（**提案**。採否はユーザーが決める）

仕様 §11 は「L0 完了時に RAM 上限・遅延上限・許容スループット低下を記録し、それ以後のレベルの
判定基準とする」と書いている。以下は §2.1 の実測から引いた**提案**で、まだ決定ではない。

| 項目 | 提案値 | 何の実測から引いたか |
| --- | --- | --- |
| ジョブ件数予算（1 ターンで drain するジョブの上限） | **16 件**。超えたらジョブ境界でホストへ戻る | F は毎フレーム 62 件で drain 30.27 ms＝**約 0.49 ms/件**、D は 41 件で 2.95 ms＝**約 0.07 ms/件**（どちらも実測、base）。16 件なら F で約 7.8 ms、D では予算に当たらない。今日の最大件数は 68（F、all） |
| 時間予算（1 ターンの JS = `frame()` + drain） | **8 ms**。超えたら次のジョブ境界で戻る | 30 fps = 33.3 ms のうち、転送だけで約 7.7 ms（`PERF send`、実測）、UI の tick+draw が 0.9〜1.5 ms（実測）。8 ms なら 6 本のうち B（`frame()` 0.33 + drain 0.01 ms）と E（0.04 + 1.14 ms）は今のまま収まり、A（117.56）・C（49.75）・D（8.66 + 2.95）・F（1.55 + 30.27）が分割対象になる（すべて base の中央値、実測） |
| 応答遅延の上限（完了記録 → resolve/reject） | **p95 8 ms / 最大 20 ms** | 今は**ターン長がそのまま遅延**: audio 条件で A（turn 118 ms）は lat 中央値 98.58 ms・p95 109.59 ms、B（turn 0.73 ms）は 15.40 / 28.20 ms（実測）。時間予算 8 ms を守ればこの範囲に入る |
| ハンドラ到達の上限（完了 → `.then` の本体） | **2 ターン以内**（時間予算 8 ms なら 16 ms 以内） | 上界 = `lat` + `call` + `drain`（`app_tick()` の pump → `frame()` → drain の順序から）。今日の最悪は A の all 条件で **286.4 ms**（p95 どうしの和、実測からの計算） |
| RAM 増分の上限（L1 の追加分） | **静的 DIRAM +8 KiB 以内**（専用タスクのスタックを含む）、ゲスト上限は 160 KiB 据え置き、**実行中の空きヒープ最小値を今より 4 KiB 以上減らさない** | プローブ有効ビルドの増分が実測 +4,320 B。実行中の空きヒープ最小は wifi/all で **14,768 B**、最大空きブロック最小は **5,632 B**（実測）。ここに 8 KiB の常駐を足すと、無線を上げた状態の余裕がほぼ無くなる |
| 許容スループット低下 | **同一ワークロードの turn 中央値で +5% 以内**（同一バイナリで比較すること） | 反復間のばらつきが A の base で +5.6%（117.85 / 117.87 / 124.42、実測）。**これ未満の差は主張できない** |

決める前に見ておく点:

- **件数予算だけでは足りない。** 0.49 ms/件（F）と 0.07 ms/件（D）で 7 倍違うので、件数と時間の
  両方が要る。仕様 §6 の「件数・時間によってジョブ間でホストへ戻る」はこの通り。
- **遅延の上限は時間予算の従属変数。** 別々に決めると矛盾する。
- **RAM の上限は無線を上げた状態で判定する。** base だけを見ると 100 KiB 空いているように見える。

## 2.3 仕様 §5 の完了条件（点検表）

| §5 が求めるもの | 状態 | 根拠 |
| --- | --- | --- |
| エンジン revision・コンパイラ・最適化・`sizeof(JSValue)` / `JSStackFrame` / `JSVarRef` | 満たす | `VMPROBE STATIC`（セッションごとに 1 行）。8 / 48 / 32、`-Os`、gcc 15.2.0 |
| ジョブ件数 | 満たす | `jobs`（フレームごとの全標本） |
| 最大キュー長 | 満たす | `qpeak_max`（窓ごとの最大、`JS_EnqueueJob` / `JS_ExecutePendingJob` を数えた実物） |
| drain 時間 | 満たす | `drain`（取り込み済み `guest.c` で計測、`frame()` と分けて全標本） |
| I/O 完了からハンドラ実行までの遅延 | **一部** | `lat` は完了記録 → resolve/reject まで。ハンドラ実行までは同じターンの drain に入るので**上界 = `lat` + `call` + `drain`**。この上界は出せるが、ハンドラ到達の直接計測ではない |
| フレーム時間 | 満たす | `frame`（`pocketjs_ui_turn` 全体、全標本） |
| C スタック最大使用量 | **一部** | `stack_hw`（ui タスクの残りの最小値、1 マスごとの起動以来）。ホーム画面の描画の方が深い場合はそちらが出る。**JS の到達段数は測っていない** |
| 静的 RAM | 満たす | `tools/memlog.py`、プローブ無効 115,372 B / 有効 119,692 B |
| 空きヒープ・最大空きブロック・JS メモリ使用量 | 満たす | `heap_free_min` / `heap_largest_min` / `js_used_max`（4 Hz 標本の窓内極値） |
| 壁時計と OS 競合の区別 | 満たす（仕様の代替条項） | すべて壁時計と明記。CPU 実行時間は取れない（`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` 無効）ので、§5 の「取れない場合は壁時計値として報告する」に従う。競合の量は条件間の差として出す |
| 同期ループ・深い再帰・クロージャ・長い Promise 連鎖・I/O 待ち・async/generator の基準 | 満たす | 6 ワークロード × 5 条件 |
| UI・音声・通信との競合条件を固定 | 満たす | base / ui / audio / wifi / all。USB の 1 バイトで選び、`condition.js` が実物の面を動かす |
| 入力データ・反復数を固定 | 満たす | 各ワークロードの反復数はソースに定数で入っている（20,000 / 上限の 3/4 / 500 / 40 / sleep 10ms / 20） |
| 中央値・p95・最大値 | 満たす | 全標本に対する厳密値。p95 は nearest-rank |
| 実測・推定の区別 | 満たす | 本報告の各数値に明記 |
| 配置による差を考慮し、可能なら同一バイナリで比較 | 満たす | 90 マスすべて**同一バイナリ**（1 回の書き込み）。条件は実行時のバイトで選ぶだけで、再ビルドを挟まない |

## 3. 実機で測る手順

実機はプローブ入りのファームに書き換わる。このファームで変わるのは、USB の `A`〜`F`（ワークロード）と `P`〜`W`（競合条件）と `VMPROBE` のログ行が加わることだけ。**プローブ無効のビルドはバイト単位で従来どおり**（§2.1 の DIRAM を参照）。

```powershell
. 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1'
cd C:\devs\m5stack\cardputer-adv-pocketjs-vm
# -D SDKCONFIG は省かない。省くとプローブが共有の ./sdkconfig に入り、他の build_* もすべてプローブ入りになる
idf.py -B build_vm_l0 -D SDKCONFIG=build_vm_l0/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.vmprobe.defaults" -p COM3 flash
python tools\vm_l0_capture.py --port COM3 --seconds 15 --reps 3 --out .cache\vm\l0-matrix.jsonl
python tools\vm_l0_capture.py --summarize .cache\vm\l0-matrix.jsonl --markdown   # 本報告の表
python tools\vm_l0_capture.py --port COM3 --workloads D --conditions wifi        # 1 マスだけ
```

**1 回ごとに実機を再起動する。** `stack_hw` は起動以来の最小値なので、再起動しないと 1 本目以外は前の回の履歴を見ることになる。リセットは USB Serial/JTAG の RTS で、esptool と同じ順序（Windows の usbser.sys では RTS の変更だけでは伝わらないので、DTR を書き戻す。これを入れるまで**セッション中 2 回目以降のリセットが黙って効いていなかった**）。

**Wi-Fi の条件は保存済みの資格情報を使う。** 無ければ `VMCOND wifi NOT_AVAILABLE` が残るだけで、他の条件は普通に走る。平文 HTTP の宛先は実機自身のアドレスから作った LAN のゲートウェイで、時計も PC 側のサーバーも要らない（理由は [apps/vmprobe/README.md](../apps/vmprobe/README.md)）。

所要時間（実測）は、90 マス（6 × 5 × 3）を 15 秒ずつで約 100 分。書き込みが約 1 分。終わったら、普段の手順（本体ツリーの `build_api`）で通常のファームに戻す。

見る値:

| 行 / ワークロード | 見るもの | 何の基準になるか |
| --- | --- | --- |
| `VMPROBE STATIC` | `sizeof_jsvalue`（8 のはず）、`sizeof_stackframe`、`sizeof_varref`、`opt` | 仕様 §11 の仮置き（JSValue 8 B、フレーム 32〜64 B）の確認 |
| A `sync_loop` | `frame` の中央値と最大値 | 素のディスパッチの費用。L2 で確認地点を足したときの性能低下の比較元 |
| B `deep_recursion` | 早期終了の警告が出ないこと、`stack_hw`、`frame` | C スタック。**到達した段数はログに出ない**（未計測のまま） |
| C `closures` | `frame`、`js_used` | JSVarRef の経路 |
| D `promise_chain` / F `async_generator` | `jobs` の中央値と最大値、`qpeak_max`、`frame` | ジョブ件数と最大キュー長。L1 の件数予算の元 |
| E `io_wait` | `lat` の中央値・p95・最大 | 完了通知からの遅延。L1 で減らす対象 |
| 全ワークロード | `heap_free_min`、`heap_largest_min`、`js_used_max/js_limit` | 最大空きブロックを taffy の段（29,648 / 59,296 B）と比べる。これが実機での本当の値 |
| 全ワークロード | `frame` / `call` / `drain` の 3 本 | turn 全体、`frame()` 単体、drain 単体。UI コアの tick と draw は差で出る |

読むときの注意:

- **`frame` はフレーム全体（`pocketjs_ui_turn`）の時間で、`call` と `drain` はその内訳。** 内訳は取り込み済みの `guest.c` で測っており（`pocketjs_guest_vmprobe_take`）、`frame` −（`call`+`drain`）が UI コアの tick と draw、および引数の作成と解放になる。
- **すべて壁時計。** 仕様 §5 は「精度のある CPU 実行時間が取れない場合は壁時計値として報告する」と書いており、これはその側。`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` はこのビルドでも無効で、ui タスクが他タスクに奪われた時間は分離していない。**分離の代わりが条件の行列**で、同じバイナリの base と ui / audio / wifi の差が、そのまま OS 競合の量になる。
- **`stack_hw` は起動以来の最小値。** 採取は 1 マスごとに再起動するので「この回の起動以来」になるが、ホーム画面の描画がより深ければ JS 側の値はそこに隠れる。プローブ自身の flush も含む。
- **`lat` はハンドラが走るまでの時間ではない。** 測っているのは、完了が記録されてから resolve/reject を呼ぶまで。`.then` のハンドラが実際に走るのは同じターンの drain で、**上界は `lat` + `call` + `drain`**（`app_tick()` は pump → `frame()` → drain の順で、reaction はその drain に入る）。
- **`heap_*` と `js_used` と `stack_hw` だけが厳密値ではない。** 8 フレームに 1 回（約 4 Hz）標本し、窓の中の極値を出す。時間とジョブ件数と遅延は全標本が残る。

## 4. 台帳の要点と未確認事項

| 台帳 | 要点 |
| --- | --- |
| [01 呼び出し経路](vm-ledger/01-call-paths.md) | 通常の JS 呼び出しは C の再帰になる。フレームを解放する経路（`done` か `done_generator` か）は、コンパイル時の `b->func_kind` で決まる。C から JS への再入は ToPrimitive、getter/setter、`prepareStackTrace` を通じてどこからでも起きるので、有限のリストにはならない。**opcode の種類ではなく、実行時のネイティブ深さで判定する必要がある。** |
| [02 フレームのポインタ](vm-ledger/02-frame-pointers.md) | フレームを指すポインタは 3 系統ある: `rt->current_stack_frame`、呼び出し先の `prev_frame`、切り離されていない `JSVarRef.stack_frame`。`JS_CallInternal` の C ローカル変数（`sp`、`pc`、`var_buf` など）も、フレームへの 2 組目の生ポインタになっている。**GC はフレームを走査しない**（値は参照カウントだけで生きている）。 |
| [03 ジョブと割り込み](vm-ledger/03-jobs-interrupts.md) | 1 ティックの順序は「resolve でキューに積む → `frame()` → drain」。`frame()` が例外を投げたティックは drain しない。`app_session` の割り込みハンドラが上書きするので、guest.c 側の epoch 方式の割り込みは死んでいる。**割り込まれた `await` の Promise は、永久に pending のまま残る。** |
| [04 opcode の確認地点](vm-ledger/04-opcode-checkpoints.md) | `js_poll_interrupts` は 14 箇所（opcode 内は goto 系と if 系の 7 箇所）。call、return、yield、catch、反復系の opcode には確認が 1 つも無い。正規表現も割り込みを投げる（`js_regexp_exec`）。 |
| [05 確保](vm-ledger/05-allocation.md) | `guest_realloc` は常に malloc+コピー+free で、slack は常に 0 に見える。`JSFunctionBytecode` は自分自身を指すポインタを持つ。`js_array_buffer_update_typed_arrays` は、移動したあとで参照を貼り直す既存の前例になる。 |
| [06 アロケータ基準](vm-ledger/06-allocator-baseline.md) | §2 の通り。 |

**残っている未確認事項**（詳細は各台帳の「未確認」節）:

- `js_check_stack_overflow` の閾値の意味。
- 反復系ヘルパー（`js_for_of_next` など）と `js_function_apply` / `JS_EvalObject` / `JS_IteratorClose` の本体に、割り込みの確認があるかどうか。
- async generator が、割り込まれたときに Promise を reject せず飲み込むかどうか。
- `sizeof(max_align_t)` の実機での値。
- 144 KiB という数字の出どころ。
- 実機で本当に GC が 1 回も走らないか（ログでの確認）。
- estalloc の `assert` がこのビルドで有効かどうか。

**本報告の作成時に解消した項目:** 03 の「`pocketjs_ui_turn` の実装が見当たらない」。本体は `pocketjs_ui_qjs/src/ui_qjs.c:818` にあり、`pocketjs_guest_frame` を呼んでから UI の tick と draw を行う。03 の fact 62 の順序は正しい。台帳自体はまだ直していない。

**台帳の食い違い4件**（04 の箇所数、06 のトレースの内訳、`vmprobe.h` のコメント、vmtest の README）は、コミット前に直した。

**行番号の基準:** 台帳の file:line は `015fc62`（プローブの差分を当てた取り込み版）の `quickjs.c` を指す。

## 5. L2 設計への含意（async/generator の機構を一般化できるか）

### 事実（台帳の追記時に `quickjs.c` と照合済み）

既にあるもの:

- `async_func_init` は、引数・変数・オペランドスタック・`var_refs` を 1 つのヒープブロックに置き、`JSStackFrame` を `JSAsyncFunctionState` に埋め込む。
- 再開の入口は `sf` からインタプリタのレジスタを組み直し、`cur_sp = NULL` を「実行中」の印にする。
- 中断は 6 つの opcode が行う。いずれも `done_generator` を経て `cur_pc` / `cur_sp` を保存する。
- 解放時には `close_var_refs` が走るので、`JSVarRef.pvalue` は宙に浮かない。
- GC は、所有する GC オブジェクトを経由してだけ辿られる。

一般化を妨げるもの:

1. **C の再帰が残る。** `async_func_resume` は現在の C スタックの上で `JS_CallInternal` を呼び、再開したフレームの中の JS→JS 呼び出しも C で再帰する。中断できるのは最も内側のフレームだけ。
2. **解放の経路がコンパイル時に決まる。** `done:` と `done_generator` のどちらを通るかは `b->func_kind` で決まる。通常の関数を中断すると、`done:` を通ってフレームが破棄される。
3. **保存される状態が足りない。** `JSStackFrame` は `this`、`new_target`、`flags`、`caller_ctx` を持たない。再開時の `new_target` は `JS_UNDEFINED` 固定なので、constructor には使えない。
4. **`cur_func` の持ち方が違う。** 通常のフレームは借用し、async のフレームは dup で所有する。
5. **`prev_frame` の連鎖が古くなる。** 再開時に繋ぎ直すのは 1 フレームだけ。中断した連鎖の底は、既に消えた C スタック上の `sf_s` を指す。しかも backtrace などの walker は、連鎖が 1 本であることを前提にしている。
6. **戻り値の受け渡しが driver 固有。** `FUNC_RET_*` と `cur_sp[-1]` による取り決めは、generator や Promise の driver ごとに違う。
7. **割り込みは捕捉できない例外として実装されている。** 仕様 §3-6 の「中断は例外ではない」とは相容れない。
8. **GC の対象にならない。** GC オブジェクトとして登録されていない VM スタックはマークされない。値が壊れることは無いが、循環参照は漏れる。
9. **生ポインタの問題。** `JS_CallInternal` の C ローカル変数と `sf_s` 自体が、フレームを指す生ポインタの元になっている。L2a で移すべきものは alloca のブロックだけではなく、`JSStackFrame` 本体も含む。

### 判断と推定（Fable の分析。検証していない）

- async の機構は、**フレームの表現と再開の入口の雛形としては使える。** `cur_sp == NULL`、解放時の close、C フレームを跨いで中断しない、という不変条件はそのまま守るべき。
- **呼び出し構造の雛形にはならない。** 仕様 §7 の完了条件「C スタックが深さに比例しない」を満たすには、call 系 opcode が VM フレームを積んで同じループを続け、`done:` が VM フレームを pop する構造が要る。これは新しい作業になる（L2b）。
- **L2a（セグメント）だけでは C スタックは減らない。** 仕様 §12 の「L2a だけで任意中断を提供しない」と整合する。
- 流用できるのは `async_func_*` の約 150 行と、再開の入口の約 20 行（**推定、未計測**）。

### 本報告の見立て

L2 の価値は、長いジョブの中断のほかに、20 KiB スタックでの再帰の上限（ホストで 29 段、実機で推定約 50 段）を外すことにもある。後者の効き目は、実機の段数を測るまで評価できない。

## 6. L1 に着手する前に決めること

1. **L0 の完了の承認。** §2.3 の点検表で、§5 の項目のうち 2 つ（I/O 完了から**ハンドラ実行まで**の直接計測、JS が使った C スタックの段数）だけが「一部」で、残りは満たしている。この 2 つを L0 の範囲外と認めるか、追加で測るかを決める。
2. **L1 の予算と上限の値**（仕様 §11「L0 完了時に記録」）。**§2.2 に実測から引いた提案を置いた**（件数 16、時間 8 ms、遅延 p95 8 ms / 最大 20 ms、DIRAM +8 KiB、低下 +5% 以内）。採否はユーザーが決める。
3. **割り込みハンドラを 1 本にする。** 今は `app_session` の 250 ms の壁時計 deadline だけが生きていて、guest.c の epoch 方式は死んでいる。L1 の予算判定をどちらに載せるか決める。
4. **drain の制御点。** `drain_jobs` は取り込み済みの guest.c にあるので改変できる。一方、`frame()` → drain → UI の順序を決めているのは、取り込んでいない `pocketjs_ui_qjs`（共有の `.cache`、読み取り専用）。L1 で順序を変えるなら、`pocketjs_ui_qjs` も取り込む必要がある。
5. **runtime の所有タスク。** 初期構成のまま `ui_task` にするか、専用タスクにするか。専用タスクにすれば、そのスタック分だけ DRAM が減る。
6. **L0 で見つかった既存の不具合の扱い**（VM の改造とは独立していて、`main` にも効く）:
   - GC 閾値が上限を超えている件。`JS_SetGCThreshold` を 1 行呼べば済む。
   - OOM 時の use-after-free。
   - 割り込まれた `await` の Promise が pending のまま残る件。

   それぞれ、`main` 側で直すか、L1 で扱うか、記録だけにとどめるかを決める。
7. **コミットとタグ。** L0 の作業は `vm/p0-foundation` にコミットと push を済ませた。`vm/main` へ `--no-ff` で戻す時期を決める。`vm-L0` のタグは、実機の値が揃ってから打つのが仕様どおり。
