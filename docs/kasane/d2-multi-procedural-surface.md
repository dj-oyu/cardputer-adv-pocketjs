# D2: JS手続き面を2枚にする最小経路

既存の`kasane.procedural.beginFrame(color)`、`draw(handle, inputs)`、`commit()`、`resource()`は面0を使う。追加面はセッション内で一度だけ作れる。`createSurface({maxSegments: n})`（n = 1〜1,024）でその面のフレームを n 本分に縮められる（[面ごとの線分上限](surface-segment-cap.md)）。

```js
const proc = pocket.kasane.procedural;
const foreground = proc.createSurface();
const backImage = proc.resource();
const frontImage = proc.resource(foreground);

pocket.kasane.replace(tx => {
  tx.background(0x000000ff);
  tx.image({resource: backImage, bounds: [0, 0, 64, 48]});
  tx.image({resource: frontImage, bounds: [80, 0, 144, 48]});
});

proc.beginFrame(0x0000);
proc.draw(plan, [10, 10, 0, 0]);
proc.commit();
// 表示ACK後の別turnで更新する。
proc.beginFrame(0x0000, foreground);
proc.draw(plan, [20, 10, 0, 0]);
proc.commit();
```

`proc.unregister(handle)` は登録済みplanと点列領域を解放し slot を空ける（引数はちょうど1つ、戻り値 `undefined`）。未登録・解除済みの handle は `draw` と同じく `CLOSED`、引数不正は `INVALID_ARGUMENT`。handle 番号は再利用しないので、解除済み handle が後の plan を指すことはない。`draw` は線分を候補 frame へ写し終えているため、`beginFrame`〜`commit` の間に解除しても既に描いた分は残る。

登録済みplanは両面で共有できる。面の候補・確定frameは別々の2スロットで、描画中のscratchと8行画像cacheだけを共有する。表示待ち候補は全体で1面までとし、次の`beginFrame`はACKまたはrepairまで`BUSY`になる。面IDはAPPセッション終了で失効し、同じ番号を再利用しない。追加面は`resource(id)`を呼び出した後に`commit()`できる。APP終了ではnative画像資源を先に退役させてからframeを解放する。

FLOWERを背景にするoverlayでも画像資源としての手続き面は更新できる。overlayの表示経路は、画像面の成功ACKとI/O失敗を通常表示と同じくadapterへ返す。native FLOWER下地に描かれない従来の手続きbackdropはoverlayで`commit()`を拒否する。backdropのI/O失敗で旧確定frameのrepairが残る間は、全surfaceの新しい`beginFrame`を`BUSY`にする。

背景色が変われば画像全体をdamageとする。同色の場合は旧・新線分の外接矩形をsource座標で計算し、1:1画像ノードだけ表示座標へ写す。拡大・縮小・回転を伴う画像ノードは全表示矩形へ戻す。LCD矩形転送は16列単位へ丸める。表示失敗後は候補を同じticketで保持し、Kasaneの全面repairが成功してから確定する。

hostでは実QuickJS→手続きIR→2つのKasane画像ノード→rendererを通し、各面の独立した画素、1点移動の256 B転送、途中転送失敗後の64,800 B repair、APP reset後の両資源STALEをO0/O2で確認した。fake PIEの計画実行経路も同じ試験を通した。

通常APPの実機gateは、`KASANE_D2_MULTI_SURFACE_PROBE=ON`でVIDEO LABを一時的に`apps/kasane/proc_multi_surface_probe.js`へ差し替えた。二つの64×48画像ノードを別位置へ配置し、毎turn交互に1点を移動する。COM3で2回起動し、どちらも64回表示・57回の256 B矩形転送を記録した。2面確保後の内部heap最小空きは95,480/95,484 B、最大連続空き51,200 B、UI task stack最小余裕23,708 B。1点更新の表示時間は約0.7 msで、初回全画面表示と面初期化は別に集計した。ログは`.cache/d2-multi-surface-device-20260928a/serial.log`。診断後は通常imageを復元してflash照合・`HOME_READY`を確認した。FLOWERとの同時表示と実LCD失敗注入はこの実機gateに含まない。

別の`KASANE_D2_OVERLAY_PROC_PROBE`では、FLOWERをnative backdropとして2つのJS手続き画像面を8 turnずつ2回表示した。各回の最初の候補がpendingの4帯目でLCD送出を1回失敗させ、候補を保持して64,800 Bの全面再送後にACKし、もう一方の面と交互の更新を続けた。`tools/kasane_contract/run_d2_overlay_proc_device.py`は背景とoverlay設定を元に戻す。ログは`.cache/d2-overlay-proc-device-20260928c/serial.log`、診断image SHA-256は`f3a4a3ddebe8675ce2940a9084f87179fcdec85b663b068f70a02db9ae45d1e6`。通常imageへの復元・flash照合・`HOME_READY`まで確認した。D4 pixelとの同時使用は未検証なので診断フラグを相互排他とした。この短いgateはFLOWERとの長時間性能、音声や低heapとの同時負荷を保証しない。
