#include "ksn_shiki_jit.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
/* As ksn_shiki.c: only the real ESP32-S3 compiler reaches the device path. */
#if defined(__xtensa__) && defined(__XTENSA__) && defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_IDF_TARGET_ESP32S3
#define KSN_JIT_DEVICE 1
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_cpu.h"
#else
#define KSN_JIT_DEVICE 0
#endif

#if KSN_JIT_DEVICE
/* Two functions, assembled once with the toolchain that builds this firmware
 * (xtensa-esp32s3-elf-as, esp-15.2.0) and kept as bytes; stage 0 checks
 * that such bytes run from RAM, not how they are made (that is the encoder's
 * job, shiki-jit.md sec.4). Windowed ABI: entry a1, 32 ... retw.n; a2.. are
 * the arguments, a2 the result.
 *
 * one(a, b, d): d = a +sat b over 8 lanes (one 128-bit block); returns 42.
 *   entry a1,32; ee.vld.128.ip q0,a2,0; ee.vld.128.ip q1,a3,0;
 *   ee.vadds.s16 q2,q0,q1; ee.vst.128.ip q2,a4,0; movi.n a2,42; retw.n */
static const uint8_t ONE[]={0x36,0x41,0x00,0x24,0x00,0x83,0x34,0x80,0x83,0x64,0x08,0x9e,
                            0x44,0x00,0x9a,0x2c,0xa2,0x1d,0xf0};
/* loop(a, b, d, blocks): the same over `blocks` blocks in a zero-overhead
 * loop, pointers post-incremented by 16; returns 43. The assembler put a
 * nop.n before loopgtz so that the body starts aligned; the bytes keep that
 * only if the function starts 16-aligned, as it did when assembled.
 *   entry a1,32; nop.n; loopgtz a5,end; ee.vld.128.ip q0,a2,16;
 *   ee.vld.128.ip q1,a3,16; ee.vadds.s16 q2,q0,q1; ee.vst.128.ip q2,a4,16;
 *   end: movi.n a2,43; retw.n */
static const uint8_t LOOP[]={0x36,0x41,0x00,0x3d,0xf0,0x76,0xa5,0x0b,0x24,0x01,0x83,0x34,
                             0x81,0x83,0x64,0x08,0x9e,0x44,0x01,0x9a,0x2c,0xb2,0x1d,0xf0};
#define LOOP_AT 32u                       /* offset of LOOP in the buffer, 16-aligned */
#define CODE_BYTES 64u
#define BLOCKS 16u

typedef int (*one_fn)(const int16_t *,const int16_t *,int16_t *);
typedef int (*loop_fn)(const int16_t *,const int16_t *,int16_t *,int);

static int16_t sat16(int32_t v){return v>32767?32767:v<-32768?-32768:(int16_t)v;}

/* Copy bytes into executable memory as 32-bit words: the instruction bus
 * alias takes only word stores, and the data bus alias (when there is one)
 * is written the same way so one path serves both. */
static void put_code(volatile uint32_t *w,const uint8_t *code,size_t len,size_t at){
    uint8_t buf[CODE_BYTES];
    memset(buf,0,sizeof buf);
    memcpy(buf+at,code,len);
    for(size_t k=at/4;k<(at+len+3)/4;k++)
        w[k]=(uint32_t)buf[4*k]|(uint32_t)buf[4*k+1]<<8|(uint32_t)buf[4*k+2]<<16|(uint32_t)buf[4*k+3]<<24;
}

bool ksn_shiki_jit_probe(char *out,size_t n){
#if defined(CONFIG_ESP_SYSTEM_MEMPROT) && CONFIG_ESP_SYSTEM_MEMPROT
    const int memprot=1;
#else
    const int memprot=0;
#endif
    /* 16 bytes extra so that the code can start 16-aligned. */
    uint8_t *raw=heap_caps_malloc(CODE_BYTES+16u,MALLOC_CAP_EXEC);
    if(!raw){snprintf(out,n,"CB jit memprot=%d alloc=NULL",memprot);return true;}
    uint8_t *x=(uint8_t *)(((uintptr_t)raw+15u)&~(uintptr_t)15u);
    const int exec=esp_ptr_executable(x)?1:0;
    const int diram=esp_ptr_in_diram_iram(x)?1:0;
    volatile uint32_t *w=(volatile uint32_t *)(diram?esp_ptr_diram_iram_to_dram(x):(void *)x);
    if(!exec){
        snprintf(out,n,"CB jit memprot=%d alloc=%p exec=0 (not run)",memprot,(void *)x);
        heap_caps_free(raw);
        return true;
    }
    put_code(w,ONE,sizeof ONE,0);
    put_code(w,LOOP,sizeof LOOP,LOOP_AT);
    __asm__ volatile("memw\n" "isync\n":::"memory");

    /* Per call, not .bss: every build would carry 1 KB for one bench line. */
    int16_t *mem=heap_caps_aligned_alloc(16,4u*BLOCKS*8u*sizeof(int16_t),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(!mem){snprintf(out,n,"CB jit NO MEMORY");heap_caps_free(raw);return true;}
    int16_t *a=mem,*b=a+BLOCKS*8,*d=b+BLOCKS*8,*want=d+BLOCKS*8;
    const size_t bytes=BLOCKS*8*sizeof(int16_t);
    for(unsigned k=0;k<BLOCKS*8;k++){
        a[k]=(int16_t)(k*977-30000);
        b[k]=(int16_t)(k&1?32000-(int)k*13:-(int)k*251);
        want[k]=sat16((int32_t)a[k]+b[k]);
    }
    memset(d,0,bytes);
    const int r1=((one_fn)(void *)x)(a,b,d);
    const int ok1=r1==42&&!memcmp(d,want,16)&&d[8]==0;

    memset(d,0,bytes);
    const int r2=((loop_fn)(void *)(x+LOOP_AT))(a,b,d,BLOCKS);
    const int ok2=r2==43&&!memcmp(d,want,bytes);

    /* 7-run minimum each: the generated loop, and the same sum in C. */
    uint32_t jit=UINT32_MAX,c=UINT32_MAX;
    for(unsigned r=0;r<7;r++){
        const uint32_t t0=esp_cpu_get_cycle_count();
        ((loop_fn)(void *)(x+LOOP_AT))(a,b,d,BLOCKS);
        const uint32_t dt=esp_cpu_get_cycle_count()-t0;
        if(dt<jit)jit=dt;
    }
    for(unsigned r=0;r<7;r++){
        const uint32_t t0=esp_cpu_get_cycle_count();
        for(unsigned k=0;k<BLOCKS*8;k++)d[k]=sat16((int32_t)a[k]+b[k]);
        __asm__ volatile("":::"memory");
        const uint32_t dt=esp_cpu_get_cycle_count()-t0;
        if(dt<c)c=dt;
    }
    snprintf(out,n,"CB jit memprot=%d alloc=%p diram=%d exec=1 one=%s/%d loop=%s/%d blocks=%u cy_jit=%lu cy_c=%lu",
             memprot,(void *)x,diram,ok1?"same":"DIFF",r1,ok2?"same":"DIFF",r2,BLOCKS,
             (unsigned long)jit,(unsigned long)c);
    heap_caps_free(mem);
    heap_caps_free(raw);
    return true;
}
#else
bool ksn_shiki_jit_probe(char *out,size_t n){
    snprintf(out,n,"CB jit device only");
    return true;
}
#endif
