# Semantic service

> **Status:** Implemented M4 opt-in two-phase execution; large-catalog latency
> acceptance remains open. Enable with `torchlightd --model PATH.tlm`.

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

Schema v4 caches exact prepared text under full model/transform emb_gen. Changing
a path or application revision produces different metadata/text or session ids;
staging is abandoned when its source generations change. Unchanged text reuses
cached floats; only misses invoke inference. Complete staging validates before
atomic cache activation and resident publication. Restart reconstructs from
validated cache; model replacements use checked new tables and preserve old
query snapshots. Bad replacement files keep the last valid snapshot, or lexical
fallback when no valid snapshot exists. Full metadata/vector rebuild remains a
baseline; startup and update costs are measured, not claimed incremental.

[Lifecycle tests](../../../tests/test_semantic.py) use independent analytic
models and cover restart/cache, rename/delete, application removal, replacement,
missing models, blocked-writer deadlines, injected running cancellation,
queue saturation and write-half-close completion. Trained-model
and large-catalog measurements are in [M4 evaluation](../../m4-model-evaluation.md).
