# M9 plan: web search and finding apps to install

**Status: implemented (2026-10-30).** [ADR 0069](adr/0069-visible-web-action-and-browser-preferences.md)
updates the Web UI with a pinned action and direct website opening.
[ADR 0071](adr/0071-system-default-browser.md) makes all Web actions use the
system default browser while retaining saved engine preferences.
The sections below retain the original design and
acceptance checklist used for implementation. Actual boundaries and route
decisions are recorded here and in
[ADR 0067](adr/0067-web-search-and-software-discovery.md); measured checks and
remaining validation limits are in [M9 validation](m9-completion.md).
The 50,000-app software stress target remains open: measured p95 is 8.010 ms
against a 5 ms target. Implementation status does not imply all performance
acceptance targets passed.

## Implementation record

- Browser actions, `web ` / `install ` / `local ` modes, configuration, version-2
  typed IPC, version-1 fallback, CLI lookups, previews and activation are implemented.
- A bounded background Flatpak catalog searches configured user, system and named
  installations. Cached metadata serves immediately; refresh is independent of typing.
- `software_backend.c` replaces the proposed `software_flatpak.c` name. Generic
  process capture is a core utility. External source/ref validation runs in the
  client action worker; resident resolve stays in the daemon.
- GNOME Software and Discover app-details routes are enabled. Both leave source
  selection to the manager, so the action reads **Choose source in Software**.
  Mint's importer is not enabled because it can add a remote. Cinnamon can use
  either supported manager. Installed software rows have a badge; opening an app
  remains on its independently verified local desktop entry.
- No new dependency, SQLite schema, repository setup or direct package install
  command is part of the feature. Flatpak and manager plugins are optional runtime
  requirements. The implementation's subprocess timeout excludes bounded index
  construction; it is not a hard 60-second total-refresh wall-clock promise.

## What the first version does

Torchlight will open a web search in the default browser and find applications
available from configured Flatpak repositories. Selecting an available app will
open its details in a supported software manager, where the person reviews the
source and confirms installation. Existing file, folder, application and settings
search stays available independently of these features.

The implemented defaults are:

| Input in the popup | Results | Enter on the new result |
|---|---|---|
| `weather tomorrow` | Existing local matches, then one Web action | Search for the complete text in the browser |
| `web linux tutorials` | One Web action for `linux tutorials` | Open that search in the browser |
| `install vlc` | Matching available apps, with repository and installed state | Open the selected app's software-manager details |
| `vlc` | Existing local matches, then the Web action | An installed VLC result still launches VLC |
| `local install notes` | Local search for `install notes` | Existing local action |
| Empty input, `web ` or `install ` | An appropriate typing hint | No action |

Recognize `web`, `install` and `local` only as a complete first token followed
by whitespace, using ASCII case-insensitive matching. A bare `web` or `install`
remains an ordinary local search. Strip the prefix and surrounding whitespace
once; preserve the rest of the query, including punctuation and internal spaces.
`local ` provides an escape for filenames beginning with a reserved word and
suppresses the Web action. Paths containing those words are ordinary paths.
The daemon receives an explicit mode; the popup parses this shorthand before IPC.

Normal search keeps all ten current local result slots. Its one Web action has
a separate bounded slot below the local groups and their window children; it
does not compete with local scores. Available software appears only in explicit
`install ` mode in this milestone. An installed app keeps its current Open action;
an available app's action is labeled **Choose source in Software**.

Browser search comes first. Website results inside the popup, suggestions from
a search service, native APT/DNF/pacman packages, automatic repository setup,
and installation progress inside Torchlight are later work. Direct URL opening
is also separate from this milestone.

## What the current code requires us to change

- `include/torchlight/popup.h` and `ui/gtk/model.c` represent every result with
  a numeric id and an absolute path. The decoder rejects unknown kinds. A web
  action or repository entry needs a typed target, not a fabricated path.
- `include/torchlight/ipc.h` and `src/ipc/ipc.c` implement strict version-1
  requests. `resolve` and `open` use `file_id`; the latter records history and
  does not execute an action. Both synchronous and asynchronous clients assume
  the current response version and lexical/final lifecycle.
- `src/ipc/client.c` prints paths for normal and NUL-delimited query output.
  Existing scripts must never receive a URL or app id where they expect a path.
- `ui/gtk/launcher.c` resolves a row before dispatching `actions.c` and queues
  accepted local opens for history. New actions need their own dispatch and
  failure handling, including labels in the footer and preview.
- `src/service/writer.c` and `src/bin/torchlight.c` currently accept only
  `root`, `allow` and `exclude` configuration keys. Adding settings must update
  both consumers without putting Torchlight policy in `src/core/config.c`.
- `desktop` discovers installed `.desktop` entries. It cannot discover apps
  that have not been installed; available software needs a separate catalog.

## Module boundaries

| Component | Planned responsibility |
|---|---|
| `src/service/search_actions.c`, public `search_actions.h` | Mode parsing, validated feature settings, and bounded Web action construction; no browser launch or network calls |
| `src/index/software.c`, public `software.h` | Immutable available-app entries, stable identities and resident lexical search using existing indexing utilities |
| `src/service/software_catalog.c`, public `software_catalog.h` | Background discovery through a swappable backend, refresh scheduling, snapshot publication and source revalidation |
| `src/service/software_flatpak.c`, private header | Initial optional Flatpak command adapter; subprocesses and output parsing stay off the search thread |
| `src/service/daemon.c` and `src/ipc/` | Mode routing, versioned typed responses, bounded resolve and refresh requests |
| `ui/gtk/{model,view,preview,launcher,actions}.c` | Grouping, selection, typed activation, software-manager handoff and visible errors |
| `src/ipc/client.c`, thin `src/bin/` wiring | Explicit web/software lookup commands while preserving path output |

Stateful modules have opaque create/destroy APIs, caller-owned query scratch,
documented borrowed lifetimes and checked `tl_status` results. The Flatpak adapter
implements a small backend interface so tests can supply deterministic entries
and later package backends can be added without changing UI behavior. This is
not a general plugin framework.

The daemon owns settings and catalog search; GTK obtains them through IPC.
Only the user's client launches the browser or software manager. Dependencies
remain downward: service orchestration may call index and core utilities;
the software index never launches a subprocess or accesses SQLite.

## Web action

Proposed defaults are `web_search = true` and `web_engine = duckduckgo`, with
`google` as a second built-in choice. Use fixed HTTPS endpoint definitions and
percent-encode the query as one parameter. Do not interpret query text as a URL,
command or format string. Custom endpoint templates can be a later extension.

No web request is needed to construct this result. Query text leaves Torchlight
only when the person activates the Web action. Use GIO's existing URI-launch
support and report launch errors without losing the query. GIO provides a
cancellable asynchronous default-handler API, so no new browser dependency is
needed. [GIO URI launch documentation](https://docs.gtk.org/gio/type_func.AppInfo.launch_default_for_uri_async.html).

Preserve selection through lexical/final updates. A deliberately selected Web
action remains selected when local results arrive; a query edit invalidates it
immediately. Hide file-specific actions such as Show in folder and Copy path
for Web rows. With web search disabled, omit the fallback and show a disabled
feature explanation for explicit Web mode.

## Available software and installation handoff

### Catalog and refresh

Start with Flatpak as an optional runtime integration, using the installed
`flatpak` executable from a worker. No required build dependency is proposed.
Enumerate configured user and system installations and their enabled remotes,
then collect application metadata and installed refs using explicit columns,
scope and locale. Probe command/column support at startup. Never parse aligned
terminal output or run `flatpak search` for each keystroke.

Flatpak documents repository enumeration, available-app listing and installed
app listing. Its `--cached` option prefers cached data; it is not an offline
guarantee. Background discovery may contact configured repositories, independently
of typed queries. [Flatpak command reference](https://docs.flatpak.org/en/latest/flatpak-command-reference.html).

Use `software_search = auto` by default: enable discovery when a supported
Flatpak executable and configured sources exist. `false` creates no software
worker or catalog. Start refresh after the daemon can serve local queries;
refresh every six hours and through an explicit `software-refresh` request.
Coalesce concurrent requests into one pass. Installed-state and remote changes
should invalidate the relevant cached data through cheap change stamps or
notifications, with a bounded backstop; do not poll by spawning commands each
second. Establish the supported change signals in phase 0.

Build a new index privately, publish it atomically and reclaim old snapshots
after readers release them. Keep the last successful catalog on failure and
mark its age/error. A cold catalog reports `loading`; a missing backend or no
enabled sources reports `unavailable`. Each query returns promptly from memory,
including while refresh is running. A final reply is final: readiness changes
require Retry or a new query, not an unsolicited extra response frame.

Initial resource limits to validate: one refresh worker, 50,000 entries, 32 MiB
of collected text/output, a 60-second total refresh deadline, and bounded
diagnostics. Terminate and reap timed-out subprocesses, with a short termination
grace before forced termination. Reject an oversized or incomplete replacement
rather than silently publishing a truncated catalog. Report these limits and
failures through status. Bound old and new snapshot memory together.

The first version rebuilds from Flatpak's existing metadata on daemon startup;
it adds no durable Torchlight software cache and no SQL schema change. If cold
startup measurements justify persistence, design a migration through `store`
as a subsequent change.

### Identity, matching and installed state

Keep each source variant distinct using `(backend, installation, remote, app id,
architecture, branch)`, with a revision that also detects source replacement.
Names are display/search data, never installation targets. Show repository and
scope when variants would otherwise look identical. Skip runtimes and disabled
or unsupported sources. Reuse lexical scoring for app names, ids and summaries;
exact names precede weaker matches with deterministic ties.

Match installed desktop entries using explicit app/desktop identities and
export metadata, never a display-name guess. Flatpak installed state and desktop
launchability are separate: an installed ref without a visible desktop entry
does not become a fabricated Open result. Keep unknown mappings explicit.
AppStream defines desktop launch metadata for these associations.
[AppStream desktop application metadata](https://www.freedesktop.org/software/appstream/docs/sect-Metadata-Application.html).

In software mode, show an installed match as **Installed**; offer Open only when
it resolves to a current launchable desktop entry. Other source variants remain
distinguishable. After an external installation, refresh installed state and
let the existing desktop catalog discover the exported launcher. Do not start
a full filesystem crawl to detect application installation.

### Opening the software manager

Resolve the chosen source identity again before handoff. A removed/replaced
source or stale entry requires refreshed results. Backend revalidation can run
on its worker under an action deadline; local search must not wait for it.

Probe registered application handlers and add small, tested adapters for GNOME
Software, KDE Discover and Mint Software Manager. Verify each supported route
opens the intended application's details and requires confirmation before any
installation. A generic `appstream:` handler is a candidate, not a promise of
support on every desktop or of selecting an exact repository variant.
Mint's command-line and Flatpak-reference handling also need their own check.
[Mint Software Manager source](https://github.com/linuxmint/mintinstall/blob/master/usr/lib/linuxmint/mintinstall/mintinstall.py).

Where the manager cannot preserve repository/branch selection, say **Choose
source in Software** and show the catalog source as availability information;
do not claim an exact package will be installed. If app identity itself cannot
be preserved, do not enable that route. With no supported manager, keep details
visible and explain that installation needs a supported software manager.

Pass fixed argv or validated URIs, never a shell command built from a label.
Do not add repositories, run `sudo`, accept arbitrary command strings from IPC,
or silently fall back to direct installation. The external manager owns
confirmation, permissions, authentication, download progress and installation
errors. An accepted handoff means only that opening Software was accepted.

## IPC, configuration and compatibility

Introduce version 2 for explicit modes and targets. The new daemon continues
serving version-1 clients with the existing result shapes, ids, limits and
history semantics. Advertise version-2 support as an additive capability in a
version-1 `status` response. New GTK makes that compatible probe once; a valid
response without the capability selects local-only version 1 and explains that
the daemon must be updated for the new features. A timeout or malformed response
is not evidence of an old daemon. Explicit CLI feature commands use the same
probe. Update both IPC transports and all decoders.

Version-2 queries carry `mode = local | web | software`; local mode may request
the Web fallback. Keep local top-k in `results` and the at-most-one Web fallback
in a separate `actions` array. Web mode has only that action; software mode has
bounded software `results`. Use discriminated row types with explicit targets:
local ids/paths, Web provider plus query, or software identity plus revision.
Do not overload `path`, `file_id` or `desktop_id` with URLs or package ids.

Selection keys include the result kind and full identity. Software snapshot
revision is separate from `catalog_gen` and `emb_gen`. Preserve request ids,
strict field validation, UTF-8/query limits, bounded response sizes,
cancellation and lexical/final semantics. Include the same Web action in both
phases. Software mode does no semantic inference and returns one terminal frame.

Add typed software resolution and background refresh operations. Resolution
returns validated target data; it never launches anything. Web actions are
reconstructed from validated provider settings and the current query. The
existing `open` history operation remains for local entries only. Web searches
and software-manager handoffs are not recorded in file/desktop opening history
or fed into personal ranking in M9.

Keep `torchlight query`, `query --null`, and offline `query --db` behavior intact.
Add `torchlight web QUERY` and `torchlight software QUERY` as explicit lookup
commands with readable typed output and `--json`; they do not launch or install.
Reject `--null` for these commands with an explanation. Add
`torchlight software-refresh` to enqueue refresh and return immediately; status
reports completion/failure. Document that CLI `query` text is literal, so the
popup's prefixes do not change existing scripts.

Recognize `web_search`, `web_engine` and `software_search` alongside existing
configuration keys. Validate them in a shared domain module used by daemon and
offline CLI, keeping unknown-key errors. Restart the daemon to apply changes,
as today. Invalid optional-feature values should produce clear errors, and
missing optional executables should disable only that feature. User-facing
configuration docs are updated when each option actually ships.

## Ordered implementation and exit criteria

### Phase 0 — prove the integration routes

- In Boat, inspect supported Flatpak versions and fixed-column output across
  installed, empty, duplicate-source, localized and malformed fixtures. Verify
  identifiers, full refs, installed metadata and source-change signals.
- Verify app-details handoff on GNOME Software, KDE Discover and Mint Software
  Manager, including missing handlers, source variants and confirmation behavior.
- Record the minimum supported versions, exact commands/URIs, limitations and
  chosen default engine in this plan/ADR. If the CLI cannot supply reliable
  metadata, discuss optional `libflatpak`/AppStream dependencies before adding
  them; the repository's dependency rule applies.

Exit: a tested route from catalog identity to software details on the supported
desktops, plus a concrete unsupported-state behavior elsewhere. Web implementation
can proceed independently if a software-manager adapter needs more work.

### Phase 1 — typed actions and compatible protocol

- Implement modes, shared settings validation, version-2 codecs, transports,
  typed popup rows and namespaced selection keys.
- Implement the version-1 compatibility path and explicit CLI command parsing.
- Extend bounded model capacity for the Web action plus the existing retained
  selection row; update grouping, preview, keyboard hints and accessibility.

Exit: fixture actions render and select correctly; new/old client-daemon pairs
behave as specified; existing local result order, path bytes and CLI output match
the baseline; no URI or app id reaches a file action or local history operation.

### Phase 2 — ship browser search

- Implement built-in engines, query encoding, prefix behavior, the fallback row,
  feature settings and asynchronous browser activation.
- Cover Unicode, reserved URL characters, length boundaries, query edits,
  cancellation, missing browser handlers and failed launches.
- Verify that typing causes no network requests and no subprocess launches.

Exit: both `web QUERY` and the fallback open the expected encoded search on
activation, with local selection stable through optional semantic completion.
This is a useful independently releasable increment.

### Phase 3 — available-app catalog

- Implement the software index, mock backend, Flatpak adapter and refresh worker.
- Add explicit software mode, software status and refresh, installed-state
  mapping, variant labels and snapshot revalidation.
- Exercise refresh failure/recovery, absent Flatpak, no remotes, unsupported
  backend versions, offline startup, cancellation, limits and daemon shutdown.

Exit: software queries use only a resident snapshot, stale refreshes retain the
last complete view, and normal file/app search remains responsive under refresh.

### Phase 4 — installation handoff and desktop validation

- Connect typed software actions to the routes proven in phase 0.
- Verify app details, repository labeling, source ambiguity and unsupported
  manager messages; show Open only after installed launchability is confirmed.
- In a disposable sandbox, install one fixture/test app through each supported
  route, then verify installed-state refresh and normal application launching.
- Test cancellation before handoff, repeated activation, removed sources,
  external cancellation/failure and uninstallation. Do not report completion
  merely because the manager opened.

Exit: the user can find an available app, reach its installation confirmation,
install it externally and find/open it as an installed application in Torchlight.

### Phase 5 — performance, documentation and release

Run all builds, tests, lint, benchmarks and desktop checks in Boat, following
`AGENTS.md`; do not run them on the host. Use controlled repositories and launch
stubs for automated tests, with real installation checks confined to disposable
sandboxes. Stop or delete those sandboxes when finished.

Use `make test-desktop-matrix` for the required X11, GNOME, KDE and Cinnamon
checks; see [Desktop tests on Boat](desktop-test-matrix.md). Phase 4's real
software-manager installation/authentication checks still need dedicated VM
sessions beyond this headless container matrix.

| Area | Required evidence |
|---|---|
| Correctness | `make test` and `make lint` pass with zero warnings; regression coverage in IPC, config, popup, async, actions, desktop and CLI tests, plus new software/mode tests |
| Query work | Allocation checks and instrumented backends show no filesystem I/O, SQL, subprocess or network work in lexical/software retrieval and ranking |
| Local relevance | Existing local top-k, tie ordering, exact paths and settings matches unchanged; Web action never consumes a local result slot |
| Responsiveness | `make bench` at 50k/500k paths, features off/on, cold/ready catalogs and active refresh; record engine, IPC and popup latency separately |
| Budget | Target existing warm lexical typing p95 below 5 ms and at most 5% regression against interleaved baseline runs; measure software p95 with a 5 ms target at 50k apps, catalog build time, idle CPU and steady/peak RSS |
| Lifecycle | New input supersedes old results; selected actions survive phase changes; shutdown reaps children and releases snapshots; failed refresh never erases a valid catalog |
| Desktop | GNOME, KDE and Cinnamon supported manager routes, X11/Wayland launch contexts, missing managers and browser failures; keyboard navigation and accessible action labels |
| Packaging | `make headless` and GTK builds work with Flatpak absent; optional runtime requirements and version compatibility are documented |

These performance numbers are acceptance targets, not measured claims. Record
baseline noise and explain any gate miss before declaring M9 complete.

Update module READMEs and Status entries alongside code; add the new modules to
the docs index. Update API comments, `docs/configuration.md`, desktop setup,
architecture, evaluation and milestone status as behavior ships. Keep
`AGENTS.md` and `CLAUDE.md` identical. Change this plan from planned to implemented
only after the exit criteria have recorded evidence.

## Later increments

Direct installation inside Torchlight needs its own backend transaction API,
confirmation UI, permission/authentication handling, progress/cancellation,
restart recovery and verified completion. Native package sources need separate
catalog and installer adapters. Inline web results need an explicitly selected
search API, credentials/cost decisions, opt-in network queries, request
cancellation and independent result limits. None is required to ship M9.
