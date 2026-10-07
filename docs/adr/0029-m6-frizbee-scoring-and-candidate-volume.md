# 0029. M6 step 2: Frizbee scoring and candidate volume

- **Status:** Accepted; implemented
- **Date:** 2026-10-07

## Context

ADR 0026 selects Frizbee as the production fuzzy matcher, and the
[M6 plan](../m6-plan.md) pairs it with candidate-volume work because a faster
scorer alone was predicted to cap near 1.4x. Before this step the 500k
synthetic typing p95 was 7.36 ms (step-1 run). A per-phase profile of the
typing workload attributed about 47% of query time to scoring batches, 24% to
a full directory-context pass that ran for almost every query, 13% to prefix
hit fan-out and 13% to trigram lookup. Multiword typing re-derived the channel
evidence of every unchanged word on each keystroke.

Frizbee's C binding (v0.13.0) takes UTF-8 without normalization, scores one
string at a time with `frizbee_match_one` without allocating, but allocates when
a matcher is compiled, and its own fallback for texts over 1024 bytes allocates
per call. A matcher is not thread-safe.

## Decision

**Build.** Frizbee v0.13.0 is vendored verbatim in `third_party/frizbee`.
Two Torchlight-owned manifests in `third_party/frizbee-build` compile its core
and C binding as `libfrizbee.a` with `cargo build --release --offline --locked`
from a Makefile rule, with `panic = "abort"`. The core manifest omits the
optional `serde` dependency, so the build needs no crate registry and works with
an empty Cargo home. This replaces the planned `cargo cbuild`: cargo-c adds a
second tool for installing headers we use in place. Rust 1.89+ becomes a
build-time dependency. Frizbee picks AVX-512, AVX2, SSE4.1 or scalar kernels
at run time, so no target-CPU flag is needed.

**Scoring.** `fuzzy_matcher_create/score/destroy` wrap one compiled word.
Frizbee's affine Smith-Waterman weights are derived from the portable scorer
(match 40 = match + consecutive, gap open 25, gap extend 1, word-start bonus
32, no case or exact bonus) and scores map as `232 + frizbee_score`. A
consecutive run from the first symbol and any gap up to 64 then score exactly
as before, so `fuzzy_score_bound`, the score bands and their static asserts are
unchanged. Remaining differences: no leading-gap penalty and no bonus at
letter/digit changes. ASCII basenames and directory names are scored from
their raw bytes (case kept, so camelCase bonuses fire); other text is encoded
from the normalized symbols, writing a letter at a tokenizer boundary after a
lowercase letter in upper case. Non-ASCII membership is still decided by the
normalized symbols. Opaque (invalid UTF-8) symbols, words over 64 symbols, texts
over 1024 bytes and words containing `/` keep the portable scorer.

**Allocation exception.** Compiling a matcher is the one allocation on the
query path: at most once per word and scoring participant, never per entry.
`parallel_run` callbacks now receive their participant index so each worker
uses its own matcher.

**Candidate volume.**
- Word evidence caches (channel hits, directory scores, matchers) are keyed by
  the word's symbols and kept across queries with LRU replacement. Typing
  `notes rep` after `notes re` recomputes nothing for `notes`, and repeating a
  query allocates nothing. A failed or cancelled query drops the caches.
- The directory pass reads contiguous parent and name-mask arrays and only
  scores directories whose mask holds every symbol of the word.
- Trigram lookup scans only the `present - needed + 1` shortest posting lists
  to find candidates, then counts the longer lists by binary search (or one
  filtered pass when candidates are many). Counts of reported slots are exact.

**Not adopted.** The time-boxed list-mode spike was skipped: the channel
pipeline reached the target with recall unchanged, and replacing channel
retrieval would re-open recall contracts. Capped prefix postings and heap
gating were not needed for the target; prefix fan-out remains about 7-14% of
query time in the profile, so they stay follow-up work for whole-query tails.

## Alternatives considered

- A Rust shim with a caller-supplied arena would make matcher compilation
  allocation-free, but adds Torchlight-owned Rust and an allocator; the plan
  accepted a bounded exception instead.
- Frizbee's default weights use a different scale (and u8 kernels for longer
  words). Mirroring the portable weights keeps run and gap scores identical,
  so both scorers share one bound and the score bands need no new constants.
- Encoding every basename from symbols costs about 60 ns per call; the raw-byte
  path for ASCII names needs only a 1-bit-per-entry bitmap.

## Consequences

Typing p95 at 500k fell from 7.36 ms to between 3.9 and 5.0 ms across runs on
the loaded reference machine (load average 3.5 to 5), and whole-query p95 from
7.33 ms to between 5.0 and 5.6 ms, with held-out recall@10 unchanged at 0.539
and MRR 0.424 → 0.423 (tolerance ±0.01). The
[end-of-M6 run](../../tests/bench/results/2026-10-07-m6-lexical.txt) recorded
4.96 ms typing and 5.64 ms whole-query p95. On a real 213k-path home corpus
typing p95 fell from 4.58 to 2.65 ms.
Every binary links the Rust static library; a clean build compiles Frizbee in
about four minutes once. Workspaces hold up to 16 compiled matchers (about
50 KB each for typical words). The allocation test now interposes the aligned
allocators too and asserts the per-word bound and zero allocations on repeats.
