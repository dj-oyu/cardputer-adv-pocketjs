# chime.wav

The one piece of audio in the tree, and it exists to make streamed playback
demonstrable rather than theoretical.

`app:/` caps a file at 24,576 bytes (`FS_MAX_FILE`), which is exactly the size
of the buffer streaming removed — so nothing stored there can show that the cap
is gone. `assets:/` is flash-mapped firmware content with no such cap. This file
is **150,554 bytes, 3.135 s**: over six times what the old player would accept,
so `audio.player.open('assets:/chime.wav')` failing with `LIMIT_EXCEEDED` and
succeeding are two different builds and not two different files.

**Three passes of the source, not one, and that is also deliberate.** The
material is 1.05 s, which converts to 50,170 bytes — already over twice the old
cap, so one pass would have proved the point in bytes. It would not have proved
it in *time*: one pass is about two ring-fills, which tests a burst rather than a
sustained feed, and it stays under the old **ADPCM** ceiling of 2.05 s, so anyone
reaching for that limit instead of the byte limit would be unconvinced. Three
passes is 3.135 s and roughly 35 refills — past both old ceilings, and long
enough that a producer which fell behind would have time to show it. The cost is
100 KB of flash over the one-pass version; `--repeat 1` hands it back.

PCM16 rather than IMA ADPCM on purpose. ADPCM would compress this to about
37 KB — still over the cap, but it would also feed the ring at a quarter of the
rate. PCM16's 48,000 bytes a second is the worst case for the producer, which is
what should be under test. Clicks rather than music for the same reason: a gap
from an underrun is obvious in a click train and easy to miss in a chord.

Regenerate (converts to 24 kHz mono, low-passes at 11 kHz first so the
decimation does not alias):

```
python tools/make_wav_asset.py \
    C:/devs/m5stack/esp32p4-mqjs/assets/audio/mixkit-clear-mouse-clicks-2997.wav \
    apps/chime/chime.wav --repeat 3
```

## クリック列が捕まえられないもの（2026-09-09、追記）

上の「a gap from an underrun is obvious in a click train and easy to miss in a
chord」は、**事象については正しく、事象と事象の間については間違っている。**
クリック列はほとんどが無音で、**無音に無音を挿しても何も変わらない**——`stretch` の
アンダーランがクリックとクリックの間に落ちた場合、それは聞き取りにくいのではなく
**原理的に聞こえない**。知覚され得るのはクリックの上に落ちた分だけで、8秒に411パケット、
短い過渡音が並ぶ素材ではそれは時間軸のごく一部である。

つまりこの素材は**タイミングのずれには向き、アンダーランの検出率は低い**。
その穴を埋めるのが `apps/opusplay/tone.pok`（541.7 Hzの持続音）で、持続音には無音の
瞬間が無いので**どこに落ちたアンダーランも信号の上に落ちる**。`apps/opusplay` は
サイクルごとに両方を鳴らす。詳しい理由と、周波数が任意ではないこと（アンダーランは
ちょうど128フレーム＝1/187.5秒なので、187.5 Hzの整数倍の音は同位相で再開してポップが
鳴らない）は `apps/opusplay/README.md` と `docs/common-api.md` 9.1.2 にある。

**この素材を置き換えないこと。** 2つは別の失敗を捕まえる。
