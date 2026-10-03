# Desktop setup

Install development dependencies on Debian/Ubuntu/Mint:

```sh
sudo apt install build-essential pkg-config libsqlite3-dev libutf8proc-dev libgtk-4-dev libglib2.0-dev libx11-dev
make
```

GTK4 4.14 or newer is required for accessibility announcements. GIO/GIO-Unix ship
with GLib. Existing CLI/offline operations remain available; daemon application
search requires the same desktop session locale and XDG environment as the popup.

For a quick trial, run `./build/torchlightd` and then, from another terminal:

```sh
./build/torchlight-gtk --toggle
```

`--socket /absolute/path` is supported for an isolated daemon. Use `display`,
`screen`, `resolution`, `sound`, `keyboard`, or an installed application name.
Cinnamon's native desktop entries supply their panel commands; Enter activates
the desktop entry through GIO. Arrow keys select, Escape closes, and Ctrl+Enter
reveals files through the file manager. For non-UTF-8 filenames, reveal also
opens their parent folder because Nemo can acknowledge the item without showing
it. Opening or clearing the popup shows `Type to search` with no default results.

## Per-user installation and service

```sh
make install
systemctl --user daemon-reload
systemctl --user import-environment DISPLAY XDG_CURRENT_DESKTOP DBUS_SESSION_BUS_ADDRESS
systemctl --user enable --now torchlightd.service
systemctl --user status torchlightd.service
```

The default install is `~/.local`: three executables, a launcher desktop entry
and a systemd user unit. Ensure `~/.local/bin` is on your desktop session's PATH.
The unit is associated with graphical-session.target. On desktop sessions that
do not activate that target, add `systemctl --user start torchlightd.service` to
the desktop's Startup Applications after importing its environment. Cinnamon's
Startup Applications can run this command at login. A standalone unit started
before the desktop environment is imported can hide desktop-specific entries;
import and restart it from the graphical session in that case.

Keep a single daemon per database/socket. Stop a foreground trial daemon before
starting the unit. Configuration remains under XDG_CONFIG_HOME/torchlight and
catalog/history under XDG_DATA_HOME/torchlight. To disable recording, use a unit
override with `ExecStart=` followed by
`ExecStart=%h/.local/bin/torchlightd --no-history`.

`make install DESTDIR=/tmp/torchlight-install` creates a reviewable staged install.
The supplied unit assumes the per-user default prefix; adjust ExecStart if using
a different PREFIX. Installation does not replace desktop keyboard bindings or
automatically enable/start services.

## Cinnamon shortcut

Open System Settings → Keyboard → Shortcuts → Custom Shortcuts. Add **Torchlight**
with command `/home/YOUR_USER/.local/bin/torchlight-gtk --toggle`, then bind an
available shortcut such as Super+Space. Use the absolute executable path so the
binding works regardless of the desktop's PATH. Repeated invocation toggles the
focused popup; invoking it while hidden shows and focuses the search entry.

The target is Cinnamon X11. GTK's focus and placement requests on Wayland depend
on compositor policy; no global key interception or unsupported positioning is
attempted. The user's existing shortcut choices are preserved.

## Verification

```sh
make test
make lint
make bench
make test-ui  # requires Cinnamon/X11, xdotool and a session bus
```

`python3 tests/test_popup.py --matrix` repeats light/dark/high-contrast and GTK
scale overrides. `python3 tests/test_popup_native.py` is an explicit Cinnamon
probe that opens real settings panels, reveals a non-UTF-8 filename's parent and
closes only the newly created test windows. These commands require xdotool.

The UI test uses isolated fixture applications, captures accepted actions and
checks focus, keyboard search/activation, repeat invocation and history. It
never edits desktop shortcuts. See [M3 verification](m3-completion.md) for recorded
measurements and platform coverage.
