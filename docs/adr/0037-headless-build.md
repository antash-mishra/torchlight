# 0037. Headless build and install

- **Status:** Accepted
- **Date:** 2026-10-10

## Context
`torchlightd` and `torchlight` link only GLib/GIO, SQLite, utf8proc and the
vendored Frizbee library; they need no GTK, X11 or display. A daemon started
with an empty environment (no `DISPLAY`, D-Bus or desktop session) indexes and
answers queries. But `make`, `make install` and `make lint` always included the
GTK popup, so they failed wherever GTK 4.14 development files are missing:
servers, WSL and older distributions. The installed unit is also part of
`graphical-session.target`, which never starts without a graphical session, so
the daemon would not start at login there.

## Decision
- `make headless` builds only `build/torchlight` and `build/torchlightd`;
  `all` is `headless` plus the popup.
- `make install-headless` installs those two programs and
  `packaging/torchlightd-headless.service` as `torchlightd.service`, wanted by
  `default.target` with no tie to the graphical session. The same unit name
  keeps every `systemctl --user … torchlightd` instruction valid. It installs
  no desktop entry.
- `make lint-headless` runs the non-GTK clang-tidy and cppcheck passes; `lint`
  is `lint-headless` plus the popup passes. `make test` already compiles no GTK
  code (the unit-tested popup model, windows and actions use only GIO).
- GTK flags stay recursively expanded, so only popup recipes ever query
  `pkg-config` for `gtk4` or `x11`. `tests/test_headless.py`, run by
  `make test`, dry-runs the headless targets with a `pkg-config` stand-in that
  fails those lookups and asserts none happen, with the full build as control.

## Alternatives considered
- **A `HEADLESS=1` variable** switching `all`, `install` and `lint`: one
  forgotten variable on `make install` would install the popup's graphical
  unit over a headless build. Named targets state the intent per command.
- **Detect GTK and skip the popup when missing:** a desktop with a broken GTK
  setup would silently get no popup. Building the popup stays explicit.
- **Also drop GIO:** the desktop catalog (`GDesktopAppInfo`) and the shared IPC
  client use it, GLib is present on practically every Linux system, and the
  daemon already runs without a desktop session. Not worth the refactor.
- **Disable application search in headless installs:** a separate, runtime
  choice; the daemon still reads `.desktop` entries if the machine has any.

## Consequences
- Machines without GTK 4.14 can build, install, test and lint the daemon and
  CLI; contributors there run `make lint-headless` instead of `make lint`.
- Without lingering (`loginctl enable-linger`), a user service still runs only
  while the user has a session, as for any user unit.
- `make install-headless` over an existing desktop install replaces the
  graphical `torchlightd.service`; `make install` restores it.
