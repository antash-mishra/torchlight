# Native popup captures

These are actual GTK snapshots of the popup window from `tests/gtk/test_view.c`,
rendered through Cairo with animations disabled. They use Mint-Y icons and the
desktop's 13pt interface font size; the stylesheet sets Ubuntu Sans and the
night palette of [ADR 0048](../adr/0048-torch-in-the-dark-popup.md), so the GTK
theme only matters for high contrast. They were rendered on a private X display
with Metacity compositing and GTK's GL renderer (software Mesa), so the window
has its transparent halo margin: the PNGs are transparent around the surface.
The font and icon theme were set in a temporary `gtk-4.0/settings.ini`. The rows
are fixture data in the one-line layout of
[ADR 0036](../adr/0036-one-line-results.md); native IPC and actions are checked
separately.

- [Empty popup](native-empty.png)
- [Results: an application with a window, files and a folder](native-results.png)
- [The same results in high contrast](native-results-contrast.png), which uses the
  theme's colors and no halo

Replay after building `build/test_popup_view`, on a display with a compositing
window manager (for example `metacity --sm-disable` on a private Xvfb display)
and with GL rendering, so the halo shows:

```sh
GSK_RENDERER=ngl GTK_THEME=Mint-Y-Dark TORCHLIGHT_TEST_VIEW_CAPTURE="$PWD/docs/ui" ./build/test_popup_view
```

For the high-contrast capture, run the same command with `GTK_THEME=HighContrast`
and a temporary capture directory, and keep its `native-results.png` as
`native-results-contrast.png`.

[Validation results](validation.json) record the theme/scale matrix, AT-SPI checks
and native sanitizer checks for this presentation (`make test-ui-isolated` and
`python3 tests/run_popup_checks.py --a11y`; both need Xvfb and xdotool). The native
view test disables LeakSanitizer for GTK/font caches; ASan and UBSan still check
memory errors and timer teardown.

See [the GUI specification](../m3-gui-design.md) and
[ADR 0048](../adr/0048-torch-in-the-dark-popup.md).
