# 0067. Web search actions and a separate available-software catalog

- **Status:** Accepted; implemented
- **Date:** 2026-10-30

Web placement and popup preferences are superseded by
[ADR 0069](0069-visible-web-action-and-browser-preferences.md).

## Context

Torchlight searches files, folders, installed applications and settings. The
requested additions are web search and an app-installation option. Web actions
have no filesystem path, and uninstalled apps are absent from the desktop
catalog. The current popup and CLI assume path-bearing results, while IPC
version 1 rejects unsupported request fields. Query latency must not depend on
network access or another application's responsiveness.

## Decision

Implement M9 as described in the [implementation record](../m9-plan.md):

1. Open web searches in the default browser. Construct one bounded action from
   a built-in HTTPS engine and an encoded query, with no network access while
   typing. Normal popup searches append it after local results; explicit
   `web ` mode searches only the web. `local ` escapes reserved prefixes.
2. Search available apps only in explicit `install ` mode initially. Build a
   separate resident software index from configured Flatpak sources in a
   background worker. Reuse the lexical engine and bounded snapshot lifetimes;
   do not run package tools per query. Flatpak is an optional runtime backend.
3. Keep app identity, installation scope, repository, architecture and branch
   distinct from display labels. Revalidate before action and show source
   variants explicitly. Installed state alone does not establish launchability.
4. Open a supported software manager's app-details view and let that manager
   own confirmation and installation. Enable verified GNOME Software and KDE
   Discover details routes. When a route cannot select the source, label that choice as
   happening in Software. Do not claim an accepted handoff installed anything.
5. Add version-2 typed targets and explicit query modes while retaining the
   version-1 daemon contract. Keep existing CLI path output unchanged and
   expose new lookup commands separately. Advertise support through an additive
   capability in version-1 status; new GTK can use local-only version 1 with an
   older daemon. Do not put URLs or package ids in path fields.
6. Keep browser/manager activation in the client, discovery in the service and
   matching in the index. Web and software actions do not enter existing
   file/desktop history. Reuse Flatpak's metadata without a new SQLite schema
   in this milestone. Discuss new dependencies before introducing them.

## Alternatives considered

- Inline website results need a search API and network-query policy. Opening
  browser searches delivers the first feature without those dependencies.
- Mixing all available software into normal queries would alter existing
  relevance and expose duplicate installed/available entries. Explicit mode
  gives the feature a defined initial scope.
- Running `flatpak search` on each query would couple latency and cancellation
  to external processes. A background-built resident index preserves the query
  contract.
- Reusing file ids/paths for web and software actions would make existing
  resolve, history, preview and CLI behavior ambiguous. Typed targets keep
  activation and output contracts explicit.
- Direct installation introduces transaction progress, authentication,
  cancellation and recovery. A software-manager handoff delegates those
  responsibilities for the first release.
- A general third-party provider/plugin framework is unnecessary for these
  two features; a small software backend interface is sufficient.

## Consequences

Software discovery requires memory and background refresh work with measured
limits, while its failure leaves local search available. Software-manager
compatibility is a release gate rather than an assumption about every desktop.
The protocol and UI require an explicit compatibility change, but existing
scripts retain their path output and local history retains its meaning.

## Integration decisions from implementation

GNOME Software receives `--details=APP_ID`; Discover receives
`--application appstream://APP_ID`. These routes identify the app but do not
promise the catalog row's exact installation, remote, architecture or branch.
The popup therefore says **Choose source in Software** and displays the selected
source separately. Opening details never claims an installation succeeded.

Mint Software Manager's Flatpak reference importer may add a system remote.
That violates this milestone's no-repository-setup boundary, so that route is
not enabled. Cinnamon can use GNOME Software or Discover with Flatpak support;
otherwise the popup shows a missing-manager error. No new build dependency is
introduced. Tested runtime versions and evidence are in
[M9 validation](../m9-completion.md).

Resolve checks the resident identity and revision. External source/ref
revalidation runs in the client's cancellable activation worker immediately
before handoff, rather than blocking the daemon's search coordinator. The
adapter uses fixed argv, output limits, a deadline and process-group cleanup.
The background worker alone downloads metadata; typing never invokes it.

Software rows expose installed state but retain the manager action. Open remains
on the existing installed desktop-catalog row, whose executable provenance is
already checked. No display-name join is used to infer launchability.

The backend lives in `software_backend.c` with a public testable interface;
generic subprocess capture lives in core `process.c`. Discovery shares a
60-second subprocess budget and 32 MiB output cap, while construction is bounded
by 50,000 entries and 32 MiB of text. The 60 seconds is not a claim that index
construction itself has a wall-clock cutoff.
