# 0049. The popup on Wayland: sized from its monitor, placed by the compositor

- **Status:** Accepted
- **Date:** 2026-10-14

## Context

The popup was built and validated on Cinnamon X11, where the launcher asks X for the
pointer, picks that monitor's work area, sizes the popup for it and moves the window
to the upper part of the screen. All of that was one X11-only function. On any other
display it returned at once, so `popup_view_configure` never ran: the surface stayed
at its 680-pixel default, the preview panel never showed, the result list had no
monitor height budget, and the footer links and idle strip were never fitted to the
width.

Run under a headless Weston compositor (native GTK Wayland, driven through AT-SPI),
the popup searched and grouped results correctly but opened narrow, without its
preview panel, wherever the compositor put it.

Wayland does not let ordinary clients position their windows. A launcher can float
above other windows with the layer-shell protocol (the `gtk4-layer-shell` library),
which KDE and wlroots compositors such as Sway and Hyprland support and GNOME does not.
Wayland also gives clients no portable way to list other applications' windows
([ADR 0035](0035-open-windows-under-applications.md)).

## Decision

1. **Size from the monitor.** On displays other than X11 the launcher sizes the popup
   for the monitor its surface is on, or the first monitor until the surface has
   entered one, and sizes it again when it enters a monitor. The rules are the X11
   ones: 880 pixels with the preview panel when the monitor's width minus 48 allows
   it, otherwise up to 680, and at most 70% of the monitor's height. Wayland has no
   work area, so the whole monitor counts.
2. **Placement stays with the compositor.** No layer-shell: it would add a library
   dependency and still leave GNOME, the most common Wayland desktop, to the
   compositor. Where it opens follows the compositor's placement policy, which differs
   between desktops, their settings and the windows already open.
3. **A natural width for wrapped preview text.** The preview panel's wrapping labels
   ask for about 28 characters of width, roughly the panel's own. They had asked for
   one character so that long text could not widen the panel, but GTK sizes Wayland
   windows partly from heights measured without a width, and such text then folded
   one character per line: with two results the window grew to 712 pixels instead of
   about 350.
4. **A Wayland acceptance test.** `tests/test_popup_wayland.py` (`make test-ui-wayland`,
   also run by `make test-ui-isolated`) starts a headless Weston, a private D-Bus bus
   and a fixture daemon, types a query through AT-SPI and checks the size, the preview
   panel and the results. It skips when Weston is not installed.

## Alternatives considered

- **Layer-shell placement** (`gtk4-layer-shell`): a centred overlay on KDE and wlroots
  compositors, at the cost of a new dependency and nothing on GNOME. Not now.
- **Running under XWayland** (`GDK_BACKEND=x11`): window positioning and the X11
  window list would apply, but only to other XWayland windows, and focus rules still
  depend on the compositor.
- **Leaving Wayland as it was**: the popup worked, but without its preview panel and
  without a height limit from the monitor.

## Consequences

- On Wayland the popup has the same layout as on X11; where it opens depends on the
  compositor.
- Open windows are listed on X11, and on GNOME's Wayland session through a GNOME Shell
  extension ([ADR 0051](0051-open-windows-on-gnome-wayland.md)).
- Whether the popup takes keyboard focus when the shortcut toggles it depends on the
  compositor's activation policy and on how the desktop's shortcut starts the command.
- Panels are not subtracted from the monitor on Wayland; the 70% height limit and the
  48-pixel side room keep the popup clear of them in ordinary layouts.
