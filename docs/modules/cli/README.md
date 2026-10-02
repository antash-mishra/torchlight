# cli

> **Status:** Implemented (M1/M2): offline indexing/local queries and resident IPC client
> **Source:** `src/bin/torchlight.c` · **Header:** `—`
> **Tests:** `tests/test_cli.py`, `tests/test_daemon.py`

## Behavior and ownership

The executable wires config, crawl, store and lexical modules. Syntax:

```sh
./build/torchlight index [--db PATH] [--config PATH] [ROOT...]
./build/torchlight query [--db PATH] [--limit 1..1000] [--null] QUERY
./build/torchlight query [--socket PATH] [--json | --null] [--limit N] QUERY
./build/torchlight status|reconcile|history-clear [--socket PATH]
./build/torchlight resolve [--socket PATH] [--json | --null] FILE_ID
./build/torchlight record [--socket PATH] FILE_ID EVENT_ID [SEARCH_ID]
```

**Index without ROOT** syncs the catalog to the [configuration](../config/README.md):
configured `root`s (or `$HOME` when none) are canonicalized, repeated or covered
roots are scanned once, `allow` entries re-include hidden/ignored directories,
and registered roots that are no longer configured are forgotten together with
their entries. **Index with ROOT...** refreshes just those roots and forgets
nothing; a root indexed only this way is forgotten by the next configuration
sync unless it is configured.

Everything happens in one transaction. Unreadable paths below a root keep their
saved entries, and an unavailable root keeps all of its entries (a warning names
it). The command fails and changes nothing when no root at all could be scanned.
If any configured root cannot be canonicalized, unmatched registered roots are
kept and their removal is deferred until a later sync can resolve every root.
This protects aliases (trailing slashes, dot components and symlinks) whose
canonical identity is unavailable; kept scopes also survive pruning by a scanned
ancestor. A diagnostic reports deferred removals. Unrelated root removals may
therefore wait until the unavailable root returns.

The save callback records its status separately from the crawl result. Storage
errors fail the command and roll back the whole transaction, including writes
from roots scanned earlier, without advancing `catalog_gen`. Only filesystem
failures are treated as unavailable roots.
Unknown configuration keys and malformed lines are reported with their name or
line number.

The default database uses XDG data paths; custom parents must already exist.
Offline indexing takes the daemon's canonical database lock and refuses to race
a running service. Explicit `query --db` loads/seals an engine and creates scratch;
queries without `--db` use the resident daemon. `--socket` overrides the XDG
runtime socket. `--db` and socket/JSON modes cannot be combined.
Empty queries return registered roots; limits default to 10. Use `--` before a
dash-prefixed positional argument.

Plain output is a safe UTF-8 display; `--null` writes original paths followed by
NUL, preserving invalid bytes/newlines. Diagnostics go to stderr. Invalid syntax
returns 2; operation failures return 1. Integration tests use temporary byte-path
fixtures, isolated XDG directories and sanitizer-enabled binaries. JSON output
preserves the terminal envelope with ids/status and exact paths. Resolve validates
current ids; record enqueues history without launching an application. The M2
client expects one `final` response; semantic two-phase execution arrives in M4.
Regressions cover unavailable root aliases and ancestor pruning, deferred removal
after recovery, and injected SQLite failures before/after a healthy root scan.

## Related

- [Implementation increment ADR](../../adr/0006-m1-prefix-subsequence-baseline.md)
- [M1 completion ADR](../../adr/0008-m1-completion-channels-directories-config.md)
- [Refresh failure safety ADR](../../adr/0009-unresolved-roots-and-refresh-failures.md)
- [Evaluation](../../evaluation.md)
- Public headers document parameters, lifetimes and error contracts.
