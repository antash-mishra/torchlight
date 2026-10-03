# M3 GTK popup design

**Status: implemented; see [M3 verification](m3-completion.md) for acceptance coverage.** Follow the launcher behavior
in [PLAN.md](../PLAN.md#milestones) and the visual/interaction specification below.
The [interactive preview](m3-gui-preview.html) illustrates the layout and states;
the GTK implementation uses the desktop's theme. There is no existing Figma file
or checked-in GTK design to reproduce.

## Presentation

A compact floating search window, with one search field, a vertical result list
and a quiet footer. The current target session is **Cinnamon on X11**. Validate
activation and focus there first; document Wayland results separately during M3.

| Element | Specification in logical pixels |
|---|---|
| Window | 680 wide; clamp to monitor work area minus 48; maximum height 70% of the work area. No title bar or resize handle. |
| Placement | Horizontally centered on the active monitor, near its upper third. Placement is a window-system request, verified in the target session. |
| Surface | Native GTK background, foreground, border, shadow and light/dark theme. Outer corner radius 12; inner spacing 16. |
| Search | 52 high; symbolic search icon 20; text 20; placeholder `Search apps, settings, files and folders`. Entry has focus when shown. |
| Results | Request ten; show up to eight rows before scrolling. Each row 58 high, with a 24-pixel symbolic file/folder icon and a 12-pixel text gap. |
| Row text | Basename on the first line, desktop font 15; parent path on the second line, 12 with muted foreground. End-ellipsize the name and middle-ellipsize the parent. |
| Selection | Native accent background/foreground with a visible focus treatment. Name and parent both remain legible in high-contrast themes. |
| Footer | 32 high; status on the left and `↑↓ Select   Enter Open   Ctrl+Enter Reveal   Esc Close` on the right. Hide secondary hints before clipping them on narrow screens. |

Use GTK4 widgets and symbolic theme icons. Keep theme-derived colors; avoid a
fixed accent or a separate theme library. Reuse the desktop's font and scale
factor. Do not expose scores, `catalog_gen`, wire fields or performance counters
in the normal popup.

Result labels use the daemon's safe display representation. Escape control
characters, including embedded newlines, into visible single-line text. Hover
or keyboard inspection may show the complete safe display path in a tooltip.
Opening always uses the exact bytes returned by resolve.

## Keyboard, mouse and focus

| Input | Behavior |
|---|---|
| Desktop shortcut | Invoke `torchlight-gtk --toggle`. One application instance shows/focuses the popup or hides its already-focused window. A suggested binding is Super+Space; installation documents how to choose an available Cinnamon shortcut. |
| Show | Clear the previous query and results, show `Type to search` and focus the search entry. No default result is selected. |
| Type / paste | Keep the entry focused, reset deliberate selection and issue a new query. Respect the protocol's 256-byte UTF-8 query limit; show a small inline message for an oversized paste. |
| Up / Down | Move selection, clamped to the list ends; scroll the selected row into view. Keep text-entry focus. |
| Enter | Resolve the selected file id, then open its current exact path with an argv-based process launch. |
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
| Empty query | No result rows; `Type to search` as context. Whitespace-only input behaves the same. Cancel pending queries and invalidate old responses immediately on clearing. |
| Results | Basename/parent rows; first row selected initially. |
| Pending query | Keep typing responsive. Show `Searching…` after 120 ms to avoid flashing a spinner on fast queries. |
| No matches | `No matches` and `Try a filename or part of its folder path.` Keep the entry focused. |
| Indexing | Existing searchable results remain available; footer says `Updating index…`. |
| Degraded coverage | Existing results plus `Some folders are unavailable`; status details explain offline/unreadable folders or limited watching. |
| Daemon unavailable | `Search service unavailable` with a Retry action. Retry reconnects and requests the latest query. |
| Deleted / stale result | `This file is no longer indexed`; refresh the current query and preserve entry focus. |
| Open / reveal failure | `Could not open this file` or `Could not reveal this file`; retain results for another choice. |

Status is supplementary: its polling and errors do not replace a successful query
list. Use readable text as well as icons, with accessible names and announced
result counts/selection. Respect reduced motion; scrolling is the only necessary
movement.

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

M3 extends the search placeholder to applications, settings, files and folders.
Desktop rows use localized names and application icons with Application/Settings
subtitles. Enter resolves their session-scoped result id and activates the native
desktop entry; Ctrl+Enter reveals its desktop file through the same exact-byte
action path. File/folder rows retain basename/parent labels and symbolic icons.
The implemented GTK4 surface and acceptance evidence are in
[M3 verification](m3-completion.md); the interactive HTML remains the original
file-oriented design reference, not a screenshot of the application.
