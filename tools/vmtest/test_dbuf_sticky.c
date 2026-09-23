// Does a DynBuf keep taking writes after it has failed to grow?
//
//   gcc -O1 -g -Wall -Werror -fsanitize=address,undefined \
//       -I components/quickjs-ng/quickjs-ng \
//       tools/vmtest/test_dbuf_sticky.c -o /tmp/t && /tmp/t
//
// (-Wextra is left out on purpose: cutils.h itself has unused parameters.)
//
// cutils.h's fast paths (dbuf_putc, dbuf_put_u16/u32/u64, dbuf_put) test only
// whether the write fits the slack; the error flag was consulted in
// dbuf_claim alone. So after a failed growth a smaller write could still land,
// and a byte stream decoded by instruction length came out shifted: the
// operand that failed was missing and the next opcode sat in its place. The
// parser's bytecode is such a stream. This fails (exit 1) on the unpatched
// header and passes once a failure collapses allocated_size to size.
#include <stdio.h>
#include <stdlib.h>
#include "cutils.h"

static int budget;   // reallocs allowed before failing

static void *rf(void *opaque, void *ptr, size_t size)
{
    (void)opaque;
    if (size == 0) { free(ptr); return NULL; }
    if (budget-- <= 0) return NULL;
    return realloc(ptr, size);
}

int main(void)
{
    static const uint8_t ten[10];
    DynBuf b;

    budget = 2;                            // two growths succeed, the third fails
    dbuf_init2(&b, NULL, rf);
    dbuf_put(&b, ten, 10);                 // first growth is exact: allocated 10
    dbuf_putc(&b, 0xAA);                   // second growth is x1.5: allocated 15
    while (b.allocated_size - b.size > 3)  // leave exactly 3 bytes of slack
        dbuf_putc(&b, 0xBB);
    printf("allocated=%zu size=%zu slack=%zu\n", b.allocated_size, b.size,
           b.allocated_size - b.size);

    size_t before = b.size;
    int r32 = dbuf_put_u32(&b, 0x11223344); // needs 4, slack 3: grows, fails
    int r8 = dbuf_putc(&b, 0xCC);           // fits the 3-byte slack
    printf("put_u32=%d error=%d | putc after error=%d | size %zu -> %zu\n",
           r32, (int)b.error, r8, before, b.size);

    int bad = b.error && b.size != before;
    printf("%s\n", bad ? "NOT STICKY: a write landed after the failure"
                       : "sticky: nothing written after the failure");
    dbuf_free(&b);
    return bad;
}
