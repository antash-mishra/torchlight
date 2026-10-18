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
and a systemd user unit in `~/.local/share/systemd/user`, where systemd looks for
it. Ensure `~/.local/bin` is on your desktop session's PATH.
The unit is wanted by graphical-session.target, which only sessions that systemd
runs start (GNOME, KDE Plasma). Cinnamon, Xfce, MATE and LXQt never start it, so
the packages also install an autostart entry that starts the unit at login there
([ADR 0059](adr/0059-daemon-starts-at-login-on-every-desktop.md)). `make install`
does not; to start a source install at login on those desktops, install the entry
yourself:

```sh
mkdir -p ~/.config/autostart
sed "s|^TryExec=.*|TryExec=$HOME/.local/bin/torchlightd|" packaging/torchlightd-session.desktop \
    > ~/.config/autostart/torchlightd.desktop
```

A unit started before the desktop hands its environment to systemd (for example
one wanted by default.target) hides the applications meant only for that
desktop; import the environment and restart it from the graphical session in
that case.

Keep a single daemon per database/socket. Stop a foreground trial daemon before
starting the unit. Configuration remains under XDG_CONFIG_HOME/torchlight and
catalog/history under XDG_DATA_HOME/torchlight. To disable recording, use a unit
override with `ExecStart=` followed by
`ExecStart=%h/.local/bin/torchlightd --no-history`.

`make install DESTDIR=/tmp/torchlight-install` creates a reviewable staged install.
The unit starts `PREFIX/bin/torchlightd`, so another `PREFIX` needs no edits; with
`PREFIX=/usr/local` it goes to `/usr/local/share/systemd/user`, for every user.
Installation does not replace desktop keyboard bindings or automatically
enable/start services.

## Enable M4 semantics

Building includes the native backend; loading model weights requires `--model`.
Export the pinned Potion model using the [M4 evaluation instructions](m4-model-evaluation.md).
Keep the exported file outside `build/`, for example under
`~/.local/share/torchlight/models/potion-retrieval-32M-256.tlm`, so cleaning build
artifacts does not remove it. For the default per-user installation, add this
override with `systemctl --user edit torchlightd.service`:

```ini
[Service]
ExecStart=
ExecStart=%h/.local/bin/torchlightd --model %h/.local/share/torchlight/models/potion-retrieval-32M-256.tlm
```

Run `systemctl --user daemon-reload` and `systemctl --user restart torchlightd.service`.
`torchlight status --json` reports semantic building progress and availability.
Initial queries use lexical fallback until the matching embedding snapshot is
published; ready eligible queries then receive lexical and hybrid final phases.
See the [semantic service](modules/semantic/README.md) for cache retry/deadline behavior.

## Cinnamon shortcut

Open System Settings → Keyboard → Shortcuts → Custom Shortcuts. Add **Torchlight**
with command `/home/YOUR_USER/.local/bin/torchlight-gtk --toggle`, then bind an
available shortcut such as Super+Space. Use the absolute executable path so the
binding works regardless of the desktop's PATH. Repeated invocation toggles the
focused popup; invoking it while hidden shows and focuses the search entry.

The target is Cinnamon X11. On Wayland, bind the same command in the desktop's own
shortcut settings. The popup sizes itself for its monitor, but the compositor decides
where it opens and whether it takes focus ([ADR 0049](adr/0049-popup-on-wayland.md)).
No global key interception or unsupported positioning is attempted. The user's existing
shortcut choices are preserved.

| Desktop | Shortcut | Open windows |
|---|---|---|
| GNOME | Settings → Keyboard → Custom Shortcuts | Through Torchlight's GNOME Shell extension, installed with the package or `make install`: log out and back in once so GNOME finds it; the popup enables it the first time it opens ([ADR 0051](adr/0051-open-windows-on-gnome-wayland.md)) |
| KDE Plasma | System Settings → Shortcuts → Custom Shortcuts | Through short KWin scripts; nothing to set up ([ADR 0053](adr/0053-open-windows-through-kwin-scripts.md)) |
| Cinnamon (Wayland session) | System Settings → Keyboard → Shortcuts → Custom Shortcuts | Through Torchlight's Cinnamon extension, installed with the package or `make install`; the popup turns it on the first time it opens, with no new login ([ADR 0054](adr/0054-open-windows-on-cinnamon-wayland.md)) |
| COSMIC | Settings → Keyboard → Keyboard Shortcuts → Custom Shortcuts | Through COSMIC's window protocols; nothing to set up ([ADR 0052](adr/0052-open-windows-through-wayland-taskbar-protocols.md)) |
| Sway | `bindsym Mod4+space exec torchlight-gtk --toggle` in `~/.config/sway/config` | Through the task bar protocol; nothing to set up. Sway floats the popup because its size is fixed |
| Hyprland | `bind = SUPER, space, exec, torchlight-gtk --toggle` in `~/.config/hypr/hyprland.conf` | Through the task bar protocol. If Hyprland tiles the popup, add a window rule that floats the class `org.torchlight.Launcher` |
| labwc (Budgie, LXQt, Xfce) | A `<keybind key="W-space">` with an `Execute` action for `torchlight-gtk --toggle` in `~/.config/labwc/rc.xml`, or the desktop's own shortcut settings | Through the task bar protocol; nothing to set up |

Use the full path of `torchlight-gtk` when it is not on the desktop's `PATH` (as with
`make install` or the portable archive's installer, into `~/.local/bin`).

## Open windows

Running applications list their open windows beneath them in the popup, and Enter
switches to the most recent one. Where that works:

| Desktop | Open windows | How |
|---|---|---|
| Any desktop on X11 | Yes | The window manager's list |
| GNOME on Wayland | Yes, from your next login after installing | A small GNOME Shell extension that comes with Torchlight |
| KDE Plasma 5.27 and 6 on Wayland | Yes | Short scripts that KWin runs |
| Cinnamon on Wayland | Yes | A small Cinnamon extension that comes with Torchlight |
| Sway, Hyprland, labwc (Budgie, LXQt, Xfce), Wayfire, niri | Yes | The desktop's task bar protocol |
| COSMIC | Yes | COSMIC's window protocols |
| Other Wayland compositors (Weston, kiosks) | No | They offer no way to see other windows |

The popup turns the GNOME and Cinnamon extensions on by itself the first time it opens;
if you turn one off later, it stays off. Hyprland, niri and Wayfire use the same protocol
as Sway and labwc but were not tested on their own. On Wayland the desktop decides where
the popup opens.

## Verification

```sh
make test
make lint
make bench
make test-ui  # requires Cinnamon/X11, xdotool and a session bus
make test-ui-isolated  # private Xvfb/Metacity session, theme/scale/error matrix
make bench-ui  # native paint timing during a 500k-catalog rebuild
```

`python3 tests/test_popup.py --matrix` repeats light/dark/high-contrast and GTK
scale overrides. It refuses to reuse an already running launcher. The isolated
runner requires Xvfb, Metacity, xdotool and D-Bus; custom executable paths can be
supplied as `XVFB=/path/to/Xvfb XDOTOOL=/path/to/xdotool` to make. Add `--a11y`
to `python3 tests/run_popup_checks.py --matrix` for native AT-SPI checks
(Python GI/AT-SPI and the system accessibility bus must be available). Its
`--screen`, `--wm` and `--output` options choose the screen, window manager and
JSON result file. `--bench` measures after-paint timing rather than window focus.

`python3 tests/test_popup_native.py` is an explicit Cinnamon
probe that opens real settings panels, reveals a non-UTF-8 filename's parent and
closes only the newly created test windows. These commands require xdotool.

The UI test uses isolated fixture applications, captures accepted actions and
checks focus, keyboard search/activation, repeat invocation, failure/restart
recovery, keyboard Retry, long/raw-byte names and disabled history. It
never edits desktop shortcuts. See [M3 verification](m3-completion.md) for recorded
measurements and platform coverage.
