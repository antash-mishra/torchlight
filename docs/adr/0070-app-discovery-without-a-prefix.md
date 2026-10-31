# 0070: App discovery without a prefix

Date: 2026-10-30. Status: accepted and implemented.

## Problem

Requiring `install vlc` hides discovery behind a command users must learn.
The Flatpak catalog also knows only Flatpak installation state: a native VLC
launcher can exist while the Flatpak variant still appears available.

## Decision

Ordinary popup searches request up to three available-app suggestions alongside
the existing ten local results and pinned Web action. Each suggestion must
match the app's name by normalized word starts or initials; matching only a
description is insufficient. A bounded 64-entry lexical shortlist supplies the
candidates. Unicode normalization uses the existing tokenizer. Distinct app IDs
are shown once; a Flatpak ID marked installed anywhere in the shortlist is
excluded, including its other source variants.

When the local desktop catalog recognizes a named installed application, omit
installation suggestions for that query. This prioritizes opening what the
person already has, including native, Snap or Flatpak launchers visible in that
catalog. It is a query preference, not a package-identity join: equal display
names never establish that two packages are the same app. Files, settings and
fuzzy desktop matches do not trigger this suppression.

An exact normalized available-app name puts its group first in the scrollable
results and selects it, unless a local exact path/name or named setting already
matches. Other queries preserve local ordering and append suggestions. No local
matches selects the available app, otherwise Web. Website/path-like queries
receive no automatic software suggestions. The Web row remains pinned above
the scrollable area. Installed application rows say **Open**, available rows
say **Install**, and explicitly requested installed software says **Manage**.
Enter on Install opens Software's app details; Software still owns source
choice, final confirmation and progress. Typing never invokes an installer.

## Protocol and ownership

Status advertises `capabilities.software_suggestions: 1`. Only clients seeing
this capability send the optional v2 local-query flag `software_suggestions`.
When disabled, the encoder omits the field entirely for older v2 servers.
The response adds `software_suggestions` (zero to three typed software results)
and `prefer_software` (boolean) without changing local `results` or its limit.
The coordinator copies suggestion metadata into fixed request-owned storage
before releasing catalog leases. Both semantic phases preserve that copy and
preference. V1, CLI local queries and older v2 clients retain local-only results.

`local QUERY` disables suggestions and Web; `install QUERY` remains an optional
way to browse all matching sources. No new dependency, SQL or package command
is introduced on the query path. The software worker still refreshes in the
background, and its generation change refreshes an ordinary popup query too.

## Limits and validation

This does not inventory native/Snap/AppImage packages or unify their identities
with Flatpak. An app without a visible desktop launcher can remain undetected;
explicit software mode can still offer another package format. Installed-state
changes appear after the existing desktop/software catalog refresh.

Regressions cover native launchers with different IDs, installed Flatpak source
variants, exact-file precedence, Unicode names, capacity and phase ownership,
no package work while typing, and the ordinary-name Enter-to-Software flow.
GTK checks cover Open/Install/Manage labels, compact/wide keyboard navigation,
themes, scaling and accessible selection. Warm allocation checks and the
software/IPC benchmarks include the automatic suggestions path.
