# 0048. Tekst: the popup's own look

- **Status:** Accepted; implemented: the look, type, motion, grouped results and the
  preview panel.
- **Date:** 2026-10-13

## Context

ADR 0034 made the popup follow the desktop theme: its named colors, its accent
and its interface font. In use it read as plain. On the target machine the
interface font is Noto Mono 13, so every label rendered in monospace, the colors
were Mint-Y's flat grey and green, and the window had no shadow at all, because
Muffin draws none for windows with an alpha channel. The user asked for a popup
that stands apart from other windows, with good typography, styled edges and
motion, while staying as clean as it was.

A first version of this ADR, "a torch in the dark" (a night-blue surface, an
amber rim and halo, Ubuntu Sans), looked like a generic dark search bar with an
orange glow on the real desktop. Three further studies that changed mostly
colors and borders (`docs/designs/popup-looks/`) were rejected as "just changing
the background color". The user asked for typography-led design, "Nordic
minimalism meets retro", with motion, light and dark palettes, a results layout
and a preview panel, and chose this design and its Kringkasting type from an
interactive prototype (`docs/designs/tekst/`).

## Decision

**Tekst** sets teletext's information design on quiet Nordic surfaces. The Nordic
broadcasters kept teletext running long after everyone else (NRK Tekst-TV, SVT
Text and Yle Teksti-TV still publish pages), so the two halves share a history:
page numbers, color-coded sections, inverse-video highlights and four colored
links, on calm surfaces with generous space. Color is rare and always means
something; it appears as small square blocks, never as a wash.

1. **Palettes.** Snø (light) and Mørketid (dark) live in `palette-light.css` and
   `palette-dark.css` as `@define-color` tokens; the main stylesheet names them.
   GTK 4.14 has no color-scheme query, so the view loads the dark palette when the
   desktop prefers dark or its theme's name contains "dark" (Mint-Y-Dark,
   Adwaita:dark), and swaps palettes when that changes.

   | Token | Snø | Mørketid | Use |
   |---|---|---|---|
   | surface | `#f4f5f1` | `#171a1d` | background |
   | panel | `#ebede7` | `#1c2024` | the preview panel, tooltips |
   | ink | `#1d2226` | `#e8ebe6` | text, the selection block |
   | muted | `#626b72` | `#979fa5` | system text |
   | faint | `#a0a7ab` | `#5f686e` | placeholder, dotted rule |
   | red | `#c2463e` | `#e26b61` | caret, Open |
   | green | `#4e8a47` | `#87b872` | windows, Show in folder |
   | yellow | `#b8811a` | `#e6b04a` | clock, folders, Copy path |
   | cyan | `#24838f` | `#5fb6c1` | apps, Close |
   | magenta | `#94508d` | `#c78cc1` | settings |
   | blue | `#3a5f93` | `#7c9fd3` | files |

2. **Type (Kringkasting).** Schibsted Grotesk, the Norwegian news grotesque of the
   Schibsted media group, sets what you read: the query at 1.6em, weight 500 and
   −0.025em tracking, and result names. Bedstead, a recreation of the teletext
   character set, sets what the system says: the header, folders, the idle strip,
   the footer and inline status. Both are vendored in `third_party/fonts`
   (Schibsted Grotesk under the SIL OFL, Bedstead public domain) and installed by
   `make install` to `share/fonts/torchlight`; Ubuntu Sans, Cantarell or Noto Sans,
   and Ubuntu Sans Mono, stand in when they are missing. Sizes stay relative to the
   desktop font.
3. **Header.** Row 0 is a teletext page header: "P100", "Torchlight", the result
   count, and a clock with teletext's slash seconds (`Tue 13 Oct  21:52/07`) that
   ticks while the popup is shown.
4. **Query line.** The field has no box. Its placeholder rotates on each show: the
   plain prompt, then tips that teach the forgiving search ("Try raedme: typos are
   fine"). A rule beneath it is dotted at rest and drawn in ink while typing. The
   caret is red.
5. **Idle strip.** Under an empty query, one line says what the index is doing:
   up to date, updating or degraded; the folders watched; the last scan's time.
   Each fact has a color block. A large desktop font drops the last scan, then the
   folders, rather than widen the popup. The launcher asks for status as soon as
   the popup shows.
6. **Selection.** An inverse-video block: ink with surface-colored text. Its left
   edge takes the row's kind color (apps cyan, settings magenta, folders yellow,
   files blue, windows green). Hover draws a hairline frame and no fill, so the
   block stays the only filled thing.
7. **Fastext footer.** The status, then four colored links: red Enter Open, green
   Ctrl Enter Show in folder, yellow Ctrl C Copy path and cyan Esc Close. Copy path
   is new: Ctrl+C with no query text selected copies the selected file's or
   folder's path as shown, and the link says "Copied". Close, Copy path and then
   Show in folder give way when a large font would widen the popup.
8. **Shadow.** A soft drop shadow, drawn in a transparent margin (20px at the
   sides, 10 above, 28 below) whose clicks reach the windows beneath. It needs a
   compositor and GTK's GL or Vulkan renderer: in alternating runs on a private X
   server, blurred shadows under the software (Cairo) renderer raised the median
   input-to-results paint from 32ms to 51ms, against 34ms without them.
9. **High contrast** uses the theme's own colors, with no shadow.
10. **Grouped results.** Results are listed in sections, Apps, Settings, Folders and
    Files, each group placed where its best row ranks and its rows in rank order,
    so the strongest result is still first and Enter still opens it. A section
    header (the group's color block, name and count, in Bedstead) stands above
    each group. Headers are not list rows, so arrow keys, selection retention and
    the windows listed under applications work as before.
11. **Preview panel.** When the work area has room for an 880-pixel popup, a
    320-pixel panel on the panel color sits right of the results and describes the
    selected item: its icon, its name in bold Schibsted Grotesk and its kind behind
    its color block (a file's type, from its name), then what Torchlight knows (a
    file's size, modified time and folder; a folder's items, modified time and
    place; an application's or settings panel's description and an application's
    open windows), then the selected item's actions with their keys. Narrower work
    areas keep the 680-pixel list alone. What needs the disk (a stat, a folder
    listing of names only, up to 999, and a desktop entry) is read on a worker
    thread once the selection rests for 70ms, so arrow keys and typing never wait
    for it; new results show their first row's preview at once, so the window grows
    once. Another item of the same result, such as one of its windows, updates the
    panel in place.

Motion follows ADR 0034's rules: nothing waits for an effect, decorative motion
never loops, and disabled GTK animations turn it all off (the clock still ticks).
The opening is the only motion that is not a response to input.

- **Open:** the surface drops into place (230ms). The header's parts step in from
  left to right, as if transmitted (200ms in four steps, 40ms apart), and the idle
  strip's facts rise one after another.
- **Typing:** the rule draws in ink (220ms); the search icon brightens.
- **Results:** rows rise into place top to bottom, 16ms apart (190ms each).
- **Arrow keys:** the block glides (130ms); its edge changes to the new row's
  color and the row's text inverts.
- **Preview:** when the selection rests on a new result, the panel redraws line by
  line, like a page arriving (170ms each, 22ms apart).
- **Enter:** the block flashes twice, teletext's flash attribute (300ms).
- **Close:** a CRT switch-off: the page squeezes to a bright line, then the line
  shrinks away (200ms).

## Alternatives considered

- **Keep following the theme.** It blends in, which is what the user wanted to
  change.
- **The torch in the dark.** The first version of this ADR read as a generic dark
  search bar on the real desktop.
- **Beam, Reticle and Monolith.** Restyles of color and edges; rejected because
  the typography and structure stayed the same.
- **Raw teletext** (Bedstead throughout, pure broadcast colors on black). Authentic
  but loud.
- **Other type in the Tekst prototype:** Dobbelhøyde (Bedstead throughout, with a
  double-height query), Punktmatris (a dot-matrix query with Familjen Grotesk) and
  Ubuntu Sans. The user chose Kringkasting because it keeps the popup simple.
- **A fourth idle fact, "favorites learned".** At a 13pt desktop font the strip no
  longer fit one line, and a wrapping flow box made the window reserve room for
  four lines.
- **Registering the bundled fonts at runtime.** That would add fontconfig as a
  direct dependency; installing them is enough.
- **A light following the rim, full-size gradient rims, or blurred halos under
  software rendering.** Each cost several milliseconds per frame on the private X
  server (a conic gradient about 15ms).

## Consequences

- The fonts install with Torchlight; without them the popup falls back to the
  stand-ins, so the look degrades but nothing breaks.
- Light or dark follows the desktop's preference or theme name; a preference set
  only through the settings portal (libadwaita's color scheme) is not seen.
- The empty popup is taller: the header and idle strip add about 55px at a 13pt
  desktop font. Tests check which parts show in each state instead of one height.
- The styles are split into two embedded resources (the stylesheet and the
  palettes) to keep each within ISO C's 4,095-byte string literal limit.
- Ctrl+C now copies a path when no query text is selected; with a selection it
  still copies the text.
- Grouping can list a row below a lower-ranked row of another kind: with a file,
  an app and a second file, both files come before the app. The best result, and
  so Enter, is unchanged.
- On wide work areas the popup is 880 pixels wide instead of 680. With few results
  its height follows the preview panel, which scrolls past the monitor budget.
- The wider popup and the panel cost time under software rendering: in alternating
  runs on the private X server with GTK's Cairo renderer, the median
  input-to-results paint was about 38–41ms with the panel, 32ms for an 880-pixel
  list without it and 29ms for the 680-pixel list. Unlike the shadow, the panel is content, so it stays
  under every renderer. GPU rendering, as on the target desktop, was not measured.
- The preview reads the selected item from disk, which the popup never did before:
  a stat, up to 1,000 names of a folder and a desktop entry, on a GIO worker thread
  and cancelled when the selection moves. On a slow mount the details arrive late;
  nothing waits for them.
- This supersedes ADR 0034's colors, font, field ring and highlight tint, and ADR
  0036's footer hints. ADR 0034's motion rules, ADR 0035's open windows and ADR
  0036's one-line rows stand.
