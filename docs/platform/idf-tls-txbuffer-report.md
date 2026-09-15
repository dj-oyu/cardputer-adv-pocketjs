# ESP-IDF dynamic-buffer TLS: the record overhead is counted twice

**Status: draft. Nothing has been filed, posted, commented, or sent.** This
document is a recommendation plus a ready-to-submit issue body, for a human to
decide about.

---

## Recommendation

**Report it.** Three things have to hold for that to be the right call, and all
three do:

1. **It is still a bug upstream.** `tx_buffer_len` is byte-identical on
   `master` and on `release/v6.0`, and the call sites that feed it
   `MBEDTLS_SSL_OUT_BUFFER_LEN` are still there on both. No fix commit exists in
   the file's history. This is not a v6.0.1 artefact.
2. **It is not already reported.** The nearest prior art is issue #14444 /
   PR #14614, which are about the *RX* buffer being **under**-sized. Nothing
   found on the tracker or the forum is about TX over-allocation.
3. **The "deliberate slack" reading does not survive the source.** See
   *Arguing the other side*, below — the same file allocates a bare
   `MBEDTLS_SSL_OUT_BUFFER_LEN` in two other places, including the one that
   serves `mbedtls_ssl_write`, which is the largest legitimate use of the
   buffer. If the full buffer plus 333 bytes were needed, those paths would be
   broken; they are not.

Two findings not in the original brief carry more of the report than the
byte counts do, and the draft leads with both:

- **The same mistake is on the RX side**, at `esp_mbedtls_dynamic_impl.c:161`,
  with `MBEDTLS_SSL_IN_BUFFER_LEN`, in the file that defines the helper. That
  turns "a wrong constant at some handshake states" into "the port misreads its
  own helper in both directions", which is the version a maintainer will keep
  reading. (Our linker wrap does not cover it — it only touches TX.)
- **The contract is undocumented.** No comment above `tx_buffer_len()`'s
  definition, no doc comment on the prototype in `esp_mbedtls_dynamic_impl.h`
  (that header has none at all), nothing in the commit history. This is the
  reason the bug was possible, the reason we cannot be certain we are right, and
  a thing worth fixing on its own terms. The draft asks for the comment
  separately from the sizes, so there is something to agree with even if they
  reject the rest.

Two caveats a human should weigh before pressing send:

- **The impact framing should stay modest.** This is over-allocation, not
  corruption or a leak. It costs 333 bytes per outgoing-record allocation in our
  config, and it worsens fragmentation because the block is taken and released
  repeatedly, but nothing fails that would otherwise succeed except on a board
  already at the edge. Our board is at the edge; most are not. Report it as a
  correctness/efficiency defect in a memory-optimisation feature — which is
  where it is embarrassing — rather than as a crash.
- **Our reproduction is arithmetic, not a runnable test case.** The draft below
  is honest about that. The arithmetic is checkable by anyone against their own
  `sdkconfig` in about a minute, which is the strongest part of the evidence;
  the byte counts from our hardware are corroboration, not proof.

The draft issue body is at the end of this document, under *Draft issue*. It is
written to be pasted as-is.

---

## Part 1 — what the code does

### Read locally, in `C:\esp\v6.0.1\esp-idf`

Version: `git log -1` on that checkout gives `8c19b156 change(version): Update
version to 6.0.1`.

`components/mbedtls/port/dynamic/esp_mbedtls_dynamic_impl.c:67-80`:

```c
static int tx_buffer_len(mbedtls_ssl_context *ssl, int len)
{
    (void)ssl;

    if (!len) {
        return MBEDTLS_SSL_OUT_BUFFER_LEN;
    } else {
        return len + MBEDTLS_SSL_HEADER_LEN
                   + MBEDTLS_MAX_IV_LENGTH
                   + MBEDTLS_SSL_MAC_ADD
                   + MBEDTLS_SSL_PADDING_ADD
                   + MBEDTLS_SSL_MAX_CID_EXPANSION;
    }
}
```

So the parameter is a **content** length: zero means "give me the whole
buffer", and anything else is a payload size that the record overhead still has
to be added to.

The overhead it adds is exactly what mbedTLS already folded into
`MBEDTLS_SSL_OUT_BUFFER_LEN`. From the bundled mbedTLS,
`components/mbedtls/mbedtls/library/ssl_misc.h`:

```c
/* :331 */ #define MBEDTLS_SSL_PAYLOAD_OVERHEAD (MBEDTLS_MAX_IV_LENGTH +          \
                                                 MBEDTLS_SSL_MAC_ADD +            \
                                                 MBEDTLS_SSL_PADDING_ADD +        \
                                                 MBEDTLS_SSL_MAX_CID_EXPANSION    \
                                                 )
/* :404 */ #define MBEDTLS_SSL_HEADER_LEN 13
/* :416 */ #define MBEDTLS_SSL_OUT_BUFFER_LEN  \
               ((MBEDTLS_SSL_HEADER_LEN) + (MBEDTLS_SSL_OUT_PAYLOAD_LEN))
```

where `MBEDTLS_SSL_OUT_PAYLOAD_LEN` is `MBEDTLS_SSL_PAYLOAD_OVERHEAD +
MBEDTLS_SSL_OUT_CONTENT_LEN` (same file, symmetric with the `IN` form at :337).

That makes the defect config-independent and provable by substitution alone:

```
tx_buffer_len(ssl, MBEDTLS_SSL_OUT_BUFFER_LEN)
  = MBEDTLS_SSL_OUT_BUFFER_LEN + HEADER_LEN + PAYLOAD_OVERHEAD
  = tx_buffer_len(ssl, 0)      + HEADER_LEN + PAYLOAD_OVERHEAD
```

The excess is `MBEDTLS_SSL_HEADER_LEN + MBEDTLS_SSL_PAYLOAD_OVERHEAD`, whatever
the build options. No arithmetic about a specific `sdkconfig` is needed to see
it.

### The contract is written down nowhere

Before the call sites, the thing that makes all of this inference rather than
demonstration: **no comment in the port states what `tx_buffer_len`'s second
argument means.**

- `esp_mbedtls_dynamic_impl.c:66` is a blank line. The function at `:67` follows
  the closing brace of `esp_mbedtls_parse_record_header` directly, with no
  comment block, no parameter note, nothing.
- `esp_mbedtls_dynamic_impl.h:80` declares
  `int esp_mbedtls_add_tx_buffer(mbedtls_ssl_context *ssl, size_t buffer_len);`
  as a bare prototype. There is not one doc comment anywhere in that header
  (`:66`–`:100`), on this function or any other.
- Nothing in the file's commit history describes it either.

So every reading of that parameter — Espressif's, ours, a future maintainer's —
is inferred from usage. That is worth naming three times over: it is the reason
the bug was possible, the reason this report cannot be certain, and a defect
worth fixing on its own even if the maintainers disagree about everything else
here. A two-line comment saying "`len` is a content length; pass 0 for the whole
buffer" would have prevented nineteen call sites from getting it wrong.

### The call sites that pass the wrong unit

`esp_mbedtls_add_tx_buffer` applies `tx_buffer_len` to its argument at
`esp_mbedtls_dynamic_impl.c:296` and allocates `SSL_BUF_HEAD_OFFSET_SIZE +
buffer_len` at :298.

In v6.0.1 a `*_BUFFER_LEN` macro is passed where a content length is expected at
**nineteen** sites, in both directions:

- **`esp_mbedtls_dynamic_impl.c:161`, on the RX side** — inside
  `esp_mbedtls_dynamic_set_rx_buf_static()`:
  `int buffer_len = tx_buffer_len(ssl, MBEDTLS_SSL_IN_BUFFER_LEN);`. The same
  mistake with the IN macro, in the file that defines the helper. This one
  matters out of proportion to its count: it means the confusion is not a
  slipped constant at one handshake state but a misreading of the helper by the
  port itself. (1)
- `esp_ssl_cli.c` — via a local `size_t buffer_len = MBEDTLS_SSL_OUT_BUFFER_LEN;`
  at lines 49/51, 143/145, 150/152, 157/158, 173/175, 180/182, 239/241; via
  `MAX(...)` at 136/138; and passed inline at 224, 229, 235. (11)
- `esp_ssl_srv.c` — same shapes at 60/62, 81/83 (`MAX`), 98/100, 116/118,
  123/125, 185/187, 192/194. (7)

Our linker wrap covers only the TX sites; `:161` is untouched by it.

The correct callers, in the same tree, are worth listing because they establish
the contract rather than merely being consistent with it:

- `esp_ssl_tls.c:330` (`__wrap_mbedtls_ssl_write`), `:415`
  (`__wrap_mbedtls_ssl_send_alert_message`), `:430`
  (`__wrap_mbedtls_ssl_close_notify`) all pass **0**.
- `esp_mbedtls_dynamic_impl.c:408` passes `in_msglen`, a parsed record content
  length, and `:417` passes `MBEDTLS_SSL_IN_CONTENT_LEN` — both content lengths,
  both correct.

### Read upstream, on GitHub

Fetched from `raw.githubusercontent.com/espressif/esp-idf/master/...`:

- `tx_buffer_len` on `master` (lines 69–80) is **byte-identical** to the v6.0.1
  text quoted above.
- `release/v6.0` is identical too; the only difference in that whole file
  between the branches is an unrelated pointer-width fix to the
  `out_msg`/`in_msg` offset handling.
- `esp_ssl_cli.c` on `master` still has `size_t buffer_len =
  MBEDTLS_SSL_OUT_BUFFER_LEN;` at 49, 143, 150, 157, 173, 180, 239 and inline
  `esp_mbedtls_add_tx_buffer(ssl, MBEDTLS_SSL_OUT_BUFFER_LEN)` at 224, 229, 235.
- `esp_ssl_srv.c` on `master` has the same pattern; upstream line numbers drift
  from ours (roughly 67–130, 173, 204–211, 227).
- `esp_ssl_tls.c` on `master` still calls with `0` only.
- No commit in the file's history back to 2021 mentions `tx_buffer_len`,
  double-counting, or TX over-allocation. **There is no fix to point at.**

### Already reported? No

Searched `repo:espressif/esp-idf` issues and PRs for `tx_buffer_len`,
`esp_mbedtls_add_tx_buffer`, `MBEDTLS_SSL_OUT_BUFFER_LEN`, over-allocation and
dynamic-buffer terms, plus the esp32.com forum. The only hits on this code
family are:

- Issue [#14444](https://github.com/espressif/esp-idf/issues/14444), "Mbedtls
  Dynamic Port - Memory leak" (closed) — the **IN** buffer being too small when
  a client sends >16 KB to an HTTPS server. Different direction, different bug.
- PR [#14614](https://github.com/espressif/esp-idf/pull/14614), "Fix memory leak
  in mbedtls by adjusting RX buffer size" (closed) — the fix for the above.

Forum results were generic dynamic-buffer allocation-failure threads. **Nothing
reporting the TX double-count was found.**

---

## Part 2 — arguing the other side

The claim only holds if `tx_buffer_len`'s non-zero branch really takes a content
length. Here is the best case that it does not, and why it fails.

**The slack reading.** Perhaps the non-zero branch is not "content length +
overhead" but "a floor, plus headroom", and the handshake sites deliberately ask
for the whole buffer plus a record's worth of slack because mbedTLS can write a
handshake message and then a record header past the end of the payload region.
Under that reading the extra 333 bytes are a safety margin and removing them is
the bug.

**Why it fails.** Four independent pieces of the same tree contradict it:

1. **`__wrap_mbedtls_ssl_write` passes 0** (`esp_ssl_tls.c:330`). Application
   data is the *largest* legitimate consumer of the out buffer — a full
   `MBEDTLS_SSL_OUT_CONTENT_LEN` payload plus its own record framing. It runs on
   a bare `MBEDTLS_SSL_OUT_BUFFER_LEN`. Handshake records are smaller than
   application-data records. If the margin were needed, the busiest path would
   be the one that lacks it.
2. **`esp_mbedtls_reset_add_tx_buffer` allocates the bare macro.**
   `esp_mbedtls_dynamic_impl.c:223-226` calls
   `esp_mbedtls_alloc_tx_buf(ssl, MBEDTLS_SSL_OUT_BUFFER_LEN)`, and
   `esp_mbedtls_alloc_tx_buf` (:181-198) does **not** route through
   `tx_buffer_len` — it allocates `SSL_BUF_HEAD_OFFSET_SIZE + len` directly.
   Two functions in one file, both meaning "the whole out buffer", producing
   sizes that differ by 333 bytes. At most one of them can be intended.
   `esp_mbedtls_reset_add_rx_buffer` (:236-268) does the same on the RX side
   with the bare `MBEDTLS_SSL_IN_BUFFER_LEN`.
3. **The correct in-file callers pass content lengths.** `:408` passes
   `in_msglen` straight out of `esp_mbedtls_parse_record_header`; `:417` passes
   `MBEDTLS_SSL_IN_CONTENT_LEN`. Those are payload sizes, and they are the only
   non-zero arguments in the file that are not one of the `*_BUFFER_LEN` macros.
4. **Espressif's own design note gives the formula.** The port ships
   `components/mbedtls/port/dynamic/dynamic_buffer_architecture.md`, which
   describes the feature as one that "**right-sizes buffers** based on actual
   message requirements" (line 20) and writes the sizing rule out at line 155 as
   `buffer_len = in_msglen + overhead; // Add necessary TLS overhead`.

   That line is worth citing carefully, because it is easy to overstate. It sits
   in the document's *Reception (RX)* section and describes
   `esp_mbedtls_add_rx_buffer()`; it is not a statement about the TX path. Its
   weight here is indirect: `tx_buffer_len()` is a single shared helper that both
   directions call, and the RX call it documents —
   `esp_mbedtls_dynamic_impl.c:408`, `tx_buffer_len(ssl, in_msglen)` — is one of
   the two places in the file that pass a genuine content length. So the
   document tells us how *this function's* second argument is meant to be used,
   in the one path where Espressif wrote the usage down. It does not
   independently tell us anything about TX beyond that.

   Even discounted that way it cuts against the slack reading: deliberate
   headroom is the opposite of "right-sizes", and 333 bytes of silent margin
   inside a feature whose whole purpose is to save a few kilobytes would be a
   strange thing to want without a comment saying so.

**A related units confusion, which supports the reading rather than undermining
it.** `esp_ssl_cli.c:122-136` computes a genuine content length for the client
certificate chain, then does `buffer_len = MAX(buffer_len,
MBEDTLS_SSL_OUT_BUFFER_LEN)`. That `MAX` compares a content length against a
buffer length — the floor should be `MBEDTLS_SSL_OUT_CONTENT_LEN` if the units
were consistent. It errs toward over-allocating, so it is not dangerous, but it
is the same unit mix-up showing up in a place where the author clearly *did*
think in content lengths a few lines earlier. That is what a units bug looks
like from the inside.

**What I cannot rule out.** I have not found a comment, commit message, or
review thread stating the intent either way, and I have not run the port with
the overhead removed on anything but our own board. It is conceivable that some
mbedTLS path known to Espressif writes past `out_buf +
MBEDTLS_SSL_OUT_BUFFER_LEN` during a handshake state that `mbedtls_ssl_write`
never reaches. I consider it unlikely — the reset path at (2) would be equally
exposed — but a maintainer should be given room to say so, and the draft below
asks rather than asserts.

---

## Part 3 — what it costs, on our board

Everything in this section is **measured on our hardware**, not derived from
source. Board: M5Stack Cardputer ADV, ESP32-S3FN8, 341,760 bytes of DRAM, no
PSRAM. Config: `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`,
`CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y`,
`CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096`.

For that config the constants resolve to `MBEDTLS_SSL_MAC_ADD` 48 (SHA-384,
`ssl_misc.h:308`), `MBEDTLS_SSL_PADDING_ADD` 256 (:320),
`MBEDTLS_MAX_IV_LENGTH` 16 (`tf-psa-crypto/drivers/builtin/include/mbedtls/private/cipher.h:193`),
`MBEDTLS_SSL_MAX_CID_EXPANSION` 0 (:328), `MBEDTLS_SSL_HEADER_LEN` 13 (:404).
So `PAYLOAD_OVERHEAD` is 320 and the double-counted excess is 333.

```
MBEDTLS_SSL_OUT_BUFFER_LEN   4429   = 13 header + 320 overhead + 4096 content
tx_buffer_len adds again    +  333   = 13 + 16 + 48 + 256 + 0
SSL_BUF_HEAD_OFFSET_SIZE    +    8
                             ----
                             4770
```

With a custom mbedTLS allocator recording every request, the largest single
allocation in a handshake was exactly **4,770 bytes**. After passing 0 instead,
exactly **4,437** (= 4,429 + 8). The difference, **333 bytes**, matches the
arithmetic to the byte.

**On the number 674.** Our commit `fb41a02` and the original version of the
comment in `pocket_tls_txfix.c` both said 674, which charged the 8-byte
`SSL_BUF_HEAD_OFFSET_SIZE` against a baseline that does not pay it either way.
The waste is 333. The source comment has since been corrected and now names the
commit message as wrong; the commit itself is pushed history in a tree several
sessions share, so it stands with a correct pointer rather than being rewritten.
Nothing that goes upstream should say 674.

The block is taken and released about six times in one handshake, so the cost is
also six chances to leave a hole in a heap that has no PSRAM to fall back on.
For calibration on the same board: a handshake wants roughly 6,600–8,300 bytes
of free heap in total, so this single buffer is a large fraction of it.

Our local correction is `main/pocket/pocket_tls_txfix.c`: a linker `--wrap` on
`esp_mbedtls_add_tx_buffer` that rewrites the argument to 0 on exact equality
with `MBEDTLS_SSL_OUT_BUFFER_LEN` (wired up at `main/CMakeLists.txt:35`). We did
not edit the shared IDF checkout. That is a workaround, not a proposed patch —
the proper fix is upstream, at the call sites.

---

## Draft issue

> Everything below is proposed text. It has not been posted.

**Title:** `mbedtls dynamic buffer: TX record overhead is added twice (esp_ssl_cli.c / esp_ssl_srv.c pass MBEDTLS_SSL_OUT_BUFFER_LEN to tx_buffer_len)`

---

### Answers checklist

- **IDF version:** v6.0.1 (`8c19b156`). Also verified present on `master` and
  `release/v6.0` by reading the files on GitHub.
- **Espressif SoC revision:** ESP32-S3FN8 (no PSRAM), but the defect is in
  target-independent code.
- **Operating System / toolchain:** Windows, ESP-IDF v6.0.1 as installed by the
  IDF Installer.
- **Relevant sdkconfig:** `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`,
  `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y`,
  `CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096`. Any config with
  `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y` is affected; only the size of the waste
  changes.

### Expected behaviour

`esp_mbedtls_add_tx_buffer()` should allocate one outgoing record buffer of
`MBEDTLS_SSL_OUT_BUFFER_LEN` bytes (plus the 8-byte `esp_mbedtls_ssl_buf`
header) when the whole buffer is wanted.

### Actual behaviour

It allocates `MBEDTLS_SSL_OUT_BUFFER_LEN + MBEDTLS_SSL_HEADER_LEN +
MBEDTLS_SSL_PAYLOAD_OVERHEAD` bytes at every handshake call site, because the
record overhead is added a second time. The same thing happens once on the RX
side with `MBEDTLS_SSL_IN_BUFFER_LEN`.

### Details

`components/mbedtls/port/dynamic/esp_mbedtls_dynamic_impl.c` (master, lines
69–80):

```c
static int tx_buffer_len(mbedtls_ssl_context *ssl, int len)
{
    (void)ssl;

    if (!len) {
        return MBEDTLS_SSL_OUT_BUFFER_LEN;
    } else {
        return len + MBEDTLS_SSL_HEADER_LEN
                   + MBEDTLS_MAX_IV_LENGTH
                   + MBEDTLS_SSL_MAC_ADD
                   + MBEDTLS_SSL_PADDING_ADD
                   + MBEDTLS_SSL_MAX_CID_EXPANSION;
    }
}
```

The non-zero branch takes a **content** length and adds the record overhead —
which is how the two correct callers use it: `esp_mbedtls_add_rx_buffer()`
passes the parsed `in_msglen`, and, under `MBEDTLS_SSL_PROTO_TLS1_3`,
`MBEDTLS_SSL_IN_CONTENT_LEN`.

The port's own `dynamic_buffer_architecture.md` writes that rule out at line 155
as `buffer_len = in_msglen + overhead; // Add necessary TLS overhead`. To be
precise about what that line is: it sits in the document's *Reception (RX)*
section and describes `esp_mbedtls_add_rx_buffer()`, so it is not a statement
about the TX path. It is relevant because `tx_buffer_len()` is a single helper
shared by both directions, and the RX call it documents is one of the only two
sites in the tree that pass a genuine content length — so it records how this
function's second argument is meant to be used, in the one path where the usage
was written down.

I should also say plainly that **the contract is undocumented at the two places
that would normally carry it**: there is no comment above `tx_buffer_len()`'s
definition, and `esp_mbedtls_dynamic_impl.h` declares
`esp_mbedtls_add_tx_buffer(mbedtls_ssl_context *ssl, size_t buffer_len)` as a
bare prototype with no doc comment (that header has none at all). Every reading
of the parameter, including mine, is therefore inferred from usage. That gap
looks like the proximate cause of the divergence between the call sites, and
documenting it is probably worth doing regardless of what you conclude about the
sizes below.

But `MBEDTLS_SSL_OUT_BUFFER_LEN` is a **buffer** length that already contains
that overhead (`library/ssl_misc.h`): it is `MBEDTLS_SSL_HEADER_LEN +
MBEDTLS_SSL_OUT_PAYLOAD_LEN`, where `MBEDTLS_SSL_OUT_PAYLOAD_LEN` is
`MBEDTLS_SSL_PAYLOAD_OVERHEAD + MBEDTLS_SSL_OUT_CONTENT_LEN`, and
`MBEDTLS_SSL_PAYLOAD_OVERHEAD` is precisely `MBEDTLS_MAX_IV_LENGTH +
MBEDTLS_SSL_MAC_ADD + MBEDTLS_SSL_PADDING_ADD + MBEDTLS_SSL_MAX_CID_EXPANSION`.

So for every call site that passes the macro:

```
tx_buffer_len(ssl, MBEDTLS_SSL_OUT_BUFFER_LEN)
  == tx_buffer_len(ssl, 0) + MBEDTLS_SSL_HEADER_LEN + MBEDTLS_SSL_PAYLOAD_OVERHEAD
```

Affected call sites on `master`. I have put the RX one first because it is the
clearest sign that this is a systematic misreading of the helper rather than a
wrong constant at one handshake state — it is in the same file as the
definition, and it is on the other side of the connection:

- `components/mbedtls/port/dynamic/esp_mbedtls_dynamic_impl.c`, **RX side**, in
  `esp_mbedtls_dynamic_set_rx_buf_static()`:
  `int buffer_len = tx_buffer_len(ssl, MBEDTLS_SSL_IN_BUFFER_LEN);`.
  `MBEDTLS_SSL_IN_BUFFER_LEN` is likewise already `MBEDTLS_SSL_HEADER_LEN +
  MBEDTLS_SSL_PAYLOAD_OVERHEAD + MBEDTLS_SSL_IN_CONTENT_LEN`, so the same
  overhead is added twice here too. Note that a few lines away, in the same
  file, `esp_mbedtls_reset_add_rx_buffer()` allocates the bare
  `MBEDTLS_SSL_IN_BUFFER_LEN` without going through `tx_buffer_len` at all.
- `components/mbedtls/port/dynamic/esp_ssl_cli.c` — `size_t buffer_len =
  MBEDTLS_SSL_OUT_BUFFER_LEN;` at lines 49, 143, 150, 157, 173, 180, 239;
  `MAX(buffer_len, MBEDTLS_SSL_OUT_BUFFER_LEN)` at 136; and inline at 224, 229,
  235.
- `components/mbedtls/port/dynamic/esp_ssl_srv.c` — the same pattern (around
  lines 67–130, 173, 204–211, 227).

`esp_ssl_tls.c` is unaffected: `__wrap_mbedtls_ssl_write`,
`__wrap_mbedtls_ssl_send_alert_message` and `__wrap_mbedtls_ssl_close_notify`
all pass `0`.

That last point is also the reason I do not think the extra bytes are intended
slack: `mbedtls_ssl_write` is the path that fills the out buffer most fully, and
it runs on a bare `MBEDTLS_SSL_OUT_BUFFER_LEN`. `esp_mbedtls_reset_add_tx_buffer()`
in the same file likewise allocates the bare macro through
`esp_mbedtls_alloc_tx_buf()`, which does not call `tx_buffer_len` at all — so
the file contains two "give me the whole out buffer" paths whose sizes differ by
the overhead. If I have misread the intent here I would be glad to be corrected.

### How to reproduce

The arithmetic is checkable against any `sdkconfig` without hardware. With
`CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`, `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y` and
`CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096` (SHA-384 HMAC and CBC padding
available, no DTLS CID):

```
MBEDTLS_SSL_HEADER_LEN            13
MBEDTLS_SSL_PAYLOAD_OVERHEAD     320   = 16 IV + 48 MAC + 256 padding + 0 CID
MBEDTLS_SSL_OUT_BUFFER_LEN      4429   = 13 + 320 + 4096
tx_buffer_len(ssl, 4429)        4762   = 4429 + 13 + 320
+ SSL_BUF_HEAD_OFFSET_SIZE         8
                                ----
malloc size at handshake        4770   (should be 4437)
```

The function already has the instrumentation to show this: building with
`CONFIG_LOG_MAXIMUM_LEVEL=VERBOSE` and raising the `Dynamic Impl` tag to
verbose makes `esp_mbedtls_add_tx_buffer()` print `add out buffer %zu bytes`,
which reports 4762 where 4429 is expected — on any TLS connection, e.g. a plain
`esp_http_client` GET over HTTPS.

**Measured on our hardware** (ESP32-S3, no PSRAM, custom mbedTLS allocator
recording every request): the largest single allocation during a handshake was
exactly **4,770** bytes; after changing the argument to `0` it was exactly
**4,437**. 333 bytes per allocation, and the buffer is allocated and freed about
six times per handshake, so on a no-PSRAM part it is also a repeated contiguity
cost. Everything else in this report is read from the sources, not measured.

### Suggested fix

Pass `0` at the sites that mean "the whole out buffer", so they take the `!len`
branch — i.e. replace `size_t buffer_len = MBEDTLS_SSL_OUT_BUFFER_LEN;` with `0`
at the `esp_ssl_cli.c` / `esp_ssl_srv.c` call sites listed above, and use
`MBEDTLS_SSL_IN_BUFFER_LEN` directly (not via `tx_buffer_len`) in
`esp_mbedtls_dynamic_set_rx_buf_static()`. The one site that computes a real
content length (`esp_ssl_cli.c`, client certificate chain) should keep passing
its own number; its `MAX(..., MBEDTLS_SSL_OUT_BUFFER_LEN)` floor arguably wants
to be `MBEDTLS_SSL_OUT_CONTENT_LEN` for the same units reason, though as written
it only over-allocates.

Alternatively, make `tx_buffer_len()` clamp or reject arguments that are already
`>= MBEDTLS_SSL_OUT_BUFFER_LEN`, which would fix all sites at once.

Either way, please consider adding the two-line comment that would have made
this unambiguous — on `tx_buffer_len()`'s definition and on the
`esp_mbedtls_add_tx_buffer()` prototype in `esp_mbedtls_dynamic_impl.h` — saying
that the length parameter is a *content* length and that `0` requests the full
buffer. That is useful even if you conclude the current sizes are intended,
since it is the one change that settles the question for the next reader.
