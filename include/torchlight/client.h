/* CLI-side terminal-response validation and byte-preserving result output. */
#ifndef TORCHLIGHT_CLIENT_H
#define TORCHLIGHT_CLIENT_H
#include "torchlight/ipc.h"
#include <stdio.h>
/** Probe v1 status for v2 action support. TL_OK with false only for a valid old
 * daemon response; malformed/timeouts remain errors. Allocates CLI scratch;
 * inputs borrowed, no launches or ownership transfer. */
tl_status client_actions_supported(const char *socket_path, bool *supported);
/** Exchange a request and write its terminal response to borrowed output.
 * json_output writes the wire JSON; null_output writes exact result paths with
 * NUL separators; plain result output escapes controls/invalid bytes. Status
 * actions print JSON. Server errors are printed in JSON mode and return
 * TL_STATE; other errors TL_INVALID/NOMEM/IO/LIMIT. Allocates only in CLI. */
tl_status client_request(const char *socket_path, const tl_ipc_request *request, bool json_output,
                         bool null_output, FILE *output);
#endif
