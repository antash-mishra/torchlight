# What is done and what comes next

M1, M2 and M3 have their planned features implemented. M3's review bugs are fixed
and covered by regression tests. Detailed evidence and remaining desktop checks
are in [M3 verification](m3-completion.md).
The next sequence is M3 Part 2, M4, then M5, as recorded in [the plan](../PLAN.md).

| Milestone | In simple words | Status |
|---|---|---|
| M1 | Find files and folders by their names or paths, including short forms and typos. Use it from the terminal. | Implemented |
| M2 | Keep a background service running, remember the file list, and update it when files change. | Implemented |
| M3 | Show a keyboard popup; search apps, settings, files and folders; open or reveal the selected item. | Implemented; review fixes verified |
| M3 Part 2 | Improve name search, unfinished typos, abbreviations and ranking; test useful results among competing apps and files. | Planned next; Chrome ranking/tie fixes implemented |
| M4 | Combine improved name search with optional local embeddings and vector retrieval for meaning-based matches. | Planned after M3 Part 2 |
| M5 | Use optional opening history to recommend personally useful files and apps higher, with privacy and history controls. | Planned after M4 |

M3 Part 2 still needs explicit name/keyword/folder scoring, incomplete-word typo
retrieval, better fuzzy scoring and a real labeled relevance evaluation. The
existing application-name weighting fixes the Chrome case; it does not finish
the whole milestone. [The search quality review](search-quality.md) explains
the approach and comparisons. Exact names and paths must keep their priority.

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
