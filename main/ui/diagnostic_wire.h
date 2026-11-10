#ifndef DIAGNOSTIC_WIRE_H
#define DIAGNOSTIC_WIRE_H
#include "diagnostic_control.h"
#include <stddef.h>
/* Reserved native D1 ASCII transport. Request cap includes GS and LF.
 * bind/init before tasks; one input-task parser/writer, no UI callbacks here.
 * begin is called after the shared parser has consumed GS D; feed consumes
 * the rest through LF, including invalid/oversized input. New GS aborts via
 * the shared parser, never by reinterpreting the tail as ordinary keys. */
enum { DIAGNOSTIC_REQUEST_MAX=64, DIAGNOSTIC_RESPONSE_MAX=80 };
typedef bool (*diagnostic_wire_emit)(void *,const uint8_t *,size_t);
typedef struct diagnostic_wire {
    diagnostic_control *control;
    diagnostic_wire_emit emit;
    void *context;
    uint8_t bytes[DIAGNOSTIC_REQUEST_MAX];
    uint8_t length;
    bool dropping;
} diagnostic_wire;
void diagnostic_wire_init(diagnostic_wire *,diagnostic_control *,diagnostic_wire_emit,void *);
void diagnostic_wire_begin(diagnostic_wire *);
void diagnostic_wire_abort(diagnostic_wire *);
bool diagnostic_wire_feed(diagnostic_wire *,uint8_t byte);
#endif
