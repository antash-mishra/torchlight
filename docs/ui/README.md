# Native popup captures

These are actual GTK widget snapshots from `tests/gtk/test_view.c`, rendered
through Cairo with animations disabled, on Cinnamon X11 with the Mint-Y-Dark theme
and the desktop's Noto Mono 13 interface font (the light capture uses Mint-Y).
The result row is fixture data; native IPC and actions are checked separately.

- [Empty popup](native-empty.png)
- [One selected result, dark](native-results.png)
- [One selected result, light](native-results-light.png)

Replay after building `build/test_popup_view` (set `GTK_THEME` to choose a theme):

```sh
GTK_THEME=Mint-Y-Dark TORCHLIGHT_TEST_VIEW_CAPTURE="$PWD/docs/ui" ./build/test_popup_view
```

[Validation results](validation.json) record the earlier Quiet System theme/scale
matrix, AT-SPI checks and native sanitizer checks; re-run
`make test-ui-isolated` (needs Xvfb and xdotool) to refresh them. The native view
test disables LeakSanitizer for GTK/font caches; ASan and UBSan still check memory
errors and timer teardown.

See [the GUI specification](../m3-gui-design.md) and
[ADR 0034](../adr/0034-theme-following-popup-with-motion.md).
