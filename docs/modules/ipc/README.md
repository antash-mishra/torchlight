# ipc

> **Status:** Implemented (M2/M3/M4/M9): strict JSON-lines, Unix sockets, asynchronous exchanges and capability-gated app suggestions (ADR 0070)
> **Source:** `src/ipc/ipc.c`, `src/ipc/targets.c`, `src/ipc/client.c`, `src/ipc/async.c`
> **Headers:** `include/torchlight/ipc.h`, `include/torchlight/client.h`, `include/torchlight/async.h`
> **Tests:** `tests/unit/test_ipc.c`, `tests/unit/test_json.c`, `tests/test_daemon.py`

The CLI parser reserves 65,536 tokens within the existing response byte limit,
enough for 1,000 software rows with nested source identities. The integration
suite exercises a full-sized page; local path and NUL output retain their format.

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
are rejected. The generic parser bounds depth to eight and tokens to 64 here.

| op | Additional fields |
|---|---|
| `query` | required `query` (0–256 UTF-8 bytes), optional integer `limit` (1–1000, default 10) |
| `status` | none |
| `resolve` | required `file_id` (nonzero decimal string, at most INT64_MAX) |
| `open` | required `file_id`, `event_id` (1–128 bytes), optional `search_id` (up to 128 bytes) |
| `reconcile` | none |
| `history_clear` | none |

Responses include `version`, `request_id`, `search_id`, `catalog_gen`,
`emb_gen`, `phase`, `status`, `reason` and bounded `results`. Lexical-only
queries return one final frame. Available M4 semantics return a lexical frame
followed by a final frame with matching snapshot generations. Every phase
includes current indexing/history/semantic status. Query frames superseded by
newer buffered frames receive `status: cancelled`; active duplicate ids receive
`reason: duplicate_active_request_id`. Malformed requests without a valid decoded
envelope receive an empty request id and the connection closes after the error.

Results encode ids as decimal strings and have a valid UTF-8 `display` plus
exact `path` for valid UTF-8, or canonical `path_b64` for other bytes. JSON escapes
embedded controls/newlines. Display never supplies an action target. Resolve
returns the current exact path or `stale_result`; open records only an accepted
history request. Responses release catalog leases before queuing socket output.
The daemon bounds clients, four active ids and 1 MiB total output per client.
It drains earlier output before queuing a semantic final; two individually
valid frames need not fit in the output queue simultaneously. Client deadlines
and cancellation still bound retained jobs and snapshots.

`ipc_call` validates and consumes at most one lexical frame before returning
the terminal frame within the original deadline, without allocating.
`client_request` validates terminal envelopes, prints JSON, safe plain paths or
original NUL-delimited paths, and returns a failure for server errors. It may
allocate in the CLI; daemon request decoding/result encoding do not allocate.

See [daemon](../daemon/README.md), [core JSON](../core/README.md) and
[ADR 0011](../../adr/0011-m2-daemon-writer-and-reconciliation.md).

M3 results add kind (`file`, `folder`, `application`, `settings`). Desktop results
also include localized name, desktop_id, decimal-string desktop_revision and
GIcon string icon, plus `wm_class` (the entry's `StartupWMClass`) when it has one
of at most 255 bytes; the popup matches open windows with it (ADR 0035). The
field is optional and additive, so the protocol version stays 1. File paths remain raw bytes, and desktop paths are actual
entry filenames. Desktop ids occupy a session-scoped range above 2^62; the
existing file_id request field resolves or records either kind. Changed/removed
entries become stale. catalog_gen still identifies the file snapshot only.

`ipc_exchange_create/destroy` provide main-context asynchronous GIO connections,
peer verification, bounded framing and request/version/phase validation. A
callback borrows one response; lexical frames may precede a final frame. Errors,
EOF and the five-second overall deadline terminate it. Destroy cancels pending
work and suppresses callbacks; internal references reclaim outstanding operations.
`tests/unit/test_async.c` fragments frames and checks phases, EOF and wrong ids.

## Version 2 typed actions

Version 1 remains supported and status adds `capabilities: {search_actions: 2}`.
New clients probe this valid v1 terminal status before using v2. A missing
capability on a valid response means old-daemon local-only support. Invalid
responses and timeouts remain errors. Responses match the request's version.

V2 query requires `mode: local|web|software` and accepts `web_fallback: bool`
(default false, valid only for local). Local results retain their original
fields, order and limit; `actions` contains at most one Web action independently:
`{kind: web, provider: duckduckgo|google, query: TEXT}`. Web mode returns no local
results. Both semantic phases retain the frozen provider/query action.
Software mode returns one final frame with typed results and an empty actions
array. Software targets never carry paths, local ids or desktop ids.

A software result contains kind, name, summary, installed and a target object.
The target has kind=software, backend=flatpak, installation, remote, app_id,
arch, branch, revision and source_revision. Revisions are decimal strings.
V2 resolve accepts exactly one such target instead of file_id and returns
stale_result when it no longer matches. Client action revalidation checks the
external source before handoff. Software actions never use the open/history op.

V2 software_refresh has no additional fields and enqueues bounded background
work. Status/software queries report software.state, count and catalog_gen.
Loading/absent/failed catalogs still return a prompt final frame. The service
never sends unsolicited completion frames when a catalog becomes ready.

Ordinary-name discovery adds `capabilities.software_suggestions: 1`. Only after
seeing it may a client send `software_suggestions: true` on a v2 local query.
False is omitted by the request encoder so older v2 parsers remain compatible.
Opted-in replies include a separate `software_suggestions` array of at most
three typed software results and a boolean `prefer_software`. Local results and
limits remain unchanged. Frozen, owned suggestion metadata and preference are
copied across semantic phases; errors return an empty array and false preference.
V1 and explicit Web/software queries cannot enable the flag. See
[ADR 0070](../../adr/0070-app-discovery-without-a-prefix.md).
