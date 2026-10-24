# 0063. Opt-in Omarchy integration outside the search and UI core

- **Status:** Accepted
- **Date:** 2026-10-24

## Context

On Omarchy 4, installed apps already have XDG desktop entries, but many settings
are actions in the Omarchy JSONC menu or Quickshell bar panels. Torchlight's Tekst
palette is independent of Omarchy's theme. A regular Hyprland window also gives
the popup an extra rectangular border around its own rounded surface.

These are Omarchy integration concerns, not reasons to change the look or
installation behavior on every Linux distribution.

## Decision

Ship `scripts/omarchy.py` in the source tree and portable archive as an explicit
post-install command. It refuses systems whose `/etc/os-release` ID is not
`omarchy`, and requires Omarchy 4+ for its Lua config/menu/Quickshell interfaces.
Neither the portable installer nor the GTK application invokes it automatically.
It uses Python's standard library; no new core or mandatory portable dependency.

The helper owns per-user integration files: settings desktop entries, a color
override imported into user GTK CSS, a Hyprland Lua module, a five-minute systemd
sync timer and an Omarchy theme-set hook. Existing desktop config text remains;
uninstall removes only its exact additions and marked generated files. Replacing
Super+Space requires a separate explicit flag. No packaged Omarchy files change.

The menu remains the authority for actions. Generated entries carry stable menu
IDs, not copied shell commands. Sync evaluates the same trusted visibility
conditions as Omarchy and launch rechecks them before delegating. Dynamic
providers stay in their original pickers. The five bar settings panels use their
existing Quickshell summon interface. Normal apps still use the existing catalog.

Colors come from `omarchy theme color --all`, including Omarchy's own legacy-key
fallbacks. Only `tk_*` color names are overridden; no generic GTK selectors are
added. The theme hook refreshes the palette and terminates the current user's
cached Torchlight GUI, so the next invocation reloads it. It never stops the
search daemon. No C/UI/search-path behavior is changed.

## Consequences

This integration requires an Omarchy desktop session and user systemd. Omarchy 3
and non-Omarchy Hyprland installations are explicitly unsupported. Menu actions
can alter system settings when selected, exactly as in Omarchy itself; sync runs
only visibility predicates, never actions. Published XDG settings are also visible
to other launchers. Users remove the optional integration before uninstalling
Torchlight. Tests use isolated homes and stand-in desktop commands, including
negative tests that leave Arch and Ubuntu homes byte-for-byte unchanged.
