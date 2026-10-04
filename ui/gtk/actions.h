/* Private launch task data, shared only by popup orchestration and actions. */
#ifndef TORCHLIGHT_GTK_ACTIONS_H
#define TORCHLIGHT_GTK_ACTIONS_H
#include "torchlight/popup.h"
#include <gio/gio.h>
struct launch {
    tl_popup_row row;
    tl_ipc_request event;
    GAppLaunchContext *context;
    bool reveal;
};
/** GTask worker: task_data is borrowed launch data until completion. Returns
 * accepted boolean through task; checks cancellation before launch. Performs
 * desktop validation, argv spawn and cancellable file-manager DBus outside main
 * context. Cancellation prevents an unaccepted fallback; accepted actions retain
 * their result for asynchronous history even if later canceled. */
void actions_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancel);
#endif
