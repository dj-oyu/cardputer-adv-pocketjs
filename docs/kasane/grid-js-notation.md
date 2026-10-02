# Grid JS 記法

`gridFold.fold(spec, build)` は登録時に一度だけ `build` を実行し、型付き int16 grid IR を作る。描画中の出力座標・tap の反復と命令の実行は native 側が行う。`pocket.kasane.grid.register(program)` はその IR を解析し、`run(handle, buffers, params?, options?)` は合法性と費用を見て scalar/PIE を選ぶ。JS の式形だけでは PIE 使用を保証しない。

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

## 実行と入力の寿命

`grid.run(handle, buffers, params?, options?)` は同期実行する。`buffers` は buffer ID をキー、`Int16Array` を値とする入力mapで、`params` を使わなければ省略または `undefined` にする。`grid.run(handle, buffers, params, {backend: 'AUTO'|'SCALAR'})` の第4引数で比較用の実行経路を指定できる。通常は第4引数を省略して自動選択する。`SCALAR` はその実行だけPIEを無効にする。入力、出力、表示資源は同じhandleを使う。

通常の `ArrayBuffer` を backing store とする `Int16Array` は、入力をnativeの保持領域へコピーせず、**その呼び出し中だけ借りる**（scoped borrow）。view と backing buffer のJS参照をnative実行の終了までローカルに保持し、入力ポインタも参照もhandleへ残さない。ポインタ取得からカーネル終了まではJSコールバックを呼ばないため、通常のJS操作で途中にdetachやresizeが割り込むことはない。`run` が返った後は、呼び出し元が入力を書き換えてよい。

出力の `DEST` は従来どおりnative専用の二重バッファで、JS入力とは共有しない。確定済み出力を候補側へseedしてから実行し、表示資源の確定・ACKの規則も変えない。入力を借りる期間は表示完了まで延びない。

### getter と入力確定の順序

`run` は次の順で入力を取得する。

1. `options.backend` を取得・検査する
2. `DEST` 以外の入力mapのプロパティを buffer ID の昇順で取得し、返されたJS値を保持する。`undefined` / `null` は入力なし、それ以外が `Int16Array` でなければその場で拒否し、後続のgetterへ進まない
3. `params` の長さと各値を取得・検査する
4. すべてのJS getterを終えてから、保持した各viewの**現在の**長さ・offset・backing buffer・ポインタを一度だけ取得・検査し、native実行へ進む

これは入力ごとのコピーをgetterの途中に挟んでいた実装からの変更である。たとえば入力0を返した後に入力2や `params` のgetterがそのviewの要素を書き換えれば、**書き換え後の値**で実行する。後続getterがdetachやresizeを行った場合も、変更後のメタデータで検査する。入力0を取得した時点の内容を保存するスナップショットではない。一方、後続getterが `buffers[0]` を別のviewへ差し替えても、既に取得したviewを使い、mapを読み直さない。各param値はそのgetterを読んだ時点の値を使う。

共有backing buffer（`SharedArrayBuffer`）と、nativeから渡された奇数アドレスのbacking bufferは、呼び出し内だけのコピーへフォールバックする。このコピーもhandleには保持しない。通常の借用入力には入力用のnative領域を確保せず、フォールバック時だけ毎回一時領域を確保・解放する（従来のhandleごとの入力capacity再利用は行わない）。共有入力の取得・コピー中に、nativeや別スレッドから内容を書き換えたりbacking bufferを成長させたりする使い方は未対応で、同時変更に対する原子的なスナップショットは保証しない。native管理の外部 `ArrayBuffer` は、借用・実行中ずっとbacking storeを生存させ、解放・移動・サイズ変更・同時書き換えを行わないことが呼び出し元の責任になる。

共有入力のscoped-copy対応は、firmware全体でgrowable `SharedArrayBuffer` を使えるという保証ではない。このQuickJSのgrowable SABはmaxByteLength分を確保するSAB allocator callbackの設定を前提とするが、現在のfirmwareのguestはその設定を行っていない。hostのgrowable SAB検査はcallbackを設定したruntimeで行う。既存のruntime側の制約はこのgrid変更では修正しない。

## 実行時間の計測

### 明示した入力でカーネルを比較する

`grid.measure(handle, buffers, params, repeats = 8, strategy = 'AUTO')` は、渡された**現在の入力とparams**でscalarとPIEを比較する。foldのhandleを使い、事前の `run` は不要である。paramsがなければ `undefined` を渡す。`repeats` は1〜16、`strategy` は `'AUTO'` / `'GATHER'` / `'AFFINE'`。返り値は `repeats`、`scalarUs`、`pieUs`、`strategy`、`equal` で、時間は各カーネルを `repeats` 回実行した合計のµs。両経路が利用でき、出力が一致した場合だけ成功する。

```js
const buffers = {0: source, 2: weights};
const params = [pitch];
grid.run(handle, buffers, params);
const measured = grid.measure(handle, buffers, params, 8, 'GATHER');
```

旧形式の `grid.measure(handle, 8, 'GATHER')` は使わない。入力は `run` にも `measure` にも毎回明示する。`measure` 用に前回の入力内容やparamsをhandleは保存しないため、前回の実行を比較したければ、**同じ内容の入力と同じparam値**を呼び出し元が用意する。同じ `Int16Array` オブジェクトを渡すだけでは、途中で内容を変えていた場合の再現にはならない。getterによる変更と取得の時点も同様に考慮する。

計測はscalar用とPIE用に独立した、ゼロ初期化されたscratch `DEST` を使い、表示中の出力や候補出力を変更しない。前回の `run` の確定済み `DEST` をseedしないので、逐次参照や `DEST` に依存するfoldでは、同じ入力・paramsでも前回の出力状態を再現するものではない。`measure` は入力取得や表示処理を含むアプリ全体の計測ではない。

### 実際の run の累計を読む

`grid.profile(handle)` はそのhandleで成功した `run` の累計を返して計数をリセットする。`measure` の再実行とは別のAPIである。返り値は `runs`、`copyUs`、`bindUs`（合法性検査と選択）、`kernelUs`、`totalUs`、`maxTotalUs`、`heapFree`、`heapLargest`、`heapMinFree`。

`copyUs` は互換のため名前を残しているが、現在はbackendやparamsを含む**引数・入力の取得と `DEST` のseed**にかかった時間を表す。通常の入力のmemcpy時間ではない。例外的なscoped-copy経路ではそのコピー時間も含む。

時間はµs、heapは内部8-bit RAMのbyte。`totalUs` には候補画像の確定処理なども含むため、前3段の和より大きい。入力のローカル参照とscoped-copy領域の解放、および返り値の生成は計時終了後であり、`totalUs` に含めない。Kasaneの再合成とLCD送出はこの計数の外である。`heapMinFree` は起動以来の全体低水位で、profileの計数リセットでは戻らない。
