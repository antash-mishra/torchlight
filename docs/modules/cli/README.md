# cli

> **Status:** M1 local index/query CLI implemented
> **Source:** `src/bin/torchlight.c` · **Header:** `—`
> **Tests:** `tests/test_cli.py`

## Behavior and ownership

The executable wires config, crawl, store and lexical modules. Syntax:

```sh
./build/torchlight index [--db PATH] ROOT
./build/torchlight query [--db PATH] [--limit 1..1000] [--null] QUERY
```

Index one root per command, preserving ids and pruning only successful scopes.
Unreadable paths below the root keep their saved entries; their count is
reported on stderr and the command still succeeds.
The default database uses XDG data paths. Custom parents must already exist.
Queries load/seal the engine and create scratch before searching, so CLI startup
is distinct from warm-engine latency. Empty queries return registered roots;
limits default to 10. Use `--` before a dash-prefixed positional argument.

Plain output is a safe UTF-8 display; `--null` writes original paths followed by
NUL, preserving invalid bytes/newlines. Diagnostics go to stderr. Invalid syntax
returns 2; operation failures return 1. Integration tests use temporary byte-path
fixtures and sanitizer-enabled binaries. JSON output, daemon IPC, status fields,
resolve/open and two-phase behavior arrive in subsequent milestones.

## Related

- [Implementation increment ADR](../../adr/0006-m1-prefix-subsequence-baseline.md)
- [Evaluation](../../evaluation.md)
- Public headers document parameters, lifetimes and error contracts.
