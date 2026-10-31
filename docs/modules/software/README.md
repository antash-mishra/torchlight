# software

> **Status:** Implemented (M9), including ordinary-name suggestions (ADR 0070); [validation](../../m9-completion.md).
> **Source:** src/index/software.c, src/index/software_suggest.c
> **Header:** include/torchlight/software.h
> **Tests:** tests/unit/test_search_actions.c, tests/test_search_actions.py

Available software has its own immutable lexical index. Each Flatpak variant
is identified by installation (`user`, `system`, or `system:NAME`), remote,
application ID, architecture and branch. Comparisons always use every field,
including when a hash is used as a lookup aid. Display names are not identities.
Two sources for an app, or two apps with the same name, remain distinct.

The builder copies UTF-8 names and descriptions and computes a revision from
identity, source revision, metadata and installed state. It sorts identities,
rejects duplicates, then creates a lexical engine. Its internal search key is
the app name; that key is never exposed as a filesystem path. App IDs supply
generic-name evidence and descriptions supply keyword evidence. Installed state
comes from an exact installation/origin/full-ref match.

Bounds are 50,000 entries, 32 MiB of source text, 255-byte names, 1,023-byte
descriptions and the existing 256-byte query/1,000-result limits. The reader owns
its lexical scratch and borrows the sealed index. Search and resolution perform
no filesystem, SQL, subprocess or network work. Like the existing lexical
engine, new words may compile bounded Frizbee matchers; warm repeated queries
allocate nothing. Readers must be destroyed before their index.

`software_suggest` filters a bounded 64-entry lexical shortlist into at most
three distinct app IDs. Every query word must match a name word start or its
initials; descriptions alone do not qualify. Exact normalized names come first.
Installed IDs present in the shortlist suppress their other source variants.
The daemon additionally prefers installed desktop matches and exact local
files; see [ADR 0070](../../adr/0070-app-discovery-without-a-prefix.md).
