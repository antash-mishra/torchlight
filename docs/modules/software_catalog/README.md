# software_catalog and software_backend

> **Status:** Implemented (M9), including leased ordinary-name suggestions (ADR 0070); [validation](../../m9-completion.md).
> **Source:** src/service/software_catalog.c, src/service/software_backend.c
> **Headers:** include/torchlight/software_catalog.h, include/torchlight/software_backend.h
> **Tests:** tests/unit/test_search_actions.c, tests/test_search_actions.py

One background worker owns external discovery. A small copied function-pointer
interface supplies a builder and cheap change stamp; tests substitute an
instrumented backend. A query takes a short exclusive lease on the active index
and reader. The worker builds the next pair outside the lease, swaps under the
lock, then destroys the old pair outside it. No query waits for a package command
or metadata rebuild.

The Flatpak adapter enumerates the user, default system and configured named
system installations. It reads enabled, enumerable remotes and full tab-separated
refs, names and descriptions with fixed CLI columns. Runtime refs are excluded.
The process locale is C.UTF-8. Installation and source identity are retained
separately, and malformed/oversized output fails the refresh instead of
publishing an incomplete catalog. Source revisions include configuration and
trust-key file change metadata; replacing a source invalidates existing actions.
Installed matching normalizes the optional `app/` prefix: Flatpak 1.14's `list`
omits it although `remote-ls` includes it. Scope, origin, ID, arch and branch
must all match; the integration test covers both formats.

Startup first uses Flatpak's cached metadata, then schedules a download.
Manual refresh and six-hour download refreshes coalesce; the worker checks
configuration/install stamps every 15 seconds and rereads cached metadata at a
ten-minute backstop. A shared 60-second subprocess deadline and 32 MiB captured
output budget bound each discovery. Index construction has the separate
50,000-entry/text bounds. Stop cancels and reaps the child process group and joins
the worker.

States are disabled, loading, ready, unavailable, failed and stale. Disabled
creates no worker. Missing Flatpak/no enabled remotes yield unavailable. Other
refresh failures retain the last good snapshot as stale, or failed when none
exists. Queries always return promptly with a terminal response and current
state. Successful publication advances software.catalog_gen; popup status polling
can retry the current software query when readiness or the snapshot changes.
The same lease supports `software_catalog_suggest`; missing snapshots return
zero suggestions promptly. Ordinary popup queries also refresh when the software
generation advances. Native installed-app preference belongs to the daemon's
desktop merge, not to this Flatpak backend.

Resolve checks the resident typed identity and revision. Immediately before
handoff, the client's action worker checks the current source revision and exact
cached remote ref under its own five-second deadline. External revalidation is
outside the daemon's search coordinator. No adapter adds a remote,
installs an app, invokes a shell, or records local open history.
