# 0064. The popup's window stays clear and keeps its height on X11

- **Status:** Accepted
- **Date:** 2026-10-24

## Context

Moving from Cinnamon to GNOME 46 on X11 (mutter 46.2, GTK 4.14, an NVIDIA card), the user saw
an outer border with a gap around the popup, and a flicker while searching, neither of which
Cinnamon showed. A 60 fps screen recording found three causes:

1. **A user stylesheet filled the window.** GNOME themes for libadwaita applications link
   their stylesheet to `~/.config/gtk-4.0/gtk.css`. GTK loads that file at its user priority,
   above the popup's application priority, so its rules win over `popup.css` property by
   property. Its `.background` rule (every `GtkWindow` carries the class) painted the theme's
   window colour across the whole clear window: a dark box with square corners around the
   rounded surface, filling the shadow margin. GTK also then told the compositor that the
   whole window was opaque.
2. **Mutter shows a self-resized window at the wrong size.** The window grew when results
   arrived and shrank when they cleared. For about four frames (70 ms) after each resize,
   mutter drew the new buffer in the old size or the old one in the new: the popup collapsed
   to a line, or its content was drawn stretched or twice. Muffin, Cinnamon's compositor,
   showed none of this, and Wayland compositors resize a surface atomically.
3. **Redraws beneath showed through.** With the window clear, an occasional frame showed the
   window beneath through part of the surface, a strip at the top of the preview panel. GTK
   4.14 sets no opaque region for a window whose background is clear, so mutter blends the
   windows beneath into all of it. GTK 4.16 works out the opaque parts from what it draws.
   Forcing full redraws (`GSK_DEBUG=full-redraw`) did not stop the strip, so GTK's damage
   tracking is not the cause.

## Decision

1. **No window background.** The view drops the window's `background` style class, so no
   stylesheet's window colour can fill the window around the surface. `popup.css` still
   clears the window itself.
2. **A steady height on X11.** Where the window can be see-through (a compositor and an
   RGBA visual), the launcher keeps it at the popup's tallest height, 70% of the work area
   with the shadow margin, as its minimum size. The surface sits at the top with its natural
   height and grows inside the window, which never resizes while results come and go. The
   input region follows the surface's border box on every layout, so clicks in the clear
   room below reach the windows beneath. Without a compositor the window still fits the
   surface, since the room would be drawn black.
3. **An opaque surface, before GTK 4.16.** On each layout the view sets the window's opaque
   region to the surface inside its translucent 1px edge and 14px corners. It sets none
   while the surface fades in or out, when it is translucent. On GTK 4.16 and later, GTK
   does this itself and the call is not compiled.

## Alternatives considered

- **Load `popup.css` above the user priority:** it would also override the person's own
  customizations of `.torchlight` in that file, and still leave every theme rule
  `popup.css` does not set. Dropping the class is the narrow fix.
- **Add an exception to this desktop's `gtk.css`:** it fixes one machine, and the file
  belongs to the theme.
- **Keep resizing and wait for mutter:** every GNOME X11 session flickers until then.
- **Show results only after mutter has shown the new size:** X11 has no signal for when a
  compositor shows a window that resized itself.

## Consequences

- On X11 with a compositor the popup's window is taller than what is seen, clear below the
  surface; window geometry from `xdotool` or the window manager reports its full height.
- On the user's GNOME X11 desktop, 60 fps recordings no longer show the squeezed or doubled
  frames. Over a moving test pattern, nothing showed through the surface in about 75
  result updates, against three in five without the opaque region.
- `tests/gtk/test_view.c` renders the popup under a user-priority stylesheet with a red
  `.background` and checks that the window's corner stays clear. `tests/test_popup.py`
  checks that a see-through window keeps its height while the surface grows, that clicks
  below the surface pass through, and that the opaque region lies within the surface. The
  probe reports the surface's size, whether the display can show see-through windows, and
  the opaque region. The isolated runner gives the session an empty `XDG_CONFIG_HOME`, so a
  desktop's own `gtk.css` cannot restyle the popup there or break the view checks with its
  parser warnings.
