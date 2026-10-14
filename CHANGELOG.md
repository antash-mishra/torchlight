# Changelog

Each release's notes are taken from its section here (docs/release.md).

## Unreleased

- Torchlight is open source, under the MIT License.
- A selected window, beneath its application, is easy to read again: its title takes the
  selection's text color instead of staying gray.

## 0.1.4 (2026-10-14)

- The popup's page header shows how long each search took, in teletext cyan beside the
  clock, instead of the number of results.

## 0.1.3 (2026-10-14)

- Running applications list their open windows, and Enter switches between them, on the
  other common Wayland desktops too: KDE Plasma 5.27 and 6, Cinnamon, COSMIC, Sway,
  Hyprland, labwc (Budgie, LXQt and Xfce) and Wayfire. Nothing needs setting up. On
  Cinnamon the popup turns on a small extension that comes with the package the first time
  it opens, and leaves it off if you turn it off later.
- On Cinnamon's Wayland session the popup no longer closes for good when it is shown a
  second time.

## 0.1.2 (2026-10-14)

- On GNOME's Wayland session the popup lists each running application's open windows
  and switches between them, as on X11, through a small GNOME Shell extension that comes
  with the package. Log out and back in once after installing; the popup turns the
  extension on the first time it opens, and leaves it off if you turn it off later.

## 0.1.0 (2026-10-14)

The first packaged release, for Linux Mint 22 and Ubuntu 24.04 or newer.

- Install it from a `.deb` that also adds Torchlight's signed APT repository, so the
  system's update manager installs new versions. After an update the search daemon
  restarts at once and the popup restarts the next time it is hidden.
- The popup's own look, Tekst: teletext's page design on calm light and dark palettes,
  results grouped into Apps, Settings, Folders and Files, and a preview panel with the
  selected item's details and actions on wide screens.
- Forgiving search over applications, settings panels, files and folders: prefixes,
  initials, abbreviations, one-letter typos and folder context.
- Running applications list their open windows beneath them (on X11).
- On Wayland the popup sizes itself for its monitor; the desktop decides where it opens.
- `--version` for `torchlight`, `torchlightd` and `torchlight-gtk`.
