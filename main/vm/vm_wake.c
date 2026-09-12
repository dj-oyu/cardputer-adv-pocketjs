#include "vm_wake.h"

// One owner task at a time, like the guest itself (spec sec.3 rule 1). Plain
// static, not atomic-with-a-lock: it is written once from the owner task
// before any producer can exist (ui_task binds at its top, and every producer
// runs because ui_task started it), and only ever read afterwards.
static TaskHandle_t owner;

void vm_wake_bind(void) { owner = xTaskGetCurrentTaskHandle(); }

void vm_wake_post(void) {
  TaskHandle_t task = owner;
  if (task == NULL)
    return;
  // A completion the OWNER records during its own turn must not shorten the
  // frame cap. Most completions are posted from this very task -- elapsed
  // sleeps in pocket_app_pump(), UART bytes in pocket_io_pump(), link and scan
  // results in pocket_net_pump(), a picker's answer in tick_run() -- and
  // pocket_api_pump() settles every one of them later in the SAME turn. A
  // notification from here would still be sitting in the counter when the loop
  // reaches vm_wake_wait(), so the wait would return at once and the guest
  // would run its next frame() 8 ms after the last one instead of 33: an app
  // that awaits a sleep every frame would get a doubled frame rate, and a UART
  // reader would get it permanently, for a completion that was already
  // delivered. The wake exists for the completions this task cannot see coming
  // -- another task's, or an ISR's -- and those are exactly the ones this test
  // lets through.
  if (!xPortInIsrContext() && xTaskGetCurrentTaskHandle() == task)
    return;
  // The context test is not defensive tidiness: pocket_api_complete() is
  // documented as ISR-safe and IS called from one (the RMT transmit-done
  // callback in pocket_io.c), and xTaskNotifyGive() from an ISR is an abort.
  // Putting the branch here rather than at the call site keeps the producers
  // from having to know which of them can be an ISR.
  if (xPortInIsrContext()) {
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(task, &woken);
    if (woken != pdFALSE)
      portYIELD_FROM_ISR();
    return;
  }
  xTaskNotifyGive(task);
}

uint32_t vm_wake_wait(TickType_t max) {
  // pdTRUE: the count is cleared on exit, so one wait consumes every wake
  // posted since the last one. Waking once for three completions is correct --
  // the turn that follows pumps all three.
  return ulTaskNotifyTake(pdTRUE, max);
}
