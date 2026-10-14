# Open windows on every Wayland desktop: plan

**Status: proposed (2026-10-14).** Nothing here is built yet. Each mechanism below was
tried on this machine with throwaway test clients (see [What was checked](#what-was-checked-2026-10-14)),
except Hyprland, niri, Wayfire and Plasma 6, which are taken from their sources and are
covered by [CI](#5-checks-that-cannot-run-here).

## In simple words

On X11 ([ADR 0035](adr/0035-open-windows-under-applications.md)) and on GNOME's Wayland
session ([ADR 0051](adr/0051-open-windows-on-gnome-wayland.md), release 0.1.2), each
running application lists its open windows beneath it, most recently used first, and
picking one switches to it. On every other Wayland desktop the list is empty, because
Wayland stops applications from seeing each other's windows.

Every desktop has a door for trusted tools such as task bars, but each desktop has its own
door. There are four kinds, so Torchlight needs four small pieces behind the existing
`tl_window_source` interface. The list, matching and switching code stays as it is:

| Desktops | Their door | Torchlight's piece |
|---|---|---|
| Sway, Hyprland, labwc (Budgie 10.10's compositor, and an option for LXQt and Xfce), Wayfire, niri | The task bar protocol `zwlr_foreign_toplevel_manager_v1` | Part 1: one Wayland protocol source |
| COSMIC (Pop!_OS 24.04) | The standard list protocol `ext_foreign_toplevel_list_v1` plus COSMIC's own state and switching protocols | Part 1, same source |
| KDE Plasma 5.27 and 6 | Scripts that KWin loads over D-Bus | Part 2: a KWin script source |
| Cinnamon on Wayland | A shell extension, as on GNOME | Part 3: a port of the GNOME extension |

Users will install nothing extra and change no settings. The package will carry
everything, and the popup will turn the Cinnamon extension on by itself, as it does on
GNOME.

## What was checked (2026-10-14)

Each desktop ran headless or nested on a private display and session bus, with two GTK 4
test windows (`tests/fixtures/test_window.py`) and one X11 window under Xwayland (`xlogo`).
A throwaway client listed the windows and switched between them.

| Desktop, version | How it ran | Listed | Switched | Notes |
|---|---|---|---|---|
| Sway 1.9 (Ubuntu 24.04) | headless, unpacked under `/tmp` | yes | yes | Protocol version 3. Xwayland windows report their WM_CLASS class (`XLogo`) as the app id. Needs `--unsupported-gpu` on NVIDIA machines, even when rendering in software. |
| labwc 0.7.1 (Ubuntu 24.04) | headless, unpacked | yes | yes | Same results as Sway. |
| COSMIC compositor (Pop!_OS archive, built for 24.04) | nested on Xvfb | yes | yes | The window state only arrives when the compositor renders, so the test needs `LIBGL_ALWAYS_SOFTWARE=1`. Ubuntu 24.04's `wayland-scanner` warns about COSMIC's newer `deprecated-since` XML attribute but generates working code. |
| KWin 5.27 (Ubuntu 24.04) | `kwin_wayland --virtual`, 128 packages (31 MB) unpacked | yes | yes | A script's results come back through `callDBus`. Load, run and unload took 15 ms, including starting three `gdbus` processes. The script object is at `/<id>`. X11 class names arrive lower-cased (`xlogo`). Scripts cannot read `workspace.stackingOrder`, but each window's own `stackingOrder` works. |
| Cinnamon 6.4 (installed) | `cinnamon --wayland --nested` on Xvfb | yes | yes | Adding the extension to `org.cinnamon enabled-extensions` loads it at once, with no new login. Cinnamon's window manager has no `list_all_windows`; `global.get_window_actors()` works instead. |
| GTK popup on Sway | headless | n/a | n/a | The popup floats by itself because its size is fixed, so Sway does not tile it. |

These were taken from sources, not run here:

- **Hyprland:** ships both `wlr-foreign-toplevel-management` and `ext-foreign-toplevel-list`
  (the protocol headers in Arch's hyprland 0.55 package).
- **niri:** has had the wlr protocol since 0.1.1.
- **Wayfire:** its `foreign-toplevel` plugin is in the default plugin list.
- **Plasma 6:** KWin's source and kdotool give `workspace.windowList()`, a writable
  `workspace.activeWindow`, each window's `stackingOrder`, `desktopFileName`,
  `resourceClass` (no longer lower-cased) and `internalId`, and the script object at
  `/Scripting/Script<id>`.
- **COSMIC:** offers its protocols to every unsandboxed client (`client_not_sandboxed` in
  cosmic-comp's `state.rs`).

## 1. The Wayland protocol source (wlroots family and COSMIC)

**How.** `ui/gtk/windows_wayland.c` uses GTK's own Wayland connection
(`gdk_wayland_display_get_wl_display`). At start it reads the compositor's globals on a
private event queue, so GTK's events are not dispatched underneath it. It then binds one of
two protocol sets:

- **wlroots family:** `zwlr_foreign_toplevel_manager_v1`. Each window sends its title, app
  id and state. `activate` with the popup's seat switches to it.
- **COSMIC:** `ext_foreign_toplevel_list_v1` gives the title and app id,
  `zcosmic_toplevel_info_v1.get_cosmic_toplevel` gives each window's state, and
  `zcosmic_toplevel_manager_v1.activate` switches to it.

The bound objects move to GTK's default queue, so their events arrive through GTK's main
loop and nothing polls. `list` copies the current state and `activate` sends one request,
so both return at once. The popup's own window (`org.torchlight.Launcher`) is left out.

**Most recently used first.** These protocols report which window is active but keep no
history. The resident popup therefore counts activations from its start and puts the most
recent first. Windows that have not been active since then follow, in the compositor's
order. Sway's and Hyprland's own sockets could give the exact history later
([Out of scope](#out-of-scope)).

**Matching.** A native Wayland window's app id is usually its desktop file's name, which the
existing app-id rule matches. An Xwayland window reports its WM_CLASS class in the same
field. The source therefore copies the id into the class and instance fields too, so the
existing class and desktop-name rules match those windows.

**Dependency (needs your approval).** The source links `libwayland-client` directly, as the
X11 source links `libX11` today. GTK already loads that library on every Wayland session.
`wayland-scanner` generates the protocol code at build time, and Ubuntu's `libgtk-4-dev`
already pulls it in, so builders and users install nothing new. The protocol files go
under `third_party/wayland-protocols/`: the wlr toplevel protocol, the ext list protocol,
COSMIC's toplevel info and management, and the two workspace protocols those two
reference. All of them use the permissive licence of the Wayland protocols. The
alternative needs no new link: talk to Sway and Hyprland over their own JSON sockets. But
that would leave out labwc (so Budgie, LXQt and Xfce), Wayfire, niri and COSMIC.

**Tests.**
- **Unit tests:** the activation order and the id copying are plain functions.
- **`make test-ui-wlr`:** runs the real popup with the fixture daemon under headless Sway
  and labwc (`TORCHLIGHT_WLROOTS_ROOT`, unpacked like Weston). It checks the listed and
  switched windows through AT-SPI, as `test_windows_gnome_shell.py` does.
- **`make test-ui-cosmic`:** does the same under the nested COSMIC compositor
  (`TORCHLIGHT_COSMIC_ROOT`).

**Effort:** about 2 days.

## 2. KDE Plasma through KWin scripts

**How.** `ui/gtk/windows_kwin.c` writes a short script to `$XDG_RUNTIME_DIR` and calls
`org.kde.KWin /Scripting loadScript`. It runs the script at `/Scripting/Script<id>`
(Plasma 6) or `/<id>` (5.27), then unloads it. One script serves both versions by checking
which names exist (`windowList` or `clientList`, `activeWindow` or `activeClient`). For
each normal window the script sends back the id (`internalId`), title, desktop file
(`desktopFileName`, an exact application match), class and stacking position. It sends
them with `callDBus` to the popup's own bus name and ends with a marker. Calls that do not
come from the owner of `org.kde.KWin` are ignored.

`list` must answer synchronously, so the source keeps its own connection on a private
main context and waits for the marker for at most 250 ms, like the GNOME source. Switching
runs a second script that sets the active window.

**Most recently used first.** Sort by each window's `stackingOrder`, top first. The X11
source uses the same rule, and KWin raises a window when it is activated.

**Matching.** `desktopFileName` fills the app id. Plasma 5.27 lower-cases X11 class names,
so this source compares classes without regard to case.

**Why not KDE's own window protocol.** KWin offers `org_kde_plasma_window_management` only
to programs on its allowlist (`X-KDE-Wayland-Interfaces` in their desktop file), and the
protocol is one of Plasma's private ones. Scripts are KWin's public interface, and kdotool
has relied on them through Plasma 6. The protocol stays as the fallback if KDE ever
restricts scripting.

**No new dependency** (GIO only).

**Tests.**
- **Unit test:** a fake KWin on a private bus, like the fake GNOME Shell.
- **`make test-ui-kwin`:** runs headless KWin 5.27 here (`TORCHLIGHT_KWIN_ROOT`).
- **Plasma 6:** runs in CI.

**Effort:** about 1.5 days.

## 3. Cinnamon on Wayland through a Cinnamon extension

**How.** Port the GNOME extension to Cinnamon's format and install it under
`/usr/share/cinnamon/extensions/torchlight@antash-mishra.github.io/`. Cinnamon's format is
`metadata.json` plus `init`, `enable` and `disable`. Set `cinnamon-version` to `["6.0"]`:
Cinnamon reads each listed version as a minimum, so later versions keep loading it. The
port exports the same `org.torchlight.Windows` interface on `org.Cinnamon`, using
`Cinnamon.WindowTracker`, `global.get_window_actors()` and
`Main.activateWindow(window, time, workspace)`. `windows_gnome.c` takes the bus name as a
parameter and serves both shells.

**Turning it on.** Once per person, the popup adds the extension to `org.cinnamon
enabled-extensions` and writes a marker, the same rule as on GNOME, so a later manual
disable stands. Cinnamon loads it at once.

**Tests.** `make test-ui-cinnamon` runs a real Cinnamon Wayland session nested on Xvfb
here.

**Effort:** about half a day.

## 4. Choosing the source

Choose by what the session offers, not by the desktop's name. LXQt and Xfce let people
pick their compositor (labwc, Wayfire, KWin and others), so `XDG_CURRENT_DESKTOP` does not
say which door exists. The order:

1. An X11 display uses the X11 source.
2. A Wayland compositor that offers the wlr manager uses the wlroots set.
3. One that offers COSMIC's info and manager uses the COSMIC set.
4. Otherwise, check the session bus: `org.gnome.Shell` uses the GNOME extension,
   `org.Cinnamon` the Cinnamon extension, and `org.kde.KWin` KWin scripts.
5. Otherwise there is no source, and no windows are listed.

## 5. Checks that cannot run here

Plasma 6 and Hyprland are not in Ubuntu 24.04. A separate GitHub Actions workflow runs the
same end-to-end test in containers that have them: Arch Linux has Hyprland, Plasma 6 and
niri. It runs weekly and on demand, not on every release, so a flaky desktop never blocks
a release. Hyprland renders with OpenGL and may need a GPU device that CI machines lack.
If so, it is checked on a real machine before each release that touches it.

## 6. Packaging and docs

- **Package:** ships the Cinnamon extension. `make test-packaging` checks that it is there.
- **ADRs:** one per part.
- **Module docs:** `docs/modules/ui` and the windows module README, with Status.
- **Glossary:** "foreign toplevel" and "KWin script".
- **README:** a "works on" table.
- **desktop-setup.md:** each desktop's shortcut line, and a Hyprland float rule for
  `org.torchlight.Launcher` if Hyprland tiles the popup.
- **CHANGELOG:** an entry for each release.

## Steps

1. Part 1 (wlroots family and COSMIC): most desktops per line of code, and all of it can
   be tested here.
2. Part 2 (KDE), with the CI workflow for Plasma 6 and Hyprland.
3. Part 3 (Cinnamon on Wayland).

About 5 days in all. All three parts ship together in one release.

## Out of scope

- **Exact history on Sway and Hyprland** through their own sockets (`GET_TREE` focus order,
  `focusHistoryID`), after part 1 proves itself.
- **Compositors with no task bar door at all** (plain Weston, kiosk compositors), which
  cannot be supported.
- **Sandboxed installs** (Flatpak, Snap). Compositors hide these doors from sandboxed
  apps, and Torchlight ships as a `.deb`.

## Decisions

Decided (2026-10-14): all three parts ship together in one release, not part by part.

Still open:

1. Link `libwayland-client` and vendor the protocol files for part 1. The alternative
   covers only Sway and Hyprland, through their sockets.
2. Add the weekly GitHub Actions workflow with Arch containers for Plasma 6 and Hyprland.
