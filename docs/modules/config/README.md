# config

> **Status:** Implemented (M1/M2): XDG paths, configuration and canonical database identity
> **Source:** `src/core/config.c` · **Header:** `include/torchlight/config.h`
> **Tests:** `tests/unit/test_config.c`, `tests/test_cli.py`

## Behavior and ownership

`config_create(application, database_override, file_override, error_line, out)`
returns owned settings.

**Database.** A valid absolute `XDG_DATA_HOME` wins; otherwise `HOME` plus
`.local/share`. Default directories are created with mode 0700, and the default
state directory is canonicalized so the crawler can always exclude it. An
explicit database path is canonicalized without creating its parents. Existing
file symlinks resolve to the same identity; new files use their canonical parent.
This keeps daemon/offline singleton locks consistent across path spellings.

**File.** `file_override`, else `$XDG_CONFIG_HOME/<application>/config`, else
`~/.config/<application>/config` when it exists (a missing default file is not
an error). The format is deliberately small:

```text
# Lines starting with '#' and blank lines are ignored.
# Values starting with "~/" expand with HOME; keys may repeat in order.
root  = ~/Documents
root  = /data/projects
allow = ~/.config/nvim
```

Keys are `[a-z_]+`, values are nonempty and surrounding whitespace is trimmed.
There are no inline comments: a `#` after a value is part of the value.
A malformed line fails with TL_INVALID and its line number. The module stays
generic: it exposes entries in order (`config_entry_count`, `config_entry`), and
callers decide which keys exist. The CLI accepts `root` and `allow` and rejects
any other key. Getters borrow until destruction.

## Related

- [M1 completion ADR](../../adr/0008-m1-completion-channels-directories-config.md)
- [cli](../cli/README.md), [crawl](../crawl/README.md)
- Public headers document parameters, lifetimes and error contracts.
