# 0048. A torch in the dark: the popup's own look

- **Status:** Accepted; implemented
- **Date:** 2026-10-13

## Context

ADR 0034 made the popup follow the desktop theme: its named colors, its accent
and its interface font. In use it read as plain. On the target machine the
interface font is Noto Mono 13, so every label rendered in monospace. The
colors were Mint-Y's flat grey and green. The only edge was a faint 1px line,
and the window had no shadow at all, because Muffin draws none for windows with
an alpha channel. The user asked for a popup that looks futuristic and stands
apart from other windows, with good typography, styled borders and motion,
while staying as clean as it was.

## Decision

The popup takes its look from its name: **a torch in the dark**. One deep
surface, lit only where attention is: the rim when the popup is summoned, the
horizon while typing, and the beam on the selected result. Light starts amber at
the torch (top left) and cools to indigo where it fades (bottom right), and
every gradient follows that direction. The layout of ADR 0036 is unchanged.

1. **Palette.** Always dark, from `@define-color` tokens at the top of
   `popup.css`: Night `#0e1424` for the surface (`#172036` at its top), Ink
   `#e8ecf8` for text, Haze `#8a94b4` for secondary text, Lumen `#ffb547` for
   light and Dusk `#5c6cff` where light fades. Secondary text keeps a contrast
   of about 6:1 on Night.
2. **Type.** Ubuntu Sans, a variable font installed with Mint and Ubuntu, with
   Ubuntu, Noto Sans and Cantarell as fallbacks. The query is Light at 1.5em,
   names are Regular, folders use the font's condensed width at 0.9em and
   keycaps are Medium. Sizes stay relative, so the desktop font's size and
   scaling still apply. Nothing is monospace and nothing is below 0.9em.
3. **Rim and halo.** The 1px border is transparent and shows a lit rim, from
   amber at the top left to indigo at the bottom right. The rim is four thin
   gradient strips, one per edge, beneath an opaque interior layer, so it shows
   only through the border. Two strips meet in the same color at each corner.
   When a compositor and an RGBA visual are available and GTK renders on the
   GPU, the view reserves a transparent **halo margin** around the surface
   (20px at the sides, 18 above, 28 below). The halo spills amber light from
   the torch's corner and indigo from the far one, above a soft shadow. The
   launcher grows the window by the margin and centers the window, so the
   surface stays centered. The surface's input region excludes the margin, so
   clicks there reach the windows beneath. Otherwise the surface fills the
   window as before, still with its lit rim. GTK's software (Cairo) renderer
   blurs each shadow and, on X11, uploads it on every full redraw. In
   alternating runs on a private X server with Metacity, the halo raised the
   median input-to-results paint from 32ms to 51ms; with the halo limited to
   GPU rendering the popup measured 34ms against master's 33ms. GTK's GL
   renderer caches blurred shadows.
4. **Horizon.** The search field has no box. A 1px line under the query
   separates it from the results. It is absent on the empty popup, Haze while
   results show, and lit amber to indigo with a soft glow while typing, with the
   same 700ms hold as before. The search icon lights with it.
5. **Beam.** The selection highlight is a 3px amber rail at its left edge with a
   glow around it, and light that falls off to the right. The selected name
   warms to `#fff4e2`. Other rows' icons are dimmed by 10%, and the selected
   row's icon is not.
6. **Children.** An application's windows and actions hang from a guide line
   beneath its icon, and the line ends in an elbow at New window. Child rows
   carry their kind as a style class (`window`, `more-windows`,
   `new-window`).
7. **Footer.** A slightly darker base plate holds the status and the keycap
   hints.
8. **High contrast** drops the night palette, the halo, the glows and the guide
   lines, and uses the theme's colors. It applies when the theme name, or the
   `GTK_THEME` override that GTK honors before the setting, contains
   `HighContrast`.

Motion keeps ADR 0034's rules: it never delays input, results or actions, it
never loops, and disabled GTK animations turn all of it off. The opening is
the only motion that is not a response to input:

- **Ignition.** The surface rises 10px from 97% scale while fading in (200ms).
  The halo flares and settles (640ms), and the torch sweep becomes one band of
  light crossing the whole popup (560ms after 120ms). Beneath the interior it
  lights the rim's edges, and on the field it runs along the horizon line, so
  the band shows only on those lines.
- **Typing** lights the horizon and the search icon.
- **Results** are revealed from the left: 10px and a fade, 160ms each, 18ms
  apart (eight steps at most). Later new rows still only fade in.
- **Selection.** The beam glides 130ms, as before.
- **Launch.** The beam brightens across the row and the icon pulses.
- **Dismiss.** A 110ms fade and slight shrink.

## Alternatives considered

- **Keep following the theme with a stronger accent and a chosen font.** That
  is still Mint's grey and green, and the popup still blends in, which is what
  the user wanted to change.
- **Cyan and magenta neon.** This is the stock "futuristic" look, and it has
  nothing to do with the name. Amber light on night blue follows from the torch.
- **A light variant.** GTK 4.14 has no CSS custom properties or color-scheme
  queries, so a second palette would duplicate every rule. The torch also only
  reads in the dark. High contrast stays the accessibility path.
- **A comet circling the rim** (a rotating conic gradient). GTK interpolates the
  rotation, but its software renderer takes about 15ms per frame for a conic
  gradient this size. The sweep costs a small fraction of that and repeats the
  torch sweep that already identifies the popup.
- **One full-size rim gradient beneath the interior.** It is simple, but it is
  painted over the whole surface for a 1px ring. With the first draft's three
  full-size layers, a full software render of the results popup took 7.1ms,
  against 1.2ms with every effect off. Edge strips and a solid interior bring
  it to 3.6ms. GTK's GL renderer, which the target desktop uses, draws all of
  these on the GPU.
- **A bundled font** (for example Inter). That is a new dependency, and GTK CSS
  cannot load fonts from resources. Ubuntu Sans ships with the target desktop
  and has fallbacks.
- **Frosted glass.** GTK has no backdrop blur, and see-through text over a busy
  wallpaper is hard to read. The surface is opaque.
- **GTK's client-side decoration shadows.** They would bring in the
  decorated-window machinery. A margin plus an input region on the undecorated
  window does the same job.
- **A halo of stacked unblurred rings** (box-shadows with growing spread and
  falling alpha), cheap in every renderer. The rings showed as visible contour
  bands around the corners.
- **A halo drawn with gradients in the window's margin** instead of blurred
  shadows, to keep it under software rendering. GTK cannot offset positions
  from the right or bottom edge except with `calc()`. Edge glows built from
  ellipses either stop short at the window's edge or meet at visible seams in
  the corners. Software-rendered sessions keep the lit rim instead.

## Consequences

- The popup no longer changes with the light or dark theme or the accent color;
  only high contrast changes it.
- With a compositor and GPU rendering the window is 720 wide for a 680 surface.
  Its top stays at one fifth of the work area, so the surface sits 18px lower
  than before. The halo is decided when the launcher configures the realized
  window, because only then does it have a renderer. Geometry checks use the
  surface for the popup's size and the window for its placement.
- The isolated checks force GTK's Cairo renderer, so they see no halo. The
  native view test therefore also runs under GL (software Mesa on the private
  display) and checks the halo against the display and renderer. The probe
  reports the halo margin, and a `gl-halo` matrix configuration checks that the
  margin passes the pointer through while the surface does not.
- GL emulated in software (Mesa llvmpipe, as on machines without a GPU driver)
  counts as GPU rendering and keeps the halo, and its frames are slow either
  way. In alternating runs on the private display the halo raised its median
  input-to-results paint from 34ms to 56ms, with run medians from 34 to 126ms.
  GTK offers no way to tell software GL apart, so this is left as a known cost;
  `GSK_RENDERER=cairo` drops the halo on such machines.
- Desktops without Ubuntu Sans get the fallback fonts. Folders are condensed
  only where the font has a condensed width.
- The compressed stylesheet is 3.6 KB of the 4,095-byte ISO C string limit. A
  much larger stylesheet would need splitting into two resources.
- The field's and the beam's layer stacks keep the same shapes in every state.
  GTK then interpolates their colors in transitions instead of crossfading
  whole images, which would flash a parked sweep band back into view. The
  surface fills its sweep slot only while opening, and it has no transition.
- This supersedes ADR 0034's colors, font, field ring and highlight tint. Its
  motion contracts (opening and closing timers, the glide, the typing hold, the
  cascade and fade, the spinner and the launch flash) and ADR 0036's one-line
  rows stand.
