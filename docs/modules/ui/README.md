# ui

> **Status:** Implemented (M3 + Tekst look, motion, grouped results and preview panel + open windows under applications), verified: GTK4 popup, asynchronous IPC, native desktop actions, X11 window switching, shadow margin and its input region, light and dark palettes, preview details read off the main thread, Wayland sizing (headless Weston), window switching on Wayland desktops and repeated showing on Cinnamon's Wayland session (`tests/test_windows_desktops.py`)
> **Source:** `ui/gtk/{launcher,model,actions,view,path_label,selection_track,header,index_strip,preview,wayland_globals}.c`, `ui/gtk/popup.css`, `ui/gtk/palette-{light,dark}.css`, `src/bin/torchlight-gtk.c`; fonts in `third_party/fonts`; open windows in [windows](../windows/README.md)
> **Headers:** `include/torchlight/{launcher,popup,async,windows}.h`
> **Tests:** `tests/gtk/test_view.c`, `tests/unit/test_popup.c`, `tests/unit/test_async.c`, `tests/unit/test_actions.c`, `tests/unit/test_windows.c`, `tests/test_popup.py`, `tests/run_popup_checks.py`, `tests/test_popup_wayland.py`, `tests/bench/bench_popup.py`, `tests/test_popup_native.py`

The thin executable owns an opaque launcher. GtkApplication enforces one instance;
`torchlight-gtk --toggle` shows/focuses or dismisses its window. Showing clears the
query and results, shows only the search field and focuses the entry. Empty or
whitespace-only input cancels pending query work, invalidates old responses and
leaves no actionable selection. No empty-query IPC request or search-history
entry is created by the popup. Escape and focus loss
hide it. Arrow selection keeps entry focus; Enter opens; Ctrl+Enter reveals;
clicks use the same resolve path. The scroller displays eleven 40-pixel one-line
rows ([ADR 0036](../../adr/0036-one-line-results.md)).

The native surface has its own look, Tekst, as specified in
[the GUI specification](../../m3-gui-design.md) and
[ADR 0048](../../adr/0048-tekst-popup.md). `tl_popup_view` owns the presentation:
embedded CSS (`popup.css`, plus `palette-light.css` and `palette-dark.css` in a
second resource so each stays within ISO C's string literal limit). The view loads
the dark palette when the desktop prefers dark or its theme's name contains
"dark", swaps palettes when that changes, and marks the surface `dark` or `light`.
Schibsted Grotesk sets the query and names; Bedstead, a teletext face, sets the
header, folders, idle strip, footer and inline status. Both are vendored in
`third_party/fonts` and installed by `make install`; stand-ins apply when they are
missing. No library dependency is added. With a compositor, an RGBA visual and a
GPU renderer (anything but GTK's Cairo renderer, which would blur and upload the
shadow on every full redraw), `popup_view_configure` gives the surface a
transparent shadow margin (the `shadowed` class and widget margins, reported by
`popup_view_margins`); it runs after the launcher realizes the window, when the
renderer exists. The window's input region, updated on each surface layout,
excludes the margin, so clicks in it reach the windows beneath. High contrast (the
theme setting, or `GTK_THEME` as GTK resolves it) uses the theme's colors and no
shadow.

The surface stacks a page header (`header.c`: `P100`, `Torchlight`, how long the
search took in cyan ([ADR 0055](../../adr/0055-search-time-in-the-page-header.md): the
daemon's `timing.engine_us`, "3.2ms", fixed width, changed only when results land, and
left out of popups too narrow for it; the header shows no result count, which the group
headers and screen reader announcements carry) and a clock with slash seconds, ticking on each whole second while the view is active),
the search line, the idle strip, the results and the footer. The
search line is a borderless field (icon or spinner, the plain GtkEntry with a red
caret, inline feedback) above a rule that is dotted at rest and drawn in ink while
the `typing` class holds. Each `set_active` shows the next placeholder tip, the
plain prompt first. Under an empty query only, the idle strip (`index_strip.c`)
shows the index's state, the folders watched and the last scan's time from the
daemon's status reply, which the launcher requests as soon as the popup shows and
then every 2s; on one line, it leaves out the last scan, then the folders, when a
large desktop font would widen the popup. No-match/offline/limit feedback sits at
the end of the field only when it has text; `Searching…` is shown by the spinner
instead, so the entry never shifts while typing. Results and footer appear only
with matches. Entry position and X11 window top remain fixed. Chrome is measured
before limiting scroll height against the work area, including scaled/small
screens; the same measurement hides the Close, Copy path and then Show in folder
links when a large desktop font would otherwise widen the popup. The footer shows
the status, then the Fastext links: Enter Open, Ctrl Enter Show in folder, Ctrl C
Copy path and Esc Close, each behind its teletext color. Ctrl+C with no query text
selected copies the selected file's or folder's display path to the clipboard,
and `popup_view_set_copied` makes the link say "Copied" for 1.2s; with selected
text, the entry copies it as usual.

Results are grouped ([ADR 0048](../../adr/0048-tekst-popup.md)): the model lists
the groups (apps, settings, folders, files; `popup_row_group`) in the order of
their best-ranked row and keeps rank order inside each, and the launcher's list
header function puts a section header (`popup_view_group_header`: color block,
name, `popup_model_group_count`) above each group's first result. Headers are not
list rows, so list positions, arrow keys and selection retention are unchanged;
scrolling up to a group's first row brings its header into view too.

When the work area has room, the launcher makes the surface
`POPUP_VIEW_PREVIEW_WIDTH` (880) wide instead of `POPUP_VIEW_LIST_WIDTH` (680), and
`popup_view_configure` shows the preview panel (`preview.c`, in its own scroller
capped at the monitor budget) beside the results. On each selection the launcher
passes `popup_view_set_preview` the selected item, its result and the window
titles listed under an application; the view adds the row's icon. The panel copies
what it is given. A new result waits until the selection rests for 70ms, except
for new results (the launcher passes `at_once`), then its lines rise in and a GTask
worker reads what needs the disk: a file's type (from its name, through the MIME
database), size and modified time (one `g_file_query_info`), a folder's items
(names only, which GIO lists without a stat each, stopping past 999) and an
application's description (its desktop entry). The task holds the panel; a new
show, clear or dispose cancels it and its late result is dropped. Another item of
the result shown (one of its windows, New window) redraws in place and keeps the
details already read. The actions it lists follow the launcher's keys: Open or
Switch to window, Show in folder or Show desktop file, Copy path.

Each row is one line: a 24px icon, the name (its natural width up to 40
characters, end-ellipsized) and, for files and folders, the short folder,
dimmed and right-aligned to the row's end. Applications and settings
show their name alone, with no subtitle or tooltip; file rows keep the full
path as their tooltip. Each row carries its kind as a style class (`kind-app`,
`kind-setting`, `kind-folder`, `kind-file`, or `kind-window` for children). Hover
draws a hairline frame and no fill. Icons are full color: desktop icons, `folder`,
or a file type guessed from the name only (`g_content_type_guess`, no I/O;
unplaced names keep `text-x-generic`). The launcher remembers which ids are on
screen, so first results rise in top to bottom and later renders fade in only new
rows. `selection_track` wraps the list box and draws one inverse-video block
beneath transparent rows; `popup_view_select` gives the block the target row's
kind class, so its left edge takes that kind's color. Selected names, a window's
beneath its application too, take the block's ink. It glides 130ms on a
frame-clock tick callback, jumps when results first appear or relayout, and hides
at once when cleared. The view's `set_searching` crossfades the icon to a spinner
after the 120ms pending timer; `set_launching` makes the block flash twice until
the launch is accepted or fails. Showing adds an `opening` class removed by a
450ms timer: the surface drops in, the header's parts step in and the idle strip's
facts rise. `popup_view_dismiss` hides the window after a 200ms CRT switch-off;
`set_active` and destroy cancel it, and queries/actions are canceled before it
starts. The typing state keeps its 700ms hold; blur, dismissal and destruction
cancel it. Disabled GTK animations turn every effect off and make dismissal
immediate; the clock still ticks.

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
icons align with the application's name; windows use the application icon. Each
child's content carries its kind as a style class (`window`, `more-windows`,
`new-window`), so a guide line hangs from the application's icon and ends in an
elbow at New window.
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
keeps revealing its desktop file. Where the session offers no window source
there are no windows and nothing else changes.

Actions first resolve the id. A separate GTask performs desktop revision checking,
GAppInfo launch, argv xdg-open or FileManager1.ShowItems with parent fallback.
An application launches only if the desktop's own menu lists it, by the catalog's
rule (`desktop_shown_in`, [ADR 0057](../../adr/0057-applications-the-desktop-menu-lists.md)),
so GNOME Terminal starts on Cinnamon, whose menu lists it, though GIO alone would refuse.
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
and scroller height to its work area, and handles integer GTK scale factors. The
window is the 680px (or clamped) surface plus the shadow margin on each side, and
is centered as a whole, so the surface stays centered.
Clamping occurs before mapping so window-manager constraints cannot misplace
an initially oversized window at scale 2. Monitor selection uses full geometry,
so the pointer may be over a panel while placement still uses the work area.
A simulated reserved-panel regression checks this placement with the pointer
outside the work area.
On Wayland ([ADR 0049](../../adr/0049-popup-on-wayland.md)) clients cannot position
their windows, so the launcher only sizes the popup: for the monitor its surface is
on, or the first monitor until the surface has entered one (`enter-monitor` sizes it
again), with the same width rules and 70% of the monitor's height, since Wayland has
no work area. The compositor places it. `tests/test_popup_wayland.py` checks this on a
headless Weston, typing through AT-SPI, and skips without Weston. Focus on toggle
follows the compositor's activation policy. Open windows are listed on X11 and on
Wayland through the compositor's task bar protocol (Sway, Hyprland, labwc, Wayfire, niri,
COSMIC), KWin scripts (KDE Plasma) or Torchlight's GNOME Shell and Cinnamon extensions,
which the launcher turns on once; `windows_session_source` picks the source when the
launcher builds its window ([windows](../windows/README.md), ADRs
[0051](../../adr/0051-open-windows-on-gnome-wayland.md) to
[0054](../../adr/0054-open-windows-on-cinnamon-wayland.md)). Where the compositor's
`gtk_shell1` is older than version 4 (Cinnamon's), GTK 4 cannot release a hidden window's
`gtk_surface1`, and showing it again was a protocol error that disconnected the popup. There
`popup_view_set_fresh_surfaces` unrealizes the window on each hide, so each show starts a
new surface ([ADR 0054](../../adr/0054-open-windows-on-cinnamon-wayland.md);
`wayland_global_version` in `ui/gtk/wayland_globals.c` reads the version).

The resident popup picks up updates by itself
([ADR 0050](../../adr/0050-deb-and-signed-apt-repository.md)): the primary instance
watches its own executable (`/proc/self/exe`'s path, with a `GFileMonitor`) and, when the
file there is replaced by another one (a different device or inode, as dpkg's rename or a
rebuild leaves), quits at once if hidden or when `close_popup` next hides it, so the next
`--toggle` starts the new version. `torchlight-gtk --version` prints the version from
`include/torchlight/version.h`. The X11 acceptance test runs a copy of the popup and
replaces it at the end to check this.

The isolated test session owns a private Xvfb display, window manager and bus.
It does not auto-start unrelated portal/secret services. A test-only preload
observer records native GTK after-paint frames and entry-edit timestamps;
production binaries have no measurement I/O. Small-screen theme/scale checks,
error/restart recovery, no-history mode and 500k rebuild timings are repeatable
through `make test-ui-isolated` and `make bench-ui`. Native presentation tests check
highlight geometry and an observed intermediate glide, file-type icons, entrance
classes, opening/closing timers, spinner and launch states, large pasted queries,
Unicode and distinguishing paths, geometry and timer destruction, rows and child
rows with their kind classes and the selection block taking the selected row's
kind, a selected window's name taking the block's ink, application rows without
a window count, the header's search time (its
formatting, fixed width and narrow-popup rule, and no result count beside it) and ticking
clock, the idle strip's states and fitting (shown only under an empty query), the
placeholder tips, the Copy path confirmation, the light or dark palette class and
a forced dark theme, the shadow margin against the display, the renderer, the
`shadowed` class and the window size, and a `GTK_THEME` high-contrast override that
drops the shadow. The isolated session runs the native view test under GTK's Cairo
and GL renderers. The probe reports the shadow margin, and the `gl-shadow` matrix
configuration checks that the pointer reaches the window beneath in the margin but
not on the surface. The acceptance run maps two real windows with
`tests/fixtures/window_app.c` and checks Enter, choosing a window, Left/Right,
a window closed after the snapshot, an application ranked below an exact file
name and New window. They pass on a bare
X server (GTK's default theme) and on Cinnamon with Mint-Y, and the
`test-ui-isolated` matrix and AT-SPI run pass with the ADR 0048 presentation. These checks supplement the
recorded Cinnamon tests; font overrides do not validate real fractional scaling.

See [desktop setup](../../desktop-setup.md), [M3 verification](../../m3-completion.md),
[IPC](../ipc/README.md), [desktop](../desktop/README.md) and
[ADR 0015](../../adr/0015-m3-desktop-catalog-and-launcher.md) and
[review fixes](../../adr/0017-m3-snapshot-cancellation-and-acceptance.md).
