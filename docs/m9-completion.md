# M9 validation: web search and software discovery

Implemented on 2026-10-30. All execution took place in disposable Boat sandboxes;
the host edited/transferred files and orchestrated validation jobs. The later
authorized installation starts the built service on the user's desktop.
The original acceptance record below precedes the
[Web UX follow-up](#web-ux-follow-up-2026-10-30).
The subsequent [ordinary app discovery follow-up](app-discovery-validation.md)
removes the required install prefix and prioritizes installed desktop apps.

## Behavior

Normal popup searches retain ten local slots and append one Web action.
`web QUERY` opens a percent-encoded DuckDuckGo search (Google is configurable).
`install QUERY` searches configured Flatpak repositories and opens app details
in GNOME Software or Discover, which owns source selection and installation.
`local QUERY` escapes reserved prefixes and suppresses Web. Bare prefix words
remain local searches. Empty input launches nothing.

No shell, network call, package tool or filesystem read runs per software query.
A background worker builds a separate bounded catalog. Typed identity retains
installation, remote, app ID, architecture, branch and source/revision tokens.
Activation validates the current source and cached exact ref before handoff.
Web/software actions never enter local search/open history.

CLI commands `web`, `software` and `software-refresh` are explicit. Existing
query and NUL-delimited path output stay local. Version-1 clients still work;
new clients probe the additive capability and fall back to local-only operation
against older daemons.

## Automated checks

Sanitizer unit/integration tests, allocation checks, headless checks and lint
passed during feature validation in both the development VM and refreshed
Docker base image. This includes the 1,000-row software CLI regression and
failed-child process-group cleanup. Native popup checks also passed with a missing browser
handler, absent software manager and a replaced source.
The new unit/integration coverage includes mode parsing, URI encoding, strict
typed IPC, full software identity, source changes, duplicate labels, installed
state, bounded subprocess output/deadlines/cancellation, failed-refresh retention,
disabled/missing backends, old-daemon negotiation, phase-consistent actions,
allocation checks and no per-query backend calls.

New GTK was also run against a pre-M9 daemon built from commit `7767890`:
local search and the prefix escape worked; explicit Web/software modes showed
upgrade guidance. `build/m9/popup-compat.json` records the assertions.

The dedicated native popup test verifies ten local slots plus Web, prefix
handling, browser activation, manager argv, missing managers, stale sources,
empty-mode hints and unchanged local history. Native GTK captures cover Web and
software previews and exclude file-specific details/actions.

The five Dockerfiles built successfully. After final transport fixes, the
compiled application and exact source were copied from the rebuilt base into
the existing desktop images, preserving their installed desktop environments.
The refresh Dockerfiles, original/final image identities, source archive/hash,
test/lint logs and orchestration scripts are retained in
`build/desktop-matrix/20261030-m9-acceptance/`. GNOME, KDE and Cinnamon passed
there with zero skips. X11 passed the new actions group but caught a local
launch-error message regression: the generic local failure message had been
replaced with the underlying GIO error. Local messages are restored while
typed actions retain detailed browser/source/manager errors. The collector also
now expects all three X11 groups, with a regression covering complete and
incomplete reports.

Those final changes passed a new cold build under
`build/desktop-matrix/20261030-m9-x11-final/`, including test/lint and all three
X11 groups. Earlier failed runs remain separate; the three passing Wayland
lanes above precede only the local-error-message and collector fixes.

| Final validation | Result |
|---|---|
| `make test` (ASan/UBSan, integration and allocation checks) | Passed; 19 environment-runner regressions included |
| `make lint` | Passed |
| X11 web/software keyboard actions | Passed |
| X11 theme/font/scale and local interaction matrix | Passed |
| X11 wide popup and AT-SPI accessibility | Passed |
| GNOME, KDE and Cinnamon Wayland checks | Passed |

All six desktop groups executed with zero skips. Both final run artifact trees
have empty cleanup-failure lists, and their sandboxes were removed. The
combined artifact index is `build/desktop-matrix/20261030-m9-verification.md`.

## Performance

`make bench` completed at 50k and 500k local paths. The comparison uses the
optimized pre-M9 `7767890` build on the same Boat VM with the same Rust matcher.
Three interleaved unpinned pairs and two reversed, single-CPU pairs are retained
under `build/m9/bench/`. The single-CPU runs are a variability diagnostic, not
the normal four-worker latency target. Both binaries showed substantial run
variation; no claim of a passed 5% regression gate is made from these samples.

At the 50,000-app cap, the software stress fixture measured:

| Measurement | Result |
|---|---:|
| Index and reader construction | 0.473 s |
| Warm query p50 / p95 / p99 | 2.552 / 8.010 / 9.870 ms |
| Peak process RSS | 143.6 MiB |

This fixture has four repeated name families (12,500 apps each). Its multiword
`video stu` query has 10.557 ms p95. The 5 ms software target remains open;
functional completion is not a claim that this stress target passed.

Resident daemon typing measurements (two runs per condition, milliseconds):

| Local files / condition | Engine p95 range | IPC p95 range |
|---|---:|---:|
| 50k, features disabled | 0.445–0.540 | 0.529–0.642 |
| 50k, features enabled | 0.422–0.467 | 0.492–0.553 |
| 500k, pre-M9 daemon | 3.826–3.989 | 4.064–4.200 |
| 500k, features disabled | 3.339–3.742 | 3.520–3.951 |
| 500k, features enabled | 3.163–4.611 | 3.327–4.896 |

With a software refresh deliberately blocked, local engine p95 was
0.429–0.501 ms at 50k and 3.363–4.294 ms at 500k. These use a three-entry fake
software catalog to isolate orchestration from the separate 50k-app stress test.
Idle CPU across conditions was 0.1–0.3% of one core; enabling that fixture added
roughly 0.5–0.8 MiB resident memory. Cold-probe and startup figures, raw samples'
percentiles and memory high-water marks are in the accompanying JSON reports.

## Runtime routes and limits

Real Flatpak 1.14.6 enumeration of a configured user Flathub remote produced
3,525 app refs; `vlc` ranked `org.videolan.VLC` first with user/flathub/x86_64/stable
identity. Real GNOME Software 46.0 and Discover 5.27.12 opened VLC's details and
showed an Install button. These are tested versions, not asserted minima.

Both managers installed VLC after confirmation in the disposable VM. The
catalog then marked the exact user/flathub ref installed, and local search
returned the exported `org.videolan.VLC.desktop` entry. Screenshots and JSON
results are retained under `build/m9/managers/` and `build/m9/live/`.
The cloud VM's NetworkManager reported no connectivity despite working network
access; the manager acceptance session therefore used
`GIO_USE_NETWORK_MONITOR=base`. Discover also showed an unrelated PackageKit
offline warning; a second Install click completed its Flatpak transaction.
This validates user-scope installation, not privileged authentication flows.

The manager routes select the app ID, not an exact source variant. The UI says
**Choose source in Software**. Mint Software Manager's reference-import route
is deliberately disabled because it can configure a remote; Cinnamon requires
one of the supported managers. Missing managers produce a visible error.

Software rows show installed state but use the manager action. Existing local
desktop entries provide verified Open behavior. No automatic join by app name
is used. Native distro package discovery, inline website results, direct
installation and progress reporting inside Torchlight remain outside M9.

## Web UX follow-up (2026-10-30)

[ADR 0069](adr/0069-visible-web-action-and-browser-preferences.md) replaces the
original Web placement with a fixed row below the query. Website addresses
select a direct visit; ordinary text selects the best local match. All ten
local slots remain available. Alt+Enter searches even when Enter would visit.
The gear popover saves the browser and engine in a separate `web.ini` and
applies them immediately. Missing selected browsers produce an error.

Validation used a fresh Ubuntu 24.04 Boat sandbox (`bx_dbwc4j5p`) with GTK 4.14,
the unchanged cached Frizbee library, and strict C warnings. `make test` passed
with ASan/UBSan, integration, allocation and headless checks. The sandbox grants
CAP_DAC_READ_SEARCH by default; the permission-denial fixture initially caught
that environment difference. Rerunning through `setpriv --inh-caps=-all
--ambient-caps=-all` removed the capability and passed the full suite without
changing the permission assertion.
`make lint` and the optimized portable release build also passed.

Native Cairo/OpenGL checks verify the pinned row's position after scrolling
and readable shortcut/browser text when selected. End-to-end tests cover
keyboard and mouse visits, Alt+Enter, filename queries, unchanged local slots,
preference save/restart, explicit browser choice after the system default changes,
missing-browser errors, Escape, and focus moving to another application.
The action suite passed on both 800×600 and 1280×800 X11 desktops.
The local interaction/theme/font/scale matrix and wide AT-SPI checks also passed.
Earlier four-desktop Docker evidence above belongs to the original M9 change;
it is not a claim of repeating those four lanes for this follow-up.

`make bench` completed with typing p95 of 0.373 ms at 50k paths and 2.871 ms at
500k; whole-query p95 was 0.504 ms and 3.612 ms. These are one-run measurements,
not a new comparison or a resolution of the separate software stress target.
Logs, JSON reports, screenshots, source snapshot and installation backup are
retained under `build/web-ux/`.
