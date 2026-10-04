# ui

> **Status:** Implemented (M3), review fixes verified: GTK4 popup, asynchronous IPC, native desktop actions
> **Source:** `ui/gtk/{launcher,model,actions}.c`, `src/bin/torchlight-gtk.c`
> **Headers:** `include/torchlight/{launcher,popup,async}.h`
> **Tests:** `tests/unit/test_popup.c`, `tests/unit/test_async.c`, `tests/unit/test_actions.c`, `tests/test_popup.py`, `tests/run_popup_checks.py`, `tests/bench/bench_popup.py`, `tests/test_popup_native.py`

The thin executable owns an opaque launcher. GtkApplication enforces one instance;
`torchlight-gtk --toggle` shows/focuses or dismisses its window. Showing clears the
query and results, shows `Type to search` and focuses the entry. Empty or
whitespace-only input cancels pending query work, invalidates old responses and
leaves no actionable selection. No empty-query IPC request or search-history
entry is created by the popup. Escape and focus loss
hide it. Arrow selection keeps entry focus; Enter opens; Ctrl+Enter reveals;
clicks use the same resolve path. The scroller displays eight 58-pixel rows.

The native themed surface follows [the GUI specification](../../m3-gui-design.md).
Files and folders have symbolic icons, desktop entries use their application
icons, and settings/application subtitles identify their action type. Labels
escape controls and never become launch targets. Footer status and selection
are announced through GTK accessibility. The search entry and its internal
editable control have explicit accessible names. AT-SPI acceptance verifies
named input, editable text, result labels and selected-row state.
Oversized pastes remain editable and
show the protocol's 256-byte limit. Scores and wire metadata stay out of the UI.

The model copies bounded, validated response data, retains deliberate selection
by id and row position across lexical/final phases, and permits one retained row
outside top ten. A new request disables activation until its response arrives.
`popup_model_clear` removes rows and metadata and invalidates the request id;
late phases cannot repopulate an empty search.
Exact path/path_b64 bytes are decoded independently of safe display strings;
NUL, duplicate ids, malformed results and obsolete request ids are rejected.

IPC exchanges run on GIO async operations, with one active query and the newest
pending edit, 16 ms coalescing and a delayed Searching indicator after 120 ms.
Each uses a fresh same-user connection, so idle expiry and daemon restart do not
reuse old connection identities. Supplementary status polls have a separate
slot, preserve good results and reconnect/requery after service recovery.
Retry is keyboard accessible; Enter on its focused button follows GTK's normal
button activation rather than the result-opening shortcut.

Actions first resolve the id. A separate GTask performs desktop revision checking,
GAppInfo launch, argv xdg-open or FileManager1.ShowItems with parent fallback.
For non-UTF-8 filenames, it also opens the parent after an acknowledged reveal,
covering Nemo accepting the URI without displaying the item. An isolated D-Bus
fixture verifies the exact decoded URI bytes and the parent argv fallback.
Bus acquisition and ShowItems share the action's cancellation token; a canceled
request cannot start a new fallback. An accepted action retains its history
result, but a canceled worker completion cannot dismiss or overwrite a newer
popup search. A delayed accepted-opener fixture verifies Escape/reopen behavior.
No socket, launch filesystem or D-Bus wait blocks GTK's event loop. Accepted
launches enqueue unique-event history asynchronously and dismiss the popup;
resolve/launch errors preserve the query. History never claims external success.

Cinnamon X11 focus, native keyboard launches and repeated toggle are exercised by
the desktop test. X11 requests centering on the pointer's monitor, clamps width
and scroller height to its work area, and handles integer GTK scale factors.
Clamping occurs before mapping so window-manager constraints cannot misplace
an initially oversized window at scale 2. Monitor selection uses full geometry,
so the pointer may be over a panel while placement still uses the work area.
A simulated reserved-panel regression checks this placement with the pointer
outside the work area.
Wayland focus/placement is compositor-controlled and remains unverified.

The isolated test session owns a private Xvfb display, window manager and bus.
It does not auto-start unrelated portal/secret services. A test-only preload
observer records native GTK after-paint frames and entry-edit timestamps;
production binaries have no measurement I/O. Small-screen theme/scale checks,
error/restart recovery, no-history mode and 500k rebuild timings are repeatable
through `make test-ui-isolated` and `make bench-ui`. These checks supplement the
recorded Cinnamon tests; font overrides do not validate real fractional scaling.

See [desktop setup](../../desktop-setup.md), [M3 verification](../../m3-completion.md),
[IPC](../ipc/README.md), [desktop](../desktop/README.md) and
[ADR 0015](../../adr/0015-m3-desktop-catalog-and-launcher.md) and
[review fixes](../../adr/0017-m3-snapshot-cancellation-and-acceptance.md).
