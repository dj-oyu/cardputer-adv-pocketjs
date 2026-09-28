# Grid JS 記法

`gridFold.fold(spec, build)` は登録時に一度だけ `build` を実行し、型付き int16 grid IR を作る。描画中の出力座標・tap の反復と命令の実行は native 側が行う。`pocket.kasane.grid.register(program)` はその IR を解析し、`run(handle, buffers, params)` は合法性と費用を見て scalar/PIE を選ぶ。JS の式形だけでは PIE 使用を保証しない。

## 座標と入力

`gridFold.index` は **1個のオブジェクト** を受ける。

```js
const output = gridFold.index({x: 1, y: width});
const mirror = gridFold.index({base: width - 1, x: -1, y: width});
const source = gridFold.view({
  buffer: 0, x: 2, y: {param: 0, scale: 2}, tapX: 1,
  tapY: {param: 0}
});
```

添字は `base + x*X + y*Y + tapX*TX + tapY*TY`。省略した項は0である。各項は整数、または `{base, scale, param}` と書く。後者の値は `base + scale*params[param]` で、`param` は0〜7、`scale` の省略値は1、`base` の省略値は0。係数は登録時に int32 の定数と int16 の倍率へ検査・変換される。`index(0, 1, width, 0, 0)` の位置引数形式と係数の配列形式は受け付けない。

`gridFold.view({buffer, base, x, y, tapX, tapY})` は登録時に入力 buffer ID と座標式を束ねる。従来の `view(buffer, axes)` は受け付けない。式の中では `g.load(source)` と書ける。直接 `g.load(buffer, gridFold.index({...}))` と書いても同じ IR になる。`g.acc` は出力セルごとに `spec.initial` で初期化され、tap ごとに更新される。出力セルは `spec.output` の添字へ書く。

```js
const at = gridFold.index({x: 1, y: width});
const program = gridFold.fold({
  width, height, tapWidth: 1, tapHeight: 1,
  output: at, shift: 2
}, g => g.add(g.acc, g.mul(g.load(0, at), g.constant(2))));
```

この前段は `CONST`、`LOAD`、`ADD`、`MUL`、`MIN` と有限の矩形反復を表す。`g.min` や出力面からの逐次参照も書けるが、未対応の式は合法性確認後に scalar で実行される。float を暗黙に Q14 へ変換しない。PIE 候補と選択理由は実行後の `grid.explain(handle)`、登録費用と解析量は `grid.registration(handle)` で確認する。

## 実行時間の計測

`grid.run(handle, buffers, params, {backend: 'AUTO'|'SCALAR'})` の第4引数で比較用の実行経路を指定できる。通常は第4引数を省略して自動選択する。`SCALAR` はその実行だけPIEを無効にする。入力、出力、表示資源は同じhandleを使う。

`grid.profile(handle)` はそのhandleで成功した `run` の累計を返して計数をリセットする。返り値は `runs`、`copyUs`（JS入力の取得・native領域へのコピー）、`bindUs`（合法性検査と選択）、`kernelUs`、`totalUs`、`maxTotalUs`、`heapFree`、`heapLargest`、`heapMinFree`。時間はµs、heapは内部8-bit RAMのbyte。`totalUs` には候補画像の確定処理なども含むため、前3段の和より大きい。Kasaneの再合成とLCD送出はこの計数の外である。`heapMinFree` は起動以来の全体低水位で、profileの計数リセットでは戻らない。
