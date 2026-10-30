# ayatsuri 親子変換の共通評価核

`feat/ayatsuri-common-core`、基準`1a71235`からの最小縦断実装。PR #110の検証候補を変更せず、3体のキャラappが使う配置と親子変換だけをC/JS共通にする。host simulation、製品リンク、実機の結果は[計測記録](../perf/character-materials.md)で区別する。実機での性能改善は未確認。

## 実装する境界と残る範囲

`main/ayatsuri/ayatsuri_transform.h`の純粋C評価関数を、C利用側とJS native adapterが同じ実装として使う。JSの`Ayatsuri.scene()`はscene graph、変更依存、可視性、材質、UV、法線/射影の評価、renderの順序を引き続き所有する。この段階はCのscene/handle API全体や共有rendererの完成ではない。

利用者のJSは従来どおり`part.place({position, rotation, scale})`と`scene.evaluate(time)`を呼ぶ。内部の`pocket.ayatsuri.compose`が共通核へ接続し、device側に別のJS合成数式を残さない。host専用の数値referenceは検証用で、native API不足を埋めるdevice fallbackには使わない。honeの描画/変形APIとayatsuriの評価adapterは別の名前空間とする。

最初の題材は人・キツネ・雲の親groupの傾き、部位の配置、圧縮伸長。モデル固有の名前や演技をC核へ入れない。HOMEへの接続、C sceneの生成/破棄・世代handle、材質/UV/clip全体の共通化は後続であり、broker/登録flowやflashの問題を回避するための入口を追加しない。

## 数値と座標

- xは右、yは下、zは手前。角度はrad。列ベクトルに対する`T * Rz * Ry * Rx * S`、worldは`parentWorld * local`。
- poseはposition/rotation/scaleの9個、world matrixはrow-majorの3×4 affineの12個。C/JSとも保存型はIEEE binary32相当の`float`/`Float32Array`、式の中間は`double`/JS Number。位置やscaleへ固定小数点や整数化を追加しない。
- pose、parent、出力範囲は有限値を必要とする。scaleの各要素は0を拒否し、負scaleは許す。中間結果のbinary32変換はJSのMath.froundと同じnearest/ties-to-evenを使う。最大有限値をわずかに超えても有限値へ丸め戻る範囲は許し、Infになる境界以降を出力書込み前に拒否する。subnormal/0へのunderflowも通常の丸めに従い、後段の法線評価の縮退判定を置き換えない。
- C利用側はIEEE binary32/binary64、binary64を超える過剰精度を持たない中間評価と、既定のnearest/ties-to-even環境を使う。fast-mathを使わず、`-ffp-contract=off`でコンパイルする。x87の拡張精度を使う32bit hostでは`-msse2 -mfpmath=sse`等で評価条件を揃える。kernelはglobalな丸めmodeを変更しない。firmwareのnative adapterとhost試験も同条件にする。CとJSのtrig実装差を実機で未確認のまま全入力のbit一致保証にはしない。

## C APIと所有権

初版のC核はheader-onlyで、ESP-IDF/QuickJSやallocatorに依存しない。型と実際の署名はheaderを正とし、poseは`position[3]`、`rotation[3]`、`scale[3]`、transformは`m[12]`を持つ。

```c
ayatsuri_status ayatsuri_transform_evaluate(
    const ayatsuri_pose *pose,
    const ayatsuri_transform *parent, /* NULL for a root */
    ayatsuri_transform *out);
```

例えばCで親の傾きを評価する呼出しは次の形になる。公開画像の更新はこの関数の外で行い、返却statusを確認してから後段へ進む。

```c
ayatsuri_pose body = {
    .position = {0, 0, 0}, .rotation = {0, 0, 0.2f}, .scale = {1, 1, 1}
};
ayatsuri_transform body_world;
ayatsuri_status status = ayatsuri_transform_evaluate(&body, NULL, &body_world);
/* Use body_world only when status == AYATSURI_OK. */
```

入力と出力のstorageは呼出し側が所有する。入力は同期呼出しの間だけ読まれ、pointerも内容も関数内に保持しない。内部候補を検証した後だけ`out`を更新する。失敗時は`out`全体が以前のbytesを保つ。`out == parent`は許すが、Cの型に反するpointer castや破棄済みpointerの検出はAPIの保証に含めない。

関数はmalloc、clock、所有するglobal mutable state、native frameの開始/公開、callbackを持たない。算術のFP例外flagは通常どおり発生し得る。異なるstorageの独立呼出しは状態を共有しない。同じstorageの同時変更を防ぐ責任はCのownerにあり、mutexやtask切替を内部へ隠さない。

| C status | 条件 | JS adapterのcode |
| --- | --- | --- |
| `AYATSURI_OK` | 全入力と全出力の検証に成功 | `undefined`で成功 |
| `AYATSURI_INVALID` | pose/outがNULL、入力が非有限、scale要素が0 | `INVALID_ARGUMENT` |
| `AYATSURI_RANGE` | binary32へ丸めた出力が非有限になる | `LIMIT_EXCEEDED` |

JSの型/長さ/immutable出力の不正はC呼出し前に`INVALID_ARGUMENT`となる。Cのpointerが指すstorageの存在/長さ/整列は呼出し側の責任であり、C核に任意のpointerを検査する機能はない。

## JS adapterと更新順

内部adapterの署名は`pocket.ayatsuri.compose(pose, parent, out)`。poseは`Float32Array(9)`、parentは`null`/`undefined`または`Float32Array(12)`、outは書込み可能な`Float32Array(12)`を受ける。成功時の返却値は`undefined`。通常のauthoringでこのbufferやoffsetを手作業する必要はない。

この内部namespaceは既存`kasane.shapes` manifest gateのセッションへ同じinstall経路で載せる。新しい権限/capabilityやapp_sessionの入口を増やさない。C kernel自体はmanifestやPocketJSを必要としない。

adapterはtyped viewの型/長さ/書込み可否を検証し、同期呼出し中にだけ借用する。C型への入力snapshotと成功した出力の反映を境界にし、入力とoutが同じbacking storeの重なるviewでも部分更新されたparentを再読しない。CのINVALIDは`INVALID_ARGUMENT`、RANGEは`LIMIT_EXCEEDED`へ対応させる。このadapterはbufferを後のframeまでretainせず、API呼出し自体でnative画像を公開しない。

入力snapshotはpose 36Bと親がある場合の48B、JS出力への反映は成功時の48Bである。C核にも候補matrixのstagingがあり、compilerがどこまでcopyを除くかは別の実測対象とする。utilityのcopiedBytes=0はJS内bulk copyの範囲で、native境界のcopyやアプリ全体のcopy 0を意味しない。

JSの更新順は`setter → scene.evaluate(time) → appのbeginFrame → scene.render() → commit → 提示ACK`のまま。共通核の呼出しはtransformまたは親のworldが変わったnodeに限定する。光/色/UVだけの変更で親子行列を作り直さない。timeを読む隠れたclockはなく、同じstored pose/parentならseekや呼出し間隔によらず同じ評価になる。

既存のscene busy guardは評価/visitor/backend callback中の同sceneへの再入・変更・disposeを拒否する。transform単位のC出力は成功時だけ更新するが、scene全体を二重bufferでrollbackする実装へ変えたわけではない。他nodeを評価した後の失敗ではsceneを未評価状態にし、render/visitを拒否し、修正後のevaluateで復帰する。appは失敗した候補をcommitせず、hone側の最後の成功した画像と提示ACK契約を維持する。

## 検証と未保証

直接C利用でroot/親子/非一様/負scale、入力不変、parentとoutのalias、NaN/Inf/0scale/overflow、失敗時のcanaryと全出力不変を検査する。host専用JS referenceとの数値比較はC出力の検算であり、二つのdevice実装を許す理由にはしない。

キャラappでは3体と同じ操作/照明を使い、更新なし、親だけ、部位だけ、光だけ、UVだけの依存を検査する。実QuickJS/native経路で前のcommitted frameを保つ失敗/復帰、再現可能なframe位置、強制GCの有無、heap制約と資源再利用を回帰させる。JS heap/native free/最大連続領域/stack/最大単一alloc、hostとdeviceの時間は混同しない。

この文書は契約と検証対象を示す。host試験が通った項目、同head CI、実機未確認は[計測記録](../perf/character-materials.md)とPRの実績を正とし、未実施をPASSにしない。公開名が増えたことだけでCフレームワーク全体やHOME利用が完成したとは扱わない。
