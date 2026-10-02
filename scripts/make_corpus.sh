#!/bin/sh
# Write a NUL-separated list of every path under ROOT (one filesystem) for
# `make bench BENCH_PATHS=FILE`. Real paths make representative benchmark
# corpora; the list may contain private names, so keep it out of the repo.
set -eu
if [ "$#" -ne 2 ]; then
    echo "usage: $0 ROOT OUTPUT" >&2
    exit 2
fi
find "$1" -xdev -print0 2>/dev/null > "$2" || true
test -s "$2"
