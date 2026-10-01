# Fixtures

`tests/test_cli.py` creates temporary directory trees with hidden files/directories,
ignored build trees, a symlink cycle, non-UTF-8 filenames and embedded newlines.
Generating these byte paths at test time avoids source-control filename encoding
and checkout differences. Unit tests contain labeled Unicode and abbreviation pairs.
