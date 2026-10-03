/* Main-context asynchronous bounded IPC exchanges with terminal deadlines. */
#ifndef TORCHLIGHT_ASYNC_H
#define TORCHLIGHT_ASYNC_H
#include "torchlight/ipc.h"
typedef struct tl_ipc_exchange tl_ipc_exchange;
/** Borrow response for callback only; TL_OK includes validated lexical/final
 * envelopes. terminal is true for final/errors. Context remains caller-owned. */
typedef void (*tl_ipc_callback)(void *context, tl_status status, const char *response,
                                size_t length, bool terminal);
/** Start one nonblocking exchange on current GLib main context. Copies request
 * and socket path; out owned until destroy. Callback may run multiple times for
 * lexical then final. Bound response bytes and five-second overall deadline.
 * TL_INVALID/NOMEM; no callback on immediate error. Current-user socket peer is
 * verified before sending. Call all exchange APIs on the same main context. */
tl_status ipc_exchange_create(const char *socket_path, const tl_ipc_request *request,
                              tl_ipc_callback callback, void *context, tl_ipc_exchange **out);
/** Cancel/free caller ownership, suppress future callbacks. May be called in
 * callback; pending GIO operations finish cleanup independently. NULL allowed. */
void ipc_exchange_destroy(tl_ipc_exchange *exchange);
#endif
