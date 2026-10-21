# Changelog

Each release's notes are taken from its section here (docs/release.md).

## 0.1.6 (2026-10-21)

Searches show what you meant instead of software internals: SDKs, virtual environments and
caches are left out, and a typed word lists real matches, with the settings tagged with it
first.

### Cleaner results

- Folders that tools manage are no longer indexed: Python virtual environments, Conda
  environments, Android SDKs, Go's module cache, folders marked as caches (`CACHEDIR.TAG`)
  and dependency folders such as `node_modules` and `site-packages`, wherever they are.
  "camera" and "sound" used to list Android SDK files, often twice (once per installed
  version); now they list your own files. On one developer's home folder the index shrank
  from 213,520 entries to 26,169 and the search daemon's memory from 176 MiB to 38 MiB. To
  search such a folder anyway, add `allow = PATH` to `~/.config/torchlight/config`.
- Typing one word lists what matches it by the start of a word: a name, a word of a name or
  its initials. Near matches, such as typos or names that only share a few letters, appear
  when nothing else matches, so a misspelled word still finds things. Files you have opened
  before stay.

### Settings by what they are for

- An application or settings panel is found by the words it is tagged with: "wifi" lists
  Network, "audio" Sound, VLC and Rhythmbox, "battery" Power Statistics, "microphone" Sound.
  They come after files named exactly like the word and before other files; part of a tag
  ("aud") does not count.

## 0.1.5 (2026-10-18)

Torchlight now installs on many more distributions, finds applications it used to miss,
and starts with every session on Linux Mint.

### On more distributions

- Releases also come as a portable archive, `torchlight-0.1.5-linux-x86_64.tar.gz`, for
  Fedora, Arch, openSUSE, Debian 13 and other 64-bit distributions with GTK 4.14 or newer.
  Its `install.sh` sets Torchlight up just for you, in `~/.local`, without root; run a newer
  release's installer to update, or `install.sh --uninstall` to remove it.

### Starting at login

- The search daemon starts at every login on Cinnamon (Linux Mint), Xfce, MATE and LXQt.
  Until now it started right after installing but not again after logging in, so the popup
  could not search: those desktops never start the systemd target its service waits for.
  The package now also starts it from an autostart entry, "Torchlight search daemon", which
  Startup Applications lists. After updating, log out and back in once, or run
  `systemctl --user start torchlightd`.

### Finding applications

- Typing "terminal" on Linux Mint finds the Terminal. Applications are listed as your
  desktop's own menu lists them: Cinnamon's menu shows GNOME's Terminal and Power
  Statistics, and now so does Torchlight, and they launch.
- An application whose name you type comes first, even when many files or folders are named
  exactly that word: "google" lists Google Chrome before the `google` folders of Go module
  caches and SDKs, "time" lists Date & Time, "cli" CLion. A typed path still lists its file
  first.
- A word written in CamelCase later in a name matches when you type it whole: "sqlite" finds
  DB Browser for SQLite, "virtualbox" Oracle VM VirtualBox, and the same goes for files.

### Also

- A selected window, beneath its application, is easy to read again: its title takes the
  selection's text color instead of staying gray.
- `make install` puts the search daemon's service where systemd finds it
  (`~/.local/share/systemd/user`).
- Torchlight is open source, under the MIT License.

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
