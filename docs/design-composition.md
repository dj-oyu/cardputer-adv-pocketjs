# コンポーネント合成・キャッシュ・モーダル仕様 v0.2

2026-09-13。設計として確定する契約。実装完了を意味しない。
本書は[基本仕様](design-system.md)の重なり・透過・部品・モーダルを拡張し、該当するv0.1の制限に優先する。
既存の座標、UTF-8、owner task、2バンク、提出ID、失敗時再送の契約は維持する。

## 1. 必須と追加機能

| 優先度 | 項目 | 完了条件 |
| --- | --- | --- |
| MUST | 部分・全体の重なり | 移動、隠蔽、再露出、順序変更が全面参照描画と一致 |
| MUST | 透過 | 色alpha、coverage、命令opacity、部品全体opacityを区別して合成 |
| MUST | モーダル | 開閉、入力排他、失敗時維持、通知との共存、背景の最新状態復帰 |
| MUST | 明示的キャッシュ | 作成、複数同時表示、非表示保持、再表示、破棄、容量エラーと寿命検証 |
| BETTER | 軽量な更新参照API | JSオブジェクトから明示的に局所更新。不要な木走査や毎フレーム割当なし |
| BETTER | 視覚効果 | blur、2D変形、平面の3D投影を同じ入出力・予算・damage契約で定義 |

MUSTの機能を未実装のfallbackだけで完成扱いしない。BETTERの演出失敗は必須の操作や表示を失わせない。
ネイティブに自動レイアウト木や命令ごとのheapオブジェクトを追加しない。

## 2. 部品と重なり

部品は局所座標系の命令列と、その範囲の名前付き更新点である。描画命令以外の育成値・保存状態等はアプリが所有する。
部品の表示実体をinstance、再利用可能な不変の命令列をtemplateと呼ぶ。
instanceの配置はoffset、画面座標clip、visible、opacity、表示順で決まる。boundsは内容から求めた描画範囲であり、入力領域とは別。

描画順は `APP通常内容 → APPモーダルbackdrop → APPモーダル内容 → SYSTEM`。
同じ段階では明示配列順。同じtemplateのinstanceもそれぞれの位置で合成する。
部品内部は命令順で描く。instance内部へ別のinstanceの命令を割り込ませない。必要なら部品を分割する。
基本clipは解決済みの矩形。角丸のcoverageはその命令の輪郭であり、親の角丸が暗黙に子をclipすることはない。
部品を出す・外す・並べ替える操作はREPLACE、既存instanceの位置・clip・visible・opacityの変更はPATCHで行う。

部分的に覆われた部品、完全に覆われた部品とも、状態と参照を保持する。不可視化は破棄ではない。
再露出は旧/新のboundsとclipの交差範囲をdamageに含め、背景から順に再合成する。
半透明な前景がある場合、背後の変更も前景を含めて再合成する。前景の古い画素へ再度alphaを掛けない。
最初は完全隠蔽による描画省略を行わない。後で導入する場合も、alpha=255かつcoverage=255の領域だけを隠蔽根拠とする。
同一templateの変更版へ差し替える場合は、その版を使う全instanceの旧/新範囲をdirtyにする。

入力は画素のalphaから推測せず、明示hitRectとfocus表で決める。隠れたinstanceを自動クリック対象にしない。
ポインタ入力を導入する場合の競合は同一入力scope内の表示順を逆に探索し、明示的に有効な最前面を選ぶ。

## 3. 透過と部品全体opacity

### 命令の透過

APIの色はstraight RGBA8。`mul8(a,b)=floor((a*b+127)/255)`とする。
実効alphaは `mul8(mul8(colorAlpha, coverage), commandOpacity)`。丸める順序も契約に含む。
不透明RGB565へは基本仕様の整数source-overで合成する。alpha=0は書換えなし、255は上書き。
画像のalphaもcoverageとは区別し、画像自身のalphaをcolorAlphaとして扱う。

### 部品全体の透過

instanceの`opacity`は、部品内部を透明な背景へ合成した結果に1回だけ適用する。
子2枚が重なる部品のopacity=128を、各子のopacity=128へ展開してはならない。
`opacity=255`、`opacity=0`には同じ意味の高速経路を許す。

グループ内の中間値はpremultiplied RGBA8。命令は `P=mul8(C,A)` に変換し、
`Pout=Ps+mul8(Pd,255-As)`、`Aout=As+mul8(Ad,255-As)` を各段階で0..255へ制限する。
instance opacityは合成済みPとAの両方へmul8で適用する。
最後に `Cout=P+mul8(Cbackground,255-A)` を0..255へ制限してRGB565へ量子化する。
premultipliedであることを表す形式タグを必須とし、straightとの二重乗算を禁止する。
RGB565直接経路とグループ経路では中間丸めが異なり得る。各経路に独立した参照式を持ち、同じ意味の描画内ではdirty範囲やキャッシュ有無で経路を切り替えない。

全画面の中間画像は不要。最大64画素×RGBA8=256 Bの行タイルへ部品を再生して合成し、既存の共用512 B scratch内で逐次処理する。
初期版の実行時グループ隔離は1段。template内のtemplate参照は生成時に平坦化し、循環参照と隔離グループの入れ子はINVALID。
グループ内の命令数も展開後の80/16命令予算に数える。タイルごとの再走査によるCPUコストは別途測る。

## 4. 明示的キャッシュ

### 保存するもの

標準は命令キャッシュ。templateの命令・文字・資源ID・局所boundsを不変の版として保持する。
Flash定義と、呼出時にコピーして作るRAM定義を提供する。動的なJSオブジェクトへの借用ポインタは保存しない。
標準のRAM templateは既存の32 B命令と文字バイト列を保存する。生成元JSを再評価せずに再表示できる。
名前付き更新点等のmetadata、alignment、文字終端の有無も使用量に含める。
命令キャッシュはJSでの再構築を省く機能であり、pixel生成やSPI転送をゼロにする機能ではない。

初期プロファイルはtemplate最大8件、同時表示instance最大8件（APP/SYSTEMの合計）。
同じtemplateから最低2個のinstanceを同時表示できることをMUSTの受入条件とする。
各表示でoffset・clip・opacity・visible・表示順は独立し、内容の版は共有する。
同じtemplateを3回使えば、描画の展開命令数は3回分数える。共有を使って96命令の上限を迂回しない。
初期実装ではtemplate定義をcacheに1回だけ保存し、instantiate時に各表示の解決済み命令を2bankへ展開する。
したがってJSで再構築せず再利用できるが、表示中の命令32 Bまで物理共有するものではない。instanceごとのPATCHと単純なdamage比較を優先した選択である。
文字も表示ごとに展開した予約量でquotaを判定するが、物理保存は不変templateを共有してよい。論理quotaと実保存量を区別して報告する。
hiddenなinstanceも登録枠を消費する。detachedなtemplateは表示命令枠を消費せず、キャッシュ枠と保存領域を保持する。

### 操作と寿命

| 操作（API名は提案） | 契約 |
| --- | --- |
| cache.create(definition) | 不変templateを生成。全検証後にIDを返す。失敗時は保存領域を戻す |
| cache.instantiate(tx, template, placement) | REPLACEへinstanceを追加。内容は共有し、instance IDは別発行 |
| instance.setVisible(tx, false/true) | PATCHで非表示/再表示。templateとinstanceの寿命を保持 |
| 次REPLACEでinstanceを省略 | detach。表示成功後に表示側の保持が外れる |
| cache.fork(template, changes) | 独立した不変版を作る。元版と既存instanceは変えない |
| cache.release(template) | 表示・構築・提出・captureから参照中ならBUSY。未参照なら即時破棄 |
| session.reset | ゲストの呼出経路を止め、構築/提出/captureを取消してから全借用を解放 |

IDは生のアドレスではなく所有セッションと世代を検証するハンドル。RAM領域を再利用しても古いIDは復活しない。
個別release可能なcacheと、resetまで追記のみのFlash image資源表は別の寿命を持つ。
複数表示で使うtemplateの中身をPATCHで直接変更しない。全表示を変更する場合は新しい版を作り、1回のREPLACEで対象instanceを一括で差し替える。
頻繁に変わる数値・文字は静的templateから分けて通常命令として保持できる。更新のたびに部品全体をforkする使い方を必須にしない。
差し替えがabort/discardされた場合は旧版が残る。旧版の解放はpresent成功後まで待つ。
cache参照はGCだけでは自動破棄しない。releaseとsession resetを基本とし、finalizerは解放要求をownerへ渡すだけで描画状態を変更しない。
明示キャッシュをLRU等で勝手に追い出さない。容量超過はLIMIT、設定予算内でも確保不能ならOOM。
一時的な再作成時も旧版＋新版のピークを事前予約し、足りなければ旧表示を維持する。
cache操作を含むトランザクションをabortした場合は、coreのabortとcache.abortを同じowner境界で対にして呼ぶ。
present/discard後もcache.resolveを直ちに呼び、候補位置を確定/破棄して、省略されたinstanceをdetachする。

### ピクセルキャッシュ

明示的な追加方式`raster`を用意できる。MUSTは命令キャッシュで満たし、rasterはBETTERとする。
不透明RGB565は2WH B、透過premultiplied RGBA8は4WH B。stride、タグ、行scratchも別途計上する。
背後の画面を焼き込まない。背景込みsnapshotはモーダルのbackdrop資源として区別する。
同じrasterを複数表示しても画像本体は1個。配置・alphaによる最終合成は表示ごとに行う。
通常命令とpixel cacheで量子化経路が変わる場合は別の明示modeとし、品質差を隠して自動切替しない。

## 5. モーダルウィンドウ

MUSTの最小構成は同時1個・入れ子なし。複数cache instanceの同時表示とは独立した制限。
SYSTEM通知はモーダルより上。システムの確認操作、音量、強制終了は既存のホスト優先順位を維持する。
モーダル中はAPP通常内容へのfocus・activate・backの配送を止める。閉じる操作で発生した入力を背景へ再配送しない。
閉じた後は保存していたfocus keyへ戻し、存在しなければ有効な先頭へ、対象なしならfocusなしとする。

backdropの必須modeは`solid`と`dim-live`。
solidは指定した不透明背景＋モーダル内容。背後のnative命令を保持せず、アプリの論理状態から復帰する。
dim-liveは通常内容＋半透明の遮光矩形＋モーダル内容を同時に合成する。背景の時計やペットは継続して変化できるが入力は受けない。
dim-liveの背景・遮光・モーダルは合わせてAPPの80命令/896 B/6 track内に収める。超過時のsolid切替は呼出元が明示許可した場合だけ行う。
`frosted-static`はBETTER。既存の縮小snapshot＋blurを使い、backgroundの論理状態は進めても表示snapshotは更新しない。

状態遷移は `CLOSED → PREPARING → OPEN → CLOSING → CLOSED`。
PREPARINGでは資源予約と必要なcaptureを行う。開く提出の全転送成功でOPENと入力scopeを同時に確定する。
CLOSINGでは最新domain stateから復帰画面を提出し、表示成功後にfocusを戻してbackdropを解放する。
PREPARING/CLOSING中はAPPへのactivateを一時保留せず捨て、ホストの取消・強制終了だけを受ける。取消時に同じキーで背景の操作を発火しない。
通知runtimeとdomainの更新は止めない。表示の待ち時間は無期限BUSYにせず、host側の取消可能な作業として扱う。

captureは明示的な世代付きhandleを返し、modal提出へ`attach(tx, capture)`する。暗黙の「次のreplaceへ適用」は廃止する。
capture対象はその開始時の表示済みAPPのみ。SYSTEMとモーダル自身を含めず、生成中に別時点の背景を混ぜない。
capture中のAPP表示更新は合流させ、SYSTEM更新も2bankの利用可能時点まで待つ。1回のcapture作業は最大1帯としてownerへ制御を返す。
captureの既定期限は開始から250 ms。ownerへ戻るたびに期限を検査し、超過時は取消して明示fallbackまたは旧画面へ戻す。
通常の未完builderもJSターンを跨いで保持せず、例外・yieldでownerへ戻った時点で未提出ならabortする。必要な構築は最新状態から再試行し、SYSTEMを無期限にBUSYにしない。
native呼出中のプリエンプションをこの期限契約だけで保証しない。VMが安全に制御を返す境界で適用する。
取消・OOM・未対応時は旧表示を維持するか、明示されたsolid fallbackを提出する。失敗した新modalへ入力scopeだけを移さない。
SPI部分失敗では旧論理基準を保持して全帯修復する。修復待ちの入力はAPPへ配送しない。
モーダル表示中のcache解放も通常の参照規則に従い、表示に必要なtemplateを先に解放しない。

## 6. 軽量な更新参照API（BETTER）

更新参照は、セッション、instance ID、template内の更新点IDを持つ小さなJSラッパーとする。
ネイティブのbankポインタやJSから書込み可能な共有ArrayBufferを公開しない。
共通prototypeのsetterで明示PATCHを生成し、反応性のための全オブジェクト巡回・仮想木diff・effect再実行は要求しない。
wrapperは取得時に1回だけ作り、毎フレームのproxy/closure/Promise/更新キューを作らない。

```js
// Proposed API, not currently exported by the runtime.
const meter = view.ref("food-fill");
view.patch(tx => meter.setRect(tx, [8, 110, 8 + foodWidth, 118]));

const card = cache.create(cardDefinition);
view.replace(tx => {
  cache.instantiate(tx, card, { key: "left", offset: [8, 8] });
  cache.instantiate(tx, card, { key: "right", offset: [124, 8] });
});
```

template内部の内容変更はforkを使う。通常の非共有部品は更新点から既存命令へ直接対応させる。
外側のinstance参照は位置・clip・visible・opacityを更新するもので、共有template内部への書込み権ではない。
同じkeyを再構築しても古いwrapperを暗黙に別instanceへ付け替えない。再取得を必要とし、STALEを検出する。
wrapper作成数の上限はAPP公開参照32件を初期値とし、超過時はLIMIT。参照のJS heap実測を容量レポートへ含める。

end成功はSUBMITTED。hostは最後の提出結果を固定stateの`{ticket, status, reason}`として保持し、JSは安全な境界でpollする。
statusはSUBMITTED/PRESENTED/DISCARDED、reasonは取消・資源不足・転送失敗等を区別する。転送失敗して再試行中ならSUBMITTEDを維持する。
adapterは結果を消費し、候補のinstance/ref対応表を確定または破棄してから次のJS更新を許す。提出ごとの無制限Promise列を作らない。
DISCARDEDのREPLACEが発行したwrapperは無効。アプリは最新domain stateを保持して再構築する。domain更新自体は巻き戻さない。
この方式は軽量な参照APIであり、特定フレームワークの内部構造との互換性は要求しない。

## 7. 視覚効果の共通契約（BETTER）

効果は `source → filter → transform/project → viewport clip → opacity → source-over` の固定順とする。
sourceは明示boundsを持つ不変cache版。効果の入力にLCDの既描画画素を暗黙に使用しない。
backdrop効果だけは、明示captureから得た固定画像をsourceとする。
初期チェーンはfilter最大1個、transform/project最大1個。循環参照、feedback、任意shaderは未対応。
各効果は入力bounds、出力bounds、必要halo、scratchBytes、保持Bytes、再評価期限、fallbackを持つ。
変更のないsource/parameterは再評価しない。表示位置だけの変更では不変なfilter出力を再利用できる。

| 効果 | 定義 | 初期上限・fallback |
| --- | --- | --- |
| blur | premultiplied RGBAの各成分に分離box filter。半径r、重み1/(2r+1)、水平→垂直で各pass最近接丸め | r=1/2、透明source外は0。未対応時はfilterなし |
| frosted-static | 不透明APP縮小snapshotをbox blurし、bilinear拡大後にtint | downsample=8/4、r=1/2。既存2/6 KiB予算、solid fallback |
| affine2d | 同次3×3行列、最後の行[0,0,1]。出力画素中心を逆写像してsourceをsample | 平行移動・回転・scale・shear。逆行列不能はINVALID |
| project3d | source矩形を1枚の平面quadとし、4×4 model/view/projectionでclip空間へ変換 | 同時1投影quad、照明・奥行バッファなし。未対応時は元の2D配置 |

blurの透明端は出力boundsを各辺rだけ拡張する。不透明backdropは既存仕様通り端画素複製とし、この違いをmodeに固定する。
sampleの標準はnearest。bilinearは明示指定しpremultiplied成分を補間する。透明端でstraight RGBを補間して色縁を作らない。
キャッシュ更新では旧/新の効果出力boundsをdamageへ含める。blurのsource変更はhaloを含む依存範囲へ伝播する。

数式上、affineは列ベクトルの`p'=M p`、行列は配列へrow-majorで保存する。平行移動はsource→画面の向き。
3Dは`q=P V M [x,y,0,1]^T`、clip空間で`-w≤x,y,z≤w`と`w>0`にclipしてから`(x/w,y/w)`をviewportへ写す。
画面はx右向き・y下向き。正規化座標から`screenX=(nx+1)*W/2`、`screenY=(1-ny)*H/2`。
near面を跨ぐquadは投影前にclipする。w≈0の頂点を巨大な矩形へ変換して確保しない。
内部は三角形へ分割し、共有辺のtop-left規則とperspective-correctなUVで二重合成・隙間を防ぐ。
z-bufferは持たずquad全体の表示順で合成する。複数物体の相互貫通や自己交差の正しい隠面処理は対象外。
数値表現は境界で有限値・範囲を検証し、画面boundsは外向きに丸めてclipする。固定小数点化/PIE化は参照描画との誤差規約を定めてから行う。
効果用の補間精度をまだ実装検証していないため、現時点でbit一致や処理時間の達成を主張しない。

## 8. RAM予算と容量判定

基本描画の16 KiB目標を維持する。template/instanceの固定metadataも基本領域内で再配分し、実装時にsizeofとリンクmapで確認する。
初期のmetadata設計上限はtemplate 8×32=256 B、instance 8×32×2bank=512 B。既存管理領域への収容は未検証であり、現在の9,216 B coreへ追加できるとはまだ主張しない。
展開命令・文字・trackの予算は共有instanceを含めた同時表示分に適用し、metadataと二重計上しない内訳表を実装時に更新する。

RAM cache本体は明示追加予算`cacheBytes`、0..4,096 B。0ではFlash templateのみ利用可能。RAM cache対応はMUSTで、予算を有効にした構成でも受入試験を行う。
基本16 KiBに4 KiBを加えたcache構成は最大20 KiB目標。frostedの2/6 KiBは別で、同時使用時は22/26 KiB目標になる。
これは新要求に伴う明示的な追加枠であり、16 KiB達成と読み替えない。cache未使用時に4 KiBを先取りしない。
キャッシュ領域は事前に確保した小ブロックで管理し、単一確保3,072 B以下、フレーム内heap確保0を維持する。
rasterやその他効果の中間画像はさらに`effectBytes`で明示する。filter scratchと完成版・生成中版の同時保持を含めて予約する。
frosted自身の画像/scratchはbackdropBytesだけへ計上し、effectBytesとの重複計上をしない。
予算不足はLIMIT、物理確保失敗はOOM。MUSTの操作を維持した明示fallbackがなければ、旧画面を維持して失敗を返す。

## 9. 受入試験と実装順

| 対象 | 必須試験 |
| --- | --- |
| 重なり | 半分/全体を覆う、前景を外す、背後のみ更新、順序入替、画面外clip、各状態で全面参照描画と比較 |
| 透過 | alpha 0/1/127/128/254/255、coverage境界、同じ子2枚の重なり、グループopacity、透明画像の色縁 |
| cache | 1版を2〜8箇所へ表示、片方のみ移動/非表示、全detach後再表示、参照中release、版差替え失敗、枯渇、reset後STALE |
| modal | 開閉各段階の取消/OOM/SPI失敗、入力漏れなし、focus消失、SYSTEM確認、時計継続、最新状態復帰 |
| 更新参照 | 反復PATCHでwrapper数が増えない、REPLACE破棄の候補無効化、再生成keyへの古い参照拒否 |
| 効果 | blur halo、透明端、特異行列、near面交差、clip外投影、共有辺、fallbackとピーク予算 |

優先順は、重なり/命令alphaの既存試験拡張 → 命令cacheと複数instance → グループopacity → solid/dim-live modalと提出結果poll → 軽量JS API → frosted/変形/投影。
各段階で通常・音声・Wi-Fi併用の時間とRAMを測る。MUST完了前にBETTERの画質最適化を優先しない。

## 10. 制作スキーマへの反映

v0.2の意味モデルは`templates`、画面内の`instances`、`modal.backdrop.mode`、`effects`、`budget.cacheBytes/effectBytes`を持つ。
instanceは`key/template/offset/clip/visible/opacity`、templateは不変な`nodes`と`storage=flash|ram`を持つ。
公開キーは制作時だけ文字列で扱い、生成物では数値IDへ変換する。参照切れ・循環・展開後容量・ピーク保持量を生成時に検査する。
既存`design-schema.json`はv0.1専用のまま維持する。v0.2の実行時表現と生成器が実装されるまでは、新フィールドを既存JSONへ混ぜて「検証済み」としない。
本書がv0.2の規範であり、機械可読スキーマと生成器の実装は別の完了条件として追跡する。

## 11. 初期cache実装の状態

2026-09-13、`ds_cache`を4,096 Bのcaller-owned固定領域として実装した。
上限はtemplate 8、instance 8、保存命令48、文字領域1,024 B。共有template/instance IDのプロセス寿命カウンタ8 Bを加え、native計上は4,104 B。
現段階のcreateはrect/roundRect/strokeのみ。文字領域とstatsは予約・公開済みだが、text/image/gradientのcache encode/decode、fork、グループopacityは未実装。
opacity=255以外のplacementはUNSUPPORTEDで提出を変更しない。命令opacityと重なりは既存矩形rendererで動作する。

実装済み操作はcreate、instantiate、place、setVisible、release、abort、present/discard後のresolve。
releaseは参照中BUSY、REPLACEで省略してpresentしたinstanceはdetach、discardした位置変更は確定位置へ戻る。
解放したtemplate領域は固定配列内でcompactし、IDは再利用しない。LRU、heap確保、GC finalizerはない。

ホストではASan/UBSanと`-O2 -fstrict-aliasing`で、同じtemplateの2表示、片方だけの移動・非表示、abort/discard、参照中release、detach後release、reset後の古いhandle、ID枯渇、unsupported opacityを検査した。
ESP32-S3実機ではtemplate 1件からinstance 2件を表示し、片方の非表示を7帯・26,880 B・4,419 µsで転送。無変更は0帯・0 B。
cache操作を含むPATCH→place→end→discard/resolve 1,000回は平均19 µs、最大201 µs、free heapは前後248,196 Bで同値だった。
最終の転送前32,400画素は期待値と一致しHOME_READYへ復帰した。1回の診断値でありp95や物理LCD readbackではない。
