# 0071. Always use the system default browser

- **Status:** Accepted; implemented
- **Date:** 2026-11-01

## Context

A saved `browser=google-chrome.desktop` made Torchlight keep launching Chrome
independently of the desktop default. The user requested that Torchlight use
the system default browser for every user.

## Decision

Web searches and website visits always call GIO's default URI handler at
activation time. Remove explicit browser activation and ignore legacy browser
keys in `web.ini`, without rewriting other users' settings. The Web row says
**System default** rather than caching a particular browser's name.

Keep loading the search engine independently from `web.ini`. A valid engine
works with no browser key or with an obsolete browser override. Desktop MIME
associations choose the handler for HTTP and HTTPS, so changing the desktop
default takes effect without restarting Torchlight.

## Consequences

This supersedes ADR 0069's explicit browser selection. URI validation,
cancellation, visible launch errors and exclusion from local history remain.
There is no browser discovery during typing, new dependency, IPC change or
ranking change. Existing browser fields remain in the internal model options
for compatibility, but the action worker cannot launch a named override.

Native popup regressions cover a legacy Chrome override, engine-only settings,
switching the system default while the popup stays running, direct visits and
searches reaching the new handler, and a missing system handler.
