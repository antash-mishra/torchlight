# search_actions

> **Status:** Implemented (M9 plus website recognition; ADR 0069); [validation](../../m9-completion.md).
> **Source:** src/service/search_actions.c, src/service/web_address.c
> **Header:** include/torchlight/search_actions.h
> **Tests:** tests/unit/test_search_actions.c, tests/test_search_actions.py

This module contains pure query-mode, feature-configuration and browser-search
policy. The popup recognizes an ASCII case-insensitive first token followed by
ASCII whitespace: `web`, `install`, or `local`. It trims surrounding whitespace
once and preserves the remaining punctuation and internal spaces. Bare prefix
words remain literal local queries. The CLI's existing query command remains
literal; explicit CLI commands select a mode without parsing popup prefixes.

The daemon owns the settings. Web actions carry a built-in provider and query,
never an arbitrary URI. The client constructs a fixed HTTPS endpoint and
percent-encodes each non-unreserved UTF-8 byte into its single query parameter.
Typing performs no browser, network or subprocess operation. The one Web action
occupies a separate protocol array and a pinned popup slot. The address helper
validates HTTP(S) hosts, ports and escapes and recognizes a conservative set of
bare domain suffixes. It rejects paths, credentials, unsafe schemes and common
filename suffixes without probing a host or file. UTF-8 path bytes are encoded;
internationalized hosts can use their ASCII/punycode form. The desktop browser
identifier helper rejects paths and command fragments. See
[ADR 0069](../../adr/0069-visible-web-action-and-browser-preferences.md).

Configuration defaults are web enabled, DuckDuckGo, and automatic optional
software discovery. Google is the other supported engine. Duplicate feature
keys, invalid values and unknown keys fail configuration validation. File-root
keys may still repeat. No state, ownership, I/O or heap allocation is needed by
the pure helpers. See [ADR 0067](../../adr/0067-web-search-and-software-discovery.md).
