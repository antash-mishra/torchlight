# Quiet System native captures

These are actual GTK widget snapshots from `tests/gtk/test_view.c`, rendered
through Cairo on the private X11 test display, with animations disabled.
The result row is fixture data; native IPC and actions are checked separately.
No shader or browser artwork is included in these images or the GTK build.

- [Empty popup](native-empty.png)
- [One selected result while editing](native-results.png)

Replay after building `build/test_popup_view`:

```sh
TORCHLIGHT_TEST_VIEW_CAPTURE="$PWD/docs/ui" python3 tests/run_popup_checks.py
```

[Validation results](validation.json) include the theme/scale matrix, AT-SPI
checks and native sanitizer checks. The native view test disables LeakSanitizer
for GTK/font caches; ASan and UBSan still check memory errors and timer teardown.

See [the GUI specification](../m3-gui-design.md) and [ADR 0027](../adr/0027-quiet-system-native-popup.md).
