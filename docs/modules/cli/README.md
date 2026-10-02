# cli

> **Status:** Implemented (M1 local index/query CLI)
> **Source:** `src/bin/torchlight.c` · **Header:** `—`
> **Tests:** `tests/test_cli.py`

## Behavior and ownership

The executable wires config, crawl, store and lexical modules. Syntax:

```sh
./build/torchlight index [--db PATH] [--config PATH] [ROOT...]
./build/torchlight query [--db PATH] [--limit 1..1000] [--null] QUERY
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
Unknown configuration keys and malformed lines are reported with their name or
line number.

The default database uses XDG data paths; custom parents must already exist.
Queries load and seal the engine and create scratch before searching, so CLI
startup is distinct from warm-engine latency (the M2 daemon keeps it resident).
Empty queries return registered roots; limits default to 10. Use `--` before a
dash-prefixed positional argument.

Plain output is a safe UTF-8 display; `--null` writes original paths followed by
NUL, preserving invalid bytes/newlines. Diagnostics go to stderr. Invalid syntax
returns 2; operation failures return 1. Integration tests use temporary byte-path
fixtures, isolated XDG directories and sanitizer-enabled binaries. JSON output,
daemon IPC, status fields, resolve/open and two-phase behavior arrive later.

## Related

- [Implementation increment ADR](../../adr/0006-m1-prefix-subsequence-baseline.md)
- [M1 completion ADR](../../adr/0008-m1-completion-channels-directories-config.md)
- [Evaluation](../../evaluation.md)
- Public headers document parameters, lifetimes and error contracts.
