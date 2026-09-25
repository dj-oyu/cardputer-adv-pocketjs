#ifndef KSN_REPAIR_SEND_PROBE_H
#define KSN_REPAIR_SEND_PROBE_H

#include <stdint.h>

/* Diagnostic-only classification. The shell's repair state is 1 while an
 * injection is armed and 2 after KSN_IO until a successful full repair. */
typedef enum {
    KSN_REPAIR_SEND_NORMAL,
    KSN_REPAIR_SEND_INJECT,
    KSN_REPAIR_SEND_RECOVER,
    KSN_REPAIR_SEND_CLASS_COUNT
} ksn_repair_send_class;

typedef struct {
    uint32_t frames[KSN_REPAIR_SEND_CLASS_COUNT];
    uint32_t max_us[KSN_REPAIR_SEND_CLASS_COUNT];
} ksn_repair_send_probe;

static inline ksn_repair_send_class ksn_repair_send_classify(
    unsigned stage_before,unsigned stage_after){
    if(stage_before==1u&&stage_after==2u)return KSN_REPAIR_SEND_INJECT;
    if(stage_before==2u)return KSN_REPAIR_SEND_RECOVER;
    return KSN_REPAIR_SEND_NORMAL;
}

static inline void ksn_repair_send_record(ksn_repair_send_probe *probe,
                                          ksn_repair_send_class kind,uint32_t us){
    if(!probe||(unsigned)kind>=KSN_REPAIR_SEND_CLASS_COUNT)return;
    probe->frames[kind]++;
    if(us>probe->max_us[kind])probe->max_us[kind]=us;
}

#endif
