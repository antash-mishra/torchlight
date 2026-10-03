# ipc

> **Status:** Implemented (M2/M3): strict JSON-lines, Unix sockets and asynchronous exchanges
> **Source:** `src/ipc/ipc.c`, `src/ipc/client.c`, `src/ipc/async.c`
> **Headers:** `include/torchlight/ipc.h`, `include/torchlight/client.h`, `include/torchlight/async.h`
> **Tests:** `tests/unit/test_ipc.c`, `tests/unit/test_json.c`, `tests/test_daemon.py`

The default socket is `$XDG_RUNTIME_DIR/torchlight.sock`; its directory must be
owned by the current user without group/other write access. Explicit socket paths
must be absolute and have an existing parent. The listener verifies same-uid
peers, creates a mode-0600 socket and uses a persistent adjacent lock file.
Only an owned stale socket can be replaced; foreign files, symlinks and regular
files are rejected. The database has its own canonical-path singleton lock shared
by offline indexing. Client calls have a five-second total deadline.

Requests are flat version-1 JSON objects, at most 8 KiB including newline.
Common fields are integer `version: 1`, string `request_id` (1–64 UTF-8 bytes)
and `op`. Unknown/duplicate fields, invalid UTF-8, decoded NUL and wrong types
are rejected. The generic parser bounds depth to eight and tokens to 32 here.

| op | Additional fields |
|---|---|
| `query` | required `query` (0–256 UTF-8 bytes), optional integer `limit` (1–1000, default 10) |
| `status` | none |
| `resolve` | required `file_id` (nonzero decimal string, at most INT64_MAX) |
| `open` | required `file_id`, `event_id` (1–128 bytes), optional `search_id` (up to 128 bytes) |
| `reconcile` | none |
| `history_clear` | none |

Responses include `version`, `request_id`, `search_id`, `catalog_gen`,
`emb_gen: null`, `phase: final`, `status`, `reason`, `indexing`, `history` and
bounded `results`. Query success uses `reason: lexical_only`; semantics/two-phase
execution arrive in M4. Query frames superseded by newer buffered frames receive
`status: cancelled`; active duplicate ids receive
`reason: duplicate_active_request_id`. Malformed requests without a valid decoded
envelope receive an empty request id and the connection closes after the error.

Results encode ids as decimal strings and have a valid UTF-8 `display` plus
exact `path` for valid UTF-8, or canonical `path_b64` for other bytes. JSON escapes
embedded controls/newlines. Display never supplies an action target. Resolve
returns the current exact path or `stale_result`; open records only an accepted
history request. Responses release catalog leases before queuing socket output.
The daemon bounds clients, four active ids and 1 MiB total output per client.

`client_request` validates terminal envelopes, prints JSON, safe plain paths or
original NUL-delimited paths, and returns a failure for server errors. It may
allocate in the CLI; daemon request decoding/result encoding do not allocate.

See [daemon](../daemon/README.md), [core JSON](../core/README.md) and
[ADR 0011](../../adr/0011-m2-daemon-writer-and-reconciliation.md).

M3 results add kind (`file`, `folder`, `application`, `settings`). Desktop results
also include localized name, desktop_id, decimal-string desktop_revision and
GIcon string icon. File paths remain raw bytes, and desktop paths are actual
entry filenames. Desktop ids occupy a session-scoped range above 2^62; the
existing file_id request field resolves or records either kind. Changed/removed
entries become stale. catalog_gen still identifies the file snapshot only.

`ipc_exchange_create/destroy` provide main-context asynchronous GIO connections,
peer verification, bounded framing and request/version/phase validation. A
callback borrows one response; lexical frames may precede a final frame. Errors,
EOF and the five-second overall deadline terminate it. Destroy cancels pending
work and suppresses callbacks; internal references reclaim outstanding operations.
`tests/unit/test_async.c` fragments frames and checks phases, EOF and wrong ids.
