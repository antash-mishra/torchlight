# daemon

> **Status:** Implemented (M6 step 1): dedicated search thread with per-client
> request queues, supersession and cancellation, over the M4 two-phase service
> **Source:** `src/service/daemon.c`, `src/bin/torchlightd.c`
> **Header:** `include/torchlight/daemon.h`
> **Tests:** `tests/test_daemon.py`, `tests/test_semantic.py`,
> `tests/unit/test_catalog.c`, `tests/unit/test_writer.c`

`torchlightd` loads the saved catalog and begins serving before its worker's
background reconciliation finishes. `daemon_create/run/destroy` own the poll
loop, catalog, writer, client buffers, socket/database singleton locks and signal
descriptor. Configuration outlives the daemon. SIGINT/SIGTERM are blocked before
creating the worker and consumed through signalfd; destruction joins the worker
and restores the creating thread's signal mask.

Two threads serve clients. The IPC thread reads, frames and writes sockets.
The search thread takes the head of one client's queue at a time (rotating
across clients), leases the resident catalog and desktop snapshot, searches,
encodes into a private scratch buffer and appends the frame to the client's
output under the daemon lock before releasing its leases. It performs no SQLite
calls, filesystem reads or heap allocation. Sixteen clients can remain
connected. Inputs are 8 KiB, total queued output is 1 MiB per client, and four
request ids may be outstanding (queued, running or answered but unsent). Slow
or partial clients expire after five seconds and cannot pin old catalogs.

A new query marks that client's queued queries superseded and sets the engine's
cancellation flag if its query is already running; those queries still answer,
in order, with `status: cancelled` and `reason: superseded`, and non-query
frames between them are served. Duplicate outstanding ids are rejected. A peer
that closes its socket outright (as the popup does when a newer keystroke
replaces an exchange) is closed at once and its queued or running work is
dropped; a peer that only shut its write half still receives every reply. See
[ADR 0028](../../adr/0028-m6-search-thread-and-persistence-owner.md).

Every M2 request ends with `phase: final`; query completion uses
`reason: lexical_only` when no model is configured. With `--model`, ready matching
snapshots send lexical then final hybrid/fallback through the semantic service. Each accepted query receives a random-session/sequence
search id before one optional history enqueue. Resolve leases the current view;
stale ids return `stale_result`. Open recording is asynchronous and validates the
current catalog id, without launching an external application.

Status includes active indexing, degraded/recovering state, offline roots,
unreadable scopes, watch coverage and loss counters, history
pending/dropped/failures/written counts, and last reconciliation duration. `timing.engine_us` measures lexical
search only; IPC acceptance, encoding and socket queues belong to round-trip
timing. See [evaluation](../../evaluation.md) for benchmarks and limitations.

Tests exercise live changes/moves, raw paths/newlines, stable/stale ids, concurrent
clients/updates, SQL lock isolation, failure rollback, restart reconciliation,
unavailable roots, watch exhaustion, disabled/deduplicated history, malformed
requests, cancellation, rapid typing with interleaved status frames, abandoned
connections, duplicate ids, size bounds and client deadlines.

See [writer](../writer/README.md), [IPC](../ipc/README.md) and
[ADR 0011](../../adr/0011-m2-daemon-writer-and-reconciliation.md).

M3 owns a separate desktop catalog worker, merges its bounded lexical results
with files by score, and adds typed launch metadata. Desktop name-prefix
weighting is applied inside its engine before truncation. Tied applications
retain their engine order and precede tied files, keeping the same head for
different result limits. See [ADR 0018](../../adr/0018-application-name-ranking.md).
Query/result encoding holds
a short desktop lease and a file-catalog lease, releases both before socket
output, and does no new filesystem/SQL/allocation work. Resolve/open dispatches
desktop session ids to that current catalog. Open still records acceptance only;
actual activation belongs to the UI. Reconcile wakes both background catalogs.
See [desktop](../desktop/README.md), `tests/test_desktop.py` and ADR 0015.

## M4 integration

See [native backend/service decision](../../adr/0023-m4-native-potion-and-two-phase-search.md)
and [measured model evaluation](../../m4-model-evaluation.md).

Status includes semantic availability, staging progress, vector bytes and the
last background error. The coordinator enforces semantic deadlines independently
of blocked inference/cache work. Write-half-closed clients can still receive
both responses. Response-limit terminal errors cancel pending semantic jobs.

The coordinator drains earlier socket output before taking a semantic final,
so two individually valid frames may exceed 1 MiB together without overflowing
the per-client queue. The semantic job remains pending until transmission space
is available, cancellation occurs or the client expires. Every phase carries
current indexing/history/semantic status, including degraded indexing warnings.
The worker reserves 1 KiB of its frame budget for this coordinator-owned status.
Large fast/slow-reader exchanges and degraded-root regressions exercise this
contract. See [ADR 0024](../../adr/0024-m4-response-backpressure-and-model-inputs.md).
