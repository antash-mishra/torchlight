/* Service-aware permission to end an idle popup without stopping launched apps. */
#ifndef TORCHLIGHT_UI_LIFECYCLE_H
#define TORCHLIGHT_UI_LIFECYCLE_H
#include "torchlight/common.h"
#include <gio/gio.h>
#include <stdbool.h>
/** Inspect borrowed Linux cgroup text. True when a nearest unit is a service;
 * scopes do not stop their members when the popup exits. NULL is conservative. */
bool lifecycle_in_service(const char *groups);
/** Interpret borrowed systemd Service properties for this process ID. Missing
 * properties fail closed; no I/O, allocation, ownership transfer or errors. */
bool lifecycle_service_allows_exit(GVariant *properties, guint32 pid);
/** Check whether this process can exit without service teardown/restart.
 * Worker thread only: reads cgroups and may query the user manager. Writes false
 * on TL_IO/INVALID; D-Bus cancellation or an unavailable manager returns TL_IO.
 * cancel is borrowed for this call; allowed is caller-owned. No process changes. */
tl_status lifecycle_can_exit(GCancellable *cancel, bool *allowed);
#endif
