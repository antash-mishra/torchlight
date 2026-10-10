# Native popup captures

These are actual GTK widget snapshots from `tests/gtk/test_view.c`, rendered
through Cairo with animations disabled, with the Mint-Y-Dark theme (the light
capture uses Mint-Y), Mint-Y icons and the desktop's Noto Mono 13 interface font.
They show the one-line rows of [ADR 0036](../adr/0036-one-line-results.md) and were
rendered on a private X display, with the font and icon theme set in a temporary
`gtk-4.0/settings.ini`. The result row is fixture data; native IPC and actions are
checked separately.

- [Empty popup](native-empty.png)
- [One selected result, dark](native-results.png)
- [One selected result, light](native-results-light.png)

Replay after building `build/test_popup_view` (set `GTK_THEME` to choose a theme):

```sh
GTK_THEME=Mint-Y-Dark TORCHLIGHT_TEST_VIEW_CAPTURE="$PWD/docs/ui" ./build/test_popup_view
```

[Validation results](validation.json) record the theme/scale matrix, AT-SPI checks
and native sanitizer checks for this presentation (`make test-ui-isolated` and
`python3 tests/run_popup_checks.py --a11y`; both need Xvfb and xdotool). The native
view test disables LeakSanitizer for GTK/font caches; ASan and UBSan still check
memory errors and timer teardown.

See [the GUI specification](../m3-gui-design.md) and
[ADR 0034](../adr/0034-theme-following-popup-with-motion.md).
