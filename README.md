# Torchlight

A Linux filename/path launcher written in C17. The first M1 increment provides
a local index/query CLI with a SQLite catalog and Unicode-aware prefix and
subsequence matching. M1 is **in progress**; trigram/typo retrieval and the latency
acceptance gate remain open. See [PLAN.md](PLAN.md) and [docs](docs/README.md).

## Build and use

On Debian/Ubuntu/Mint, install `build-essential`, `pkg-config`, `libsqlite3-dev`,
`libutf8proc-dev`, `clang-format`, `clang-tidy`, and `cppcheck`. SQLite and utf8proc
were approved for this implementation. No third-party source is vendored.

```sh
make
./build/torchlight index "$HOME/Documents"
./build/torchlight query "prjnts"
./build/torchlight query --limit 20 "work notes"
./build/torchlight query --null "report"  # exact paths, NUL-separated
```

The default catalog is `$XDG_DATA_HOME/torchlight/catalog.db`, or
`$HOME/.local/share/torchlight/catalog.db`. Index one explicit root per invocation;
repeat for additional roots. `--db /absolute/path/catalog.db` selects another
catalog whose parent directory already exists. Use `--` before a query beginning
with a dash. An empty query lists indexed roots.

Indexing preserves IDs for unchanged paths and prunes absent/excluded entries
only after a completely successful scan. Unreadable scopes roll back the root's
entire refresh. Symlinks are indexed without following directory symlinks.
Hidden directories and common build/cache trees are skipped; hidden files remain
searchable. Explicit hidden roots can be indexed directly. Hidden allowlists,
config files, watching, history and daemon IPC are later work.

Plain output is a UTF-8 display with invalid bytes replaced and controls escaped.
Use `--null` when passing exact paths to another program. Search never changes
stored path bytes. The local CLI reloads/builds the engine on each query; it is
not representative of warm daemon latency.

## Checks

```sh
make test    # ASan + UBSan + leak checks, unit and CLI integration tests
make lint    # clang-tidy and cppcheck, warnings fail the build
make format
make bench   # release engine, synthetic 50k and 500k paths
```

Measurements and limitations are recorded in [evaluation](docs/evaluation.md).
The benchmark excludes SQLite loading and CLI startup. The latency target is
not met yet. A local `machine.mk` (ignored) can set `DEPS_PREFIX`, `CLANG_TIDY`,
and `CPPCHECK` for an unpacked development environment; normal builds use
`pkg-config`. This workspace's validation packages were unpacked under
`/tmp/torchlight-deps` because system installation required a sudo password.
