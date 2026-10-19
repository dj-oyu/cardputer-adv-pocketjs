#ifndef KSN_PROC_EXECUTION_INTERNAL_H
#define KSN_PROC_EXECUTION_INTERNAL_H
#include "ksn_procedural.h"
#ifdef KSN_PROC_VALIDATED_PLAN
/* Only the owned, validated plan implementation may supply these views.
 * Public callers use the snapshot APIs or the pin-checked validated APIs. */
bool ksn_proc_validate_program(const ksn_proc_program *program,uint8_t loop_end[KSN_PROC_CODE]);
ksn_proc_status ksn_proc_execution_begin(ksn_proc_execution *execution,
    const ksn_proc_program *program,const uint8_t loop_end[KSN_PROC_CODE],
    const float input[KSN_PROC_INPUTS],const ksn_proc_state *state,ksn_proc_frame *frame);
ksn_proc_status ksn_proc_execution_step(ksn_proc_execution *execution);
ksn_proc_status ksn_proc_execution_run(ksn_proc_execution *execution);
bool ksn_proc_execution_capture(const ksn_proc_execution *execution,ksn_proc_state *state);
#endif
#endif
