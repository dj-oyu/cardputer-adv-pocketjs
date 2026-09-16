# compiler_builtins の cgu.04（`__gedf2`）は我々の側からは落とせない

## 結論

`main/pocket/pocket_av.c` の `gain>=0.0` が `__gedf2` を出しており、それが Rust
compiler_builtins の cgu.04（配置 2248 B）を引き込んでいる、という見立ては**半分だけ正しい**。

- 正しい点: 基準マップの "Archive member included to satisfy..." は cgu.04 について
  `esp-idf/main/libmain.a(pocket_av.c.obj) (__gedf2)` の1行しか出さない。
- 誤っている点: その1行は「そのとき最初に undefined だった参照」を記録しただけである。
  `__gedf2` の参照元は我々の **5 ファイル**（`pocket_av.c` / `pocket_app.c` / `pet_hub.c` /
  `file_picker.c` / `sd_picker.c`）と、QuickJS 本体・Rust 内部・picolibc にもある。

我々側の5箇所すべてを等価な非 `__gedf2` 式に置き換えたスタブで再リンクしても、cgu.04 は
**2248 B のまま**残り、抽出理由が `pocket_av.c (__gedf2)` → `quickjs.c.obj (__gedf2)` に
移っただけだった。会員は1つも落ちず、配置合計も 1,820,667 → 1,820,665 B（−2 B、後述の
リンカ緩和ゆらぎ）で実質変わらない。

同じ理由で **sinf 側（cgu.01 の `libm_math::sinf::sinf` 2490 B とその呼び先）も解放されない**。

## 我々側の `__gedf2` 5箇所（ビルド済みオブジェクトの relocation から特定）

| ファイル | 行 | 旧式 | スタブで使った式 |
| --- | --- | --- | --- |
| `main/pocket/pocket_av.c` | 226 | `if(!(gain>=0.0 && gain<=1.0))` | `if(gain<0.0 \|\| gain>1.0)` |
| `main/pocket/pocket_app.c` | 345 | `fps_value>=0?...:JS_NULL` | `fps_value<0?JS_NULL:...` |
| `main/pet/pet_hub.c` | 167-168 | `(seconds==0\|\|seconds>=1)&&seconds<=604800` | `(seconds==0\|\|!(seconds<1))&&seconds<=604800` |
| `main/pocket/file_picker.c` | 275-276 | `ms>=1&&ms<=PICK_TIMEOUT_MS` | `!(ms<1)&&ms<=PICK_TIMEOUT_MS` |
| `main/pocket/sd_picker.c` | 227-228 | `ms>=1&&ms<=SD_PICK_TIMEOUT_MS` | `!(ms<1)&&ms<=SD_PICK_TIMEOUT_MS` |

`xtensa-esp-elf-nm -u <obj> | grep __gedf2` で5ファイル全部に出る。`>=` を `!(<)` に移す案は
NaN に対してだけ差が出る（`a>=k` は false、`!(a<k)` は true）が、`pet_hub.c` は直前の
`isfinite()` が、`file_picker.c`/`sd_picker.c` は直前の `ms==(double)(int64_t)ms` が NaN を
先に落とすので、どの入力でも結果は同じになる。つまり**書き換え自体は安全にできる**。
問題は落ちるかどうかで、落ちない。

## 計測（方法は下の「判定手順」）

| ビルド | cgu.04 | compiler_builtins 合計 | 配置合計 / 会員数 | cgu.04 の抽出理由 |
| --- | --- | --- | --- | --- |
| 基準 `bd0fa43` | 2248 B | 46,329 B / 16 会員 | 1,820,667 B / 837 | `pocket_av.c.obj (__gedf2)` |
| 較正（無変更で再リンク） | 2248 B | 46,329 B / 16 会員 | 1,820,667 B / 837 | `pocket_av.c.obj (__gedf2)` |
| スタブ `pocket_av.c` のみ | 2248 B | 46,329 B / 16 会員 | 1,820,667 B / 837 | `pocket_app.c.obj (__gedf2)` |
| スタブ 上記5ファイル全部 | 2248 B | 46,329 B / 16 会員 | 1,820,665 B / 837 | `quickjs.c.obj (__gedf2)` |

−2 B の中身（会員単位の差）: `pocket_av.c.obj` −8 / `pet_hub.c.obj` +7 /
`pocket_app.c.obj` +3 / `pocket_random.c.obj` −4。触っていない `pocket_random.c.obj` が
動くのは Xtensa のリンカ緩和（`call8`→`call` など、対象関数が動くと近傍の選択が変わる）
によるもので、会員の脱落ではない。`sinf` / `cosf` / `tanf` はいずれも配置されたまま。

## 誰が本当に `__gedf2` を必要としているか

- **QuickJS 本体**: `quickjs.c.obj` の生きた13関数（`js_relational_slow`（配置 0x2db。
  JS の比較演算子そのもの）、`JS_ToArrayLengthFree`、`js_Date_UTC`、`js_date_constructor`、
  `js_date_setYear`、`time_clip`、`set_date_fields`、`js_number_toFixed`、`js_function_bind`、
  `js_typed_array_indexOf`、`js_atomics_wait`、`js_string_fromCodePoint`、
  `JS_ToInt64SatFree`）。
- **Rust 内部**: `compiler_builtins-cgu.13`（`rem_pio2_large` の literal プールが 0x24 配置済み）/
  cgu.14 / cgu.15 / cgu.09、`core-cgu.09`、`pocketjs_core-cgu.02`、
  `render_rgb565` 側の `pocketjs_core-cgu.02`。
- **picolibc**: `libc.a(libm_math_s_remainder.c.o)`。

`-lgcc` はリンク行の最後にあり libgcc にも `__gedf2` はあるが、アーカイブ走査順で
`libpocketjs_idf_ui_core.a`（cgu.04 が居る）が先に来るため選ばれない。cgu.04 を本当に落とすには
QuickJS 側の `__gedf2` 参照を消す（＝ QuickJS の改変）か compiler_builtins を切るしかない。
加えて、我々の C は `sinf` を直接呼ぶ（`stars.c` / `glass_rain.c` / `flower.c` /
`flower_species.c` / `sound.c` / `solar_sail.c` / `mp3_decode.c`）ので、cgu.04 は
`__gedf2` と `sinf` の二重に留められている。

## sinf は解放されない

`libm_math::sinf::sinf`（cgu.01、配置 0x420a46fc / 0x90a B、会員計上 2490 B）の参照元は
`compiler_builtins-cgu.04` の 15 B ラッパ `sinf` と `cgu.09` の2つだけで、前者は
`.literal.sinf` / `.text.sinf`（配置 0x420a6c20）、後者の `partial_availability::sinf` は
**破棄**されている（マップの Discarded input sections に 0xf で出る）。

つまり cgu.04 を落とせない以上、我々の C が `sinf` を呼ぶ限り 15 B ラッパは残り、その先の
2490 B も落ちない。今回のスタブ（5箇所）でも `sinf`/`cosf`/`tanf` は全て配置されたままで、
1 バイトも解放されなかった。**sinf 側の解放はこの作業では起こらない**。

逆に言えば、この 2490 B を狙う唯一の入口は我々の C 側で、`sinf` を呼ぶ7ファイル
（`stars.c` / `glass_rain.c` / `flower.c` / `flower_species.c` / `sound.c` / `solar_sail.c` /
`mp3_decode.c`）を止めるか、`main/` に強い `sinf` を置いて picolibc 側へ逃がすかのどちらか。
どちらもシーンの見た目が変わりうる（§7 の画素比較とユーザー判断が要る）ので、ここでは触らない。

## 判定手順（6分ビルドを回さずに会員の増減を確かめる）

基準ビルドのリンクを ninja から取り出して、差し替えたいオブジェクトだけ作り直して再リンクする。
基準ビルドと同じ入力・同じリンカなので、マップの差は差し替えた分だけになる。

```bash
cd /workspace/pjs-vm/build_base
ninja -t commands esp-idf/main/CMakeFiles/__idf_main.dir/pocket/pocket_av.c.obj | tail -1  # コンパイル行
ninja -t commands cardputer_pocketjs.elf | tail -1                                        # リンク行
# コンパイル行: -c のソースを差し替え、-o を /tmp に、-MD/-MT/-MF は削る
# リンク行:  esp-idf/main/libmain.a を差し替え用のコピーに、--Map= と -o を /tmp に
python3 /workspace/knscratch/size/tools/member_bytes.py /tmp/stub/stub.map compiler_builtins
python3 /workspace/knscratch/size/tools/member_bytes.py /tmp/stub/stub.map --total
```

**較正**: 無変更のソースでこの手順を回すと、できたオブジェクトは基準ビルドのものと
**バイト一致**し、配置合計 1,820,667 B / 837 会員、`compiler_builtins` 46,329 B / 16 会員が
基準マップと完全に一致する。ここが一致しないうちは結論を出さないこと。

## 次に効きそうなこと

- 同じ「`included by` の1行＝我々だけが理由」という見立ては **cgu.13（10,244 B、入口は
  `pocket_bridge.c.obj (__fixdfsi)`）** と **cgu.08（1188 B、入口は `shell.c.obj (__divdf3)`）**
  にもある。今回の教訓は「1行は最初の参照を記録しただけ」なので、着手前に上の判定手順で
  参照元を全部数えること。`__fixdfsi` は `quickjs.h` の `__JS_NewFloat64` 付近からも出ている
  ように見えるので、こちらも我々だけが理由ではない可能性が高い。
- `sinf` を消したいなら、呼び出し側（シーンの trig）を止めるか `main/` に強い `sinf` を
  置いて picolibc 側へ逃がす。ただし逃がした先（picolibc の `sinf` とその呼び先）が同じくらい
  のバイト数になる可能性があるので、まず上の判定手順で差を見ること。cgu.04 の側は
  `__gedf2` で残るので、この作業とは独立に効く。
