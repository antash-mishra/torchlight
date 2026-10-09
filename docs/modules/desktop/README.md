# desktop

> **Status:** Implemented (M3 Part 2), including independent name/generic/keyword fields; M5 personal boosts
> **Source:** `src/index/desktop.c` · **Header:** `include/torchlight/desktop.h`
> **Tests:** `tests/unit/test_desktop.c`, `tests/test_desktop.py`

`desktop_create/destroy` own discovery, immutable lexical snapshots and a refresh
worker. Configuration comes from standard XDG data paths, locale and
XDG_CURRENT_DESKTOP; keep the environment stable and restart to change it.
Applications are searched independently of the configured file roots, including
hidden user data directories normally excluded by the file crawler.

User `applications/` entries precede system directories in XDG_DATA_DIRS order.
Nested paths become hyphenated desktop ids. A hidden or malformed higher-priority
entry masks lower copies. GIO supplies localized display/generic names and
keywords and enforces desktop visibility and TryExec. Settings-category entries
receive a settings result kind; no Cinnamon-specific launch command is invented.
Category lists use GIO's list parser, including valid lists without a final
semicolon.

Localized generic names and keywords are copied into separate lexical fields;
the display name stays the primary basename. A distinct `Name` alias is retained
with the generic-name evidence when `X-GNOME-FullName` changes the display label.
Metadata never enters the parent directory tree. Generic/keyword prefixes score
2800/2600, plus 128 for complete tokens. Primary-name prefixes remain stronger;
metadata fuzzy evidence uses the same bounded optimal scorer. Search paths are
internal only: wire responses and resolve return the actual desktop filename
plus launch metadata. See [ADR 0020](../../adr/0020-m3-part2-search-quality.md).

The desktop engine adds 2,000 to each basename/token/initials prefix hit. This
lets `chrome` match the name token in `Google Chrome` ahead of ordinary file
prefixes such as `chromepolicy…`, even when more than ten files match. It applies
before desktop top-k selection, so a small result limit cannot discard a name
match before weighting. Generic names, keywords and weak fuzzy matches keep
their separate lower field scores; exact filenames and paths retain their priority.
The daemon preserves the desktop engine's tie order across result limits.
See [ADR 0018](../../adr/0018-application-name-ranking.md).

The refresh worker scans every second, preserves unchanged ids, builds changed
engines/workspaces privately and publishes under the query lease. Discovery and
reclamation never execute in desktop_query. Acquire/query/resolve/release borrow
metadata only during the lease; each workspace has one query coordinator.

Ids use a random daemon-session namespace above 2^62; changes/removal retire the
id rather than allowing old results to launch a replacement. Responses include
a content revision checked by the launch worker. Metadata and revision come
from one parsed keyfile read, so atomic replacement cannot attach a new revision
to an old label or prevent the next refresh from retiring its id. An injected
replacement integration test covers this exact race. They are not persisted file
ids. File catalog_gen and desktop publication are independent.

Entries keep `StartupWMClass` as `wm_class` when it is present and at most 255
bytes; longer values are treated as absent. Results carry it so the popup can
list the application's open windows ([ADR 0035](../../adr/0035-open-windows-under-applications.md)).

Bounds: 8,192 encountered desktop ids (including hidden overrides), 16 nested
directories, 4,096-byte fields/filenames and 64 KiB desktop files. Directory
symlinks are not traversed. Failed snapshot builds retain the prior catalog.
A complete scan is periodic rather than driven by inotify, avoiding extra watch
pressure. Application install/remove lag is scan/build time plus at most one
second. See [ADR 0015](../../adr/0015-m3-desktop-catalog-and-launcher.md) and
[review fixes](../../adr/0017-m3-snapshot-cancellation-and-acceptance.md).

## M4 metadata integration

The service can copy sorted entry names, generic names, keywords, icons and
revisions under the existing exclusive lease. desktop_gen identifies its view;
owned semantic metadata is used after releasing this lease, before inference.

## Personal boosts (M5)

`desktop_query_boosted` boosts entries by sorted-id position in the leased
snapshot (`desktop_entry`), through `lexical_workspace_boost`. Callers remap
positions when `desktop_gen` changes; the daemon's
[personal](../personal/README.md) state matches application usage by desktop
id. Each query replaces the previous boosts, so `desktop_query` stays
unboosted. Editing a `.desktop` file gives its entry a new session id, which
sorts last, so its position changes; tests in `test_personal.c` and
`test_desktop.py` check that boosts follow it.
