/* Bounded child processes for background discovery and explicit user actions. */
#ifndef TORCHLIGHT_PROCESS_H
#define TORCHLIGHT_PROCESS_H
#include "torchlight/common.h"
#include <stdatomic.h>
#include <stdbool.h>
typedef struct {
    uint64_t deadline_ms;
    size_t remaining_bytes;
    const atomic_bool *cancel;
} tl_process_budget;
/** Monotonic milliseconds, zero on clock failure. No ownership or allocation. */
uint64_t process_now(void);
/** Run a NULL-terminated argv without a shell, capturing stdout into owned text
 * freed by process_output_destroy. Inputs borrowed for the call. A shared budget
 * bounds total wall time and stdout/stderr bytes across multiple calls. Output
 * rejects embedded NUL. TL_STATE for missing executable, TL_IO for failed child,
 * TL_LIMIT for budget exhaustion, TL_CANCELLED, TL_INVALID/NOMEM. out NULL on
 * error. Child process group is killed and reaped on failure; never use on a
 * query thread. Environment inherited with LC_ALL=C.UTF-8. */
tl_status process_capture(const char *const *argv, tl_process_budget *budget, char **out);
/** Free captured output; NULL allowed, no errors. */
void process_output_destroy(char *output);
#endif
