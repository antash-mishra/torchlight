# What is done and what comes next

M1, M2 and M3 have their planned features implemented. M3's review bugs are fixed
and covered by regression tests. Detailed evidence and remaining desktop checks
are in [M3 verification](m3-completion.md).
M3 Part 2 and M4's functional opt-in semantic path are implemented. M4 acceptance
remains open. One new milestone, M6, executes next: worker separation, then
Frizbee SIMD search, then incremental indexing. M5 follows M6 and remaining M4
acceptance work, as recorded in [the plan](../PLAN.md).

| Milestone | In simple words | Status |
|---|---|---|
| M1 | Find files and folders by their names or paths, including short forms and typos. Use it from the terminal. | Implemented |
| M2 | Keep a background service running, remember the file list, and update it when files change. | Implemented |
| M3 | Show a keyboard popup; search apps, settings, files and folders; open or reveal the selected item. | Implemented; review fixes verified |
| M3 Part 2 | Improve name search, unfinished typos, abbreviations and ranking; test useful results among competing apps and files. | Implemented; held-out relevance and performance measured |
| M4 | Combine improved name search with optional local embeddings and vector retrieval for meaning-based matches. | Functional opt-in implementation; native Potion/cache/two-phase RRF tested; performance and broader relevance acceptance open |
| M5 | Use optional opening history to recommend personally useful files and apps higher, with privacy and history controls. | Planned after M6 and remaining M4 acceptance work |
| M6 | Separate workers, use Frizbee SIMD matching, then update indexes incrementally. | In progress: step 1 (search thread, request queues, persistence thread) implemented; steps 2 and 3 planned |

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

M5 still needs file/application ranking changes that learn from usage and tests
proving they help without hiding exact matches. Recording and clearing optional
history are already implemented; using it to personalize ranking is future work.

Separate remaining work is real fractional/multiple-monitor validation, a human
screen-reader session, and Wayland behavior. The automated tests cover small
X11 screens, integer GTK scales, theme/font overrides and AT-SPI accessibility.
M6 now owns the original 5 ms lexical search target, full-rebuild delay and peak
memory optimization. Frizbee is selected; implementation validation still checks
correctness, relevance and performance, without a library-selection phase. Worker
separation keeps the UI/IPC loop, search, indexing and history/persistence
independent, while catalog/history database writes remain serialized. See
[ADR 0026](adr/0026-search-workers-simd-and-incremental-indexing.md) and the
[M6 working plan](m6-plan.md) for the step-by-step flow and exit criteria.
Enabling the installed service and choosing a global shortcut
are user setup steps described in [desktop setup](desktop-setup.md).
