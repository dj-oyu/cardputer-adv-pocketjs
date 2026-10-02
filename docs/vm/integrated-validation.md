# codex/vm-improvements の統合検証

2026-10-02 UTC。`codex/vm-improvements` は検証済み候補をまとめる専用 feature branch。
`main` / `vm/main` には統合していない。新しい独立 worktree で作業し、既存の枝・
worktree は削除していない。ESP-IDF の対象ビルドと実機は、この統合版では未実施。

## 基点と取り込み範囲

fetch 後の `origin/vm/main` = `bd871f610f65c01bde21f83e8c5dec7888be5227` を基点にした。
各候補の元 commit を cherry-pick し、最新の FINISH 軽量化を保持した。
旧候補基点 `89a71ae72c721206dfbb63acfeffc74b38dde604` から新基点までに変わった
16 ファイルは統合後も新基点とバイト一致する。

| 対象 | 元 commit | 統合時の扱い |
|---|---|---|
| A frame entry | `22e2e14f2ce4da33c4b1dabdc4375b16f81c6406` | 直接入口・例外報告を保持 |
| C parser shrink | `ea72fbb90c8551f2e069c550b0f14a08bde31150` | parser の余剰容量返却 |
| E array trim | `c17d3c5cfee9996994f50bc4bdbc4b15e4058e75` | quota 圧迫時の空 fast array の backing 返却 |
| F precompile | `1e14ab786f7db671fd12cc7ffa779b3f8350d718` | build-only 実験、既定 OFF。runtime loader は変更しない |
| H typed put | `85798a73ccf75ef243dde3728fe163f58420e27a` | 既定 OFF を保持 |
| Y scoped grid borrow | `c5a4993f399e57c1df31054373f1c42cf62b9e57` | scoped inputs と明示 measure 入力 |
| plan compiler | `754c76f0f4b99c8524c2c83c7efefc54665366ee` | source 保持、loop shadow 拒否、警告伝播の3修正 |
| Windows 検証道具 | `9731b5eb9c47bf650de92f423b8c5e7d8218fec0` + `8fc0043df1e7979d660f7ef3fb02bcf789c09659` | pinned 比較と native exit code 修正を保持 |

B / D / G / I / X と lazy-OOM 改修は含めていない。
統合に伴う production code の追加変更はない。通常検査だけを選べる test mode と
統合版の build-only 道具を追加した。

## 競合の解消

production / compiler code の競合はなかった。QuickJS 共有ファイルの C / E / H / Y
は別の関数・分岐への変更で、すべて自動適用された。

- `docs/README.md`: C / E / H の隣接する索引行をすべて残した
- `docs/platform/test-commands.md`: F と compiler のホスト検査コマンドを両方残した
- `docs/vm/cloud-validation-89a71ae.md`: add/add。A / C / E / F / H の元レポートを
  候補ごとの履歴として全部残し、統合版の結果と区別した
- C / E 共通の `fault_allocator.h` は同一内容だった

履歴文書の古い OOM・device pending・独立候補という記載は、その時点の記録。
今回の結果はこの文書を正とする。

## 今回のホスト検証

Linux x86-64、vendored QuickJS と本番 source を使用。ASan/UBSan は
`ASAN_OPTIONS=detect_leaks=0:halt_on_error=1` を指定した。
LeakSanitizer は実行環境の ptrace 制限で使用できない。ログと生成物は
`.cache/integration/` 以下に保存し、Git に含めていない。

- A: `test_frame_entry.sh o2 --functional-only` と `asan --functional-only` が PASS。
  本番 guest / console / source-entry、yield/resume、receiver/argv の GC 保持、40連続呼出し、
  async/job の順序と例外、18例外形式、旧表示との11比較、watchdog、通常停止・破棄8例を確認
- scheduler clock: O2 / ASan+UBSan で call-relative 時間、wrap、count mode が PASS
- flash budget: 2 tests PASS（ホストの予約境界テストのみ）
- QuickJS: H=0 / 1 の両方で構文確認、`-Werror=incompatible-pointer-types` が PASS
- production / compiler の単独候補所有9ファイルは元 commit と完全一致

- compiler: `test_kir.mjs` / `test_plan_js.mjs` / `test_lower_plans.mjs` が PASS。
  現行 DERBY は20 plan・774命令・32 ROM patch。file/directory lowering の JS は一致し、
  lower/ROM の IR・命令数・警告は全20件一致。生成 C は `-Wall -Wextra -Werror` でコンパイル
- F: 通常26件が通常ビルド / ASan+UBSan とも PASS。source/bytecode の意味論14対と
  例外位置1対、現行 DERBY 6 chunk の生成・manifest 検証、実 CLI の `--verify` を確認。
  CMake そのものは使用できず、既定 OFF・opt-in 境界・source embed 保持は静的確認のみ
- 実 QuickJS + Kasane renderer: H OFF、ASan+UBSan、通常 LIGHT 1レースが
  `NORMAL_RESULT RESULT` と `DERBY_HOST PASS` で終了。CSV は1,474フレーム。
  全 tier / カメラ / race matrix は実行していない

- C: O2 / ASan+UBSan で moving shrink・zero・regrowth と5 program の
  compile/serialize/read/execute/reuse が PASS。完全 bytecode の基点比較は162件一致、
  8件が同じ compile rejection、差分0。後者は compile-only で JS 自体を実行しない
- E: O2 / ASan+UBSan で容量1/2/3/16/512/4096、閾値 unlimited/below/exact、
  非確保GC・通常再成長、cycle 回収、holey length、非空保持、push/unshift/pop と
  prototype setter が PASS。allocation 拒否・OOM 復帰は対象外
- Y: scoped-only の H OFF/ON × O2/ASan+UBSan、計4構成が PASS。実 QuickJS→PIE model→image で
  scoped input、measure、所有と通常 lifecycle を確認。追加の通常 resize 入力も
  各構成21検査、計84検査が PASS（offset/shared/unaligned、getter変更、RAB成長、detach寿命）。
  旧 harness 全体と I/O/ACK の故障注入は除外
- Windows 道具: Python offline 17件 PASS（mock 9・新規静的 guard 8）。
  PowerShell 本体が無いため native parser/runtime は未実行。
  `Build-Validation.ps1` / `Common.ps1` / `Flash-Validation.ps1` / `Test-Device.ps1` は
  元 `8fc0043` とバイト一致
- Node/Python/bash 構文、diff whitespace、一般的な credential pattern の差分確認が PASS

- H: inspect 済み通常 corpus 42件 × OFF/ON × O2/ASan/O2強制yield = 252 pass、失敗0。
  各構成に typed-put の791検査を含む。OFF/ON ごとに別ディレクトリで build し、
  object cache と flag を混在させていない

## 再実行時の範囲

通常検査と allocation fault / OOM 試験を混同しないこと。今回 allocator の拒否、
故障注入による OOM、lazy-OOM の再現、hardware / serial 操作は実施していない。
既存の全 suite の既定動作にはそれらが含まれるため、そのまま全実行した結果ではない。

```bash
export ASAN_OPTIONS=detect_leaks=0:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
export VMTEST_CFLAGS='-fno-pie -no-pie'
export VMTEST_OUT="$PWD/.cache/integration/recheck"
bash tools/vmtest/test_frame_entry.sh o2 --functional-only
bash tools/vmtest/test_frame_entry.sh asan --functional-only
bash tools/vmtest/parser_trim.sh o2 --functional-only
bash tools/vmtest/array_trim.sh o2 --functional-only
node tools/kasane_ir/test_kir.mjs
node tools/kasane_ir/test_plan_js.mjs
node tools/kasane_ir/test_lower_plans.mjs
python3 tools/test_precompile_apps.py --cc gcc --functional-only
python3 tools/test_precompile_apps.py --cc gcc --functional-only --sanitize
python3 tools/kasane_contract/run_pocket_grid_qjs.py --scoped-only --out .cache/integration/grid-recheck
python3 tools/kasane_contract/run_pocket_grid_qjs.py --scoped-only --sanitize --out .cache/integration/grid-recheck-asan
python3 -m unittest discover -s tools/device_validation -p 'test_*.py' -v
```

H の通常 corpus は以下の42件。`regexp_oom` は名前に OOM があるが、内容は通常の
正規表現と構文エラーの意味論テストであり、割り当て拒否を使わない。

```bash
tests=(array_push_length_hole bench_alloc bench_calls bench_closure bench_generator
       bench_loop bench_promise bench_proxy bench_sort builtin_reentry closures
       coro_closure_gc coro_prologue_gc error_toplevel generators job_throw
       l2b_async_flat l2b_flat_calls l2b_floor_argc lazy_builtins lazy_call_inputs
       lazy_intrinsics microtask_order promise_chain regexp_oom rejections rom_atoms
       seg_add_deep seg_boundary_bigframe seg_closure_survives seg_generator_frames
       seg_return_reuse special_calls sync_loop try_finally typed_put_int_fast
       yield_async_from_sync yield_dynamic_import yield_module_tla yield_job_tails
       yield_thenable yield_native_reentry)
for h in 0 1; do
  export VMTEST_OUT="$PWD/.cache/integration/h-recheck-$h"
  export VMTEST_CFLAGS="-fno-pie -no-pie -DPOCKET_VM_TYPED_PUT_INT_FAST=$h"
  bash tools/vmtest/build.sh all
  bash tools/vmtest/run.sh --variant o2 "${tests[@]}"
  bash tools/vmtest/run.sh --variant asan "${tests[@]}"
  bash tools/vmtest/run.sh --variant o2 --force-yield "${tests[@]}"
done
```

## Windows での次の検証

既存 `Build-Validation.ps1` の baseline=89a71ae と Y を含む各 candidate のピンは
変更していない。統合版は別の `Build-Integrated.ps1` を使い、**レビューした
完全な commit SHA** を `-Commit` に渡す。fresh RunRoot、detached worktree、
ビルド別 sdkconfig で隔離する。H / F は明示 OFF、build-only で serial を開かない。
詳しくは [端末検証手順](../../tools/device_validation/README.md) を参照。

PowerShell runtime、ESP-IDF firmware、ESP32 の32bit ABI、物理 LCD、heap、FPS、
実機タイミングは今回のホスト結果では証明していない。A の bound/proxy の native floor
制約、F の runtime 未接続、H の既定 OFF、Y の API 変更は各候補文書どおり残る。
Y の shared input は capture 中の並行 write/grow の排他が必要。外部呼び出し元も
`measure(handle, buffers, params, repeats, strategy)` へ追従する必要がある。
