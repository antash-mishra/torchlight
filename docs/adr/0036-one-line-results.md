# 0036. One-line results with short folders

- **Status:** Accepted; implemented
- **Date:** 2026-10-10

## Context

ADR 0034's rows took two lines: a name, then the parent folder at full length
whenever it fit, so a header in an Android NDK read
`~/Android/Sdk/ndk/27.0.12077973/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include`.
Every application row repeated "Application" or "System settings". Hovering an
application showed its desktop file's location, which means nothing to a user.
The mouse-hover tint was a second highlight competing with the gliding
selection, and the footer carried a result count and four key hints. In use,
the popup did not look clean, and only about seven results fit before scrolling.

## Decision

1. **One line per result.** The icon (24px), the name, then the short folder,
   dimmed and right-aligned to the row's end. Rows are 40px; eleven fit
   before scrolling. The name takes its width first, up to 40 characters, and
   the folder gets the rest. Children of applications (ADR 0035) are 32px rows
   aligned with their application's name.
2. **Short folders.** The home folder shows as `~`, and at most the last two
   folders are shown (`POPUP_SHORT_FOLDERS`): `~/…/docs/modules`. A folder at
   most two levels deep is shown whole (`~/Documents/notes`). When two
   same-named rows of the same kind would still look identical, the folder
   where their paths first differ is kept as well
   (`~/…/27.0.12077973/…/include`); `popup_model_distinct_prefix` now reports
   it only in that case. If even the short form is too wide, earlier folders
   give way and the last folder is cut in the middle, as before. The tooltip and
   the accessible label keep the full path.
3. **Applications and settings show their name alone.** No subtitle and no
   tooltip; their icons already tell them apart from files.
4. **One plain highlight.** Rows have no hover tint; only the gliding
   selection is drawn, without the Enter keycap it used to carry: at the
   desktop's Noto Mono 13 the right-aligned folder ran into it, and the footer
   already says Enter Open.
5. **Slim footer.** Enter Open, Ctrl+Enter Show in Folder and Esc Close. The
   result count is announced to screen readers but not shown, and the obvious
   ↑↓ Select hint is gone. When a large font would widen the popup, Close and
   then Show in Folder give way. The status area still shows real messages
   (updating, unavailable folders, errors).

## Alternatives considered

- **Two lines with the short folder.** Keeps the old structure and suits very
  long names, but fits about seven results and still repeats a subtitle under
  every application.
- **Only the parent folder's name** (`include`). Too little context: many
  folders share names such as `src` or `include`.
- **No footer.** Ctrl+Enter (Show in Folder) would become undiscoverable.

## Consequences

- More results fit, and each reads at a glance; the full path is a hover away.
- Same-named files show an extra folder only when they would otherwise look
  identical.
- Settings panels are no longer labelled "System settings" in the list; their
  icons and names identify them.
- ADR 0034's row text, path shortening and footer hints are superseded; its
  theme, motion and selection decisions stand.
