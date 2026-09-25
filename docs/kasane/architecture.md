# Kasane の現行境界と守る契約

この文書は設計判断の短い説明であり、関数シグネチャの網羅表ではない。実装・試験の入口は末尾に示す。旧資料の「案」「実装時点の未実装」を現行仕様と取り違えない。

## 責務

| 所有者 | 責務 | 持たないもの |
| --- | --- | --- |
| アプリ / domain | 音声・入力・通知内容・表示文言などの意味判断、保存、外部 I/O | Kasane の bank や LCD 転送の操作 |
| System service | 時計・電源・通知・タイマー等の state、購読と期限 | アプリ画面の構成、画素、QuickJS オブジェクト |
| Kasane schema / presenter | 不変 descriptor の展開、型付き slot、source 値の参照、可視値比較、命令提出 | `music` / `pet` といった domain の条件分岐 |
| Kasane core / view | APP・SYSTEM の2 layer、2 bank、ticket、damage、失敗時 repair | アプリ状態の意味・音声/SD待機 |
| shell / overlay | 領域、入力 gate、背景と HUD/picker の重ね順、LCD port | overlay guest の domain state |

Kasane の C core にアプリ別 `kind`、特別な source 名、status 優先順位を入れない。組込み画面は flash 上の不変 descriptor を借用し、ユーザー定義は `mount` 時に上限付き native descriptor へ一度コンパイルする。どちらも同じ実行経路を使う。JS に毎 frame の命令木や自動 diff 用コピーを保持させない。描画の意味を一般化するための CSS/仮想 DOM/任意 JS callback は実装しない。PIE 幅は renderer 内部に閉じ、descriptor/slot/source ABI に漏らさない。

`pocket.kasane.mount({version:1, slots, nodes}, initial)` と登録済み定義の `mount(name)`、`view.set(partial)` が高水準入口。型付き slot は text/bool/数値/矩形等を検証し、未知名、型違い、不正 UTF-8、容量超過では**部分反映しない**。定義は mount 後に不変、JS オブジェクトの後編集は表示に影響しない。文字は宣言した容量だけを保持し、数値 slot に文字用領域を割り当てない。`u32` source を `u16` 座標/page/reveal に暗黙変換しない。text は1命令128 B、APP全体896 B、APP80命令、SYSTEM16命令/128 Bを上限とする。低レベル `createScene` / `replace` は escape hatch であり、同じ APP lease の mounted view と混ぜない。

制作時の JSON Schema は型の入口に過ぎない。host生成器を作るなら、未登録command、参照切れ、token、UTF-8**バイト**容量、展開後の命令/文字quota、画面・modal遷移時の同時peakを意味検査する。端末上でJSONの全木やtoken辞書を保持せず、flash定数と数値IDへコンパイルする構想である。現在のJSON例はこの生成器が完成した証拠ではない。見た目の規約として、選択状態は色または点滅だけに頼らず枠/マーカーでも示す。

### source と dirty-node

producer は自分の task / domain で値を作り、世代付き・型付きの読取り専用 snapshot を公開する。owner turn はそれを安全な寿命の間だけ借り、slot に対応付ける。producer を UI の都合で待たせない。固定 pool が埋まったら有界の `BUSY` / skip とし、無制限確保や古いポインタ上書きに逃げない。値の所有権を移す必要がない場合は借用し、core が確定 bank に受理する時だけ必要なコピーを行う。

mount 時に slot→node 依存表を作る。revision/viewport が不変なら clean turn は O(1) で終え、変更した依存 node だけ解決する。ただし revision/hash だけで描画を省かず、解決後の**可視値を正確に比較**する。topology、背景、viewport、可視条件/page の切替は安全な REPLACE に戻す。命令の数・順・不変属性が一致する場合のみ PATCHし、変更 property だけ提出する。0幅・非表示命令は帯走査に載せない。pending 中は最新値へ合流し、並列 transaction を作らない。PRESENTED で参照を確定、DISCARDED は最新値から再構築する。A→B→A、旧 ticket、期限切れ source、pool 枯渇を試験対象にする。

### 表示と失敗

APP と SYSTEM は別 layer だが builder と物理 bank は共有する。`submit` は LCD 完了でなく SUBMITTED。必要な全帯の転送成功で PRESENTED となり、途中失敗は旧論理基準を保って全面 repair を要求する。失敗して cancel / discard しても repair 要求を消さない。APP 入力は修復完了まで遮断する。アプリ終了では APP lease、参照、資源、modal を片付け、SYSTEM の確定状態は残す。返却した active text pointer は次の成功表示または reset まで読取り専用で有効。この寿命が text bank の最適化を制限する。

ダメージは8行×17帯で扱い、旧/新の bounds と clip を dirty にする。対象帯は背景から再合成するため、透明・移動・消去でも「変わった命令を上書き」しない。全画面 RGB565 バッファは持たず、共用 strip を使う。画像 provider は登録後の寿命・世代・範囲を検証し、描画中に SD / network / JS / heap 確保を行わない。文字の表示幅は font advance を正本にし、JS のUTF-16長から全角幅を推定しない。コードポイントでの reveal は Unicode scalar 単位で、複雑なシェーピングや書記素クラスタは保証しない。

### overlay と音楽

overlay は第三 layer を作らず APP lease を使う。`pocket.overlay` は region とキー入力を担当し、Kasane は描画を担当する。region-local 座標を LCD 座標へ変換し viewport で切る。native 背景は Kasane の dirty state と独立に動くため、overlay 中の全画面 invalidate は残る。shell の HUD・picker は guest より上。overlay profile では Kasane modal は UNSUPPORTED（全画面 scrim と region 契約が衝突する）。音楽の play/seek/pause、曲名・status の意味は music module、文字の寸法・命令・提出は presenter。music 固有の light-only PATCH も music module に置き、Kasane core に分岐を増やさない。

### System の規約

pub/sub は「状態が変わった」合図でありイベント履歴ではない。topic は固定 mask、購読者ごとに pending を持ち、初回購読で snapshot 読取りを促す。別 task / ISR は state を直接変更せず、有界 mailbox/queue に入力し owner を wake。通知 command は同期で受付結果を返すため、dirty publish を消失できない command の代用にしない。時計は UTC と単調時刻 anchor を分け、相対 timer は時計の step で伸縮させない。電源 read はキャッシュのみで ADC を暗黙起動しない。通知は固定9件（表示1＋待機8）、timerは固定4件。満杯時は明示 FULL、発火済み timer は容量解放時まで due を保持し、過去期限の busy loop を作らない。保存 blob はモジュール分割だけを理由に変更しない。無需要の周期 wake や専用 task/stack/queue を増やさない。Sensor の sleep wake は配線・モード・実機測定なしに対応を主張しない。

### 資源と別機能

cache は明示的な不変 template（8件）と独立 instance（8件）を持ち、LRU で勝手に追い出さない。表示時の命令 quota は instance 数ぶん数える。内容を全 instance で変更する時は新しい版を作り、表示成功後に旧版を解放する。modal は同時1つ、SYSTEM通知はその上。開閉の表示確定と入力 focus の切替を同期し、失敗時は旧画面・旧 focus を保つ。グループ opacity は子の opacity の一括置換ではなく、隔離合成後に1回適用する。ディザは最終 RGB565 量子化で画面絶対座標に固定する。frosted backdrop、raster cache、汎用 blur/affine/3D、native flex は基本経路の性能・容量を犠牲にして暗黙に追加しない。これらの一部は実験的に実装されても、製品能力としては個別に検証する。

## コードと試験の入口

`main/ui/kasane/ksn_core.c`, `ksn_schema.c`, `ksn_schema_session.c`, `ksn_presenter.c`, `ksn_view*`; `main/pocket/pocket_kasane.c`, `app_view_assets.h`, `app_music_view.c`; `main/system/`; `tools/kasane_contract/run.sh`。API の正確な名前・feature flag・バイト数は、これらとビルド済みターゲットの `sizeof` / map を優先する。
