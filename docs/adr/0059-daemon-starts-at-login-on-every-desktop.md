# 0059. The daemon starts at login on desktops without graphical-session.target

- **Status:** Accepted
- **Date:** 2026-10-17

## Context

The daemon's systemd user unit is `WantedBy=graphical-session.target`, and the `.deb`
(ADR 0050), the portable installer (ADR 0056) and `make install` all rely on that target
to start it at login. Only sessions that systemd runs start the target: GNOME and KDE
Plasma do; Cinnamon, Xfce, MATE and LXQt never do. On Linux Mint's Cinnamon session the
target stays `inactive (dead)` for the whole session (checked after 42 days of uptime):
`cinnamon-session` hands its environment to systemd (`UnsetAndSetEnvironment`) but starts no
target. So on Mint, the distribution the `.deb` is built for, the daemon ran after
installing, because the package's postinst starts it for logged-in users, and never again
after the next login: the popup could not search until someone started the daemon by hand.
The development machine never showed it, because its own unit is wanted by
`default.target`.

Starting the unit with `default.target` instead does not work either: the user manager
starts before the session hands over `XDG_CURRENT_DESKTOP` and `XDG_DATA_DIRS`, and a
daemon started without them hides every application restricted to the desktop (all of
Cinnamon's settings panels; ADR 0057) for its whole life.

## Decision

1. **An XDG autostart entry starts the unit:** `Exec=systemctl --user start
   torchlightd.service`. Session managers run autostart entries after the session's
   environment reaches systemd (on X11 sessions started through `Xsession`,
   `95dbus_update-activation-env` hands over the whole environment before the session
   manager starts), so the daemon gets the desktop's environment, as it does from the
   target.
2. **Sessions that systemd runs skip it** (`X-systemd-skip=true`, honored by
   `systemd-xdg-autostart-generator`, through which GNOME and Plasma run autostart
   entries); they start the daemon with the target as before. Cinnamon, Xfce, MATE and
   LXQt do not know the key and run it.
3. **`TryExec` names the daemon,** so the entry stays inert once the program is removed
   (`/usr/bin/torchlightd` in the package; `install.sh` writes its own path).
4. **Where it goes:** the `.deb` installs it as `/etc/xdg/autostart/torchlightd.desktop`,
   a conffile, so a person who turns it off in Startup Applications keeps that choice; the
   portable archive carries it and `install.sh` writes `~/.config/autostart/torchlightd.desktop`
   when the session has a systemd user manager. Without one, that same file runs the daemon
   directly, as before; switching between the two modes replaces the file and, when the old
   entry ran the daemon itself, stops that daemon. `make install` installs no autostart
   entry, as it enables no service; [Desktop setup](../desktop-setup.md) shows how to copy
   it.

## Alternatives considered

- **`default.target` with a fixed `XDG_CURRENT_DESKTOP`:** works for one desktop only,
  misses `XDG_DATA_DIRS`, and keeps a daemon with a stale environment across sessions.
- **Have the popup start the daemon on demand:** the first search after login would wait
  for the catalog to load, and indexing would not begin until then.
- **Socket activation:** the same first-search wait, and a larger change to how the daemon
  takes its socket and lock.

## Consequences

- On Cinnamon, Xfce, MATE and LXQt the packaged daemon starts at every login, with the
  session's environment. On GNOME and Plasma nothing changes.
- The entry appears in Cinnamon's Startup Applications as "Torchlight search daemon", where
  it can be turned off; masking the unit (`systemctl --user mask torchlightd`) also stops it.
- `tests/test_packaging.py` builds a package with `build_deb.sh` from stand-in programs and
  checks the unit, the entry and its conffile; `tests/test_portable.py` checks the entry
  install.sh writes in each mode and the switch between them.
