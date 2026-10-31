/* Versioned bounded Unix-socket requests, exact byte paths and client transport. */
#ifndef TORCHLIGHT_IPC_H
#define TORCHLIGHT_IPC_H
#include "torchlight/json.h"
#include "torchlight/lexical.h"
#include "torchlight/search_actions.h"
#include "torchlight/software.h"
#define IPC_VERSION 1
#define IPC_ACTION_VERSION 2
#define IPC_REQUEST_BYTES 8192
#define IPC_REQUEST_TOKENS 64
#define IPC_RESPONSE_BYTES (1024U * 1024U)
/* Includes nested source identities in a 1,000-row software response. */
#define IPC_RESPONSE_TOKENS 65536
#define IPC_REQUEST_ID_BYTES 64
#define IPC_HISTORY_ID_BYTES 128
#define IPC_DESKTOP_ID_BYTES 4096
#define IPC_DEADLINE_MS 5000
#define IPC_SOFTWARE_SUGGESTIONS SOFTWARE_SUGGESTIONS
/* Owned snapshot of suggestions, frozen across lexical and semantic responses. */
typedef struct {
    tl_software_ref ref;
    uint64_t revision, source_revision;
    char name[SOFTWARE_NAME_BYTES], summary[SOFTWARE_SUMMARY_BYTES];
} tl_ipc_suggestion;
typedef struct {
    tl_ipc_suggestion entries[IPC_SOFTWARE_SUGGESTIONS];
    size_t count;
    bool preferred;
} tl_ipc_suggestions;
typedef enum {
    IPC_QUERY,
    IPC_STATUS,
    IPC_RESOLVE,
    IPC_OPEN,
    IPC_RECONCILE,
    IPC_HISTORY_CLEAR,
    IPC_SOFTWARE_REFRESH
} tl_ipc_operation;
typedef struct {
    tl_ipc_operation operation;
    unsigned version; /* 0 selects legacy v1 for existing callers. */
    tl_search_mode mode;
    bool web_fallback, software_target, software_suggestions;
    tl_software_ref software;
    uint64_t software_revision, source_revision;
    /* Service-only frozen settings for both lexical and semantic frames. */
    tl_search_settings action_settings;
    tl_ipc_suggestions suggestions; /* Service-only, never decoded from a request. */
    char request_id[IPC_REQUEST_ID_BYTES + 1], query[LEXICAL_QUERY_BYTES + 1];
    char search_id[IPC_HISTORY_ID_BYTES + 1], event_id[IPC_HISTORY_ID_BYTES + 1];
    /* Internal writer-only resolved desktop id; never accepted from the wire. */
    char desktop_id[IPC_DESKTOP_ID_BYTES];
    uint64_t file_id;
    size_t limit;
} tl_ipc_request;
/** Effective wire version; zero-initialized requests use v1. No ownership/I/O. */
unsigned ipc_version(const tl_ipc_request *request);
/** Encode/decode a typed Flatpak target (including both decimal-string
 * revisions). Inputs borrowed; decoder zeroes outputs on errors TL_INVALID/
 * LIMIT. Strict fields, no paths or launch commands. Encoder errors in buffer. */
void ipc_software_target(tl_json_buffer *buffer, const tl_software_ref *ref, uint64_t revision,
                         uint64_t source_revision);
tl_status ipc_software_decode(const tl_json *json, size_t token, tl_software_ref *ref,
                              uint64_t *revision, uint64_t *source_revision);
/** Encode available-app display metadata and its typed target. Borrowed inputs;
 * no allocation/I/O. Errors retained in buffer.status. */
void ipc_software_result(tl_json_buffer *buffer, const tl_software_entry *entry);
/** Append frozen ordinary-query software suggestions, only when requested.
 * Borrowed request; include=false emits an empty array. No allocation/I/O;
 * malformed count is retained as TL_INVALID in buffer.status. */
void ipc_suggestions_result(tl_json_buffer *buffer, const tl_ipc_request *request, bool include);
/** Parse an additive status capability. A valid v1 status without capabilities
 * returns supported=false; malformed/error/nonfinal status fails. No I/O. */
tl_status ipc_action_capability(const char *response, size_t length, bool *supported);
typedef struct tl_ipc_listener tl_ipc_listener;
/** Decode a bounded JSON object (newline excluded), rejecting unknown/duplicate
 * fields, wrong types, non-string file ids, decoded NUL and unsupported versions.
 * out is zeroed on errors TL_INVALID/LIMIT. No allocation; all strings copied. */
tl_status ipc_decode(const char *line, size_t length, tl_ipc_request *out);
/** Encode one request plus newline in caller buffer. TL_INVALID/LIMIT;
 * out_length zero on errors. No allocation or borrowed output paths. */
tl_status ipc_encode(const tl_ipc_request *request, char *out, size_t capacity, size_t *out_length);
/** Append result id/display/exact path to JSON buffer. UTF-8 paths use path;
 * other bytes use path_b64. Errors retained in buffer.status, no allocation. */
void ipc_result(tl_json_buffer *buffer, uint64_t id, const char *path);
/** Return owned default socket path under safe, owned XDG_RUNTIME_DIR. Free
 * with ipc_path_destroy. TL_INVALID/IO/NOMEM; out NULL on error. */
tl_status ipc_default_path(char **out);
/** Free owned IPC path; NULL allowed, no errors. */
void ipc_path_destroy(char *path);
/** Acquire owned advisory singleton lock at path (O_NOFOLLOW, mode 0600).
 * Returns fd through out; close with ipc_unlock. TL_STATE if already held,
 * TL_INVALID/IO otherwise. Reject foreign/nonregular lock files. */
tl_status ipc_lock(const char *path, int *out);
/** Close a singleton lock; negative allowed, no errors. Never unlink lock files
 * because doing so allows simultaneous locks on different inodes. */
void ipc_unlock(int fd);
/** Lock a canonical absolute database path's adjacent .daemon.lock file so
 * offline index and daemon cannot write together. Same errors/ownership as lock. */
tl_status ipc_database_lock(const char *database, int *out);
/** Create owned nonblocking listener and adjacent singleton lock, safely
 * replacing only an owned stale socket. Parent must exist, path absolute.
 * Socket mode 0600. TL_INVALID/STATE/IO/NOMEM/LIMIT; out NULL on error. */
tl_status ipc_listener_create(const char *path, tl_ipc_listener **out);
/** Close listener and remove its socket, release lock. NULL allowed, no errors. */
void ipc_listener_destroy(tl_ipc_listener *listener);
/** Borrow descriptor for poll, -1 for NULL; no ownership/errors. */
int ipc_listener_descriptor(const tl_ipc_listener *listener);
/** Accept same-uid peer into owned nonblocking CLOEXEC descriptor. TL_STATE
 * when no peer ready, TL_IO/INVALID otherwise; out -1 on errors. Close by caller. */
tl_status ipc_accept(tl_ipc_listener *listener, int *out);
/** Exchange one request with terminal response under a 5-second total deadline.
 * Consumes one optional lexical frame; terminal response copied into caller buffer;
 * TL_INVALID/IO/LIMIT. No allocation; server errors remain JSON for caller to inspect. out_length
 * zero on error. */
tl_status ipc_call(const char *path, const tl_ipc_request *request, char *response, size_t capacity,
                   size_t *out_length);
#endif
