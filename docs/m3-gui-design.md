# M3 GTK popup design

**Status: implemented; see [M3 verification](m3-completion.md) for acceptance coverage.** Follow the launcher behavior
in [PLAN.md](../PLAN.md#milestones) and the visual/interaction specification below.
The native presentation is Tekst, teletext's information design on Nordic light and
dark palettes, with bounded motion, accepted in
[ADR 0048](adr/0048-tekst-popup.md). It replaced the theme-following
look of [ADR 0034](adr/0034-theme-following-popup-with-motion.md), which had replaced the
Quiet System look of [ADR 0027](adr/0027-quiet-system-native-popup.md). The
[older interactive preview](m3-gui-preview.html) records the original M3 study, and the
[Tekst prototype](designs/tekst/index.html) the current one;
[current native captures](ui/native-empty.png) reflect the implemented layout.

## Presentation

A compact floating search window: a page header, one search line, a vertical result
list and a footer of key links. The current target session is **Cinnamon on X11**.
Validate activation and focus there first; document Wayland results separately during M3.

| Element | Specification in logical pixels |
|---|---|
| Window | Surface 880 wide, with the preview panel, when the monitor work area minus 48 allows it; otherwise 680 wide, clamped to the work area minus 48. Maximum height 70% of the work area. No title bar or resize handle. With a compositor and GTK's GL or Vulkan renderer the window adds a transparent shadow margin (20 at the sides, 10 above, 28 below) whose clicks pass to the windows beneath. |
| Placement | Horizontally centered on the active monitor, near its upper third. Placement is a window-system request, verified in the target session. |
| Surface | Snø (light) or Mørketid (dark), following the desktop's dark preference or theme name; colors are listed in ADR 0048. Outer radius 14, a 1px hairline edge and, with the shadow margin, a soft drop shadow. Schibsted Grotesk for the query and names; Bedstead for the header, folders, idle strip, footer and inline status; Ubuntu Sans, Cantarell or Noto Sans and Ubuntu Sans Mono stand in when missing. Sizes are relative to the desktop font. High contrast uses the theme's colors and no shadow. |
| Header | Row 0: `P100`, `Torchlight`, the result count and a clock with slash seconds (`Tue 13 Oct  21:52/07`) that ticks while shown, in Bedstead at 0.8em. |
| Search | A 52-high borderless field: an 18px symbolic search icon, then the query at 1.6em, weight 500, with a red caret. The placeholder rotates on each show: `Search apps, files and settings`, then tips (`Try raedme: typos are fine`, `Try prjnts: initials work`, `Try work notes: folder names count`). A rule beneath is dotted at rest and drawn in ink while typing. No clear icon. No-match/offline feedback and a red Retry sit at the field's end. |
| Idle strip | Under an empty query only: the index's state (up to date, updating or degraded), the folders watched and the last scan's time, each behind a color block, on one line; the last scan, then the folders, give way when a large font would widen the popup. |
| Results | Request ten; show up to eleven one-line rows before scrolling, reduced by the monitor budget. Results are grouped (Apps, Settings, Folders, Files), each group where its best row ranks and rank order inside it, under a section header with the group's color block, name and count (Bedstead, 0.75em). Each row 40 high, with a 24px full-color icon and 12px gaps. Hover draws a hairline frame and no fill. |
| Row text | Name in the body size, end-ellipsized past 40 characters when space is short. Files and folders then show their short folder in Bedstead at 0.75em, dimmed and right-aligned: `~` for home and at most the last two folders, plus the folder that tells same-named rows apart when they would otherwise look identical. Apps and settings show their name alone. See [ADR 0036](adr/0036-one-line-results.md). |
| Selection | One inverse-video block under the rows glides between rows: ink, with the row's text in the surface color. Its 4px left edge takes the row's kind color: apps cyan, settings magenta, folders yellow, files blue, windows green. An application's windows and actions sit beneath it with their icons under its name. High contrast uses the theme's selection colors. |
| Preview | On an 880-wide popup with results, a 320-wide panel right of the list, on the panel color behind a hairline: a 48px icon, the name (1.3em, weight 700) and the kind behind its color block (a file's type, from its name); then a file's size, modified time and folder, a folder's items (hidden ones aside, up to 999+), modified time and place, or an application's or settings panel's description and an application's open windows (up to three, then how many more); a rule; then the selected item's actions with their keys. Labels and values in Bedstead at 0.75em. Details that need the disk arrive from a worker thread and unknown ones are left out. Taller content scrolls within the monitor budget. |
| Footer | Visible only with results. Status messages left (no result count, which the header shows and screen readers hear); Fastext links right: red Enter Open, green Ctrl Enter Show in folder, yellow Ctrl C Copy path, cyan Esc Close. Close, Copy path, then Show in folder hide when they would widen the popup; Show in folder also hides on narrow screens. Copy path says "Copied" for a moment after Ctrl+C. |

Use GTK4 widgets and the icon theme: `folder`, file-type icons guessed from the
name alone (unplaced names keep `text-x-generic`), and desktop entries' own icons.
Honor desktop scale factors, font sizes, cursor preferences and disabled animations.
The search field stays fixed as matches appear or change. No shader or image
artwork is included. Do not expose scores, `catalog_gen`, wire fields or
performance counters.

Motion never delays input, results or actions, and decorative motion never loops:

| Effect | Behavior |
|---|---|
| Open | Surface drops into place from 10px above and 98.5% scale (230ms). The header's parts step in from left to right, as if transmitted (200ms in four steps, 40ms apart), and the idle strip's facts rise one after another. This is the only motion that is not a response to input. |
| Typing | The rule under the query draws in ink (220ms) and the search icon brightens; they hold until 700ms idle, then fade (380ms). Arrow navigation does not trigger them. |
| Results | First results rise into place top to bottom, 16ms apart (190ms each, at most eight steps); later renders fade in only new rows (100ms). |
| Selection | The block glides to the new row over 130ms; its edge takes the row's color and the row's text inverts. It jumps when results first appear. |
| Preview | Once the selection rests 70ms on a new result, the panel redraws line by line (170ms each, 22ms apart); new results show it at once, and another item of the same result updates it in place. |
| Searching | After 120ms pending, the search icon crossfades to a spinner until results render. |
| Launch | The block flashes twice, teletext's flash attribute (300ms), while resolving; failure clears it. |
| Dismiss | Work is canceled at once; the window hides after a CRT switch-off: the page squeezes to a bright line, then the line shrinks away (200ms). Reopening cancels the hide. |
| Clock | The header clock ticks on each whole second while the popup is shown. |

Disabled GTK animations remove all of these and make dismissal immediate.

Result labels use the daemon's safe display representation. Escape control
characters, including embedded newlines, into visible single-line text. Hovering
a file or folder shows the complete safe display path in a tooltip.
Opening always uses the exact bytes returned by resolve.

## Keyboard, mouse and focus

| Input | Behavior |
|---|---|
| Desktop shortcut | Invoke `torchlight-gtk --toggle`. One application instance shows/focuses the popup or hides its already-focused window. A suggested binding is Super+Space; installation documents how to choose an available Cinnamon shortcut. |
| Show | Clear the previous query and results, show only the search field and focus the entry. No default result is selected. |
| Type / paste | Keep the entry focused, reset deliberate selection and issue a new query. Respect the protocol's 256-byte UTF-8 query limit; show a small inline message for an oversized paste. |
| Up / Down | Move selection, clamped to the list ends; scroll the selected row into view. Keep text-entry focus. Every running application lists up to three windows and New window beneath it, and these are rows too. |
| Right | With the caret at the end of the query, list a collapsed application's windows again, or all of them from "Show N more windows". Otherwise move the caret. |
| Left | On a window or action under an application, hide them and select the application. Otherwise move the caret. |
| Enter | Resolve and open the selected result. An application with open windows brings its most recent window forward; a window row brings that window forward; New window runs the entry's new-window action. When offline with no selected result, retry the query. |
| Ctrl+Enter | Resolve, then ask the file manager to reveal a byte-preserving file URI through D-Bus; fall back to opening its parent directory. |
| Escape | Dismiss and cancel any pending query/action work for this popup. |
| Click a row | Select and open it through the same resolve path as Enter. |
| Focus leaves popup | Dismiss, except while interacting with its own status/error control. |

Keep ordinary entry editing shortcuts. A row is actionable only when it belongs
to the current query. While a replacement query is pending, old rows may remain
dimmed to stabilize geometry, with activation disabled. A launch awaiting resolve
disables repeated activation; Escape remains responsive. Successful acceptance
of a launch dismisses the popup and queues its history event asynchronously.
Failures keep the popup open with a concise message and preserved query.

## Result updates and selection

Assign a distinct request id to each query, scoped to its connection. Render only
responses for the current request and connection. M2 currently sends one
lexical-only `final` response. M4 will add `lexical` followed by `final`; display
the first phase immediately and update rows in place when the second arrives.

Selection is a file id, rather than a row number. Before deliberate movement,
the first ranked result may follow incoming ordering. After the user moves
selection, retain that id and its row through later phases. If it drops out of
the final top ten, append one retained selected row until the query or selection
changes. Resolve its id before acting. A new query resets this retention rule.

## Visible states

| State | Content |
|---|---|
| Empty query | Only the search field; no rows, divider, footer or empty-state copy. Whitespace-only input behaves the same. Cancel pending queries and invalidate old responses immediately on clearing. |
| Results | Basename/parent rows; first row selected initially. |
| Pending query | Keep typing responsive. After 120 ms, swap the search icon for a spinner and announce `Searching…`, so fast queries never flash it. |
| No matches | Compact `No matches` inside the field; full guidance in tooltip/announcement. Keep the entry focused. |
| Indexing | Existing searchable results remain available; footer says `Updating index…`. |
| Degraded coverage | Existing results plus `Some folders are unavailable`; status details explain offline/unreadable folders or limited watching. |
| Daemon unavailable | Compact `Search offline` inside the field with Retry; the full unavailable status is announced. Retry reconnects and requests the latest query. |
| Deleted / stale result | `This file is no longer indexed`; refresh the current query and preserve entry focus. |
| Open / reveal failure | `Could not open this file` or `Could not reveal this file`; retain results for another choice. |

Status is supplementary: its polling and errors do not replace a successful query
list. Use readable text as well as icons, with accessible names and announced
result counts/selection. Respect reduced motion; every transition and animation is disabled with GTK animations.

## GTK and IPC implementation contract

- Put the C application under `ui/gtk/`; keep the executable thin. Suggested
  separate contexts are popup/view, result/selection model, asynchronous IPC
  session and launch actions. Each has explicit ownership and isolated tests.
- All daemon communication goes through `ipc`. Extend its client transport for
  asynchronous bounded messages; the existing `ipc_call` is synchronous and
  has a five-second deadline, so it must not run on GTK's main thread.
- Keep at most one query in flight plus the latest pending query, coalescing
  edits within one frame (up to 16 ms). Check the latest request id again when
  applying a response. Release obsolete results promptly. A separate bounded
  status/action exchange keeps total active work below the daemon's limit of four.
- Reconnect after EOF, idle disconnection or daemon restart; assign a new
  connection identity and request the current query. Enforce IPC byte limits,
  complete newline framing, JSON validation and terminal-response deadlines.
- Copy result data before releasing transport buffers. Use decoded `path` or
  `path_b64` only for actions; never reconstruct paths from labels. Reject NUL
  in decoded paths and preserve all other bytes.
- Resolve against the current catalog immediately before each action. Construct
  argv or a correctly escaped file URI from the resolved path. Record an
  accepted action with a unique launch-event id and the response's search id;
  history retries cannot open the file a second time.
- Add the systemd user unit and desktop/hotkey instructions in M3. Discuss the
  GTK4 dependency before changing the build, as required by `AGENTS.md`.

## M3 acceptance checks

Exercise keyboard-only show/type/select/open/reveal/dismiss; repeated shortcut
activation; two popup launches; 100%, 150% and 200% scaling; dark/light and
high-contrast themes; a small monitor; long names; Unicode, invalid bytes and
control characters; no matches; offline roots; unavailable/restarted daemon;
rapid typing/backspace/paste; out-of-order phases; a selected id leaving top-k;
rename/delete/replacement before resolve; failed launch; and disabled history.

Measure hotkey-to-focus, first paint and input-to-results in Cinnamon X11 while
the daemon reconciles a large catalog. Verify there is no synchronous socket,
filesystem or database work on GTK's main thread. Record observed focus and
placement behavior instead of assuming compositor support.

Related: [ui module](modules/ui/README.md), [IPC](modules/ipc/README.md),
[ADR 0014](adr/0014-m3-gtk-popup-design.md).

## Implemented application/settings extension

The search entry has an accessible name identifying applications, settings, files and folders.
Desktop rows use localized names and application icons, with no subtitle
([ADR 0036](adr/0036-one-line-results.md)). Enter resolves their session-scoped result id and activates the native
desktop entry; Ctrl+Enter reveals its desktop file through the same exact-byte
action path. File/folder rows retain basename/parent labels and symbolic icons.
Running applications list their open windows beneath them, as specified in
[ADR 0035](adr/0035-open-windows-under-applications.md).
The implemented GTK4 surface and acceptance evidence are in
[M3 verification](m3-completion.md); the interactive HTML remains the original
file-oriented design reference, not a screenshot of the application.
