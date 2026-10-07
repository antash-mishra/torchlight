# 0028. M6 step 1: search thread and persistence owner

- **Status:** Accepted; implemented
- **Date:** 2026-10-07

## Context

ADR 0026 orders M6 as worker separation, then Frizbee scoring, then incremental
indexing. Before this change the daemon's poll thread ran every lexical search
inline, so it could not read a newer keystroke until the current search ended;
only a newer query already sitting in the same receive buffer was treated as
superseding. The writer's single worker crawled, upserted, built the engine and
wrote history in one thread, holding one SQLite write transaction across the
crawl and the engine build, so history waited behind multi-second scans.

## Decision

**Daemon.** An IPC thread owns sockets and framing. A search thread owns catalog
and desktop leases, scoring and response encoding. Each client has a bounded FIFO
of decoded requests (the existing four-active-request bound now covers queued and
answered-but-unsent frames together). A new query marks every queued query of
that client superseded and sets a cancellation flag if the head is already
running. Superseded queries still answer, in order, as `cancelled/superseded`,
so clients that pipeline requests see every request id. Non-query frames queued
between superseded queries are served normally. Completions for a client that
closed meanwhile are dropped through a per-slot generation counter. A peer that
closed its socket outright (POLLHUP, or EPIPE on a zero-byte send) is closed
immediately and its queued and running work dropped; a half-closed peer still
receives its replies. The search thread encodes into a private scratch buffer
and appends under the daemon lock; the IPC thread drains and resets the output
queue under the same lock. The semantic phase keeps its token-based stale
suppression; its job is submitted from the search thread under the daemon lock.

**Engine.** `lexical_workspace_cancel` attaches a caller-owned atomic flag.
Queries poll it before each scoring batch and every 1024 scored entries inside
parallel batches, returning the new `TL_CANCELLED` status with the workspace
reusable. `catalog_reader_cancel` passes the flag through a lease.

**Writer.** Two threads. The indexing thread drains inotify, crawls selected
roots into a private batch (copied entries, kept offline roots, rename pairs
split into before-scan and during-scan ranges), hands the batch to the
persistence thread and waits, then builds the engine from the committed rows on
its own read-only connection and publishes. The persistence thread owns the
write connection: it applies each batch in one short transaction (renames,
upserts, keeps, root bookkeeping, pruning of successfully scanned roots, commit
or rollback when nothing changed), drains the history ring and runs retention.
Scans and engine construction never run inside a write transaction. Build or
publication failure after a commit sets `recovering` and reloads the saved
catalog, as failed publication did before. Shutdown joins the indexing thread,
then lets the persistence thread answer any pending batch and drain history
within the existing deadline. Status gains `history.written`.

## Alternatives considered

- Keeping the search inline and only shrinking supersession windows would not
  let the IPC loop observe a newer keystroke during a long scan.
- A second SQLite connection for history alone would still wait behind the
  crawl-long write transaction; moving the crawl into a private batch is what
  shortens the write lock.
- Per-client output staging buffers for the search thread would cost 64 MiB;
  one scratch buffer and a short locked copy are enough for one search at a
  time.

## Consequences

Typing bursts no longer build an obsolete backlog, and history writes wait at
most for one batch commit instead of a whole scan and build. Peak memory during
a full reconcile now includes the private batch (about 100 bytes per entry) in
addition to the engine under construction; step 3 makes batches small. Search
p95 is unchanged by this step; see the recorded run in
[`tests/bench/results/`](../../tests/bench/results/) and [the M6 plan](../m6-plan.md).
Tests cover rapid typing with interleaved status frames, abandoned connections,
cooperative cancellation of the engine, and history draining while publication
is blocked.
