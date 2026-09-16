# 容量削減メソッド: cref でランキングを作り、薄いところから潰す

対象は ESP32-S3 のファーム（`cardputer-adv-pocketjs`）。目的はフラッシュを小さくすること。
この文書は**手順書**で、実測した事実は [builtins-census.md](builtins-census.md) にある。
数値はすべて `origin/vm/main` の基点 `bd0fa43`（素の `idf.py build`）時点のもの。

## 0. 原則（3行）

1. フラッシュを減らすとは、**我々のコードが要求しているライブラリ関数の「入口」を減らす**こと。
   ライブラリ側を書き換える手段は無い。
2. **参照元が薄い（我々だけ／少数）シンボルほど落とせる**。参照元が 40 以上あるものは、
   我々が全部消しても 0 B（実例: 64bit 除算 `__divdi3` 系）。薄さはそのまま期待値のランキングになる。
3. ただし薄さだけでは足りない。Rust の `compiler_builtins` は**1つの `.o`（cgu 会員）に複数の
   libm 関数を束ねている**ので、我々の 1 つの libcall が会員丸ごと（数 KB〜10 KB）を引き込む。
   金額は**会員単位**で数える（配置バイト × 取り込み理由）。

## 1. ランキングの作り方（式）

マップの3箇所を突き合わせる。視点と道具は次のとおり。

| 視点 | マップ上の場所 | 道具 |
| --- | --- | --- |
| 配置（真実のバイト数） | `Linker script and memory map` の `.text.X` + `0xADDR 0xSIZE OBJ` | `member_bytes.py` |
| 取り込み理由（誰が要求したか） | `Archive member included to satisfy reference by file (symbol)` | `member_bytes.py` / `map_census.py` |
| 相互参照（参照元の全数） | `Cross Reference Table`（`--cref`） | `map_census.py` |

```
薄さの4段階（上ほど落としやすい）
  ① 我々だけが参照元        → 値で証明できる書き換えで消せる。ここから潰す
  ② 我々＋他が1〜2          → 他が消えないと落ちない。我々側だけ直しても 0 B のことが多い
  ③ 参照元が多数（40+）     → 我々が全部消しても 0 B。触らない（サイクルの話なら別途価値）
  ④ 我々が参照元でない      → 我々には無関係

入口の金額 = その会員が所有する配置セクションの合計バイト
期待値     = 会員金額 × 落ちる確率（スタブビルドで白黒つける） − 実装コスト
優先順位   = 期待値の降順。同額なら「値で証明しやすい」順
```

注意すべき非線形性が2つある。

- **内部連鎖**: 入口の会員を落とすと、その会員が呼んでいた別の会員が連鎖で落ちる（逆に、
  内部連鎖が別の入口を持つと、我々の入口を消しても落ちない）。金額は「入口の会員」ではなく
  **連鎖込みの合計**で測る。スタブビルド 1 回で確定する。
- **複数の入口**: 同じ会員を複数の我々のファイルが押さえていることがある（`sinf` は 7 ファイル）。
  **全部の入口を外すまで落ちない**。1ファイルだけ直して「効かない」と結論しない。

## 2. 手順（コマンドつき・この順で）

### Step 0: 基準を確保する
```bash
. /opt/esp-idf/export.sh
idf.py -B build_base build && git checkout -- dependencies.lock
cp build_base/cardputer_pocketjs.map /workspace/knscratch/size/base/<commit>.map
python3 tools/memlog.py --map build_base/cardputer_pocketjs.map    # flash / DIRAM の基準
T=tools/size
python3 $T/member_bytes.py <map> --total                           # 画像全体の配置合計
```

### Step 1: 会員単位の金額と取り込み理由を出す（入口一覧）
```bash
python3 $T/member_bytes.py <map> compiler_builtins     # 会員ごとのバイト
python3 $T/map_census.py <map> --member compiler_builtins --top 20
```
`included` 節を会員ごとに読むと「ours:libmain.a(pocket_bridge.c.obj)(__fixdfsi)」のように
**我々のファイルとシンボル名**が出る。ここが入口リスト。会員名の cgu 番号も控える。

### Step 2: 参照元の分類で薄さランキングを出す
```bash
python3 $T/map_census.py <map> --library-targets --top 32
```
「own（会員の持ち主）」が `rust-builtins` / `libc` / `rust` で、`kinds` が `1xours` だけの行が ①。

### Step 2.5: 会員が「本当に落ちるか」を機械で判定する（inclusion の理由は「最初の参照元」にすぎない）
```bash
python3 $T/member_bytes.py <map> --droppable 200
```
判定規則は**その会員が持つ配置シンボル全部の参照元が我々だけ**であること。マップの
`Archive member included to satisfy reference by file (symbol)` は**最初に要求した参照元を1つ**
書くだけなので、あとから入った会員が同じ会員の別シンボルを要求していても出てこない。
実例: `libfatfs.a(ff.c.obj)` 6,863 B は理由が `sd_media.c (f_getfree)` で我々だけに見えるが、
`vfs_fat.c.obj`（`esp_vfs_fat_register` が引き込む）が `f_open` / `f_read` / `f_write` を
要求しているので、`f_getfree` を消しても 0 B。
`--droppable` はこの規則で「落ちる会員」と「見た目だけの会員（他人が同じ会員を押さえている）」の
2つのリストを出す。

### Step 3: 入口の呼び出し箇所をソース行まで落とす
```bash
O=build_base/esp-idf/main/CMakeFiles/__idf_main.dir/<path>.obj
xtensa-esp-elf-nm -u $O | grep -E 'fixdfsi|gedf2|divdf3'        # どのファイルが要求しているか
xtensa-esp-elf-objdump -dr $O | grep -nE 'R_XTENSA_(32|ASM_EXPAND)\s+__fixdfsi'
xtensa-esp-elf-addr2line -f -e $O 0x50                          # → ソースファイル:行
```
マングル名（`_RNv...4sinf4sinf`）しか見えないときも、この addr2line 経路ならソース行に着く。

### Step 4: 「本当に落ちるか」をスタブビルド 1 回で判定する
対象の libcall だけが消えるように**一時的に**書き換える（比較式を定数に、`(int)double` を整数式に、
trig を 0.0f に）。挙動は壊れてよい。**この実装はコミットしない**。
```bash
idf.py -B build_stub build && git checkout -- main/ dependencies.lock
python3 $T/member_bytes.py build_stub/cardputer_pocketjs.map compiler_builtins
python3 $T/member_bytes.py build_stub/cardputer_pocketjs.map --total
```
落ちない場合の典型: ② の理由（他の参照元がいる）／内部連鎖の別入口が生きている／
その会員が別の我々のコードの別シンボルも提供している。**落ちなければここで止める**（書き換えない）。

### Step 5: 本実装（値で証明できる書き換え）
- 整数化・固定小数化・別 API へ寄せる、のいずれか。桁溢れの見積り（`< 2^31` など）を式で示す。
- 証明: 旧式と新式の総当たり（数億点規模、不一致 0）＋**実データのハッシュ一致**。
- 見た目が動く変更は「動いた画素の割合」と「チャネルごとの最大段差」を数字で出す。
  採否はユーザーが実機で見る。ビット一致でない限り既定 ON にしない。

### Step 6: 本実装のビルドで実測してコミット
```bash
idf.py -B build_new build && git checkout -- dependencies.lock
python3 $T/member_bytes.py build_new/cardputer_pocketjs.map compiler_builtins   # 会員の前後
python3 tools/memlog.py --map build_new/cardputer_pocketjs.map                  # flash / DIRAM
stat -c %s build_new/cardputer_pocketjs.bin
```
コミットメッセージには「何を選んだか・実測値（bin / flash / DIRAM / 落ちた会員とバイト数）・
実機で未確認の点」を書く。1 関心 1 コミット。

**`memlog` の `flash` は DROM（rodata）を含まない。** rodata の塊を消す変更では、memlog の
flash がほとんど動かないのに bin が大きく減る。実測例（公開 CA 束 67.5 KB を一時的に外した
スタブ）: bin **−70,624 B** / 配置合計 **−69,798 B**（DROM −68,492、IROM ±0）に対し、
memlog の flash は −1,312 B しか動かなかった。**主張に使うのは bin と配置合計**、
領域別に見たいときは DROM（0x3C0…）/ IROM（0x420…）に分けて数える。

### Step 7: 統合して再計測する
並立枝は**単独では落ちない組み合わせ**がある（例: `sinf` は 7 ファイル＋別会員の内部呼び出し）。
統合枝で `member_bytes.py` を回し直し、合計が「各枝の和」になるとは限らない前提で測る。

## 3. この方法で出た実例（`bd0fa43` 時点）

```
我々が入口になっている compiler_builtins 会員
  cgu.13  10,244 B  <- main/pocket/pocket_bridge.c の __fixdfsi（double→int 変換）
  cgu.04   2,248 B  <- main/pocket/pocket_av.c の __gedf2（double の比較）
                        └ 内部で sinf を呼ぶため、これが居る限り trig も落ちない
  cgu.08   1,188 B  <- main/ui/shell.c の __divdf3（double の除算）
  ＋内部連鎖の会員が 25,338 B（cgu.14/.09/.11/.01/.06/.02/.15/.03）─ 上のどれかにぶら下がる
  ＋Rust core 自身が要求する cgu.10 3,786 B ── これは我々には落とせない（④）
参照元は我々だけ（①）
  sinf 2,490 B（7ファイル） / cosf 2,394 B（4ファイル） / tanf 1,925 B（wave.c のみ）
  remainder 402 B（solar_sail.c のみ） / sqrtf（flower・flower_species・motion・solar_sail）
③ で 0 B の実例
  64bit 除算 __divdi3 / __udivdi3 / __umoddi3 / __moddi3（参照元 40+。我々は 36 箇所・17 ファイル）
libgcc は 102 B（_ffsdi2.o 35 B ← GPIO、_popcountsi2.o 67 B ← efuse。どちらも IDF 由来で我々は 0 B）
```

**スタブビルドで確定したこと（前例）**: シーンの trig 参照を全部外すと `cosf` −2,402 B と
`tanf` −1,925 B は落ち、`sinf` は ±0 B で残った ── 残った理由は cgu.04 の内部呼び出しで、
cgu.04 は我々の `pocket_av.c` の `__gedf2` が入口だった。「薄い＝落ちる」ではなく
「**入口が全部消えて、かつ生き残る会員が内部で呼んでいない**」が落ちる条件である。

### 3.1 薄さランキングの外にあった最大の獲物（公開 CA 束 = データの塊）

```
69,730 B = libmbedtls.a(x509_crt_bundle.S.obj) 67,536 + libmbedtls.a(esp_crt_bundle.c.obj) 2,194
入口は main/pocket/pocket_net.c の1箇所だけ:
    .crt_bundle_attach = http.tls ? esp_crt_bundle_attach : NULL
スタブ実測（その1行を NULL に差し替えて 1 ビルド）:
    bin        2,142,864 -> 2,072,240   （−70,624 B = 画像の 3.3%）
    配置合計   1,820,667 -> 1,750,869   （−69,798 B。DROM −68,492 / IROM ±0）
    消えた会員 x509_crt_bundle.S.obj −67,536 / esp_crt_bundle.c.obj −2,194 / mbedtls の糊 −136
```
参照元は我々の 1 行だけなので**置き換えは同じ 1 行**でできる。選択肢は
(a) CA を付けない（`http.tls` でも検証しない）、(b) **自分の CA をピン留め**
（`esp_http_client_config_t.cert_pem` に PEM を1本 = 1〜2 KB、検証は維持）、
(c) 計測用ビルドにだけ入れる。ただしファーム内の `apps/netcheck` は example.com / google /
github / letsencrypt / badssl に HTTPS するアプリなので、公開 CA を外すとあの診断は成立しない。
採否は製品判断（機能の話）で、この方法の「余剰」ではない。

## 4. 機能と余剰を混同しない

参照元が我々だけ、という意味では次のものも ① に見えるが、これは**機能**であって余剰ではない。
消すなら機能を捨てる判断（ユーザーの領分）になる。数値を添えて報告だけする。

```
libquickjs-ng.a(quickjs.c.obj)          302,069 B   JS エンジン本体
libminimp3.a(minimp3.c.obj)              19,256 B   MP3 復号
libesp_driver_i2c.a(i2c_master.c.obj)     9,537 B   I2C（キーボード等）
libesp_driver_uart.a(uart.c.obj)          9,088 B   UART ドライバ
libesp_http_client.a(...)                 5,874 B   HTTP クライアント
libesp_driver_rmt.a(rmt_tx.c.obj)         6,182 B   RMT
```

## 5. 落とし穴（全部このリポジトリで実際に踏んだもの）

- **測定は素の `idf.py build` 同士で比較する**。`-D CMAKE_C_FLAGS=...` を付けた検証ビルドと
  混ぜると差分が意味を失う。`tools/memlog.py` は IDF の env を source しないと
  `esp_idf_size` が無くて失敗する。
- **`dependencies.lock` は configure のたびに書き換わる**。コミット前に `git checkout --` で戻す。
- **worktree の `.cache` は系統（`dependencies.lock`）ごとに違う**。別系統のキャッシュに繋ぐと
  古い/新しいコンポーネントが混ざる。`.cache` 自体が symlink のときに `ln -sfn <自分自身>` を
  書くと共有キャッシュの中に自己ループを作り、次の configure が
  `EXTRA_COMPONENT_DIRS doesn't exist` / `libopus sources are missing` で死ぬ。
  疑わしいときは 6 分ビルドの前に `idf.py -B /tmp/chk reconfigure` で切り分ける。
- **`objdump` は PIE 命令を `excw` と表示する**（VMULAS 等の名前は出ない）。PIE カーネルの
  命令数を数えるときは `excw` 行数を数えるか、モデル（`tools/pie/`）側で数える。
  実ビルドの `.obj` を見ること（手で組んだ `gcc -Os` は PIE オペコードを知らない）。
- **DIRAM は別勘定**。リング長や状態を増やす書き換えは flash が減っても DIRAM が増える。
  両方を `memlog.py` で出す。
- **会員名は途中で切れて表示される**ことがある（`...compiler_builtins` の後ろの cgu 番号）。
  `member_bytes.py` は会員名をそのまま集計するので diff は正しく出るが、報告に書くときは
  cgu 番号まで含めて書く（同名に見える行が並ぶと読む人が混乱する）。

## 6. 一般化（他の対象にも同じ手順が使える）

- 「我々のコードが唯一の消費者になっているライブラリ」なら何でも同じ: IDF のドライバ family、
  フォント・圧縮ライブラリ、Rust の cgu 会員、picolibc の libm 個別関数。
- 「多くのモジュールが参照している」ものは 0 B なので、**ランキングの③④を先に落として
  努力を捨てる**のがこの方法の半分の価値。
- サイクル（命令数）の削減は別軸。64bit 除算のように「サイズ 0 B だが命令は激減」する対象があるので、
  サイズの期待値が 0 でも実行頻度が高ければ別途着手する（判断は `docs/perf/backlog.md` 側）。
