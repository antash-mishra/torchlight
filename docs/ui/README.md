# Native popup captures

These are actual GTK snapshots of the popup window from `tests/gtk/test_view.c`,
drawn to PNG through Cairo with animations disabled (the preview capture instead waits
for its lines to finish rising). They use Mint-Y icons and the
desktop's 13pt interface font size; the stylesheet sets Schibsted Grotesk and
Bedstead (installed from `third_party/fonts`) and the Mørketid palette of
[ADR 0048](../adr/0048-tekst-popup.md), chosen because the Mint-Y-Dark theme's name
says dark. They were rendered on a private X display with Metacity compositing and
GTK's GL renderer (software Mesa), so the window has its transparent shadow
margin: the PNGs are transparent around the surface.
The font and icon theme were set in a temporary `gtk-4.0/settings.ini`. The rows
are fixture data in the one-line layout of
[ADR 0036](../adr/0036-one-line-results.md); native IPC and actions are checked
separately.

- [Empty popup](native-empty.png)
- [Results: an application with a window, files and a folder](native-results.png)
- [The same results in high contrast](native-results-contrast.png), which uses the
  theme's colors and no shadow
- [The preview panel](native-preview.png) beside the same results on an 880-pixel
  popup, describing the selected file (a temporary file the test writes, so its
  size and time are the test's)

Replay after building `build/test_popup_view`, on a display with a compositing
window manager (for example `metacity --sm-disable` on a private Xvfb display)
and with GL rendering, so the shadow shows:

```sh
GSK_RENDERER=ngl GTK_THEME=Mint-Y-Dark TORCHLIGHT_TEST_VIEW_CAPTURE="$PWD/docs/ui" ./build/test_popup_view
```

For the high-contrast capture, run the same command with `GTK_THEME=HighContrast`
and a temporary capture directory, and keep its `native-results.png` as
`native-results-contrast.png`.

[Validation results](validation.json) record the theme/scale matrix, a wide-screen run
with the preview panel, AT-SPI checks and native sanitizer checks for this
presentation (`make test-ui-isolated` and `python3 tests/run_popup_checks.py --a11y`;
both need Xvfb and xdotool). The native view test disables LeakSanitizer for GTK/font
caches; ASan and UBSan still check memory errors and timer teardown.

## The README's demo

[demo.gif](demo.gif) records the real popup and daemon on a private X display, typing
a few searches the way a person would. Everything in it is made up: a home folder at
`/home/demo`, a handful of applications and settings panels, and two Terminal windows,
so nothing from the recording machine shows. It starts on the first search's results,
the only frame a viewer with animation off sees, and loops from there. To record it again
after `make`:

```sh
python3 scripts/record_demo.py
```

It needs no root (it runs in a user and mount namespace), but needs Xvfb, metacity,
xdotool, xsetroot, ffmpeg, the GTK 4 Python bindings and the popup's fonts. It records
with GTK's Cairo renderer, which keeps up with typing on a busy machine but draws no
shadow.

See [the GUI specification](../m3-gui-design.md) and
[ADR 0048](../adr/0048-tekst-popup.md).
