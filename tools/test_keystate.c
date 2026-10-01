// main/hal/keystate.c on the host: the edge rules single-threaded, then one
// writer thread against two readers, checking that every snapshot is a state
// the writer actually passed through. Built twice by tools/build_keystate_test.sh
// (ASan+UBSan, then TSan): TSan is the check that the seqlock's words and
// fences are a C11-correct protocol and not a race that happens to work.
//
//   wsl -e bash -lc "cd /mnt/c/.../cardputer-adv-pocketjs && bash tools/build_keystate_test.sh"
#include "keystate.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned failures;
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#c); failures++; } } while(0)

static int idx(int r, int c) { return r*KEYSTATE_COLS+c; }

// A snapshot is one state the writer passed through iff, for every key, its
// press count minus its release count is its held bit (edges alternate from
// up), and the total is the sum of the per-key counts. A torn copy -- held
// from one edge, counters from another -- breaks one of the two.
static bool coherent(const keystate_snapshot_t *s) {
    uint32_t sum=0;
    for(int i=0;i<KEYSTATE_KEYS;i++) {
        if((uint8_t)(s->presses[i]-s->releases[i])!=(keystate_bit(s->held,i)?1:0)) return false;
        sum+=s->presses[i]+s->releases[i];
    }
    return (sum&0xffu)==(s->edges&0xffu);
}

static void single_thread(void) {
    keystate_snapshot_t a, b;
    keystate_snapshot(&a);
    CHECK(coherent(&a) && a.edges==0);
    // Chord: E, A, S, D down together (the physical cells of a WASD layout
    // shifted onto this matrix), plus Shift.
    int keys[]={idx(1,3),idx(2,2),idx(2,3),idx(2,4),idx(2,1)};
    for(int k=0;k<5;k++) keystate_apply(keys[k]/KEYSTATE_COLS,keys[k]%KEYSTATE_COLS,true);
    keystate_snapshot(&b);
    CHECK(coherent(&b) && b.edges==5);
    for(int k=0;k<5;k++) CHECK(keystate_bit(b.held,keys[k]) && b.presses[keys[k]]==1);
    // Duplicate edges change nothing.
    keystate_apply(1,3,true);
    keystate_apply(0,0,false);
    keystate_snapshot(&a);
    CHECK(a.edges==5 && a.presses[idx(1,3)]==1 && !keystate_bit(a.held,idx(0,0)));
    // A tap between two reads: one press and one release, key up.
    keystate_apply(3,13,true); keystate_apply(3,13,false);
    keystate_snapshot(&b);
    CHECK((uint8_t)(b.presses[idx(3,13)]-a.presses[idx(3,13)])==1);
    CHECK((uint8_t)(b.releases[idx(3,13)]-a.releases[idx(3,13)])==1);
    CHECK(!keystate_bit(b.held,idx(3,13)) && coherent(&b));
    // Out of the matrix: ignored.
    keystate_apply(4,0,true); keystate_apply(0,14,true); keystate_apply(-1,0,true);
    keystate_snapshot(&a);
    CHECK(a.edges==b.edges);
    // Overflow: every held key released, with a counted edge, and a later
    // real release of the same key is a duplicate.
    keystate_overflow();
    keystate_snapshot(&b);
    CHECK(coherent(&b) && b.overflows==1 && b.edges==a.edges+5);
    for(int k=0;k<5;k++) CHECK(!keystate_bit(b.held,keys[k]) && b.releases[keys[k]]==1);
    keystate_apply(1,3,false);
    keystate_snapshot(&a);
    CHECK(a.edges==b.edges);
    // 8-bit counters wrap, and the difference across the wrap is still right.
    keystate_snapshot(&a);
    for(int n=0;n<300;n++) { keystate_apply(0,5,true); keystate_apply(0,5,false); }
    keystate_snapshot(&b);
    CHECK((uint8_t)(b.presses[idx(0,5)]-a.presses[idx(0,5)])==(uint8_t)300);
    CHECK(coherent(&b));
}

static atomic_bool done;
static atomic_ulong torn, reads;

static void *writer(void *arg) {
    (void)arg;
    unsigned seed=12345;
    // Until the readers have had a real share of the overlap, not a fixed
    // count: under a sanitizer the writer can otherwise finish first.
    for(int n=0;n<400000 || atomic_load(&reads)<200000;n++) {
        seed=seed*1103515245u+12345u;
        int i=(int)((seed>>8)%KEYSTATE_KEYS);
        keystate_apply(i/KEYSTATE_COLS,i%KEYSTATE_COLS,(seed>>20)&1);
        if(n%50000==0) keystate_overflow();
    }
    atomic_store(&done,true);
    return NULL;
}

static void *reader(void *arg) {
    (void)arg;
    uint32_t last_edges=0;
    while(!atomic_load(&done)) {
        keystate_snapshot_t s;
        keystate_snapshot(&s);
        if(!coherent(&s) || s.edges<last_edges) atomic_fetch_add(&torn,1);
        last_edges=s.edges;
        atomic_fetch_add(&reads,1);
    }
    return NULL;
}

int main(void) {
    single_thread();
    pthread_t w, r1, r2;
    pthread_create(&r1,NULL,reader,NULL);
    pthread_create(&r2,NULL,reader,NULL);
    pthread_create(&w,NULL,writer,NULL);
    pthread_join(w,NULL); pthread_join(r1,NULL); pthread_join(r2,NULL);
    keystate_snapshot_t s;
    keystate_snapshot(&s);
    CHECK(coherent(&s));
    CHECK(atomic_load(&torn)==0);
    printf("keystate: %s (edges, duplicates, tap, overflow, wrap; %lu concurrent reads, %lu torn)\n",
           failures?"FAIL":"PASS",atomic_load(&reads),atomic_load(&torn));
    return failures?1:0;
}
