# Torchlight

A Linux application/settings/file launcher written in C17. M1 provides a local index/query
CLI with a SQLite catalog and Unicode-aware matching: prefixes, initials,
abbreviations (subsequences), one-edit typos, partial-word trigram overlap and
parent-folder context. The M1/M2 foundation was accepted at roughly 6 ms
p95 latency at 500k paths; M6 brought typing p95 under the original 5 ms
target. See [readiness](docs/m3-readiness.md) and [evaluation](docs/evaluation.md).

M2 provides a [resident daemon](docs/modules/daemon/README.md), bounded Unix-socket
IPC, asynchronous catalog/history writing, live inotify updates, reconciliation,
file-id resolution and status. Startup serves the saved catalog before scanning.
M3 adds installed application/settings search, the GTK4 popup and service integration, following the
[GUI design](docs/m3-gui-design.md) and [interactive preview](docs/m3-gui-preview.html).
Full index rebuilds were the M2/M3 update path; M6 replaced them for routine changes.

M4's [functional opt-in semantic path](docs/m4-implementation.md) provides native
Potion, float/int8 retrieval, background embedding caches and two-phase daemon
queries with exact-priority RRF. Large-catalog latency and broader relevance
acceptance remain open; personalization is planned for M5.

**M6** (implemented) makes search responsive and updates incremental:

- Search runs on its own thread; a newer keystroke supersedes queued and
  running queries, and SQLite writes have their own persistence thread.
- [Frizbee](third_party/README.md) SIMD Smith-Waterman scores fuzzy matches,
  with per-word evidence cached across keystrokes. At 500k synthetic paths
  typing p95 is 3.9 to 5.0 ms (from 7.4 ms) with unchanged recall.
- Filesystem changes rescan only the affected folders and publish a small
  delta over the shared index (lexical and semantic), so 100 touched files
  appear in about 110 ms at 500k instead of 4 s, without doubling memory.

On a real 213k-path home directory the resident daemon answers typing in
0.4 ms p50 and 1.9 ms p95 (optimized build). The hybrid final phase is still
dominated by a full vector scan (about 30 to 50 ms there, 95 ms p95 at 500k),
so that M4 gate stays open. M5 (personalization) follows. See
[the plan](PLAN.md), the [M6 plan and measurements](docs/m6-plan.md) and
[milestone status](docs/milestone-status.md).

## Build and use

On Debian/Ubuntu/Mint, install `build-essential`, `pkg-config`, `libsqlite3-dev`,
`libutf8proc-dev`, `libgtk-4-dev` (GTK4 ≥ 4.14), `libglib2.0-dev`,
`libx11-dev`, `clang-format`, `clang-tidy`, and `cppcheck`, plus a Rust
toolchain (`cargo`, Rust 1.89 or newer, e.g. from rustup). SQLite, utf8proc and
Frizbee were approved for this implementation. Frizbee v0.13.0, the SIMD fuzzy
matcher, is vendored in `third_party/frizbee`; `make` compiles it offline into
a static library once (about four minutes), see
[third_party/README.md](third_party/README.md).

```sh
make
./build/torchlightd                          # keep running in this terminal
```

In another terminal:

```sh
./build/torchlight query "prjnts"             # abbreviation -> projectNotes.md
./build/torchlight query "raedme"             # one-edit typo -> README.md
./build/torchlight query --limit 20 "work notes"
./build/torchlight query --null "report"      # exact paths, NUL-separated
./build/torchlight query --json "report"      # ids, exact paths and status
./build/torchlight status
./build/torchlight reconcile                 # request a background refresh
./build/torchlight history-clear             # clear persisted search/open history
```

`make` builds unoptimized (`-O0 -g3`) binaries for development. For the
lowest latency run the optimized daemon instead (about half the tail latency):

```sh
make build/torchlightd-release
./build/torchlightd-release
```

The default socket is `$XDG_RUNTIME_DIR/torchlight.sock`; `--socket PATH` selects
another socket on both commands. SIGINT/SIGTERM shut down cleanly. The daemon
accepts `--no-history`, `--history-days N` (default 30), `--rescan-ms N` (default
30000), `--watch-capacity N`, `--max-entries N` and `--max-path-bytes N`.
`resolve FILE_ID` retrieves a current path; `record FILE_ID EVENT_ID [SEARCH_ID]`
queues an accepted open record. The GTK popup opens/reveals files and activates installed desktop entries.

When the daemon is stopped, offline commands remain available:

```sh
./build/torchlight index                     # sync the configured roots
./build/torchlight index "$HOME/Documents"   # or refresh specific roots
./build/torchlight query --db "$HOME/.local/share/torchlight/catalog.db" "prjnts"
```

Offline indexing and the daemon share a database lock. Explicit `query --db`
uses a local engine; queries without `--db` use the resident daemon.

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

When a configured root cannot be resolved, removal of unmatched saved roots is
deferred until a later sync resolves every root, preserving unavailable aliases.
Accessible roots still refresh. Storage write failures roll back the entire run
and return an error.

Multiword queries need every word to match the file name or one of its folder
names (`work notes`); a word containing `/` may match across the whole path.
Use `--` before a query beginning with a dash. An empty query lists indexed
roots. Plain output is a UTF-8 display with invalid bytes replaced and controls
escaped; use `--null` when passing exact paths to another program. The local CLI
rebuilds the engine on each query, so it is slower than the warm engine the
benchmark measures; the M2 daemon keeps it resident.

## Desktop launcher

```sh
./build/torchlight-gtk --toggle
./build/torchlight query --json "resolution"
```

Enter activates the selected application/settings entry or opens the resolved
file. Ctrl+Enter reveals, arrows select and Escape closes. Results include app
icons and native Cinnamon settings panels. Install and configure the systemd user
service and your desktop shortcut using [desktop setup](docs/desktop-setup.md).
`make install` supplies the executables, desktop entry and user unit.
See [M3 verification](docs/m3-completion.md) for tested platform coverage.

## Checks

```sh
make test    # ASan + UBSan + leak checks, unit, CLI and daemon integration tests
make lint    # clang-tidy and cppcheck, warnings fail the build
make format
make bench   # release engine: latency and labeled ranking quality, 50k/500k paths
make bench-vector # M4 synthetic float reference and fusion cost, 50k/500k vectors
make test-ui # Cinnamon/X11 keyboard acceptance (requires xdotool)
make bench-daemon # release daemon: startup, IPC, indexing load, update lag and RSS
```

Measurements and limitations are recorded in [evaluation](docs/evaluation.md).
The engine benchmark excludes SQLite loading and CLI startup; the daemon
benchmark measures startup separately. To add a real corpus to the engine benchmark:

```sh
./scripts/make_corpus.sh /usr /tmp/usr.paths   # NUL-separated path list; keep it private
make bench BENCH_PATHS=/tmp/usr.paths
```

A local `machine.mk` (ignored) can set `DEPS_PREFIX`, `CLANG_TIDY`, and
`CPPCHECK` for an unpacked development environment; normal builds use
`pkg-config`. This workspace's validation packages were unpacked under
`/tmp/torchlight-deps` because system installation required a sudo password.

### Optional local semantic search

M4's provisional English backend is native Potion retrieval 32M (256d).
Export the pinned model using `scripts/export_potion.py`, then pass
`torchlightd --model /absolute/path/potion-256.tlm`. The daemon embeds names,
nearby folders and app metadata in the background; contents are not read.
Lexical results appear first, followed by a semantic final or bounded fallback.
Model loading is local and does not require Python in the launcher. Without
`--model` the daemon runs fuzzy (lexical) search only and answers each query in
a single frame; with it, every query also pays the vector scan for its final
phase. To switch an installed user service to fuzzy-only, override its
`ExecStart` without `--model` (`systemctl --user edit torchlightd`) and restart
it.
[Model research, setup and measured limits](docs/m4-model-evaluation.md) explain
the optional evaluation tools and remaining 500k performance acceptance.
