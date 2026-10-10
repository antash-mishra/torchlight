# M3 GTK popup design

**Status: implemented; see [M3 verification](m3-completion.md) for acceptance coverage.** Follow the launcher behavior
in [PLAN.md](../PLAN.md#milestones) and the visual/interaction specification below.
The native presentation follows the desktop theme with bounded motion, accepted in
[ADR 0034](adr/0034-theme-following-popup-with-motion.md), which replaced the
Quiet System look of [ADR 0027](adr/0027-quiet-system-native-popup.md). The
[older interactive preview](m3-gui-preview.html) records the original M3 study;
[current native captures](ui/native-empty.png) reflect the implemented layout.

## Presentation

A compact floating search window, with one search field, a vertical result list
and a quiet footer. The current target session is **Cinnamon on X11**. Validate
activation and focus there first; document Wayland results separately during M3.

| Element | Specification in logical pixels |
|---|---|
| Window | 680 wide; clamp to monitor work area minus 48; maximum height 70% of the work area. No title bar or resize handle. |
| Placement | Horizontally centered on the active monitor, near its upper third. Placement is a window-system request, verified in the target session. |
| Surface | Theme background and foreground (`@theme_bg_color`, `@theme_fg_color`), a faint foreground border and outer radius 10. The selected-background color is the accent. The desktop font applies; no text is smaller than 0.9em. |
| Search | Empty popup 70 high: a 44-high field with 12px padding (10 on narrow screens). A symbolic search icon, then a plain input with placeholder `Search apps, settings, files and folders` and GTK's native caret. No clear icon. No-match/offline feedback and Retry sit at the field's end. |
| Results | Request ten; show up to eleven one-line rows before scrolling, reduced by the monitor budget. Each row 40 high, with a 24px full-color icon and 12px gaps. No hover tint: the selection is the only highlight. |
| Row text | Name in the body size, end-ellipsized past 40 characters when space is short. Files and folders then show their short folder at 0.9em, dimmed and right-aligned: `~` for home and at most the last two folders, plus the folder that tells same-named rows apart when they would otherwise look identical. Apps and settings show their name alone. See [ADR 0036](adr/0036-one-line-results.md). |
| Selection | One accent-tinted highlight under the rows glides between rows; the footer names Enter, so the row carries no keycap. High contrast uses full-strength foreground for secondary text and the border. |
| Footer | Visible only with results. Status messages left (no result count, which is announced instead); keycap hints right (Open, Show in Folder, Close). Close, then Show in Folder, hide when they would widen the popup; Show in Folder also hides on narrow screens. |

Use GTK4 widgets and the icon theme: `folder`, file-type icons guessed from the
name alone (unplaced names keep `text-x-generic`), and desktop entries' own icons.
Honor desktop scale factors, fonts, cursor preferences and disabled animations.
The search field stays fixed as matches appear or change. No shader or image
artwork is included. Do not expose scores, `catalog_gen`, wire fields or
performance counters.

Motion never delays input, results or actions, and never loops:

| Effect | Behavior |
|---|---|
| Open | Surface fades in from 6px above over 160ms. |
| Torch sweep | Once per show, a band of accent light crosses the search field (560ms, after 60ms). |
| Typing glow | The field ring brightens on input (120ms), holds until 700ms idle, then fades over 420ms; continuous input extends one hold. Arrow navigation does not trigger it. |
| Results | First results fade up 18ms apart (140ms each, at most eight steps); later renders fade in only new rows (100ms). |
| Selection | The highlight glides to the new row over 130ms; it jumps when results first appear. |
| Searching | After 120ms pending, the search icon crossfades to a spinner until results render. |
| Launch | The selected row flashes and its icon pulses while resolving; failure clears it. |
| Dismiss | Work is canceled at once; the window hides after a 110ms fade. Reopening cancels the hide. |

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
