# 0052. Open windows on wlroots desktops and COSMIC, and choosing the source by the session

- **Status:** Accepted
- **Date:** 2026-10-14

## Context

The popup lists a running application's open windows beneath it and switches to one
([ADR 0035](0035-open-windows-under-applications.md)). That worked on X11 and, through a
GNOME Shell extension, on GNOME's Wayland session
([ADR 0051](0051-open-windows-on-gnome-wayland.md)). Every other Wayland desktop listed no
windows, because Wayland does not let applications see each other's windows.

Each desktop gives trusted tools such as task bars a way to see them, but each in its own
way ([the plan](../wayland-windows-plan.md)). This ADR covers two families:

- **The wlroots family:** Sway, Hyprland, labwc, Wayfire and niri. labwc is Budgie 10.10's
  compositor and an option for LXQt and Xfce. They all offer the task bar protocol
  `zwlr_foreign_toplevel_manager_v1`.
- **COSMIC (Pop!_OS 24.04):** offers the standard `ext_foreign_toplevel_list_v1` for
  listing, plus its own `zcosmic_toplevel_info_v1` (a window's state) and
  `zcosmic_toplevel_manager_v1` (activation).

KDE Plasma ([ADR 0053](0053-open-windows-through-kwin-scripts.md)) and Cinnamon's Wayland
session ([ADR 0054](0054-open-windows-on-cinnamon-wayland.md)) are covered separately.

On 2026-10-14 the user approved linking libwayland-client, shipping all three parts in one
release, and no CI workflow for the desktops that cannot run here.

## Decision

1. **One Wayland source for both families.** `ui/gtk/windows_wayland.c` uses GTK's own
   connection (`gdk_wayland_display_get_wl_display`). It is only created when GDK has seen
   one of the protocols, so other desktops pay no round trip. When created, it reads the
   globals on a private event queue, binds the wlr manager (or COSMIC's info, manager and
   list), and makes three more round trips for the windows' first descriptions and
   COSMIC's states. Then it moves every object to GTK's default queue. From then on, GTK's
   own event dispatch keeps the windows current, so `list` copies them and `activate` sends
   one request: neither waits. Activation uses the display's default seat. The popup's own
   window (`org.torchlight.Launcher`) is left out.
2. **Most recently used first, from activations.** These protocols report which window is
   active but keep no history, so `ui/gtk/windows_track.c` (plain bookkeeping, unit-tested
   without Wayland) stamps each window when it becomes active. The resident popup therefore
   knows the order of use from its start. Windows not seen active since then keep the
   compositor's announcement order; wlroots announces the newest first. A window's handle
   is the track's own number for it. Its app id also fills the class and instance, because
   an Xwayland window reports its WM_CLASS class as its app id.
3. **The dependency.** The popup links libwayland-client directly, as the X11 source links
   libX11; GTK already loads it on every Wayland session. The protocol files are vendored in
   `third_party/wayland-protocols` (HPND-sell-variant licence), and `wayland-scanner`
   generates their client code into `build/ui/wayland` at build time. On Ubuntu,
   `libgtk-4-dev` already brings the scanner. Ubuntu 24.04's scanner predates the
   `deprecated-since` attribute in COSMIC's files; it warns but generates the same code, so
   its output is shown only when it fails.
4. **Choosing the source by what the session offers.** `ui/gtk/windows_session.c`, used
   by the launcher and by the test fixture, tries in order:
   1. X11, when the display is X11;
   2. these task bar protocols, when the compositor offers them;
   3. the desktop shell that owns its bus name (GNOME Shell, Cinnamon or KWin);
   4. the desktop named in `XDG_CURRENT_DESKTOP`, whose shell may still be starting.

   Bus names decide before the desktop's name because LXQt, Xfce and others can run on
   several compositors.
5. **COSMIC's manager is only forgotten, never destroyed.** cosmic-comp has no case for
   `zcosmic_toplevel_manager_v1.destroy` and panics on it (`unreachable!()` in
   `toplevel_management.rs`, in Pop!_OS's 24.04 build and in master on 2026-10-14). That
   takes the whole session down. The source drops the manager on the client side only, and
   COSMIC releases it when the launcher disconnects.

## Alternatives considered

- **Sway's and Hyprland's own sockets** (JSON over `$SWAYSOCK` and Hyprland's socket): no
  new dependency, and they give the exact history of use. But they cover only two
  compositors, leaving out labwc (so Budgie, LXQt and Xfce), Wayfire, niri and COSMIC. They
  remain a possible refinement of the order there.
- **`ext_foreign_toplevel_list_v1` alone:** the standard protocol lists windows but
  reports neither state nor activation.
- **AT-SPI:** lists the windows of accessible applications only, and cannot switch to
  them on Wayland.
- **A newest-first fallback by the track's own numbering:** inverted wlroots' own
  newest-first announcement, found by the end-to-end test.
- **A CI workflow** for Hyprland and Plasma 6: declined by the user. Plasma 6 runs here
  from KDE neon's packages instead.

## Consequences

- On Sway, Hyprland, labwc, Wayfire, niri and COSMIC the popup lists windows and switches
  between them as on X11, with nothing to install or enable.
- The order of use is exact from the popup's first start on. Before the popup has seen a
  window become active, the compositor's order applies.
- Hyprland, niri and Wayfire were not run end to end. They speak the same protocol as Sway
  and labwc, which were.
- Tests:
  - **Unit:** `tests/unit/test_windows_track.c` covers the ordering, names and removal.
  - **End to end:** `tests/test_windows_desktops.py` (`make test-ui-desktops`) runs headless
    Sway 1.9 and labwc 0.7.1 (Ubuntu 24.04), and COSMIC's compositor nested on Xvfb, with
    the real popup and the session's source through `tests/fixtures/window_sources.c`. The
    popup listed two windows under their application, most recent first; switching to the
    older one made it the most recent, and the popup then listed it first.
  - **Selection:** the same test covers KWin and Cinnamon; GNOME stays in
    `tests/test_windows_gnome_shell.py`.
