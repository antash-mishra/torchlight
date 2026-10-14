# 0053. Open windows on KDE Plasma through KWin scripts

- **Status:** Accepted
- **Date:** 2026-10-14

## Context

On KDE Plasma's Wayland session the popup listed no windows: Wayland hides other
applications' windows ([ADR 0052](0052-open-windows-through-wayland-taskbar-protocols.md)).
KWin offers no D-Bus call that returns its windows. Its window protocol,
`org_kde_plasma_window_management`, is one of Plasma's private protocols, and KWin grants
it only to programs listed in its allowlist (`X-KDE-Wayland-Interfaces` in a desktop file
whose `Exec` matches the program). KWin's public scripting interface, however, loads a
JavaScript file over D-Bus (`org.kde.KWin /Scripting loadScript`) and runs it inside KWin,
where it sees every window. The script reports back with `callDBus`; kdotool works this
way. Ubuntu 24.04 has Plasma 5.27, and Kubuntu 25.04 and later, KDE neon and Fedora have
Plasma 6.

## Decision

1. **A short script per call.** `ui/gtk/windows_kwin.c` writes a script to
   `$XDG_RUNTIME_DIR/torchlight-<pid>.js`, loads it as `torchlight-<pid>`, runs it and
   unloads it.
   - **Where it runs:** at `/Scripting/Script<id>` on Plasma 6, or `/<id>` on 5.27. The
     source tries one path, then the other, and remembers which worked.
   - **ES5 only:** Plasma 5's engine is QtScript. One script serves both versions by
     testing for Plasma 6's names (`windowList`, `activeWindow`) before Plasma 5's
     (`clientList`, `activeClient`).
2. **The report.** The list script sends the normal windows that are not hidden from task
   bars, topmost first by each window's `stackingOrder`, at most 128 of them. Each window
   is [internal id, title, desktop file, class, instance], as one JSON string.
   - **Where it arrives:** `Report(token, text)` at `/org/torchlight/KWin`, on a bus
     connection of the source's own whose main context only its calls run. Waiting never
     runs the popup's main loop.
   - **Who may send it:** only a report from KWin's unique name, carrying the call's
     token, counts. Each call waits at most 250ms in all, like the GNOME source.
3. **Switching** runs a second script that makes the window active (`activeWindow` or
   `activeClient`) and reports whether it was still open. A window's handle is the first 64
   bits of KWin's id, a random UUID. An id goes into a script only after it was checked to
   contain nothing but hex digits, braces and dashes.
4. **Most recently used first by stacking order.** KWin raises a window when it is
   activated, and the X11 source orders windows the same way. Scripts on 5.27 cannot read
   the workspace's stacking list, but each window's own `stackingOrder` works on both
   versions.
5. **Matching.** `desktopFileName`, made a plain desktop id, fills the application id.
   Plasma 5.27 lower-cases X11 class names (`xlogo` for `XLogo`), so its windows carry
   `fold_case`, and `windows_evidence` then compares `StartupWMClass` ignoring ASCII case.
   Plasma 6 keeps the case.

## Alternatives considered

- **`org_kde_plasma_window_management`:** it pushes changes and needs no scripts. But it is
  one of Plasma's private protocols, granted only through the allowlist, which would tie
  window listing to an installed desktop file. It stays the fallback if KDE ever restricts
  scripting.
- **A script that stays loaded and pushes changes:** it would give the exact order of use
  without a per-call cost, but it must survive KWin restarts and popups that crash, and it
  leaves a script running inside KWin.
- **KRunner's window runner on KWin's bus:** it takes localized keywords and does not
  report the application.
- **Depending on kdotool:** another program to install, which also uses scripts.

## Consequences

- On KDE Plasma's Wayland session, 5.27 and 6, the popup lists windows and switches
  between them, with nothing to install or enable. The same source serves LXQt on KWin.
- Each list costs a script round trip on the popup's main thread, after the first paint:
  15ms measured on Plasma 5.27, including starting three `gdbus` processes. A KWin that
  does not answer costs at most 250ms, and then no windows.
- Tests:
  - **Unit:** `tests/unit/test_windows_kwin.c` runs a fake KWin on a private bus. It covers
    Plasma 5 and 6 script paths, a script name left loaded, the launcher's own window, an
    unsafe id and a short entry left out, activation, a silent KWin, a forged report from
    another connection, nonsense, and every script unloaded again.
  - **End to end:** `tests/test_windows_desktops.py` runs headless KWin 5.27 (Ubuntu 24.04)
    and KWin 6.7.5 (KDE neon's packages, built for Ubuntu 24.04) with the real popup.
