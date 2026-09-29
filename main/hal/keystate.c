#include "keystate.h"
#include <stdatomic.h>

// A sequence lock. The writer makes the sequence odd, changes the words, and
// makes it even again; a reader copies the words between two reads of the
// sequence and keeps the copy only if both reads were the same even number.
// Readers never block the writer and never write anything themselves, so any
// number of them can read on either core.
//
// Every word is an atomic accessed relaxed, and the ordering comes from the
// fences: that is the C11-correct form of a seqlock (plain fields would be a
// data race even though the retry discards what it read), and on the S3 a
// relaxed 32-bit atomic is an ordinary load or store, so it costs nothing over
// the racy form. 32-bit words and not a uint64 bitmap: a 64-bit atomic on
// Xtensa goes through libatomic's lock.
//
// The writer's window is also a critical section on the device. Without it,
// the input task could be preempted with the sequence odd by a higher-priority
// reader on the same core, and that reader would spin on a writer that cannot
// run until it stops spinning. Masking interrupts for the dozen stores of one
// edge closes that; a reader on the other core waits at most that long.
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
static portMUX_TYPE writer_lock = portMUX_INITIALIZER_UNLOCKED;
#define WRITER_ENTER() portENTER_CRITICAL(&writer_lock)
#define WRITER_EXIT()  portEXIT_CRITICAL(&writer_lock)
#else
#define WRITER_ENTER() ((void)0)
#define WRITER_EXIT()  ((void)0)
#endif

#define COUNT_WORDS (KEYSTATE_KEYS/4)           // four 8-bit counters per word
#define W_HELD      0
#define W_PRESSES   (W_HELD+KEYSTATE_WORDS)
#define W_RELEASES  (W_PRESSES+COUNT_WORDS)
#define W_EDGES     (W_RELEASES+COUNT_WORDS)
#define W_OVERFLOWS (W_EDGES+1)
#define W_TOTAL     (W_OVERFLOWS+1)
_Static_assert(KEYSTATE_KEYS%4==0, "counters pack four to a word");

static _Atomic uint32_t words[W_TOTAL];
static atomic_uint      seq;

static uint32_t get(int w) { return atomic_load_explicit(&words[w],memory_order_relaxed); }
static void put(int w, uint32_t v) { atomic_store_explicit(&words[w],v,memory_order_relaxed); }

static void begin(void) {
    WRITER_ENTER();
    unsigned s=atomic_load_explicit(&seq,memory_order_relaxed);
    atomic_store_explicit(&seq,s+1,memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
}
static void end(void) {
    unsigned s=atomic_load_explicit(&seq,memory_order_relaxed);
    atomic_store_explicit(&seq,s+1,memory_order_release);
    WRITER_EXIT();
}

// Increments key i's 8-bit counter in the table starting at word `base`.
static void bump(int base, int i) {
    int w=base+i/4, shift=(i%4)*8;
    uint32_t v=get(w);
    uint32_t n=((v>>shift)+1u)&0xffu;
    put(w,(v&~(0xffu<<shift))|(n<<shift));
}

// Writer only, inside begin()/end().
static void edge(int i, bool pressed) {
    int w=W_HELD+(i>>5);
    uint32_t bit=1u<<(i&31), v=get(w);
    if(((v&bit)!=0)==pressed) return;
    put(w,pressed?v|bit:v&~bit);
    bump(pressed?W_PRESSES:W_RELEASES,i);
    put(W_EDGES,get(W_EDGES)+1);
}

void keystate_apply(int row, int col, bool pressed) {
    if(row<0||row>=KEYSTATE_ROWS||col<0||col>=KEYSTATE_COLS) return;
    int i=row*KEYSTATE_COLS+col;
    // Only the writer changes the words, so reading them outside the window
    // is exact, and a duplicate edge does not disturb the sequence at all.
    if((((get(W_HELD+(i>>5))>>(i&31))&1u)!=0)==pressed) return;
    begin();
    edge(i,pressed);
    end();
}

void keystate_overflow(void) {
    begin();
    for(int i=0;i<KEYSTATE_KEYS;i++)
        if((get(W_HELD+(i>>5))>>(i&31))&1u) edge(i,false);
    put(W_OVERFLOWS,get(W_OVERFLOWS)+1);
    end();
}

void keystate_snapshot(keystate_snapshot_t *out) {
    uint32_t copy[W_TOTAL];
    unsigned before, after;
    do {
        before=atomic_load_explicit(&seq,memory_order_acquire);
        for(int w=0;w<W_TOTAL;w++) copy[w]=get(w);
        atomic_thread_fence(memory_order_acquire);
        after=atomic_load_explicit(&seq,memory_order_relaxed);
    } while((before&1u) || before!=after);
    for(int w=0;w<KEYSTATE_WORDS;w++) out->held[w]=copy[W_HELD+w];
    for(int i=0;i<KEYSTATE_KEYS;i++) {
        int shift=(i%4)*8;
        out->presses[i]=(uint8_t)(copy[W_PRESSES+i/4]>>shift);
        out->releases[i]=(uint8_t)(copy[W_RELEASES+i/4]>>shift);
    }
    out->edges=copy[W_EDGES];
    out->overflows=copy[W_OVERFLOWS];
}
