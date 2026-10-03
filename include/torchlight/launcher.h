/* GTK launcher application lifecycle; GTK types remain private. */
#ifndef TORCHLIGHT_LAUNCHER_H
#define TORCHLIGHT_LAUNCHER_H
#include "torchlight/common.h"
typedef struct popup tl_launcher;
/** Create owned single-instance GTK application/model. Main thread only.
 * TL_INVALID/NOMEM; out NULL on errors. No window until run receives command. */
tl_status launcher_create(tl_launcher **out);
/** Run desktop main loop with borrowed argv, writing process exit code. Requires
 * owned application on creating/main thread. TL_INVALID on invalid arguments. */
tl_status launcher_run(tl_launcher *launcher, int argc, char **argv, int *exit_code);
/** Cancel IPC/timers, drain launch worker and free context; call after run.
 * Main thread only, NULL allowed. All widget/model pointers expire. */
void launcher_destroy(tl_launcher *launcher);
#endif
