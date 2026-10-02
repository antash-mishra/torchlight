# Torchlight

A Linux filename/path launcher written in C17. M1 provides a local index/query
CLI with a SQLite catalog and Unicode-aware matching: prefixes, initials,
abbreviations (subsequences), one-edit typos, partial-word trigram overlap and
parent-folder context. The 5 ms p95 latency target is met at 50k paths but not
yet at 500k; see [evaluation](docs/evaluation.md), [PLAN.md](PLAN.md) and
[docs](docs/README.md).

## Build and use

On Debian/Ubuntu/Mint, install `build-essential`, `pkg-config`, `libsqlite3-dev`,
`libutf8proc-dev`, `clang-format`, `clang-tidy`, and `cppcheck`. SQLite and utf8proc
were approved for this implementation. No third-party source is vendored.

```sh
make
./build/torchlight index                      # sync the configured roots ($HOME by default)
./build/torchlight index "$HOME/Documents"    # or refresh specific roots
./build/torchlight query "prjnts"             # abbreviation -> projectNotes.md
./build/torchlight query "raedme"             # one-edit typo -> README.md
./build/torchlight query --limit 20 "work notes"
./build/torchlight query --null "report"      # exact paths, NUL-separated
```

The catalog is `$XDG_DATA_HOME/torchlight/catalog.db` (or
`~/.local/share/torchlight/catalog.db`); `--db PATH` selects another one whose
parent directory exists. The optional configuration file is
`$XDG_CONFIG_HOME/torchlight/config` (or `~/.config/torchlight/config`;
`--config PATH` overrides it):

```text
# Roots to index (default: $HOME). "~/" expands to $HOME.
root = ~/Documents
root = ~/projects
# Hidden or ignored directories to index anyway.
allow = ~/.config/nvim
```

`index` without roots syncs the catalog to this file in one transaction:
overlapping roots are scanned once, unavailable roots and unreadable folders keep
their saved entries, and roots removed from the file are forgotten (including
roots that were only ever indexed from the command line). Hidden directories and
`node_modules`/`target`/`build`/`__pycache__` are skipped unless allowlisted;
hidden files remain searchable; symlinks are indexed without following
directory symlinks. Torchlight's own state directory is never indexed.

Multiword queries need every word to match the file name or one of its folder
names (`work notes`); a word containing `/` may match across the whole path.
Use `--` before a query beginning with a dash. An empty query lists indexed
roots. Plain output is a UTF-8 display with invalid bytes replaced and controls
escaped; use `--null` when passing exact paths to another program. The local CLI
rebuilds the engine on each query, so it is slower than the warm engine the
benchmark measures; the M2 daemon keeps it resident.

## Checks

```sh
make test    # ASan + UBSan + leak checks, unit and CLI integration tests
make lint    # clang-tidy and cppcheck, warnings fail the build
make format
make bench   # release engine: latency and labeled ranking quality, 50k/500k paths
```

Measurements and limitations are recorded in [evaluation](docs/evaluation.md).
The benchmark excludes SQLite loading and CLI startup. To add a real corpus:

```sh
./scripts/make_corpus.sh /usr /tmp/usr.paths   # NUL-separated path list; keep it private
make bench BENCH_PATHS=/tmp/usr.paths
```

A local `machine.mk` (ignored) can set `DEPS_PREFIX`, `CLANG_TIDY`, and
`CPPCHECK` for an unpacked development environment; normal builds use
`pkg-config`. This workspace's validation packages were unpacked under
`/tmp/torchlight-deps` because system installation required a sudo password.
