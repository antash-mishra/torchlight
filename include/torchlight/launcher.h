/* GTK launcher application lifecycle; GTK types remain private. */
#ifndef TORCHLIGHT_LAUNCHER_H
#define TORCHLIGHT_LAUNCHER_H
#include "torchlight/common.h"
typedef struct popup tl_launcher;
/** Create owned single-instance GTK application/model. Main thread only.
 * TL_INVALID/NOMEM; out NULL on errors. No window until run receives command. */
tl_status launcher_create(tl_launcher **out);
/** Set an optional process allocator hook, called on the main thread after a
 * hidden window has released its renderer and remained idle for 500 ms. A
 * quick reopen cancels it. Configure before run: TL_INVALID for NULL launcher,
 * TL_STATE after window creation. release/context are borrowed until destroy;
 * NULL release disables the hook. The callback must not touch GTK or fail. */
tl_status launcher_set_memory_release(tl_launcher *launcher, void (*release)(void *context),
                                      void *context);
/** Run desktop main loop with borrowed argv, writing process exit code. Hidden
 * popups exit after 60 seconds when service lifetime permits; the command-line
 * --idle-exit-seconds option accepts 0..86400 (0 keeps the process resident).
 * Reopening cancels idle exit; pending launches/history complete first. Requires
 * owned application on creating/main thread. TL_INVALID on invalid arguments. */
tl_status launcher_run(tl_launcher *launcher, int argc, char **argv, int *exit_code);
/** Cancel IPC/timers, drain launch worker and free context; call after run.
 * Main thread only, NULL allowed. All widget/model pointers expire. */
void launcher_destroy(tl_launcher *launcher);
#endif
