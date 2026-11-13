# キャラクター評価とShiki frameの責務

現在の公開mesh経路は `pocket.ayatsuri.modelOpen(buffer)`、共有Modelからの
独立instance、`pocket.kasane.shiki` のframe APIを使う。
PR #110のnative sceneと軽量motionは[optional JS層](scene-motion.md)として抽出した。
旧動的character engineや資産・デモ全体を取り込んだものではない。旧例の `pocket.kasane.procedural` は
Shiki v2で削除済み。現行frame ownerは `pocket.kasane.shiki` である。

アプリまたは環境adapterが時計、入力、tick/seek/pause、begin、commitと提示完了を
所有する。utilityの `scene.evaluate(time)` は渡された時刻の評価であり、内部時計や
暗黙の呼吸・乱数・timerを進める契約ではない。native instanceの
`evaluate(camera, light)` 自体には時刻引数がない。motion layerは左右や行き帰りの
対称性を仮定せず、必要なtarget値を明示的に設定してから評価する。

## 所有権と失敗

- Modelのcold dataは読み取り専用で共有できる。instanceごとのpose、評価work、
  dirty状態、借用packetは独立している。
- instanceから得るpacket/viewはinstance所有の再利用workであり、immutable
  snapshotではない。次の変更・評価・disposeをまたいで保持しない。
- `submit` と `submitClipped` は同期中に検証し、候補frameへ必要な数値をコピーする。
  成功したframe snapshotはinstance/model/inputから独立する。submit後のsource破棄を
  理由に、提示待ちframeのstorageを解放してはいけない。
- core評価に到達した後の失敗ではinstanceを未評価状態にしてsubmitを拒否する。
  arity・型・buffer長などJS入口の検証失敗はcoreを呼ばず、以前の評価状態を保持する。
  他instanceと既存committed画像は変更しない。core失敗後は正常な再評価で復帰する。
- submitの容量・work・record上限失敗は呼出し全体をrollbackする。失敗した候補を
  アプリがcommitして成功と扱わない。新しいbeginから組み直す。
- commitは提示成功ACKではない。image IO失敗ではcandidateを保持してretryし、
  backdrop修復は旧committedを使う。提示成功でのみcommittedへ進め、resetで解放する。

## LCDと深度workspace

mainのdesign-0 LCD経路は4行の借用half-stripを使い、DMA完了前に再利用しない。
meshのdepth workspaceは別資源で、1行960 Bまたは8行7680 BのF32配列である。
8行depth workspaceをLCDの借用8行scratchと解釈しない。33ms cadence、PGO OFFと
通常のbuild defaultsを維持する。

## 検証の範囲

`python tools/kasane_contract/run_mesh_depth.py` は共有Modelの非対称2 instance、
片側の評価失敗と復帰、source破棄後の描画、両submit、OOM後の回復、rollback、
retry/ACK、depth-only damageとresetを実C/QuickJS、ASan/UBSanで検査する。
`run_meshbench_app.py` と `run_mesh_memory.py` は公開合成入力の描画・要求メモリを測る。
これらはPR #110 JS utilityとの全評価意味論の同一性、私的foxの品質、実機速度や
総RAM fit、HOME background adapter実装の証明ではない。
