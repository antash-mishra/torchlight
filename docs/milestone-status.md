# What is done and what comes next

M1, M2 and M3 have their planned features implemented. M3's review bugs are fixed
and covered by regression tests. Detailed evidence and remaining desktop checks
are in [M3 verification](m3-completion.md).
M3 Part 2 and M4's functional opt-in semantic path are implemented. M4 acceptance
is deferred: semantic search is parked as an opt-in feature
([ADR 0032](adr/0032-park-semantic-search.md)). M6 (worker separation, then
Frizbee SIMD search, then incremental indexing) is implemented. M5 (personal
recommendations) is implemented, as recorded in [the plan](../PLAN.md).

| Milestone | In simple words | Status |
|---|---|---|
| M1 | Find files and folders by their names or paths, including short forms and typos. Use it from the terminal. | Implemented |
| M2 | Keep a background service running, remember the file list, and update it when files change. | Implemented |
| M3 | Show a keyboard popup; search apps, settings, files and folders; open or reveal the selected item. | Implemented; review fixes verified |
| M3 Part 2 | Improve name search, unfinished typos, abbreviations and ranking; test useful results among competing apps and files. | Implemented; held-out relevance and performance measured |
| M4 | Combine improved name search with optional local embeddings and vector retrieval for meaning-based matches. | Functional opt-in implementation; native Potion/cache/two-phase RRF tested; prefix-shortlist vector search cut 500k final p95 from 95 to 24 ms; parked as opt-in, 10 ms gate and broader relevance acceptance deferred |
| M5 | Use optional opening history to recommend personally useful files and apps higher, with privacy and history controls. | Implemented ([ADR 0033](adr/0033-m5-personal-ranking.md)): habitual files reach first place after 3.5 keystrokes instead of 9.9 at 500k; typing p95 stays near 5 ms with the summary full |
| M6 | Separate workers, use Frizbee SIMD matching, then update indexes incrementally. | Implemented: search thread and persistence owner; Frizbee scoring with typing p95 under 5 ms at 500k; scoped rescans and delta segments (lexical and semantic) with about 110 ms update lag at 500k |

M3 Part 2 now keeps names, generic names, keywords and folders distinct, finds
unfinished one-edit typos (including `proej`), and optimizes fuzzy alignment within
a fixed size bound. A mixed labeled fixture compares the previous engine and
BM25, with target families separated between tuning and held-out queries.
[Part 2 verification](m3-part2-completion.md) records relevance improvement and
the extra latency cost. Exact names/paths and allocation-free queries are preserved.
The small fixture establishes regressions, not universal best search quality.

M4 now runs native Potion with background versioned cache and coherent two-phase
RRF. [Model evaluation](m4-model-evaluation.md) records the English fixture,
trained C parity, int8/approximate retrieval and the Documents smoke check.
The 500k final latency target and broader model comparison remain open; the
[implementation report](m4-implementation.md) separates completed features from
those acceptance gates. File contents are not read.

M5 ranks the files and apps you open higher. Usage counts live in the search
thread's memory, all saving to disk happens on the separate persistence thread,
and only items you have opened are re-scored, so typing stays fast. Exact names
always stay first, clearing history removes the boosts at once, and with
history disabled ranking is unchanged. See
[ADR 0033](adr/0033-m5-personal-ranking.md) and the
[measurements](evaluation.md#m5-personal-ranking-2026-10-08).

Separate remaining work is real fractional/multiple-monitor validation, a human
screen-reader session, and Wayland behavior. The automated tests cover small
X11 screens, integer GTK scales, theme/font overrides and AT-SPI accessibility.
M6 met its targets on the reference machine under background load: typing p95
under 5 ms at 500k synthetic paths with unchanged held-out recall, and a
100-file update published in about 110 ms instead of 4 s, without a second
engine in memory. With a model, hybrid search serves that update in 277 ms at
500k through a derived semantic snapshot, and RSS grows 0.4% instead of 39%.
Whole-query p95 at 500k sits near 5 to 6 ms. The M4 final-phase gate (p95
below 10 ms) stays open: the exhaustive int8 vector scan alone took about
68 ms at 500k (see the [M6 plan](m6-plan.md) measurements). Since then, a
two-pass vector search shortlists rows by the first half of each embedding and
rescores 8000 exactly. It keeps 99.6% of the exhaustive top ten at 500k (the
first result always) and brings the final phase to 24 ms p95 at 500k and just
under 10 ms at 50k ([ADR 0031](adr/0031-m4-prefix-shortlist-vector-search.md)).
Reaching 10 ms at 500k would need the semantic search to start alongside the
lexical search instead of after it. That work is deferred: semantic search is
parked as an opt-in feature until real use shows queries only it answers
([ADR 0032](adr/0032-park-semantic-search.md)). A phase-ordering
race from step 1, where a fast semantic final could overtake a large lexical
frame, was found and fixed with a deterministic regression test. See ADRs
[0028](adr/0028-m6-search-thread-and-persistence-owner.md),
[0029](adr/0029-m6-frizbee-scoring-and-candidate-volume.md) and
[0030](adr/0030-m6-incremental-indexing.md).
Enabling the installed service and choosing a global shortcut
are user setup steps described in [desktop setup](desktop-setup.md).
