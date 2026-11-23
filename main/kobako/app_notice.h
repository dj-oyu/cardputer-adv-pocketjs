#ifndef APP_NOTICE_H
#define APP_NOTICE_H
#include "ksn_view.h"
#include "../system/sys_notify.h"
/* Append to the caller's SYSTEM transaction: at most 5 commands/41 text bytes.
 * Does not own the transaction, runtime, resource or notice. Caller cancels on
 * error and submits/polls the whole SYSTEM frame, including other indicators.
 * NULL omits the banner from a REPLACE; no pointer survives this call.
 * Optional pet resource is a SYSTEM-owned 64x64 source, drawn at half scale. */
ksn_result ksn_notice_emit(ksn_view *,ksn_tx,const sys_notice *,ksn_resource pet,uint16_t variant);
#endif
