# 0055. The page header shows the search time instead of the result count

- **Status:** Accepted
- **Date:** 2026-10-14

## Context

Tekst's page header ([ADR 0048](0048-tekst-popup.md)) reads like a teletext header: "P100",
"Torchlight", the result count and a clock whose seconds tick. The user asked to show how
long each search took there, for the look rather than for measuring: teletext headers are
made of live readouts, and a fast search is part of the product's character. Seeing it, they
asked to drop "10 results" from the header as unnecessary: the group headers already count
each kind of result.

Every response from the daemon already carries `timing.engine_us`, the time its query took
(the catalog search and merging applications), usually 1 to 5 ms. The time a person feels,
from key press to results on screen, is about 30 ms, mostly drawing.

## Decision

1. **A field of its own** before the date and clock, in teletext's cyan beside the clock's
   yellow, where the result count was. The user chose this over the time inside the count's
   words, an inverse-video tag and a rolling counter.
2. **No result count in the header.** The group headers count each kind of result, and
   screen readers still hear the total, announced when results arrive.
3. **The daemon's search time**, labeled by its unit: "3.2ms" below ten milliseconds,
   "12ms" below a second, "1.2s" above, "<0.1ms" under 50 microseconds. It is what the
   number says, the time the search took, not the time to the screen.
4. **Calm:** the field changes only when results land, keeps the width of its longest
   text ("<0.1ms") so the header never shifts, and clears with the query. Like the rest of
   the header it is hidden from assistive technology, so screen readers do not read a
   changing number. It steps in with the header's other parts when the popup opens.
5. **Decoration gives way.** When the popup is configured for a width at which the header
   would not fit with the time beside the widest clock (a two-digit day's last second), the
   field is hidden, as the footer drops its least essential links, so it never widens the
   popup. The header is measured with that widest clock in place, since the clock has not
   started when the popup is configured.

## Alternatives considered

- **Inside the count** ("10 results in 3ms"): quieter, but it reads as a sentence rather than
  a readout.
- **An inverse-video tag** (dark digits on a cyan block): the loudest option, and it would
  compete with the selected row's block.
- **A rolling counter** like teletext's page search: more motion than a 30 ms search can
  show.
- **The time to the screen:** what a person feels, but most of it is drawing, and it would
  label the drawing as search.

## Consequences

- The header shows each search's time, in cyan, until the next results land; narrow
  popups leave it out. The X11 test matrix's high-contrast run at scale 2 (a 400-pixel
  screen) caught the widening before this rule, when the header still held the count.
- The header no longer says how many results there are; `popup_header_set_count` is gone.
- `popup_model_search_us` keeps the last response's time; `popup_header_set_search_time`
  formats it. `tests/unit/test_popup.c` covers responses with, without and with a malformed
  time; `tests/gtk/test_view.c` covers the formatting, that every value keeps the field's
  width, and that a narrow popup hides it.
