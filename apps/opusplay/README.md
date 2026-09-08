# opusplay — Opusの復号が描画フレームに何を払わせるか

`apps/streamplay` の測り方をそのまま借りて、ソースをOpusに変えたもの。借りたのは意図的で、
**比較できることがこのアプリの値打ちだから**である。streamplayが `chime.wav`
（PCM16、48,000バイト/秒）で出す `deltaTenthMs` と、これが同じ音のOpus版で出す
`deltaTenthMs` は、同じ器械の同じ目盛りの上に乗る。

素材は2つあり、**サイクルごとに交互**に鳴る（`assets:/chime.pok` と `assets:/tone.pok`）。
どちらだったかは `OPUS DONE` の `src=` が言う。理由は下の「素材が2つある理由」。

1サイクルは「プレイヤーを開いたまま60フレームのidle」→「1回の再生」で、終わったら
open し直す。**deltaTenthMs（再生中の平均フレーム時間 − idleの平均）が答えである。**

- streamplayでは供給がui/JSタスクの上にあり、実測は −0.2〜−0.4 ms、つまり**測定器の
  分解能より小さい**（`docs/common-api.md` 9.1.1）。
- こちらは復号が優先度6の専用タスクにあり、uiタスク（優先度5）を preempt できる。
  **これは測った（R5、2026-09-09）**: `deltaTenthMs` 8〜13＝**0.8〜1.3ms**、
  `worstFrameMs` 41〜45。復号そのものはレンダラーの横で `mean_us` 3,724〜4,005 で、
  ハーネス単独の3,208に対して**+16〜25%**——差は命令キャッシュの取り合いである。
  つまり「1コアの16%」は実機では18.6〜20.0%で、描画が払うのは1ms強。

サイクルを何回か回すこと。**1回目より、回を追って delta が増えないことのほうが重要**で、
増えるならプレイヤーの寿命を跨いで復号器かリングが漏れている（実測: 14サイクルで
`underruns=0`、delta 8〜13で一定）。

ファームウェア側は再生が終わるたびに1行出す：

```
OPUSDEC packets=N frames=N faults=0 mean_us=... worst_us=... prime_us=... stack_used=... of 14336
```

`mean_us` と `worst_us` は**このビルドが自分で測った復号コスト**で、ble-wtハーネスの
3,208µsを継承したものではない。`stack_used` は `opus_feed.c` の `DEC_STACK` を決めている
数字で、12,000を超えるようならその定数を動かす合図。`faults` が0でなければ、
CELT以外のパケットか復号失敗があったということで、`onState` は `error` を出す。

`prime_us` は復号タスクが生まれてから最初のスロットを published するまでで、実測7.7〜9.6ms。
**何も聞こえ得ない唯一の窓**であり、以前ここで音声タスクが空のリングを引いていた
（`docs/common-api.md` 9.1.2「起動時のアンダーラン」）。復号2回分でほぼ説明が付くので、
キャッシュが温まっても縮まない。

## 素材が2つある理由（片方では足りない）

音そのものの正しさはソフトウェアでは確認できない（`board_capture` はフレームバッファしか
見えない）ので、耳が唯一の検査になる。**そして何を鳴らすかが、アンダーランの何割が
聞こえるかを決める。**

サイクルごとに交互に鳴らす:

| 素材 | 得意なもの | 苦手なもの |
| --- | --- | --- |
| `assets:/chime.pok`（クリック列、8.2秒） | タイミングのずれ、列の乱れ | **クリックとクリックの間に落ちたアンダーラン。無音に無音を挿しても何も変わらないので、原理的に聞こえない** |
| `assets:/tone.pok`（541.7 Hz、15秒） | **時間軸のどこに落ちたアンダーランでも**——持続音に無音の瞬間は無い。`stretch` はソースを進めずに無音を挿すので、再開時に位相が飛び、ポップになる | 事象の列ではないので、タイミングの乱れそのものは分かりにくい |

**クリック列だけで検査していた間、「列の切れ目を聴け」は全部を検査したような顔で
数%を検査していた。** 置き換えではなく併用にしてあるのはそのためで、両方が別の失敗を
捕まえる。

**周波数は任意ではない。** アンダーランはちょうど128フレーム＝5.333ms＝1/187.5秒なので、
**187.5 Hzの整数倍の音は同位相で再開し、ポップが鳴らない**——1000 Hzは3回連続で、
375 Hzと562.5 Hzは何回でも無音になる。`tools/make_tone_asset.py` が探索して選んだ
541.7 Hzは、1〜8回のアンダーランに対する最悪の位相差が0.111周期。倍数に近い指定は
このツールが拒否する。定常音にしてあるのは、唸りや掃引が振幅の谷＝新しい死角を作るからで、
「どこで起きたか」は耳ではなく `positionMs` と `underruns` が言う。

**符号化の設定はクリック列とまったく同じ**（`RESTRICTED_LOWDELAY`、24 kbps VBR）。
持続音のためにビットレートを下げれば小さくなるが、そうすると聞こえた粗さが
「アンダーラン」なのか「その素材だけビットレートを下げたせい」なのか切り分けられなくなる。
**アーティファクトを探すための素材で、アーティファクトの原因を増やさない。**

## 資産

`apps/chime/chime.pok` は `chime.wav`（3.136秒）を8.2秒ぶん繰り返して符号化したもの。
**8.2秒にしたのは、`docs/opus-feasibility.md` §3 の「24,576バイトが8.06秒を買う」という
予測をその長さで検算するため**である。

実測（`make_opus_asset.sh` の出力）: 411パケット・8.20秒・**26,274バイト＝3,204 B/s**、
最大パケット121バイト、索引83エントリ（332バイト）、preSkip 60サンプル。
**24,576バイトが買う長さは7.67秒**で、予測の8.06秒より5%短い。

差はコーデックではなくコンテナである。26,274のうちヘッダ36＋索引332＋長さ接頭辞
411×2＝822、合わせて1,190バイト（4.5%）が容器の代金で、**Opusの中身だけなら25,084
バイト＝3,059 B/s**——予測の3,050 B/sと0.3%で一致する。長さ接頭辞と索引が何を買っている
かは `main/pocket/opus_feed.h` に書いてある（スロット境界をパケット境界に落とすことと、
seekがファイル全走査にならないこと）。

`tone.pok` の作り直し（15秒・48,009バイト＝3,201 B/s、クリック列と同じ密度）:

```bash
wsl -e bash -lc "cd /mnt/c/devs/m5stack/cardputer-adv-pocketjs && python3 tools/make_tone_asset.py /tmp/tone.wav --seconds 15 && bash tools/make_opus_asset.sh /tmp/tone.wav apps/opusplay/tone.pok"
```

`chime.pok` の作り直す手順:

```bash
wsl -e bash -lc "cd /mnt/c/devs/m5stack/cardputer-adv-pocketjs && python3 - <<'EOF'
import struct
d=open('apps/chime/chime.wav','rb').read(); at=12
while at+8<=len(d):
    cid=d[at:at+4]; n=struct.unpack('<I',d[at+4:at+8])[0]
    if cid==b'data': body=d[at+8:at+8+n]; break
    at+=8+n+(n&1)
need=int(8.2*24000)*2
out=(body*((need//len(body))+1))[:need]
open('/tmp/chime8.wav','wb').write(
    b'RIFF'+struct.pack('<I',36+len(out))+b'WAVEfmt '+
    struct.pack('<IHHIIHH',16,1,1,24000,48000,2,16)+b'data'+struct.pack('<I',len(out))+out)
EOF
bash tools/make_opus_asset.sh /tmp/chime8.wav apps/chime/chime.pok"
```

コンテナの検査（実機不要）:

```bash
wsl -e bash -lc "cd /mnt/c/devs/m5stack/cardputer-adv-pocketjs && \
  gcc -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
      -I main/hal -I main/pocket tools/test_opus_pak.c -o /tmp/t && /tmp/t apps/chime/chime.pok"
```

## 資産を足すとき踏む罠（`--gc-sections` は黙って捨てる）

`main/CMakeLists.txt` の `EMBED_FILES` に足すだけでは**イメージに入らない**。
`main/pocket/pocket_fs.c` の `ASSETS[]` に行が無いとシンボルが誰からも参照されず、
`--gc-sections` がまるごと捨てる——**エラーも警告も出ない**。

見分け方は簡単で、**ビルド後のFlash増分をファイルサイズと比べること**。
`chime.pok`（26,274バイト）を足したとき、イメージは5,632バイトしか増えなかった。
これがそのまま「捨てられている」という意味である。CLAUDE.mdが警告している
「`--gc-sections` で削除済みのモジュールを測っていた」の、資産版。

必要な3行（`pocket_fs.c`、`chime.pok` の行の隣）:

```c
extern const char asset_tonepok_start[] asm("_binary_tone_pok_start");
extern const char asset_tonepok_end[]   asm("_binary_tone_pok_end");
    // ASSETS[] に:
    {"tone.pok", asset_tonepok_start, asset_tonepok_end, false},
```

`text` は `false`。`EMBED_FILES` はNULを付けないので、`true` にすると
`asset_size()` が最後の1バイトを落とす（`docs/common-api.md` 9.1.1 と同じ罠）。
