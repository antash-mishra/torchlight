# 0057. Applications are listed as the desktop's own menu lists them

- **Status:** Accepted; [ADR 0066](0066-gnome-settings-panels.md) lists GNOME Settings panels,
  which say `NoDisplay`, on GNOME
- **Date:** 2026-10-17

## Context

On Linux Mint, typing "terminal" did not show the Terminal. Its entry,
`org.gnome.Terminal.desktop` from Ubuntu's gnome-terminal package, says
`OnlyShowIn=GNOME;Unity;`, and the session's `XDG_CURRENT_DESKTOP` is `X-Cinnamon`.
The desktop catalog kept an entry only when GIO's `g_app_info_should_show` agreed, and GIO
matches `OnlyShowIn` literally, so the entry was dropped. Cinnamon's menu lists it: its menu
library (libcinnamon-menu, `key_file_get_show_in`) counts an `OnlyShowIn` that names GNOME
as its own, and leaves out every entry whose `Exec` starts with `gnome-control-center`.
On the machine where this was found, Terminal and Power Statistics were the two
applications Cinnamon's menu shows that Torchlight hid. The popup's launch check called the
same GIO function, so it would have refused to start Terminal even had the daemon listed
it.

## Decision

1. **One rule, `desktop_shown_in`,** in the desktop module, decides from an entry's
   `OnlyShowIn`, `NotShowIn` and `Exec` and the `XDG_CURRENT_DESKTOP` value whether the
   desktop's own menu lists it. Everywhere it is GIO's rule (the first current desktop that
   either list names decides; otherwise only entries without `OnlyShowIn` are listed). On
   Cinnamon it is Cinnamon's: an `OnlyShowIn` naming GNOME also lists the entry, and an
   `Exec` starting with `gnome-control-center` never does.
2. **The catalog and the popup's launch check both use it,** with Hidden and NoDisplay
   checked as before and GIO still applying `TryExec` and the `Exec` program lookup.
3. **It reads the environment it is given.** The rule takes the desktop list as an
   argument instead of GIO's once-per-process copy of the environment, so tests can judge
   several desktops in one process.

## Alternatives considered

- **Ignore `OnlyShowIn` everywhere:** would list KDE's and Xfce's settings panels on GNOME
  and Cinnamon.
- **Always treat GNOME as a current desktop:** right for Cinnamon, wrong for KDE, MATE and
  Xfce, whose menus do not list GNOME-only entries.
- **Ask libcinnamon-menu:** a new dependency, only present on Cinnamon, for a rule of a
  few lines.

## Consequences

- On Cinnamon, GNOME-only applications that its menu shows (GNOME Terminal, Power
  Statistics) are searchable and launch; GNOME Settings panels stay out, as in its menu.
  Elsewhere nothing changes.
- `tests/unit/test_desktop.c` checks the rule for GIO's ordering, colon-separated desktop
  lists, GNOME-only entries on Cinnamon, KDE and MATE, and GNOME Settings panels (with a
  stand-in `gnome-control-center` on `PATH`, so only the rule can hide them);
  `tests/unit/test_actions.c` launches a GNOME-only entry on Cinnamon and refuses a
  KDE-only one.
- Other desktops whose menus widen `OnlyShowIn` would need their rule added here.
