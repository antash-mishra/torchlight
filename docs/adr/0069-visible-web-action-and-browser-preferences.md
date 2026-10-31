# 0069. A visible Web action, website intent and explicit browser preferences

Amendment, 2026-11-01: [ADR 0071](0071-system-default-browser.md) supersedes
browser selection. All Web actions follow the system default and ignore saved
browser overrides. Saved engine choices remain supported.

Amendment, 2026-10-30: at the user's request, remove the settings gear and its
popover. Keep reading existing `web.ini` choices at startup; the popup does not
write or delete them. Browser lookup now resolves only the selected handler,
and menu-specific focus handling is removed. The picker/saving design below
records the earlier implementation and is superseded by this amendment.

- **Status:** Accepted; implemented
- **Date:** 2026-10-30

## Context

M9 appended Web after ten local results and their open windows. A website query
such as `google.com` therefore surfaced unrelated local matches and required
scrolling to a search action. Its browser was implicit and its engine inherited
the daemon's DuckDuckGo default, independently of browser preferences.

## Decision

Keep the Web target in its existing separate protocol array, but render it in a
pinned row below the query, outside the local result viewport. The model orders
this item first, independently of stored result order. Normal text initially
selects the best local item; a website address, explicit `web` query or absence
of local matches selects the Web item. Up from the first local item selects Web;
Down returns to local results. The ten local result slots remain available.

Recognize HTTP(S) URLs, IPv4 addresses, localhost with a port, and a conservative
set of common domain suffixes. The primary action for recognized addresses is
**Open website**, with HTTPS added when no scheme was supplied. Validate host
labels, ports, escapes and schemes without DNS or filesystem probes. Filenames
such as `report.pdf`, `main.rs` and `archive.zip` remain ordinary queries.
Unlisted suffixes and IPv6 work with an explicit HTTP(S) scheme. `local` always
escapes automatic website handling; `web` always searches the literal text.

Alt+Enter searches the current Web target even when the primary action is to
visit a website. Ctrl+Enter retains file/folder reveal behavior and never
launches a typed action. Deliberate keyboard selection survives later phases.

Add a Web preferences popover with an engine and installed HTTPS browser
selector. A chosen browser is a validated desktop-file identifier, launched
through GIO with a URI list. Missing chosen browsers report an error rather
than switching browsers. System default continues through GIO's default handler.
The configured default is also offered explicitly when its MIME association
cache has not yet caught up. GTK controls own keyboard input while preferences
are open, including nested dropdown focus grabs; leaving the application still
dismisses the launcher.

Preferences belong to the popup, in `$XDG_CONFIG_HOME/torchlight/web.ini` (or
`~/.config/torchlight/web.ini`). Atomic saves apply immediately without restarting
the daemon. Without a saved override, the engine inherits the daemon's existing
`web_engine` setting. The CLI retains its daemon-configured search behavior.
The popup reads preferences/enumerates browsers once at startup; typing uses
copied options and pure URI helpers, with no browser discovery or file reads.

## Consequences

This supersedes ADR 0067's placement of Web after local groups. No protocol,
database, indexing or ranking change is required. Browser/search/visit actions
remain excluded from local history, and only activation opens a browser.
The conservative suffix policy favors keeping ambiguous file queries local;
it is intentionally not a complete public-suffix database. This adds no build
or runtime dependency.
