# Kasane 描画側の最適化調査 — 境界面ごとのスライス

基点 `9b5252c`（`perf/kasane-opt`、`origin/vm/design-contracts` 上）。2026-09-15。
この文書は **分析のみ**で、ソース・挙動・実機には一切触れていない。したがってここにある数は
「読んだファイルの行」「実行したコマンドの出力」「既存ドキュメントの値」のいずれかで、
**実機の時間を新たに測ったものは 1 つも無い**。各値の出所を明示する。

この歴史的調査中の `design-device-probe.md:行` / `kasane-progress.md:行` は、
資料縮約前のGit版 `docs/kasane/` への出所表記である。現行判断の入口は
[`docs/kasane/verification.md`](../kasane/verification.md)。旧行番号を現行文書の行番号として読まない。

対象は kasane（設計システム＝描画側）の内部境界面である。ホットな関数の一覧ではなく、
**仕事がどの面を越えるか**で切る。面を越えるたびに回数・コピー・間接呼び出し・ABI の
段差が発生するので、最適化の候補も面ごとに性質が違う（面の中で命令を削る／面を跨ぐ
回数を減らす／面そのものを統合する）。

---

## 0. 前提

### 0.1 機体

| 項目 | 値 | 出所 |
| --- | --- | --- |
| SoC | ESP32-S3FN8、LX7 2 コア 240 MHz | `docs/platform/hardware-constraints.md:11-12` |
| PSRAM | **無し**（FN8 内蔵なし、標準 ADV は PSRAM 無し設計） | 同 `:14`、`main/hal/board.c:299` |
| DRAM | 約 334 KiB。ホーム画面の空きヒープ実測 274 KiB（`idle_free=280,932`）、静的 DIRAM 111,383 B | `CLAUDE.md:86` |
| Flash | 8 MB、コードは flash cache 越し | `hardware-constraints.md:15` |
| 全画面 RGB565 | 240×135×2 = 64,800 B | `hardware-constraints.md:58` |
| 8 行 strip | 240×8×2 = 3,840 B、**DMA 完了前の書き換え禁止** | 同 `:59`、`main/hal/board.c:30-31` |
| LCD | ST7789V2、SPI2、**80 MHz**（40 MHz から引上げ済み。send 15.5→9.1 ms） | `main/hal/board.c:262-269` |

PSRAM は存在しないので、転送・作業バッファ・行バッファはすべて内部 SRAM である。
PSRAM 帯域やキャッシュミスの類の候補は本調査に無い。

### 0.2 使った道具（すべてホスト側）

| 道具 | 何に使ったか |
| --- | --- |
| `xtensa-esp32s3-elf-gcc 15.2.0`（`/root/.espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/`） | kasane のソースは IDF ヘッダ無しでも単体でコンパイルできる。`-Os`/`-O2` でオブジェクトを作り `objdump -d` で命令数、`-fstack-usage` でフレームサイズ、`nm -S` で型サイズを実測した |
| ホスト `cc` + 一時ハーネス（**未コミット**、`/tmp/ksncount/`） | `ksn_core.c`/`ksn_render.c`/`ksn_view.c`/`ksn_cache.c`/`ksn_modal.c`/`ksn_font.c` をそのままリンクし、`apps/kasane/demo.js` の場面を再構成して境界の通過回数を数えた。方法は §13 |
| `nm`/`objdump` | 命令数、フレーム、型サイズ、呼び出し箇所（`ksn_core_read` は `--wrap` で包み、呼び出し元アドレスごとに集計） |
| 既存ドキュメント | `docs/perf/pie-simd.md`（コストモデルと実測）、`docs/kasane/verification.md`（現在の実機ゲートと測定範囲）。旧Kasane日別記録はGit履歴に残る |

### 0.3 kasane 側に既にある最適化（境界ごとの「もう済んでいる」）

| 済んでいること | 場所 | 根拠 |
| --- | --- | --- |
| 不透明帯・不透明矩形の連続 fill が PIE（`ee.vldbc.16` + `ee.vst.128.ip`、8 画素/周） | `main/ui/kasane/ksn_render.c:11-39,268-273` | asm を `piesim` で実行する `tools/kasane_contract/fill_pie.py` が通る。帯あたり vector 本体 384 cycles は**推定**（`kasane-progress.md:208-209`）、実測は無い |
| すりガラスの span（水平補間＋tint）が PIE（QACC 融合、8 画素/セル） | `main/ui/kasane/ksn_frost.c:81-119`、`ksn_frost_kernel.h:7-8` | 同一バイナリ A/B で scalar 362,219 µs → PIE 145,690 µs（**2.486 倍**、8,960 画素×64 試行、`design-device-probe.md:264-269`） |
| damage mask による部分転送 | `ksn_core.c:549-586`、`ksn_render.c:224-226` | 実機で 7 帯/26,880 B の転送 4,453 µs、初回全面 12,698 µs（`design-device-probe.md:109`） |
| 8 行 strip 1 枚 + `tx_buf[2]` への直接 byte swap（`shared` を渡さない） | `main/hal/board.c:30-53,382-411` | 非同期化で send 7.77→1.46 ms（−6.31 ms、59 ペア）、swap-into で −0.24 ms（`pie-simd.md:546-547`） |
| template/instance による命令の再利用と ref 削減 | `ksn_cache.c` 全体 | 実機で cache 操作込みの PATCH 1,000 回が平均 19 µs（`design-device-probe.md:131`） |

**すりガラスはまだ製品経路ではない**（`ksn_ports.h:44-48` が「optional synchronous prototype」、
`kasane-roadmap.md:50` が capture handle / modal 接続 / damage-present 結線を未完と書いている）。
つまり上の 2.486 倍は、per-pixel 面の**達成可能値の前例**であって、いま速くなっているという意味ではない。

---

## 1. 境界の地図

| # | 境界 | 渡るもの | 1 フレームの回数（実測、§2-§8 の場面） | 現在のコスト |
| --- | --- | --- | --- | --- |
| 1 | JS/VM → kasane API | プリミティブ記述、change、ticket | 7〜16 呼び出し/フレーム（`demo.js` 実測）＋ JS 側 getter ~60〜90 回 | 実機 PATCH 1 サイクル **14〜25 µs**（native、`design-device-probe.md:53,112,149`）。`turn_ms` 3.21 ms の 0.5% 以下 |
| 2 | scene → render（command bank → デコード） | `ksn_frame_command`（**176 B**）1 個/回 | **114〜558 回/フレーム**（実測、§13） | 1 回 = 139 命令 + `memset(176)` + `memcpy(12)`（objdump 実測）。全面フレームで推定 0.5〜0.6 ms |
| 3 | command/group 符号化 → 仕事 | group の子命令、64 画素 tile | reads の **61〜65% が `render_group`**（実測 342/558） | 子命令 1 本につき `1 + 2×行×64画素ブロック` 回の再デコード。群の外の帯でも bounds パスが走る |
| 3' | 同上（tile の量子化） | 64 画素ブロックの合成 | 42 画素幅の instance で **22/64 画素 (34%) が空回り**（計算） | 空回り 1 画素につき子命令数ぶんの `covers`+`sample` |
| 4 | per-pixel | RGB565 画素 | **28.6 万〜58.6 万回**の補助呼び出し/フレーム（実測、§13。全面 REPLACE 286,162、demo PATCH 586,223） | 推定 **3.1 M 命令/全面フレーム ≒ 10〜13 ms**。実機 `render_ms` 12.59〜13.62 ms と同桁（§5） |
| 5 | framebuffer/present | 8 行×240 の strip | 帯ごと 1 回（3〜17 回）。全面で 64,800 B | 実機 send **3.36〜3.44 ms**（JS 診断、`kasane-progress.md:89,124,139`）、7,052〜7,076 µs/64,800 B（stress、`design-device-probe.md:224-225`）。80 MHz の理論 6.48 ms に対し 92% |
| 6 | compile/ABI・配置 | 関数呼び出し、定数、スタック | 画素あたり **2〜4 回の `call8`** | `ksn_render_rects` は **972 命令**、補助関数は非 inline（`blend` 50、`sample` 57、`covers` 52、`pack565` 35、`quantize` 19、`inside_round_rect` 36）。フレーム **928 B** 実測 |
| 7 | 計測・帰属 | PERF/SPLIT/KASANE_PAINT | — | kasane 側は `turn_ms`/`render_ms`/`send_ms` の 3 項のみ。内訳・サイクル・帯分布が無い |

以下、各面について「何が渡るか／何回か／いくらか／既に何が済んでいるか／候補（大きさ・
リスク・撤退線）／実機なしでどう検証するか」を書く。

---

## 2. 境界 1: JS/VM → kasane API

### 何が渡り、何回か

`apps/kasane/demo.js` を数えると、REPLACE は 12 のプリミティブ呼び出し＋`cache.create`＋
`replace`＋`submit` 相当で、PATCH は 7（`setRect`×2、`setColor`、`place`、`setVisible`、
`setReveal`、`setText`）。実機診断は 300 tick で、`KASANE_TICK` が commands=9〜12 を出す
（`design-device-probe.md:23`）。

ネイティブ側の入口は `main/pocket/pocket_kasane.c` で、1 呼び出しあたり:

- `mutate()` が `JS_GetOpaque` で ticket を検証（`pocket_kasane.c:673-686`）
- `parse_draw_base` がプロパティ 3 つ（`bounds`/`clip`/`opacity`）を `JS_GetPropertyStr` で取り、
  `bounds` は配列 4 要素を `JS_GetPropertyUint32` で読んで各々 `JS_ToFloat64`（`:207-275`）
- 種別ごとに `color`/`font`/`text`/`capacity`/`from`/`to`/`axis`/`dither` を追加で取得（`:291-333,447-478`）
- `tx.text` は `JS_ToCStringLen` で UTF-8 化し、128 バイト上限を先に検査（`:436-445`）

実測でいちばん近い数字は「native PATCH→change→end→discard 1,000 回 = 平均 14 µs、最大 79 µs」
（`design-device-probe.md:53-54`。キャッシュ操作込みで 19 µs/最大 201 µs、2 命令 instance で
25 µs/最大 241 µs、同 `:131,149`）。

### 既に済んでいること

- ticket/layer/世代の検証は opaque 値 1 個の比較で、1 呼び出しあたり分岐数本（`pocket_kasane.c:673-686`）。
- 文字列は上限（128 UTF-16 単位）を先に見てから QuickJS に確保させる（`:436-445`）。
- REPLACE/PATCH の rollback は例外 1 つで `ksn_view_cancel` に落ちる（`:681-685`）。

### 候補

| 候補 | 大きさ | リスク | 撤退線 | 実機なしの検証 |
| --- | --- | --- | --- | --- |
| **1a. プロパティ読みのまとめ取り**（1 プリミティブで 8〜12 回の getter を、既知の形のオブジェクトなら 1 回の走査に） | 小。`turn_ms` 3.21 ms の数 % 以下。**per-call 14〜25 µs の native 側は変わらない** | 低（受け付ける形を変えない） | 無し（既存の `test_pocket_kasane.c` と JS 診断で同値） | `tools/build_kasane_test.sh` のホスト JS テストで戻り値・例外コードを比較。数は「JS→native 呼び出しごとの getter 回数」をホストで数えて契約にする（時間には翻訳しない、`pie-simd.md:524`） |
| **1b. `core_begin` の bank 全コピーをやめる**（PATCH でも毎回 3,072+1,024 B をコピーし、REPLACE では追加で 2,560+896 B を memset。`ksn_core.c:113-126`） | 中。4,096 B ≒ 1,000〜2,000 cycles ≒ **4〜8 µs/フレーム**（推定）。14 µs/サイクルの実測の主因と考えるのが自然 | 中（表示 bank と候補 bank の独立性という契約に関わる。`ksn_core.h:59-63`） | 無し（コピーを遅延するなら bank 分離 tests が判定） | `tools/kasane_contract/test_core.c` と `test_runtime.c`（bank 独立性・reset のアドレス保持）。時間は実機の `KASANE_PAINT turn_ms`（性質上 0.1% 台） |

**この面はレバーではない。** 実機で `turn_ms` 3.21 ms に対し `render_ms` 12.59 ms、
`send_ms` 3.37 ms（同 `:24`）で、JS 境界は 3 番目に小さい。

---

## 3. 境界 2: scene → render（command bank → デコード）

### 何が渡り、何回か（実測）

`ksn_render_rects` は bank から `ksn_frame_command`（**176 B**、target 実測。`ksn_draw` 64 B +
`text[128]` + flags）へ 1 命令ずつ展開する。展開は `ksn_core_read`（`ksn_core.c:486-528`）で、
毎回 `memset(out,0,176)` + payload 12 B の `memcpy` + TEXT なら本文 `memcpy` を行う。

再構成した `demo.js` 場面（12 命令、APP 1 層）での実測:

| 場面 | 帯 | `ksn_core_read` | 内 `render_group` | 内 走査+preflight | span 呼び出し | coverage バイト |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| REPLACE 全 17 帯 | 17 | **558** | 342 | 216 | 97 | 5,320 |
| PATCH 1 矩形（実機診断に近い） | 3 | **114** | 66 | 48 | 2 | 0 |
| PATCH demo 1 tick | 14 | **472** | 292 | 180 | 97 | 5,320 |

分解は完全に説明できる（§13 の方法で呼び出し元ごとに数えた）:

```
走査+preflight = 12 (preflight) + 12 × 帯数
render_group   = (2 × 群の命令数) × 17 帯          ← bounds パス（群の無い帯でも走る）
               + (2 × 群の命令数 × その群の行数 × ceil(幅/64) ブロック)  ← paint パス
```

12 命令・全面 17 帯で 558 回、うち 61% が `render_group` である。1 回あたりの ABI コストは
`ksn_core_read` **139 命令 + `memset(176)` + `memcpy(12)`**（objdump で relocations が
`memset`/`memcpy` を指すことを確認）。

また `ksn_core_damage`（`ksn_core.c:565-586`）が 32 B の `memcmp` を命令数ぶん走り、
TEXT は本文も比較する。REPLACE/repair では mask を全帯にする（`:571-573`）。

### 既に済んでいること

- 変更が無ければ 1 帯も転送しない（実機で 0 帯/0 B、`design-device-probe.md:110,148`）。
- 画像は bank に複製せず provider を借りる（`ksn_core.h:128-130`）。

### 候補

| 候補 | 大きさ | リスク | 撤退線 | 実機なしの検証 |
| --- | --- | --- | --- | --- |
| **2a. デコード結果の行内・群内での再利用**（同じ (ticket, layer, index) を 1 フレーム内で 1 回だけ 176 B に展開し、あとは借りる） | **中〜大**。読み出し回数を 558→~20（群の bounds/paint の再読出しを消す）にできる。推定 0.4〜0.6 ms/フレーム（139 命令 + memset/memcpy の実測からの推定） | 中。`ksn_core.h:75-78`「bank ポインタは外へ出さない」という契約に触れる。**借用ビュー**なら契約は保てるが、フレーム中に bank が変わらないことが前提 | 無し（読出しは read-only。候補スイッチで旧経路を残す） | 既存 `tools/kasane_contract/` は全部この面を通る。加えて**この面の回数は実機なしで数えられる**（§13 のハーネス、`ksn_core_read` を `--wrap` で包むだけ）。契約は「1 フレームの read 回数 ≤ C」として契約化する |
| **2b. 176 B のゼロ埋めをやめる**（payload だけ書く。`memset` は 176 B/回 × 558 回 = 98 KB/フレーム） | 小〜中（98 KB の `memset` ≒ 25k cycles ≒ 0.1 ms、推定） | 低。ただし借りた側が未定義領域を読む前提に依存するので、初期化契約の変更が要る | 無し | `ksn_core_read` の呼び出し元が `out` の全フィールドを読むかはソースで追える。ホスト比較で全経路の画素一致 |

---

## 4. 境界 3: command/group 符号化 → 仕事

### 何が渡り、何回か（実測）

群（group）は「連続する子命令の範囲＋不透明度」として符号化され（`ksn_core.c:381-403`）、
描画時は `render_group`（`ksn_render.c:123-187`）が次を行う:

1. bounds パス: 子命令を全部読み、クリップ付き bbox を求める（`:131-146`）。**群がその帯に
   1 行も無くても走る**（17 帯ぶん）。
2. 行×64 画素ブロックごとに: `memset(tile,0,256)` してから、子命令ごとに
   `ksn_core_read` → `covers` → `sample` → `premultiply_over` を 64 画素ぶん走らせ（`:151-179`）、
   最後に 64 画素を `group_over` で RGB565 に落とす（`:180-184`）。

群を 1 つだけ置いた場面で数えると、群 1 つ（子 2 命令）の read は **214 回**で、
内訳は bounds パス 34（＝2 命令 × 17 帯）、paint パス 144（＝2 命令 × 36 行 × 2 ブロック）、
群を走査する側 34（1 命令 × 17 帯 × 2）、preflight 2 である。つまり **群 2 命令の read は
平坦な命令 2 本の 17 倍**発生する。**群が関係しない帯でも bounds パスが走る**ため、
34 回のうち 24 回（＝2 命令 × 12 帯）は無駄である
（`pie-simd.md:352` の「overlay は毎ストリップ呼ばれる」と同じ形。ただしそちらは 813→201 の
3/4 が捨てられていたのに対し、こちらは 34 回のうち 24 回で差は小さい）。

12 命令の全面フレームでは render_group の read が **342**（＝群 178 + instance 82 + instance 82、
各々 bounds 34 + paint）で、走査側 216（preflight 12 + 12 命令 × 17 帯）と合わせて 558。

### tile の量子化

子命令の合成は 64 画素ブロック単位で、群の bbox 全体を覆う。42 画素幅の instance では
64−42 = 22 画素 × 24 行 = **528 画素/instance** が空回りする（実測 6,408 `group_over` 呼び出し
／全面フレーム）。1 空回り画素につき子命令数ぶんの `covers` + `sample` が走る。

### 既に済んでいること

- 群は 1 本の不透明度と連続範囲で、命令あたりの付加データは 1 バイト（`ksn_core.c:399-401`）。
- tile は 256 B の 4 B/画素 premultiplied RGBA で、全画面サーフェスを持たない（`ksn_render.c:126-127`）。
- 群の位相（begin/end）は flags のビットで、連続範囲の検証は 100 命令（`ksn_core_group` の objdump）。

### 候補

| 候補 | 大きさ | リスク | 撤退線 | 実機なしの検証 |
| --- | --- | --- | --- | --- |
| **3a. bounds パスを帯で打ち切る**（群の bbox がその帯に届かなければ `render_group` に入らない。計算は走査側で 1 度だけ） | 小。read の 24/342 が消える。推定 0.02〜0.05 ms | 低（画素に影響しない） | 無し | read 回数はホストで厳密に数えられる（§13）。画素は `test_group_dither.c` の全画面比較が担保 |
| **3b. tile を 64→16 画素ブロックにする**（量子化の空回りを 34%→12% 以下） | 小〜中。空回り画素に比例（推定 0.1〜0.3 ms、群の数と幅に依存） | 低〜中。`covers`/`sample`/`premultiply_over` の反復回数が変わるだけで、丸めは 1 画素ずつ同じ式。dither の provenance 語はブロック相対だが絶対 x で bayer を引く（`:182`）ので意味は保たれる | 無し（同時に群の合成式は変えない） | `test_composition.c`/`test_group_dither.c` の全 32,400 画素一致＋ホストで合成画素数と空回り画素数を数えて契約化 |
| **3c. group opacity=255 かつ子が不透明なとき tile を経由しない** | 中（`premultiply_over` 8,704 + `group_over` 6,408 = 補助呼び出しの 5%） | **高**。`ksn_cache.h:54-55` が「instance は opacity 255 でも隔離 premultiplied 合成を使う（PATCH 下で丸めを安定させるため）」と明記している。丸めが 1 LSB 変わりうる | **あり**。丸めが変わる画素を名指しし（例: 「不透明子の上端 1 行で ±1 LSB」）、不透明でない群は必ず隔離経路に残す。runtime スイッチで群の隔離合成を旧経路に戻せば完全に撤退できる | `test_group_dither.c` の独立参照式と全画素比較（差が出たらその画素数と位置を記録）。PATCH 前後での不変性（同じ placement を 100 回繰り返して画素が動かないこと）も同じハーネスで見る |

---

## 5. 境界 4: per-pixel

### 何が渡り、何回か（実測）

再構成した `demo.js` の全面 REPLACE フレームで、**per-pixel 補助関数の起動回数**を数えた
（ホスト、`finstrument-functions`。回数は `-O0`/`-O2` で同一）:

| 補助関数 | 起動回数/フレーム | 命令数（objdump, `-Os`） | 備考 |
| --- | ---: | ---: | --- |
| `covers` | 17,672 | 52 | うち `inside_round_rect` 4,816（gradient の radius=0 判定 4,320 を含む） |
| `sample` | 13,948 | 57 | gradient のとき内部で 4 本の `quou`（後述） |
| `blend` | 5,534 | 50 | 直接経路。内部で `pack565` を呼ぶ |
| `pack565` | 11,590 | 35 | dither 時は `quantize` を 3 回呼ぶ |
| `quantize` | 12,960 | 19 | = 4,320 画素 × 3 チャネル（dither 画素ちょうど） |
| `premultiply_over` | 8,704 | ~42（inline、推定） | 群の tile 1 画素ごと |
| `group_over` | 6,408 | ~36（inline、推定） | tile → RGB565 |
| `mul8` / `clamp8` / `channel` | 112,666 / 52,984 / 34,560 | 5 / 3 / 4 | target では inline（`ksn_render.o` に symbol が無い） |

`interpolate`（`ksn_render.c:73-82`）は `sample` へ inline され、**画素ごとに 4 チャネルそれぞれ
を可変除数 `last` で割る**。`sample` の objdump は `loop a11, 1b4` の 4 回反復で各反復に
`quou` を持つ（`quou` はハードウェア除算器、実測 **16〜18 cycles/本**、`pie-simd.md:233`。

> 注意: 同文書 `:157` は「約 32 cycles」と書いているが、これは本人が後で「仮定して 2 倍外した」
> と書いた値（`:247`）で、実測は `:233` の 16〜18 である。本調査は 16〜18 を使う。

**命令量の見積り**: 上表の起動回数 × 命令数（自分の本体のみ。callee は別行）で
**約 3.1 M 命令/全面フレーム**、240 MHz・1.0〜1.3 IPC で **10〜13 ms**。実機の `render_ms` は
12.59〜13.62 ms（`design-device-probe.md:24`、`kasane-progress.md:89,124,139`）で、この面が
フレームのほぼ全部を説明する。順位は `covers`(0.92 M) → `sample`(0.80 M) → `pack565`(0.41) →
`premultiply_over`(0.37) → `blend`(0.28) → `quantize`(0.25) → `group_over`(0.23) −
ただし `covers` の 0.17 M は `inside_round_rect` の分。

とくに **`covers` + `sample` だけで 1.7 M 命令＝この面の約 55%**で、両方とも
「ほぼ定数を返すために 1 画素 1 回呼ばれる」ものである:

- `covers` は RECT/TEXT で即 `true`、GRADIENT は `inside_round_rect(radius=0)` を経由して即 `true`
  （`ksn_render.c:59-71,46-48`）。それでも **out-of-line の 52 命令 + `call8`** を画素ごとに払う。
- `sample` は RECT/ROUND_RECT/STROKE/TEXT では色を返すだけ（57 命令の先頭で抜ける）で、
  GRADIENT のときだけ 4 本の除算を走らせる。

### 既に済んでいること

- 不透明矩形（opacity 255 かつ α 255）は per-pixel を捨てて PIE の連続 fill に落ちる
  （`:268-273`）。背景帯も同じ。
- TEXT の直接経路は `covers` を呼ばず coverage の非ゼロだけを合成する（`:254-266`）。
- dither の有無・子の dither provenance はビット 2 語で持ち、画素ごとの分岐を減らしている（`:154-177`）。
- `pack565` の dither 無しはシフトのみ。`/255` は magic multiply（objdump に `muluh`+`srli 7`。つまり
  除算は既に乗算化されている）。

### 候補

| 候補 | 大きさ | リスク | 撤退線 | 実機なしの検証 |
| --- | --- | --- | --- | --- |
| **4a. RGB565 blend/pack の PIE カーネル**（直接経路の `blend`+`pack565`+`quantize`、続いて群の `group_over`/`premultiply_over`） | **大**。この面は 10〜13 ms あり、blend/pack 系は約 1.0 M 命令（1/3）。前例として同じ 565 blend が **38.5→約 10 ms（4 倍）**（`pie-simd.md:542-543`）、canopy の組み直しで 39 命令・ストール 0（`:545`）。推定 **2〜4 ms/フレーム**（`pie-simd.md` §2.6 の 1.3〜1.4 倍、`pie-simd.md` §4.6 の「床の 2.4 倍」次第） | 高。16bit レーンに収まらない式（`pie-simd.md:415-421` の恒等式）、`/255` の厳密化（`:425-435`）、丸め方向（`:439-441`）。dither の bayer 参照と group provenance の順序を壊すと**同じ画面内で段差が見える**（`:411`） | **あり**。① カーネルを runtime スイッチ（`g_ksn_pie_blend` 相当）で両方 1 バイナリに入れ、切れば完全に旧経路。② 近似を入れるなら動く画素の割合とチャネル段差を名指しし（海面の前例は 10.1%、R/B 1・G 2、`:411`）、絵は実機 capture で判断 | 3 層（`pie-simd.md:591`）: `run_models.py` に blend/565 の総当たりモデル（s 256 × dst 全値 × α 256）、`test_kernels.py` で実 asm を `piesim` 実行して scalar 参照と全画素比較、`stalls.py` で 0 ストール。加えて既存 `test_group_dither.c`/`test_primitives.c` の全画面一致と、120 フレームの framebuffer ハッシュ（`pie-simd.md:478`）をアーム間で比較。**時間はホストでは測れない**（`:478`） |
| **4b. coverage を画素の述語から行の区間へ**（RECT/STROKE/ROUND_RECT の端/radius=0 を、行ごとの x 区間として先に解く） | 中〜大。`covers` の 0.92 M の大半が消える。推定 1〜3 ms/フレーム | 中。角丸の区間式が `inside_round_rect` と画素単位で一致する必要がある（`test_group_dither.c` に独立参照式がある）。半径は 8 までなので行ごとの区間は閉形式で書ける | 無し（区間が predicate と同じ画素集合を出せばビット一致。スイッチで旧 predicate 経路を残す） | `test_group_dither.c` の独立参照（画素中心距離）と全画素比較＋角丸の全半径 0..8 × 全座標の総当たり（`pie-simd.md:455` の「表現についての主張は総当たりで決着する」） |
| **4c. gradient の除算を命令へ巻き上げる**（`interpolate` の除数 `last` は 1 命令＝1 フレームで定数。逆数 or magic multiply を command ごとに 1 回） | 小〜中。`quou` は 4 本/画素 × 4,320 画素 = 17,280 本 × 17 cycles ≒ **1.2 ms/フレーム**（推定、レイテンシが重ならない上限）。ただし消えるのは除算だけ | 低（恒等式で書ける）。**ただし前例が悪い**: このコアは除算器を持つため、除算をシフト/逆数乗算に置き換える試みは測定可能な差が出なかった（`pie-simd.md:528`） | 無し（除算経路をスイッチで残す） | `run_models.py` に `(cf,ct,i,last)` の総当たりモデルを足し、floor 除算と厳密一致を証明（`x≤65152` の `/255` 恒等式と同じ形、`pie-simd.md:425`）。**時間は実機 A/B でしか言えない**ので、この候補は「まず `rsr.ccount` で gradient 経路単独を測る」を先に置く（§8） |
| **4d. `covers` の radius=0 早期判定を呼び出し側へ移す**（`inside_round_rect` 4,816 回のうち 4,320 は radius=0） | 小。0.17 M 命令 ≒ 0.6〜0.7 ms（推定） | 低（`if(!radius)return true;` を呼ぶ前に置くだけ。画素は変わらない） | 無し | 画素は `test_primitives.c` の全画面一致。呼び出し回数はホストで数えて契約化（`inside_round_rect` の起動回数 = radius>0 の画素数） |

候補 4a と 4b は独立で、順序は「4d（1 行）→ 4c（証明は総当たり）→ 4b（構造）→ 4a（カーネル）」が
`pie-simd.md:583-594` のチェックリスト（①下限と引き算 → ②単独計測 → ③スカラー参照を残す …）に
沿う。**4a の前に必ず §8 の単独計測を入れること**（`pie-simd.md` §4.6: 命令数を削るだけの最適化が 3 回連続で
差を出さなかった前例がある）。

---

## 6. 境界 5: framebuffer / present

### 何が渡り、何回か

`ksn_render_rects` は帯ごとに `display->strip()` で 1 枚の strip（`board_strip()` = `shared`、
240×8×2 = 3,840 B、16 バイト整列、`board.c:30-31`）を受け取り、埋めてから
`display->present(ctx,y,rows,pixels)` を呼ぶ（`ksn_render.c:222-283`）。
`app_session.c:153-161` の `kasane_send` は `board_present_sync(y,rows,pixels)` を呼び、
その区間を `sent_us` に積む。`board_present_sync` は `tx_reap → board_present → tx_reap`
（`board.c:426-432`）＝**前の転送を待ち、積み、この転送を待つ**。

頻度: damage mask の帯数（3〜17）。全面 17 帯で 17 回の `present`、64,800 B。

### コスト（引用）

| 場面 | send | 出所 |
| --- | ---: | --- |
| JS 診断 300 tick の平均（帯数は非公開） | **3.36〜3.44 ms** | `kasane-progress.md:89,124,139`、`design-device-probe.md:24` |
| frost stress、全 17 帯 64,800 B | **7,052〜7,076 µs** | `design-device-probe.md:224-225,283` |
| 初回全面転送 | 12,698〜13,528 µs（合成込み） | `design-device-probe.md:56,146` |
| 理論値（80 MHz、64,800 B） | 6.48 ms | 計算: 64,800×8/80e6。上の 7,052 µs は **92%** |

DRAM/帯域の側は問題ではない: 画素の書込み 64,800 B と DMA の読出し 64,800 B を合わせて
129.6 KB/フレームで、32 bit/cycle（=4 B/cycle）なら 32,400 cycles ≒ **135 µs**。ワイヤの
6.5〜7.1 ms に対し 2% である。**この面の律速はワイヤであって内部 SRAM の帯域ではない。**

`board_present` は前回の行位置 `next_row` を追い、不連続な帯では CASET/RASET/RAMWR を
出し直す（`board.c:366-372`）。全面を順に送る呼び出しでは 3 コマンドで済むが、
kasane の部分転送は**帯ごとに不連続**なので、最大 帯数×3 コマンドになる（7 帯なら 21）。

### 既に済んでいること

- 80 MHz（40 MHz から。send 15.5→9.1 ms、`board.c:262-268`）。
- byte swap は PIE、しかも `tx_buf[front]` へ直接書く（swap と memcpy が 1 パス、
  `board.c:382-411`。−0.24 ms 実測、`pie-simd.md:547`）。
- 転送は `queue_size=1` で 1 本だけ飛ばし、`shared` を二重化しない（`board.c:44-49`）。
- damage による帯のスキップ（`ksn_render.c:224-226`）。

### 候補

| 候補 | 大きさ | リスク | 撤退線 | 実機なしの検証 |
| --- | --- | --- | --- | --- |
| **5a. 帯の転送を描画と重ねる（present を非同期化し、ack をフレーム末尾に移す）** | **大**。send は 3.36〜3.44 ms（JS 診断）／7.1 ms（全面）で、ホーム画面では既にこれで −6.31 ms を取っている（`pie-simd.md:546`）。重ねられれば **2〜3 ms 台/フレーム**（推定） | **高**。`ksn_ports.h:16` が「present は strip を同期的に消費する」と定義し、`ksn_core_presented` は全帯成功の後に bank を昇格する（`ksn_core.c:404-413`、`app_session.c:157` のコメント）。非同期化は**昇格/再送/IO 失敗のプロトコル変更**を伴う | **あり**（`board_present_sync` を残し、切れば現行）。ただしプロトコル変更は別コミット・別設計として扱う | プロトコル部分は `tools/kasane_contract/test_render.c`（途中転送失敗の再送）と `test_repair.c` が既に判定する。**時間はホストで測れない**（`pie-simd.md:478`）ので、実機 `KASANE_PAINT send_ms` の A/B（同一バイナリで窓ごとに反転、`pie-simd.md` §6.2）が必要 |
| **5b. 不連続帯の re-window を減らす**（帯を y 昇順に送る・連続帯は RAMWR を維持する。`board.c:366-372` は y 昇順の連続なら既に維持する） | 小。7 帯で最大 21 コマンド ≒ 60〜120 µs（推定、`command()` の SPI オーバーヘッド込み） | 低（順序を変えるだけ。ただし順序を変えると `present_sum`/`render_sum` の意味は変わらない） | 無し | `test_render.c` の転送順・画素一致。帯数とコマンド数のカウンタを足して契約化（§8） |

PSRAM を前提とした候補（フレームバッファの外部化、帯の PSRAM 経由、二重バッファの
PSRAM 配置）は**この機体には存在しないので書かない**。全画面バッファ 64,800 B を内部 SRAM に
置く案も、`hardware-constraints.md:58` が「常駐二重バッファを避ける」としており、かつ
ホーム画面の空き 274 KiB（`CLAUDE.md:86`）から 25% を恒久消費するので候補にしない。

---

## 7. 境界 6: compile / ABI・配置

### 実測（target ツールチェーン、`-Os`）

| 項目 | 実測値 | 方法 |
| --- | --- | --- |
| `ksn_render_rects` の命令数 | **972** | `objdump -d`（`render_group` はこの中へ inline される） |
| 補助関数の命令数 | `blend` 50、`sample` 57、`covers` 52、`inside_round_rect` 36、`pack565` 35、`quantize` 19 | 同上（**すべて out-of-line の `call8`**、`mul8`/`clamp8`/`channel`/`interpolate` は inline されたので symbol が無い） |
| 同じ補助関数の `-O2` | `blend` 62、`pack565` 67（`$part$0`）、合計 1,133 命令 | 同上。**`-O2` は命令数が増える** |
| `ksn_core_read` | 139 命令 + `memset` + `memcpy`×5（relocation で確認） | 同上 |
| `sizeof` | `ksn_frame_command` **176**、`ksn_draw` 64、`ksn_command_storage` 32、`ksn_change` 18、`ksn_core` 516、`ksn_core_command_block` 3,072 | `nm -S`（target オブジェクト） |
| stack フレーム | `ksn_render_rects` **928**、`apply_placement`/`ksn_cache_instantiate` 176、`core_add` 112、`ksn_core_damage` 80、`ksn_core_group` 64、`covers`/`blend`/`pack565`/`quantize` 32、`sample`/`inside_round_rect` 48 | `-fstack-usage` |
| 配置 | `main/` に **`IRAM_ATTR` は 1 つも無い**（コメント 1 件のみ）。kasane は flash から実行され、i-cache 越し | `grep -rn IRAM_ATTR main/` |
| 最適化レベル | kasane のソースは**プロジェクト既定の `-Os`**。`-O2` なのは `shell.c`、`scene/*`、`render_accel.c` のみ | `sdkconfig.defaults:5`、`main/CMakeLists.txt:72` |
| 定数 | `bayer4` 16 B（`.rodata`）、frost の重み 32 B（flash）、frost span の text 792 B・scalar 制御 627 B | `ksn_render.c:43`、`design-device-probe.md:297` |

`ksn_render_rects` の **928 B** は、`kasane-progress.md:229` が「描画追加 stack 1 KiB 目標の
達成は未認定」と書いている目標に対して、**レンダラ自身のフレームだけで 928 B** である。
`ksn_font` の span は単体 160 B（`design-device-probe.md:298`）なので、深い呼び出し鎖は
1 KiB を超える可能性が高い。これは最適化候補ではなく**制約の記録**（per-pixel 面の候補が
一時配列を増やすときは、まずこの 928 B を見る）。

### 候補

| 候補 | 大きさ | リスク | 撤退線 | 実機なしの検証 |
| --- | --- | --- | --- | --- |
| **6a. `ksn_render.c` を `-O2` にする** | 不明。命令数は逆に増える（972→1,133）ので、効くとしても配置と register 割当の効果 | 低（CMake 1 行） | 1 行を戻す | 同一バイナリ A/B が要る（`pie-simd.md` §6.3 の 15% 配置ノイズ）。ホストでは `objdump` の命令数と stack フレーム、画素一致だけ |
| **6b. per-pixel ホット部分に `IRAM_ATTR`** | 不明。`pie-simd.md` §6.3 は「同じカーネルがビルド間で 15% 動く（i-cache 16 KB を両コアが共有）」と書いており、配置の効果は測らないと分からない | 中（IRAM は DRAM バンクを食う。`CLAUDE.md:86` の 334 KiB 予算） | 属性を外す | ホストでは `nm`/`objdump` で配置だけ確認。効果は同一バイナリ A/B（スイッチで関数ポインタを切替）＋実機 |
| **6c. 補助関数を `always_inline` にする**（`blend`/`sample`/`covers`/`pack565`/`quantize` は per-pixel の `call8`） | 小〜中。呼び出し 1 回あたり `entry`/`retw` と register window の回転。out-of-line なのは **6.65 万回/フレーム**（実測: `covers` 17,672 + `sample` 13,948 + `blend` 5,534 + `pack565` 11,590 + `quantize` 12,960 + `inside_round_rect` 4,816。`premultiply_over`/`group_over` は inline）。1 回 2〜4 cycles なら 13〜27 万 cycles ＝ **0.6〜1.1 ms**（推定、レイテンシは重なりうる） | 低（`static` 関数の属性 1 つ）。ただしコードが膨らみ i-cache を圧迫する | 属性を外す | **ホストで数えられる**: `objdump -d` で `call8` が消えたこと、`-fstack-usage` でフレームが増えていないこと、既存テストの画素一致。時間は実機 A/B |

`pie-simd.md` §6.3「命令キャッシュのアラインメントで同じカーネルがビルド間で 15% 動く。それ未満の差を
主張するなら同一バイナリでの比較が要る」— この面の候補はどれも数 % 台なので、**単独ビルドの
比較では判定できない**。`g_board_async`/`g_garden_canopy_pie` と同じく runtime 変数にして
1 バイナリに入れる（`pie-simd.md:472`）。

---

## 8. 境界 7: 計測・帰属（ここが空いているので他が動かない）

### いま存在する計器

| 計器 | 何を出すか | 場所 |
| --- | --- | --- |
| `KASANE_PAINT` | `turn_ms` / `render_ms` / `send_ms` / `bytes`（30 フレーム平均） | `main/app_session.c:1045-1054` |
| `ksn_render_stats` | `bands`（ビットマスク）と `transferred_bytes` | `ksn_render.c:282`、`ksn_render.h:7` |
| `KASANE_TICK` | `commands`、`nativeBytes`、`inputScope` | `apps/kasane/demo.js:87-91` |
| `PERF` | `draw`/`prep`/`loop`/`kernel`/`hud`/`send`、`SCENE_AB` の同一バイナリ A/B | `main/ui/shell.c:607,620-638` |
| `SPLIT`/`SPLIT2`/`SPLIT3` | flower の `garden`/`pixels`/`decor`/`veg`、canopy のサイクル | `main/scene/flower.c:1618,1659,1691` |
| `AB arm=…` | `canopy` と `swap` のスイッチ反転＋対照欄 | `main/ui/shell.c:620-638` |
| `rsr.ccount` | カーネル単独のサイクル | `main/scene/garden.c:11,25`、`flower.c:1600` |
| 実機診断 | native コア回帰、画素回収、PIE A/B、600 フレーム stress | `tools/kasane_contract/device_probe.py`、`tools/kasane_device_test.py`、`main/ui/kasane/ksn_device_probe.c` |

### 帰属できていないもの（この面が空けている当人）

1. **`render_ms` の中身が 1 つの数**。背景 fill / span（フォント）/ 群の tile / 直接経路の
   per-pixel / `ksn_core_read` のどれも分離されていない。`PERF` の `kernel=` に相当する欄が無い。
   `pie-simd.md:520`（合成された指標を要素の代理に使わない）がそのまま当てはまる。
2. **サイクル計測が 1 つも無い**。garden は `rsr.ccount` でカーネル単独を出しているのに、
   kasane の描画経路には無い。`pie-simd.md` §4.6 が「次のカーネルを書く前にそのパスを `rsr.ccount` で
   括り床との比を出せ」と要求しているが、いまは括れない。
3. **帯分布が見えない**。`stats.bands` は既に計算されているのに、ログは `bytes` しか出さない
   （`app_session.c:1050-1052`）。7 帯なのか 14 帯なのかが分からないので、send の平均値から
   帯数を逆算するしかない（本調査でもそれができなかった。§10）。
4. **再読出し回数・per-pixel 起動回数が数えられていない**。本調査はホストのハーネスで
   数えた（§13）が、リポジトリにこの 1 つの数字を出す道具が無い。
5. **スイッチが 1 つも無い**。`board.c` の `g_board_async`/`g_board_swap_into` はあるが、
   kasane の描画経路には `g_ksn_*` が無い。`pie-simd.md` §6.1 の「最適化とその計測を同じコミットに
   入れない」「機能を書くのと同じコミットでスイッチを書く」を満たす場所が無い。
6. **`esp_timer_get_time` のコストが `render_ms` に混ざる**。`present_frame` が 1 回、
   `kasane_send` が帯ごとに 2 回呼ぶ（`app_session.c:1033,156,159`）。1 回 0.90 µs
   （`pie-simd.md:510`）で 7 帯なら 15 回 ＝ 13.5 µs。小さいが、`loop` の前例（`pie-simd.md` §6.5）では
   これが「33 cycles の残差」の正体だった。

### 追加すべき計器（この面の候補）

| 候補 | 何を出すか | 実機なしの検証 |
| --- | --- | --- |
| **7a. `rsr.ccount` を両端に置いた描画フェーズ別カウンタ**（`fill` / `span` / `tile` / `direct_blend` / `read`）を runtime スイッチ越しに | 契約する数は**サイクル**（フレームあたり、窓平均）。`PERF` の `kernel=` と同じ意味の欄が kasane にもできる | ホストでは時間を測れない（`pie-simd.md` §6.7）。だからホストの役目は「置いたカウンタが他の経路を変えないこと（画素一致）」と「回数の契約」（`read` 回数、`span` 回数、合成画素数）に分ける。§13 のハーネスが後者の雛形 |
| **7b. `stats.bands` をログに出す**（マスク、帯数、連続区間数） | 帯分布・不連続数。send の帯数逆算が不要になる | 不要（既に計算済みの値を出すだけ）。`test_render.c`/`test_repair.c` が `stats` の意味を判定している |
| **7c. `KASANE_PAINT` に内訳欄を足す**（`fill_ms`/`text_ms`/`group_ms`/`blend_ms`/`read_n`） | 要素別のミリ秒。`pie-simd.md` §6.2 の「動かないはずの欄を対照に出す」も同時に満たす | ホストは回数だけ約束し、ミリ秒は実機。計器を足すコミットと最適化を入れるコミットは分ける（`pie-simd.md` §6.1） |
| **7d. 同一バイナリ A/B の `AB` 行**（`g_ksn_pie_*` を窓ごとに 1 つ落とし、対照欄を同時に出す） | 差の判定（`pie-simd.md` §6.2）。`shell.c:620-638` と同じ形 | ホストはアーム間の framebuffer ハッシュ一致だけを保証（`pie-simd.md:478`） |
| **7e. 本調査のホスト境界カウンタを `tools/kasane_contract/` へ**（未コミットの雛形を §13 の形で） | `ksn_core_read` 回数（呼び出し元別）、`span` 回数、coverage バイト、present 回数・バイト、per-pixel 補助関数の起動回数 | それ自体がホストの道具。実機不要で毎回走る |

`pie-simd.md` §4.6 の結論をここに置く: 兄弟シーンで命令数削減が 3 回続けて差を出さなかったのは、
「床の 2.4 倍」という比そのものを変えない限り命令数を削っても当たらないからで、
効いたのは仕事の置き方を変えた構造変更だった。kasane でその判定をするには、
**まず 7a で per-pixel 面を単独に測る**必要がある。本調査の §5 の 10〜13 ms は
命令量からの推定であって、サイクルの実測ではない。

---

## 9. 順位付きショートリスト（大きさ × 確度）

| 順位 | 候補 | 大きさ（推定） | 確度 | まず何をするか |
| ---: | --- | --- | --- | --- |
| 1 | **7a/7b/7c/7d: 計測と帰属**（per-pixel 面の単独サイクル、帯分布、内訳欄、同一バイナリ A/B スイッチ） | これ自体は 0 ms。ただしこれが無いと 2〜5 のどれも `pie-simd.md` §4.6/§6.3 の判定に耐えない | 高 | カウンタと最適化を別コミットに分けて先に入れる |
| 2 | **4a: RGB565 blend/pack の PIE カーネル** | **2〜4 ms/フレーム**（この面 10〜13 ms のうち blend/pack 系 1/3、前例 4 倍）。全面フレームなら 3〜5 ms | 中 | 7a で blend 経路単独のサイクルを出し、床との比を確認（`pie-simd.md` §4.6）。次に 4d→4c で周辺を落としてから |
| 3 | **4b: coverage を画素述語から行区間へ** | 1〜3 ms（`covers` 0.92 M の大半） | 中 | 角丸の区間式を総当たりで参照と一致させる（`test_group_dither.c` の参照式を拡張） |
| 4 | **2a: デコード結果の再利用**（群の 2 パス再読出しを消す） | **0.4〜0.6 ms**（フレーム内で命令ごとに 1 回だけ展開すれば read 558→~24。1 回 139 命令 + memset/memcpy） | 高（画素に影響しない） | §13 のハーネスで read 回数を契約にする。実機 A/B は不要なほど確実だが、時間は測る |
| 5 | **3c: 不透明群の tile 迂回** | 0.3〜0.8 ms（`premultiply_over`+`group_over` の 5%） | 低（丸めが動く） | 丸めが動く画素を名指しできるか。動くなら見た目判断＋撤退線 |
| 6 | **4c: gradient の除数巻き上げ** | 上限 1.2 ms、期待は小（除算器の前例が「差なし」） | 低 | 4b と一緒に（同じループ） |
| 7 | **5a: present の非同期化** | 2〜3 ms | 中（大きさ）／低（実現性） | プロトコル設計として別に扱う。実機の `send_ms` A/B が必須 |
| 8 | **3b: tile 64→16 画素** | 0.1〜0.3 ms | 高 | ブロック幅を定数にして画素一致 |
| 9 | **6a/6c: `-O2` / `always_inline`** | 不明（配置ノイズ 15% の中） | 低 | 同一バイナリ A/B を先に作る（7d が前提） |
| 10 | 1a/1b: JS getter まとめ、bank コピー削減 | ≤0.1 ms | 高 | やるなら最後。境界 1 はレバーではない |

**最大のレバーは 4a（per-pixel の PIE カーネル）で、その前に 7（計測）が要る。** ただし
「最大」の根拠は ①per-pixel 補助呼び出しが 28.6 万〜58.6 万回/フレーム（実測）②命令量の
見積りが実機 `render_ms` と同桁（12.6〜13.6 ms）③同じ 565 blend の前例が 4 倍（引用）で、
①実測 ②推定 ③引用 が混ざっている。**実機の単独サイクルを測るまでは「大きい見込み」であって
「大きいと分かっている」ではない。**

---

## 10. 未解決（静的に決着できなかったもの）

1. **実機の帯分布**。`demo.js` の 1 tick を再構成すると 14 帯だが、実機の `send_ms` 3.37 ms
   から逆算すると 26,880 B（7 帯）程度になる。実機は `bytes` を 30 フレーム平均でしか出さず、
   `stats.bands` を出していないので、どちらが代表値かは決まらない（7b で解決する）。
2. **`render_ms` の実内訳**。span（フォント）と per-pixel と `ksn_core_read` の比は
   実機では不明。ホストの起動回数は出たが、フォント側の内部ループ（`ksn_font.c:32-49` の
   グリフごとの 64 画素窓走査）の命令量は本調査で数えていない。
3. **IPC**。命令量 → 時間の換算に 1.0〜1.3 を使った。これは `pie-simd.md` のスカラー計測
   （別のコード形）からの借用で、kasane のこの経路で測った値ではない。
4. **`esp_timer_get_time` 1 回 0.90 µs** は海面の場面で測った値（`pie-simd.md:510`）で、
   kasane の帯ループでの値ではない。
5. **テキストのフォント実体**。ホスト検証は `tools/kasane_contract/test_font.c` の合成フェイス、
   実機は jpfont パーティション。flash cache 越しの字形読みが支配的な可能性は未計測。
6. **タスク切替と PIE 状態の退避**。`pie-simd.md` §2.6 は PIE がコプロセッサ 3 なので切替ごとに 8 本の QR の
   退避・復元が起きると書く。kasane の描画経路での寄与（1.3〜1.4 倍の一部）は未計測。
7. **`ksn_render_rects` の 928 B フレーム**は `-Os` 単独ビルドの値で、実機のピーク
   （子関数鎖・span・フォント）は未計測。`kasane-progress.md:229` の 1 KiB 目標に対する
   判定ができない。
8. **`group_over`/`premultiply_over` の命令数**（~36/~42）は inline されているため
   `objdump` の関数単位では出せず、`ksn_render_rects` の中を数える必要がある。本調査では推定。

---

## 11. 触らないもの（と理由）

| 対象 | 理由 |
| --- | --- |
| `fill_blocks`/`fill565`（PIE の連続 fill、`ksn_render.c:11-39`） | 8 画素あたり 3 命令（`ee.vldbc.16` + `loopgtz` + `ee.vst.128.ip`）で、`pie-simd.md` §2.1 の床そのもの。残るのは帯あたり最大 7 画素の scalar 頭出しだけ。触る余地が無い |
| `board.c` の 80 MHz、`g_board_async`、`g_board_swap_into` | すべて実測済みの勝ち（−6.31 ms、−0.24 ms）。**byte swap を 32bit 単位にする案は実測で悪化**（16.5→24.2 ms、`pie-simd.md:554`）。同じ案を再試行しない |
| `ksn_frame_command`（176 B）と `ksn_ports.h` のポート定義 | 公開 ABI 兼契約。per-pixel の候補が構造体を小さくする案を出すなら、契約変更として別に議論する |
| `ksn_core_read` の検証・poison・世代検査（`ksn_core.c:486-528,53-61`） | 正しさの中核。回数を減らす候補（2a）は**読み出しの再利用**であって検証の削除ではない |
| 群の隔離 premultiplied 合成の丸め（`ksn_cache.h:54-55`） | PATCH 下での丸め安定性という明示的な契約。3c は「不透明なときだけ迂回」＋「動く画素を名指し」＋撤退線が無い限り触らない |
| `present` は strip を同期的に消費する（`ksn_ports.h:16`）と、ack 後に bank を昇格する（`ksn_core.c:404-413`） | 再送・IO 失敗・repair の正しさがこの 2 つに乗っている。5a はこれを変える**設計変更**として扱い、最適化のつもりで触らない |
| PSRAM を前提とする一切の案 | この機体に PSRAM は無い（`hardware-constraints.md:14`） |
| `tools/kasane_contract/` の既存テストと `docs/kasane/*` の記述 | 本調査は分析のみ。既存の期待値・記録は動かさない |

---

## 12. 検証の型（実機なしでの比較の作り方）

どの候補も次の 4 段で作る（`pie-simd.md:443-455,591` の型）:

1. **算術の総当たり**（`tools/pie/run_models.py` + `tools/pie/models/*.c`）: 式・表・丸めを
   入力の全域で参照実装と一致させる。`/255` は `(x + (x>>8) + 1) >> 8` の恒等式
   （`pie-simd.md:425`）、blend の 16bit レーン化は `pie-simd.md:418` の恒等式。
2. **アセンブリの命令実行**（`tools/pie/test_kernels.py` + `piesim.py`）: 実 asm を解釈実行して
   スカラー参照と全画素比較。未対応命令は例外で止まる（黙って通らない）。
3. **静的ストール解析**（`tools/pie/stalls.py`）: `loopgtz` の 256 B 制限、ストール 0 まで。
4. **全画面の画素一致とフレームハッシュ**: `tools/kasane_contract/run.sh` の
   `test_composition.c`/`test_group_dither.c`/`test_primitives.c`/`test_text_render.c` が
   32,400 画素を独立参照と比較する。新しい候補はこの 4 本に 1 ケースずつ足し、
   アーム間（スイッチ on/off）で 120 フレームのハッシュを比べる（`pie-simd.md:478`）。

**時間はホストでは測れない。** ホストで契約にできるのは数えられる量（呼び出し回数、
合成画素数、空回り画素数、read 回数、framebuffer ハッシュ）だけで、それをミリ秒に翻訳
しないこと（`pie-simd.md:524`、§4.2 の失敗例）。

---

## 13. 付録: 本調査が実行したコマンドと数え方

### 13.1 target 命令数・型・スタック

```sh
XT=/root/.espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin
cd /workspace/pjs-kasane
$XT/xtensa-esp32s3-elf-gcc -Os -std=gnu11 -Imain/ui/kasane -c main/ui/kasane/ksn_render.c -o /tmp/ksn_render.o
$XT/xtensa-esp32s3-elf-objdump -d /tmp/ksn_render.o | awk '/^[0-9a-f]+ </{n=$2} /^ *[0-9a-f]+:/{c[n]++} END{for(k in c) printf "%6d %s\n",c[k],k}' | sort -rn
$XT/xtensa-esp32s3-elf-gcc -Os -std=gnu11 -fstack-usage -Imain/ui/kasane -c main/ui/kasane/ksn_render.c -o /tmp/su.o   # .su ファイルが出る
printf '#include "ksn_core.h"\nchar a[sizeof(ksn_frame_command)];\n' > /tmp/sz.c
$XT/xtensa-esp32s3-elf-gcc -Os -std=gnu11 -Imain/ui/kasane -c /tmp/sz.c -o /tmp/sz.o && $XT/xtensa-esp32s3-elf-nm -S /tmp/sz.o
```

IDF ヘッダが無くても kasane のソースは単体でコンパイルできる（`sdkconfig.h` は
`ESP_PLATFORM` 定義時のみ、PIE は `CONFIG_IDF_TARGET_ESP32S3` で切り替わる）。**実機用の
ファームウェアをビルドしたわけではない。**

### 13.2 境界の通過回数（ホスト、未コミット）

`/tmp/ksncount/harness.c` が `apps/kasane/demo.js` の場面（背景・dither gradient・2 テキスト・
2 矩形の群・角丸 2 つ・stroke・2 命令 template の instance 2 つ・modal open/close）を
`ksn_view`/`ksn_cache`/`ksn_core`/`ksn_render` を通して再構成する。

```sh
cc -std=gnu11 -O2 -g -no-pie -fno-inline -finstrument-functions \
  -Imain/ui/kasane -Imain/text -Imain/hal -Itools/hostshim -Itools/kasane_contract \
  -Itools/kasane_contract/fontshim -I$(mktemp -d) \
  main/ui/kasane/ksn_{core,render,view,cache,modal}.c main/text/{ksn_font,jpfont}.c \
  harness.c -Wl,--wrap=ksn_core_read -o ksncount
python3 tools/make_font.py <生成先>   # fonts.h
```

数えているもの:

- `ksn_core_read` は `-Wl,--wrap` で包み、`__builtin_return_address(0)` で**呼び出し元ごとに**
  集計（`ksn_render_rects` / `render_group` を分ける）。アドレスは `nm` で名前に戻す。
- per-pixel 補助関数の起動回数は `-finstrument-functions`（`__cyg_profile_func_enter`）で
  アドレス別に集計。**`-O0`・`-O2`・`-fno-inline` の各ビルドで read/span/present と
  補助関数の起動回数が一致することを確認した**ので、値は最適化に依存しない構造量である（`-fno-inline` を付けると target で inline される
  `mul8`/`clamp8`/`channel` も数に入る。これは target の命令量に数えてはいけない）。
- `present` はハーネス自身のポートで回数・バイト・帯数を数える。
- **限界**: ホストはフォント face を開いていないので、ASCII は `font_rows` の 5×7、非 ASCII は
  tofu で描かれる。したがって **TEXT 経路の `blend` 起動回数はインク密度に依存**し、実機より
  多い可能性がある（coverage バイト数 5,320 と span 呼び出し回数 97 はインクに依存しないので
  契約に使える）。字形の実体を入れた版は `tools/kasane_contract/test_font.c` の合成フェイスを
  使えば作れる。

このハーネスは**リポジトリに入れていない**（本調査の唯一のコミットは本文書 1 ファイル）。
恒久的な道具にするなら、`tools/kasane_contract/` に置いて `run.sh` から呼ぶ形が既存の流儀に合う。

### 13.3 出所の一覧

- 実機の値: `docs/kasane/verification.md`（現在のゲートと到達範囲）、
  `docs/perf/pie-simd.md` §7。初期診断の詳細はGit履歴。
- モデルと作法: `docs/perf/pie-simd.md` §2・§3・§4・§5・§6、`docs/perf/backlog.md`、
  `tools/pie/README.md`。
- 本調査が自分で実行した値: §13.1 の objdump/nm/stack-usage、§13.2 のハーネス、
  `grep`（IRAM の不在、`-O2` 対象ファイル）。
