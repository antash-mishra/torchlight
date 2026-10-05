# What is done and what comes next

M1, M2 and M3 have their planned features implemented. M3's review bugs are fixed
and covered by regression tests. Detailed evidence and remaining desktop checks
are in [M3 verification](m3-completion.md).
M3 Part 2 is implemented; the next sequence is M4, then M5, as recorded in [the plan](../PLAN.md).

| Milestone | In simple words | Status |
|---|---|---|
| M1 | Find files and folders by their names or paths, including short forms and typos. Use it from the terminal. | Implemented |
| M2 | Keep a background service running, remember the file list, and update it when files change. | Implemented |
| M3 | Show a keyboard popup; search apps, settings, files and folders; open or reveal the selected item. | Implemented; review fixes verified |
| M3 Part 2 | Improve name search, unfinished typos, abbreviations and ranking; test useful results among competing apps and files. | Implemented; held-out relevance and performance measured |
| M4 | Combine improved name search with optional local embeddings and vector retrieval for meaning-based matches. | Planned after M3 Part 2 |
| M5 | Use optional opening history to recommend personally useful files and apps higher, with privacy and history controls. | Planned after M4 |

M3 Part 2 now keeps names, generic names, keywords and folders distinct, finds
unfinished one-edit typos (including `proej`), and optimizes fuzzy alignment within
a fixed size bound. A mixed labeled fixture compares the previous engine and
BM25, with target families separated between tuning and held-out queries.
[Part 2 verification](m3-part2-completion.md) records relevance improvement and
the extra latency cost. Exact names/paths and allocation-free queries are preserved.
The small fixture establishes regressions, not universal best search quality.

M4 still needs model comparison, background creation of numeric representations
of paths and application metadata, fast vector matching, and combining those
matches with the improved name search. Ordinary name search must remain
available when the model is slow or unavailable. This milestone uses names,
paths and application metadata; reading document contents is a possible later
extension.

M5 still needs file/application ranking changes that learn from usage and tests
proving they help without hiding exact matches. Recording and clearing optional
history are already implemented; using it to personalize ranking is future work.

Separate remaining work is real fractional/multiple-monitor validation, a human
screen-reader session, and Wayland behavior. The automated tests cover small
X11 screens, integer GTK scales, theme/font overrides and AT-SPI accessibility.
The original 5 ms search target, rebuild delay and peak memory also remain
optimization work. Enabling the installed service and choosing a global shortcut
are user setup steps described in [desktop setup](desktop-setup.md).
