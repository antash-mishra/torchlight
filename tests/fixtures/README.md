# Fixtures

`tests/test_cli.py` creates temporary directory trees with hidden files/directories,
ignored build trees, a symlink cycle, non-UTF-8 filenames, embedded newlines,
unreadable directories, configuration files (roots, allowlisted hidden
directories, overlapping and missing roots) and isolated XDG directories.
Generating these byte paths at test time avoids source-control filename encoding
and checkout differences. Unit tests contain labeled Unicode, abbreviation, typo
and parent-context cases.

`window_app.c` (built as `build/test_window_app`) maps one X11 window per title
with a given WM_CLASS and prints the window ids; `tests/test_popup.py` uses it
to check open windows beneath their application on the isolated display.

Benchmark corpora are generated (`tests/bench/corpus.c`) or supplied as a
NUL-separated path list via `make bench BENCH_PATHS=FILE`; real lists may contain
private names and are never committed.
