# Changelog

Each release's notes are taken from its section here (docs/release.md).

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
