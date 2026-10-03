# 0015: M3 resident desktop catalog and asynchronous GTK launcher

**Status:** Accepted, implemented

## Context

M3 searches installed applications and settings alongside files. The file catalog
is optimized for raw paths and durable file identities; desktop entries instead
have XDG precedence, localized labels, visibility and desktop launch semantics.
GTK must remain responsive while sockets, file managers and launch APIs wait.

## Decision

Use GTK4 (minimum 4.14 for accessibility announcements), its GIO/GIO-Unix desktop
APIs, and GTK's existing X11 dependency for Cinnamon placement. The plan already
specifies GTK4; implementation discussed its addition before changing the build.
No custom Exec parser, shell interpolation or additional UI framework is added.

A separate `desktop` module discovers user data applications before system data
applications, recursively derives desktop ids, and masks duplicates before
parsing visibility. GDesktopAppInfo applies localization, Hidden/NoDisplay,
OnlyShowIn/NotShowIn and TryExec. Settings are entries in the Settings category.
Metadata becomes a synthetic parent component with the visible name as basename,
allowing the existing lexical engine to search names, generic names and keywords.

Initial discovery precedes socket exposure; a worker rescans every second and
on reconcile. Unchanged content preserves ids and avoids engine rebuilds. Changed
snapshots are built outside a short exclusive query lease; publication swaps a
pointer and destruction happens after release. Queries merge bounded application
and file results by lexical score, giving application results deterministic tie
priority. Directory metadata is copied from SQLite into resident engines solely
for presentation, without changing lexical scores.

Desktop result ids occupy `[2^62, INT64_MAX]`, using a random session namespace
and monotonic allocation. They are not durable file ids. A changed entry, removal,
or daemon restart invalidates them. Wire requests retain `file_id` for compatibility;
responses add `kind`, `name`, `desktop_id`, `desktop_revision` and `icon` for desktop
entries. `catalog_gen` continues to describe the file snapshot; desktop publication
is independent. Selection and resolve must therefore use the complete result id.

The IPC module supplies cancellable GIO asynchronous exchanges with same-uid peer
checks, bounded newline frames, lexical/final callbacks and a five-second total
deadline. Every exchange uses a fresh connection; query edits are coalesced for
16 ms, with at most one query active and one newest pending query. Obsolete
responses cannot become actionable. Status, resolve and history exchanges have
separate bounded slots. The selection model retains a deliberately selected id
and position across phases, including one result outside top ten.

Resolve returns current exact bytes. Launch/reveal run in a GTask worker. Desktop
launch checks the resolved revision against the keyfile and compares execution
keys with the native filename-based GDesktopAppInfo before activation. This retains
D-Bus activation, terminal behavior and `%k` support, while rejecting changed
entries. Files use argv-based xdg-open; reveal uses FileManager1.ShowItems with a
byte-preserving URI and a parent-directory fallback. For non-UTF-8 targets it
also opens the parent after an acknowledged reveal: Cinnamon Nemo can return
success without displaying such an item. Cancellation is checked
before launch; an already accepted request still receives history.

Schema v3 adds desktop_opens keyed by unique launch event id and logical desktop
id, with nullable retained search id. The existing writer queue, no-history mode,
retention, deduplication and history-clear cover both launch kinds. File ids,
existing opens and catalog_gen are unchanged by migration or history writes.

## Consequences

The popup owns no database or index. Applications appear through the same daemon
socket and can be queried by the CLI. Application refresh is bounded by discovery
and rebuild time plus one second; locale/data-directory/desktop environment
changes require daemon restart. Desktop catalog limits are 8,192 encountered ids,
16 directory levels and 64 KiB per desktop file; failed refresh keeps the prior
snapshot. Full file-engine rebuild cost remains the M2 baseline.

The native single-instance popup and installed desktop entry can be bound to an
available Cinnamon shortcut. X11 placement uses the monitor under the pointer;
Wayland placement/focus follows compositor policy and has not been verified.
Systemd and desktop installation are explicit user steps documented in
[desktop setup](../desktop-setup.md), without changing existing shortcuts.
