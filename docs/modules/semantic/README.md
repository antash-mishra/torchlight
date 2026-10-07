# Semantic service

> **Status:** Implemented M4 opt-in two-phase execution and M6 derived
> (segmented) snapshots; large-catalog final-phase latency acceptance remains open. Buffer retries and per-phase status are regression-tested.
> Cache BEGIN failures retain staged progress. Enable with `torchlightd --model PATH.tlm`.

Public contract: [semantic.h](../../../include/torchlight/semantic.h).

One worker serializes model inference, searches and background embedding. Up to
16 jobs (one per connection slot) use preallocated buffers and immutable copied
metadata. Interactive jobs take priority between batches of eight background
rows. Model loads, filesystem metadata copies and bounded cache transactions
can delay a queued final; the coordinator independently enforces the deadline.
The default is 200 ms, configurable with `--semantic-deadline-ms` (1–4000).

File metadata is copied under a lightweight catalog pin without consuming its
lexical workspace. Application metadata is copied under its existing exclusive
lease, released before inference. Each semantic snapshot owns copied raw paths,
icons, names and revisions for both phases. Submission requires matching file
catalog_gen and desktop_gen. Lexical ids are resolved into that owned snapshot;
publication cannot invalidate a pending final. No long desktop mutex lease or
lexical workspace lease is held across inference. The copied metadata budget
is 256 MiB per snapshot; the exhaustive int8 vector budget is 150 MiB. Model,
metadata, workspaces, client buffers and old/staging views count separately in
whole-process RSS. At most active, retired and one staging snapshot coexist;
publication waits for retirement rather than accumulating old versions.

Every query gets an immediate lexical response. Ready, eligible queries receive
`lexical/semantic_pending` followed by `final/hybrid` or a lexical terminal with
`semantic_no_match`, `semantic_error`, or `semantic_deadline`. Missing/mismatched
snapshots, very short queries, path queries and queue saturation return one
lexical terminal (`semantic_unavailable` or `semantic_queue_full`). Query
inference/retrieval never runs in the IPC coordinator. RRF k=60 uses a fixed
1000-entry lexical pool and ten semantic candidates above tuning cosine 0.2;
exact path/name tiers remain explicit. Both phases carry matching request,
search, catalog and embedding ids. History is enqueued only once.

Cancellation emits a terminal `cancelled/superseded`; running work keeps its
snapshot privately until it finishes, and its obsolete output is discarded.
Connection slots cannot reuse a running job, and monotonic tokens prevent output
crossing connections. Eventfd wakes the poll loop. Closed/slow clients release
jobs; the worker reclaims retired views. Existing IPC clients already wait for
the terminal response and the popup preserves selection by id.

`semantic_take` does not consume a ready, cancelled or deadline response when
the caller buffer is too small: `TL_LIMIT` leaves the token and snapshot live
for retry or cancellation. The coordinator waits for earlier output to drain
before taking a normal final, preserving the existing 1 MiB client bound.
Worker envelopes leave 1 KiB for the coordinator to append current indexing,
history and semantic status; status is present on every phase. See
[ADR 0024](../../adr/0024-m4-response-backpressure-and-model-inputs.md).

Schema v4 caches exact prepared text under full model/transform emb_gen. Changing
a path or application revision produces different metadata/text or session ids;
staging is abandoned when its source generations change. Unchanged text reuses
cached floats; only misses invoke inference. Complete staging validates before
atomic cache activation and resident publication. Restart reconstructs from
validated cache; model replacements use checked new tables and preserve old
query snapshots. Bad replacement files keep the last valid snapshot, or lexical
fallback when no valid snapshot exists. Full metadata/vector rebuild remains a
baseline; startup and update costs are measured, not claimed incremental.

Reconciliation can hold SQLite's writer lock longer than the cache connection's
busy timeout. A failed cache `BEGIN IMMEDIATE` leaves all rows untouched, so the
service retains the staged vectors, metadata and position and retries after a
bounded wait. Interactive jobs can wake that wait. Source generations are still
checked before retrying; an obsolete stage is discarded. Failures after a batch
begins still abandon staging because rows/text ownership may have changed.
`building` remains true and `last_error` reports the failed attempt until progress
resumes. See [ADR 0025](../../adr/0025-m4-cache-staging-retries.md).

[Lifecycle tests](../../../tests/test_semantic.py) use independent analytic
models and cover restart/cache, rename/delete, application removal, replacement,
missing models, blocked-writer deadlines, injected running cancellation,
queue saturation, write-half-close completion, large two-phase responses,
degraded status, injected cache contention and FIFO model shutdown. Cache tests
hold repeated BEGIN failures, check retained progress and lexical availability,
then release contention and require a hybrid final.
[Unit tests](../../../tests/unit/test_semantic.c)
check buffer-limit retries and snapshot release. Trained-model
and large-catalog measurements are in [M4 evaluation](../../m4-model-evaluation.md).

## Incremental stages (M6 step 3c)

Each staged row keeps a 64-bit hash of its prepared embedding text. Vectors
are reused bit-identically (`vector_add_row`), including "no vector" for
unembeddable rows, whenever a row's prepared text is unchanged; only other
rows go through the cache or the model, and background steps copy up to 4096
reused rows between jobs.

**Derived snapshots.** Semantic snapshots are segmented like catalog
snapshots. A full snapshot owns every row. A derived snapshot holds one
reference on a full base, owns only the rows changed since that base (sorted
by id, with their own vector index) and keeps two bitmaps of the base entries
and vector rows it hides. When the published snapshot shares the new catalog
view's catalog base (`catalog_snapshot_base_gen`) and the same model, a stage
is derived: only the ids in the union of both views' catalog change sets
(`catalog_snapshot_changes`) are re-read, plus every application when the
desktop catalog changed. A gone id hides its base row; an unchanged base row
stays shared; any other live row becomes an own row hiding its base row. A
derived stage that would own more than max(4096, base/32) rows, a new catalog
base (after compaction) or a model change stages a full snapshot instead,
copying vectors from both segments of the published one.

Lookups resolve own rows first, then unhidden base rows. A query searches the
own vectors and the base vectors (through the derived snapshot's own base
workspace, which skips hidden rows) and merges both top lists by cosine, then
id, which equals a search over the live rows. A full snapshot replaced by a
snapshot derived from it moves to an orphan list and is freed once no derived
snapshot or job references it. The vector and metadata budgets apply per
segment, so a derived snapshot may exceed them by its own rows, at most
max(4096, base/32).

Only stages that reuse nothing re-stage the descriptor and sweep the cache,
because reused rows never touch their cache entries. Status reports `reused`
(rows of the current or last stage that were not embedded), `derived_stages`
and `full_stages`; `entries` and `vector_bytes` cover both segments. Unit tests
derive two views from one base and check counts, that replaced and removed base
rows are neither hits nor metadata, that base metadata still resolves, and that
a later full stage reuses rows of both segments and frees them. See
[ADR 0030](../../adr/0030-m6-incremental-indexing.md).
