# 0023: Native Potion and bounded two-phase search

## Status

Accepted for opt-in implementation, 2026-10-05. Performance/model-comparison
acceptance remains explicitly open.

## Context

The user chose English-first evaluation and requested a trained model tested on
small launcher data. M4's float/RRF foundation has no runtime backend. Static
Potion retrieval can run through existing C dependencies; contextual Granite R2
and BGE would need an additional transformer runtime. Publisher benchmark scores
alone cannot choose a filename/application model.

## Decision

Pin Potion retrieval 32M and compare 512/256/128 dimensions, lexical, semantic,
RRF and calibrated rank/cosine combination on separated target families. Use
256 dimensions and RRF k=60 with cosine 0.2 based on tuning rows, then report
held-out rows without selecting on them. Export checked static tables and verify
native WordPiece/pooling against Model2Vec. Keep Python only in offline evaluation.

Use exhaustive int8 cosine to fit the vector payload budget, retaining float as
the independent correctness reference. Do not deploy approximate pruning without
measured recall. Version model weights, tokenizer, prepared text, projection and
int8 format together. Schema v4 supports staged descriptors and reusable text
cache; activation prunes stale rows atomically. Background batches contain eight
rows; interactive work has priority between them.

Copy coherent file/desktop metadata into owned semantic snapshots, rather than
retaining expensive lexical workspaces or holding the desktop mutex through
inference. Submission checks both source generations. Bound snapshots/jobs/output,
use eventfd notification and coordinator deadlines; cancellation keeps running
work's memory valid while discarding obsolete output. Only explicit `--model`
enables semantics; lexical behavior remains available on every failure.

Clip semantic context at indexed roots. A native integration evaluation exposed
unrelated evidence from temporary host parents above the root; this structural
scope correction is regression-tested and evaluated without changing thresholds.

## Consequences

Small trained-model relevance improves and native parity passes. Static inference
sacrifices contextual word order. Int8 changes some neighbors; small-fixture recall
is measured, not a general bound. Exhaustive 500k latency misses the 10 ms target.
Full staging and cache validation have startup/update and peak-memory costs.
Contextual competitors, a broader corpus and measured approximate retrieval are
still required before claiming full M4 acceptance or best model selection.
