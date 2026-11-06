#ifndef KSN_MESH_Q7_ENVIRONMENT_H
#define KSN_MESH_Q7_ENVIRONMENT_H
#include <fenv.h>
#include <stdint.h>
/* Preserve the two raw Xtensa registers independently. A packed FSR|FCR
 * snapshot cannot preserve distinct readback bits: restoring that word to
 * both registers can change FCR even when the rounding mode is unchanged.
 * Do not mask the outer wrapper's raw equality check or change the caller's
 * state to make it pass. Other targets retain their libc fenv contract. */
#if defined(__XTENSA__)
typedef struct { uint32_t fcr,fsr; } ksn_q7_environment;
static inline int ksn_q7_environment_get(ksn_q7_environment *environment){
    uint32_t fcr,fsr;
    __asm__ volatile ("rur.fcr %0\n\trur.fsr %1"
        : "=&a"(fcr), "=&a"(fsr) : : "memory");
    environment->fcr=fcr;environment->fsr=fsr;
    return 0;
}
static inline int ksn_q7_environment_set(const ksn_q7_environment *environment){
    uint32_t fcr=environment->fcr,fsr=environment->fsr;
    __asm__ volatile ("wur.fcr %0\n\twur.fsr %1"
        : : "a"(fcr), "a"(fsr) : "memory");
    ksn_q7_environment actual;
    ksn_q7_environment_get(&actual);
    return actual.fcr!=fcr || actual.fsr!=fsr ? -1 : 0;
}
#else
typedef fenv_t ksn_q7_environment;
static inline int ksn_q7_environment_get(ksn_q7_environment *environment){
    return fegetenv(environment);
}
static inline int ksn_q7_environment_set(const ksn_q7_environment *environment){
    return fesetenv(environment);
}
#endif
#endif
