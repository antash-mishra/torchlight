# config

> **Status:** Partially implemented: XDG defaults and database override
> **Source:** `src/core/config.c` · **Header:** `include/torchlight/config.h`
> **Tests:** `tests/unit/test_config.c`

## Behavior and ownership

`config_create(application, database_override, out)` returns owned data-path
settings. Valid absolute `XDG_DATA_HOME` wins; otherwise use absolute `HOME` plus
`.local/share`. Default directories are created with requested mode 0700, and
the default state directory is canonicalized for crawler exclusion. An explicit
DB path is copied without creating its parents. Getters borrow until destruction.

The resolver is generic: the executable supplies the application name. Config
file parsing, hidden allowlists, multiple-root deduplication and other settings
remain planned. Unit tests cover invalid application components and overrides;
CLI integration tests exercise XDG defaults and state exclusion.

## Related

- [Implementation increment ADR](../../adr/0006-m1-prefix-subsequence-baseline.md)
- [Evaluation](../../evaluation.md)
- Public headers document parameters, lifetimes and error contracts.
