# QuickJS 改造のブランチ運用

`docs/quickjs-freertos-vm-spec.md` の L0〜L5 を進めるためのブランチと作業ツリーの規則。

## ブランチ

```text
main ──────────●──────────────●────────────────●──── 出荷できる状態を保つ
                \   (取り込み) ↓ merge            ↑ merge（段階の関所のみ）
vm/main ─────────●──────●──────●──────●───────────●── 統合。常にビルドが通る
                  \    /        \    /
vm/p0-foundation   ●──●          ●──●  vm/l1-host-sched …  段階ごとの作業ブランチ
```

- **`vm/main`**: 改造の統合ブランチ。常にファームがビルドでき、ホストの差分実行が通る状態で保つ。push 済みの履歴は書き換えない（force push しない）。
- **`vm/<段階>-<題目>`**: 作業ブランチ（例: `vm/p0-foundation`、`vm/l1-host-sched`、`vm/l2a-segments`）。`vm/main` から切り、関所の検査を通したら `--no-ff` で `vm/main` へ戻す。完了した段階には `vm-L0`、`vm-L1` … のタグを打つ。
- **`main` → `vm/main`**: 各段階の開始時に `main` を merge で取り込む。rebase はしない（push 済みのため）。
- **`vm/main` → `main`**: 段階の関所でのみ、ユーザーが判断して行う。新しい経路はビルド時選択で既定を従来経路にしておき、`main` の挙動を変えない（仕様 §12）。

## QuickJS とゲストの取り込み

- quickjs-ng は `vm/p0-foundation` でリポジトリへ取り込む。**最初のコミットは Espressif Registry 0.14.0 と同じバイト列**にし、以降の改変はすべて別コミットにする。`git diff <取り込みコミット>..vm/main -- <取り込み先>` が改変の全体になる。
- 上流を更新するときは、取り込みコミットの上に新しい版を置き、改変をその上に載せ直す。
- `.cache/` と `managed_components/` の直接編集を成果物にしない（仕様 §3-8）。

## 作業ツリー

改造は別の作業ツリーで行い、`main` の作業ツリー（他セッションが共有している）には触れない。

```text
C:\devs\m5stack\cardputer-adv-pocketjs      main（共有）
C:\devs\m5stack\cardputer-adv-pocketjs-vm   vm/*（本改造専用）
```

- 改造側の `.cache/` は、読み取りだけの依存（`pocketjs`、`native`、`bmi270`、`codecs`）を `main` 側へのジャンクションで共有し、改変しうる `components/pocketjs_guest` は複製する。
- ジャンクションにより `dependencies.lock` のパスが書き換わるため、改造側の作業ツリーでは `git update-index --skip-worktree dependencies.lock` を設定してある。コミットに含めない。
- ビルドディレクトリは `build_vm*` を使う。
- 実機（COM3）は1本しかない。書き込みと計測は、ユーザーが実施するか、明示の許可を得てから行う。
