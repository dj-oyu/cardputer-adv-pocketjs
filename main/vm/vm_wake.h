#pragma once

// VM L1 (docs/vm-L1-design.md sec.4): how a completion recorded outside the
// owner task gets the owner task running again.
//
// The honest scope, stated here because the name promises more than the level
// delivers: ui_task cannot actually WAIT today. It is a 30 fps loop that also
// draws the home screen, the pet, the overlay and every other screen, none of
// which stops because no JavaScript has anything to do. A dedicated owner task
// that could block is estimated at +24-32 KiB of DIRAM for its stack -- more
// than the whole L0 budget for L1 -- so it is deferred. What L1 replaces is
// the FRAME-CAP delay at the end of the loop: instead of sleeping out the rest
// of the period unconditionally, the loop sleeps until either the period ends
// or a completion arrives, whichever is first. That removes the "up to one
// frame period" term (measured (device), L0 sec.2.1) from completion latency
// and nothing else.
//
// Why a counting task notification and not a queue or a semaphore: the
// completion record already exists in pocket_api.c's promises[] table (no
// allocation, ISR-safe), so a queue would be a second copy of it. The counter
// is also what makes the pre-wait race unlosable -- see vm_wake_wait().

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/** Remember the calling task as the one to wake. Called once, from the top of
 * the owner task (ui_task). */
void vm_wake_bind(void);

/** Ask the owner task to stop waiting. Safe from any task AND from an ISR
 * (the context is detected inside); safe before vm_wake_bind(), when it does
 * nothing and nobody is waiting yet.
 *
 * ORDERING CONTRACT: publish the completion record FIRST, post the wake
 * second. The waiter re-checks its own state after waking, so a wake posted
 * before the record would be a wake with nothing to find. */
void vm_wake_post(void);

/** Block for at most `max` ticks, returning early on a posted wake. Owner task
 * only. Returns the number of wakes consumed (0 = the timeout expired).
 *
 * The race that kills condition-variable designs cannot happen here because
 * the notification value is a COUNTER, not a flag: if a post lands between the
 * owner's "is there anything to do" check and this call, the counter is
 * already 1 and the call returns immediately instead of blocking. If the post
 * lands before the check, the check sees the completion record (published
 * first, per vm_wake_post). Either interleaving is covered; there is no third. */
uint32_t vm_wake_wait(TickType_t max);
