# ui

> **Status:** Implemented (M3 + theme-following popup with motion + open windows under applications), verified: GTK4 popup, asynchronous IPC, native desktop actions, X11 window switching
> **Source:** `ui/gtk/{launcher,model,actions,view,path_label,selection_track}.c`, `ui/gtk/popup.css`, `src/bin/torchlight-gtk.c`; open windows in [windows](../windows/README.md)
> **Headers:** `include/torchlight/{launcher,popup,async,windows}.h`
> **Tests:** `tests/gtk/test_view.c`, `tests/unit/test_popup.c`, `tests/unit/test_async.c`, `tests/unit/test_actions.c`, `tests/unit/test_windows.c`, `tests/test_popup.py`, `tests/run_popup_checks.py`, `tests/bench/bench_popup.py`, `tests/test_popup_native.py`

The thin executable owns an opaque launcher. GtkApplication enforces one instance;
`torchlight-gtk --toggle` shows/focuses or dismisses its window. Showing clears the
query and results, shows only the search field and focuses the entry. Empty or
whitespace-only input cancels pending query work, invalidates old responses and
leaves no actionable selection. No empty-query IPC request or search-history
entry is created by the popup. Escape and focus loss
hide it. Arrow selection keeps entry focus; Enter opens; Ctrl+Enter reveals;
clicks use the same resolve path. The scroller displays eleven 40-pixel one-line
rows ([ADR 0036](../../adr/0036-one-line-results.md)).

The native surface follows the desktop theme, as specified in
[the GUI specification](../../m3-gui-design.md) and
[ADR 0034](../../adr/0034-theme-following-popup-with-motion.md). `tl_popup_view` owns
the presentation: embedded CSS (`popup.css`, stored compressed) built on GTK's named
theme colors, the desktop font, a search field holding a symbolic icon, the plain
GtkEntry with GTK's native caret, and inline feedback. No new dependency is added.
Light/dark themes, the accent and high contrast come from the theme.

Empty search collapses to the 70px search area. No-match/offline/limit feedback
sits at the end of the field only when it has text; `Searching…` is shown by the
spinner instead, so the entry never shifts while typing. Results and footer appear
only with matches. Entry position and X11 window top remain fixed. Chrome is
measured before limiting scroll height against the work area, including
scaled/small screens; the same measurement hides the Close and then Show in
Folder hints when a large desktop font would otherwise widen the popup. The
footer shows only Enter Open, Ctrl+Enter Show in Folder and Esc Close, plus real
status messages; the result count is announced to screen readers, not shown.

Each row is one line: a 24px icon, the name (its natural width up to 40
characters, end-ellipsized) and, for files and folders, the short folder,
dimmed and right-aligned to the row's end. Applications and settings
show their name alone, with no subtitle or tooltip; file rows keep the full
path as their tooltip. Rows have no hover tint, so the gliding selection is the
only highlight. Icons are full color: desktop icons, `folder`, or a file type guessed
from the name only (`g_content_type_guess`, no I/O; unplaced names keep
`text-x-generic`). The launcher remembers which ids are on screen, so first
results cascade in and later renders fade in only new rows. `selection_track`
wraps the list box and draws one plain highlight beneath
transparent rows; it glides 130ms on a frame-clock tick callback, jumps when
results first appear or relayout, and hides at once when cleared. The view's
`set_searching` crossfades the icon to a spinner after the 120ms pending timer;
`set_launching` flashes the selected row until the launch is accepted or fails.
Showing adds an `opening` class (fade-in and torch sweep) removed by a 700ms timer.
`popup_view_dismiss` hides the window after a 110ms fade; `set_active` and destroy
cancel it, and queries/actions are canceled before it starts. The typing glow keeps
its 700ms hold and 420ms fade; blur, dismissal and destruction cancel it. Disabled
GTK animations turn every effect off and make dismissal immediate.

`path_label` shows a short folder: `~` for the home directory and at most the
last two folders (`POPUP_SHORT_FOLDERS`, e.g. `~/…/docs/modules`); folders at
most two levels deep show whole. When `popup_model_distinct_prefix` reports a
same-kind, same-name row whose last two folders are the same, it keeps the
folder where the two first differ, then the last one
(`~/…/27.0.12077973/…/include`). If the short form is still too wide, Pango
measurements drop earlier folders, then shorten a giant final folder in the
middle with bounded width probes. Tooltips and accessible labels retain the full safe path; action
resolution still uses exact bytes. Font/width changes recalculate displayed paths.
The native build contains no shader, PNG art or experimental browser renderer.

Files and folders have file-type icons and desktop entries use their application
icons. Labels
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
Retry is keyboard accessible; Enter in the entry retries an unavailable search with no selection; Enter on its focused button follows GTK's normal
button activation rather than the result-opening shortcut.

Open application windows are listed beneath their result
([ADR 0035](../../adr/0035-open-windows-under-applications.md)). Each show
reads the [windows](../windows/README.md) snapshot in an idle callback after
presenting, and hiding drops it. The model gives each window to the displayed
application with the strongest evidence and flattens the list into items:
result, up to three windows (most recent first), "Show N more windows", then
"New window". Every application with windows lists them wherever it ranks;
its own row is unchanged and shows no count. Left on a child collapses its application and
selects it; Right, with the caret at the end of the query, lists them again or
reveals the rest from "Show N more windows". Expansion choices last for
one request id, and retained selection across phases and window refreshes is
keyed by (result id, item kind, window handle). Children are 32px rows whose
icons align with the application's name; windows use the application icon.
Up/Down move through every item, and select/launch/flash work on child rows
unchanged.

Enter on an application with windows brings its most recent window forward;
Enter on a window brings that one forward. Neither needs a resolve: the window
system answers at once, the popup records an asynchronous open of the
application (so personal ranking learns from it) and closes. A window that has
left the client list reports "That window has closed" and refreshes the list.
New window resolves like a launch, then the worker runs the entry's
`new-window` or `new-empty-window` desktop action, or a normal launch without
one. More windows lists every window in place. Ctrl+Enter on an application
keeps revealing its desktop file. Without X11 there are no windows and nothing
else changes.

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
through `make test-ui-isolated` and `make bench-ui`. Native presentation tests check
highlight geometry and an observed intermediate glide, file-type icons, entrance
classes, opening/closing timers, spinner and launch states, large pasted queries,
Unicode and distinguishing paths, geometry and timer destruction, child rows and
application rows without a window count. The acceptance run maps two real windows with
`tests/fixtures/window_app.c` and checks Enter, choosing a window, Left/Right,
a window closed after the snapshot, an application ranked below an exact file
name and New window. They pass on a bare
X server (GTK's default theme) and on Cinnamon with Mint-Y, and the
`test-ui-isolated` matrix and AT-SPI run pass with the ADR 0034 presentation. These checks supplement the
recorded Cinnamon tests; font overrides do not validate real fractional scaling.

See [desktop setup](../../desktop-setup.md), [M3 verification](../../m3-completion.md),
[IPC](../ipc/README.md), [desktop](../desktop/README.md) and
[ADR 0015](../../adr/0015-m3-desktop-catalog-and-launcher.md) and
[review fixes](../../adr/0017-m3-snapshot-cancellation-and-acceptance.md).
