# desktop

> **Status:** Implemented (M3), review fixes verified: resident XDG application/settings catalog
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

Localized keywords and generic names form one synthetic parent component; the
visible name is the lexical basename. Prefix/subsequence/trigram/typo matching
therefore stays reusable. Search paths are internal only: wire responses and
resolve return the actual desktop filename plus launch metadata.

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

Bounds: 8,192 encountered desktop ids (including hidden overrides), 16 nested
directories, 4,096-byte fields/filenames and 64 KiB desktop files. Directory
symlinks are not traversed. Failed snapshot builds retain the prior catalog.
A complete scan is periodic rather than driven by inotify, avoiding extra watch
pressure. Application install/remove lag is scan/build time plus at most one
second. See [ADR 0015](../../adr/0015-m3-desktop-catalog-and-launcher.md) and
[review fixes](../../adr/0017-m3-snapshot-cancellation-and-acceptance.md).
