# 0066. GNOME Settings panels are listed on GNOME, as its overview search lists them

- **Status:** Accepted; extends [ADR 0057](0057-applications-the-desktop-menu-lists.md)
- **Date:** 2026-10-26

## Context

On Ubuntu's GNOME session, typing "display" showed only files: no Displays, and no
other settings at all. "sound", "wifi" and "power" were the same. Every GNOME Settings
panel (`gnome-display-panel.desktop`, `gnome-sound-panel.desktop`, … 27 of them in
gnome-control-center 46) says `NoDisplay=true`, so the app grid leaves them out. The
Shell's overview still finds them: GNOME Settings registers a search provider, and typing
"display" there shows Displays under Settings. The catalog dropped every `NoDisplay`
entry, so on GNOME only the Settings application itself could be found, and a user had to
open it and look for the panel. On Cinnamon this never showed, because Cinnamon's own
panels (`cinnamon-display-panel.desktop`) are not `NoDisplay`.

## Decision

1. **`desktop_nodisplay_shown`,** beside `desktop_shown_in` in the desktop module, says
   whether the running desktop's own search lists an entry its menu hides. It is true only
   for a GNOME Settings panel (category `X-GNOME-Settings-Panel` and an `Exec` line starting
   with `gnome-control-center`) in a GNOME Shell session: `XDG_CURRENT_DESKTOP` names GNOME
   but neither GNOME-Flashback nor Budgie, which name GNOME for its applications and have
   no Shell overview.
2. **The catalog and the popup's launch check both use it.** An entry marked `NoDisplay`
   is kept only when it says true; the entry must still pass `desktop_shown_in`, and
   `Hidden` and GIO's `TryExec`/`Exec` checks apply as before.
3. **Panels are ordinary settings results.** They carry the Settings category, so they get
   the settings kind, and their names and keywords rank by the existing rules: "display"
   names Displays (ADR 0058), "resolution" and "night light" are among its keywords
   (ADR 0061).

## Alternatives considered

- **List every `NoDisplay` entry:** would add the URL handlers, file openers, previewers and
  window-manager session entries that no menu or search shows: 63 more on the machine
  where this was found (`code-url-handler`, Software's local-file handlers, Evince's
  previewer, `metacity`, `nm-applet`). GNOME Shell's own application search drops them
  too.
- **Ask GNOME Settings' search provider over D-Bus:** exact, including panels it hides for
  missing hardware, but it is a call per query to another process on the query path, which
  must do no I/O.
- **Only the category, without the `Exec` test:** third-party entries carry
  `X-GNOME-Settings-Panel` too; Settings' provider only lists the panels Settings itself
  ships, which all start `gnome-control-center`.

## Consequences

- On GNOME, "display" lists Displays first; Sound, Wi-Fi, Power, Keyboard, Mouse &
  Touchpad, Appearance, Date & Time and the other panels are found by their names and
  keywords and launch into GNOME Settings. Cinnamon, KDE, Flashback and Budgie are unchanged.
- GNOME Settings hides some panels from its own search when the hardware is missing
  (Wi-Fi without a wireless device, Mobile Network without a modem, Wacom Tablet without a
  tablet). Torchlight cannot see that without D-Bus calls, so it lists them anyway; on a
  wired desktop "network" also shows Wi-Fi and Mobile Network after Network.
- `tests/unit/test_desktop.c` checks the rule (`nodisplay_shown`) across sessions and
  categories, and the catalog with a `NoDisplay` panel and a `NoDisplay` non-panel on
  Cinnamon, GNOME, Flashback and KDE; `tests/unit/test_actions.c` launches the panel on
  GNOME and refuses it on Cinnamon, and refuses another `NoDisplay` entry on GNOME.
- Other desktops whose search lists entries their menus hide (KDE's System Settings
  modules in KRunner) would need their rule added here.
