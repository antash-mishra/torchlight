# desktop

> **Status:** Implemented (M3 Part 2), including independent name/generic/keyword fields; M5 personal
> boosts; M7 change-stamped refresh (parses only when an entry changed); the desktop menu's
> visibility rule (ADR 0057), named applications (ADR 0058) and query evidence (ADR 0061)
> **Source:** `src/index/desktop.c` · **Header:** `include/torchlight/desktop.h`
> **Tests:** `tests/unit/test_desktop.c`, `tests/test_desktop.py`

`desktop_create/destroy` own discovery, immutable lexical snapshots and a refresh
worker. Configuration comes from standard XDG data paths, locale and
XDG_CURRENT_DESKTOP; keep the environment stable and restart to change it.
Applications are searched independently of the configured file roots, including
hidden user data directories normally excluded by the file crawler.

User `applications/` entries precede system directories in XDG_DATA_DIRS order.
Nested paths become hyphenated desktop ids. A hidden or malformed higher-priority
entry masks lower copies. GIO supplies localized display/generic names and
keywords and enforces TryExec; which desktops an entry is for follows the
desktop's own menu (below). Settings-category entries
receive a settings result kind; no Cinnamon-specific launch command is invented.
Category lists use GIO's list parser, including valid lists without a final
semicolon.

Localized generic names and keywords are copied into separate lexical fields;
the display name stays the primary basename. A distinct `Name` alias is retained
with the generic-name evidence when `X-GNOME-FullName` changes the display label.
Metadata never enters the parent directory tree. Generic/keyword prefixes score
2800/2600, plus 128 for complete tokens. Primary-name prefixes remain stronger;
metadata fuzzy evidence uses the same bounded optimal scorer. Search paths are
internal only: wire responses and resolve return the actual desktop filename
plus launch metadata. See [ADR 0020](../../adr/0020-m3-part2-search-quality.md).

The desktop engine adds 2,000 to each basename/token/initials prefix hit. This
lets `chrome` match the name token in `Google Chrome` ahead of ordinary file
prefixes such as `chromepolicy…`, even when more than ten files match. It applies
before desktop top-k selection, so a small result limit cannot discard a name
match before weighting. Generic names, keywords and weak fuzzy matches keep
their separate lower field scores. Exact paths retain their priority; files named
exactly like the query now follow the applications it names (ADR 0058, below).
The daemon preserves the desktop engine's tie order across result limits.
See [ADR 0018](../../adr/0018-application-name-ranking.md).

The refresh worker preserves unchanged ids, builds changed engines/workspaces
privately and publishes under the query lease. Discovery and reclamation never
execute in desktop_query. Acquire/query/resolve/release borrow metadata only
during the lease; each workspace has one query coordinator. When it parses is
described under "Change-stamped refresh (M7)" below.

Ids use a random daemon-session namespace above 2^62; changes/removal retire the
id rather than allowing old results to launch a replacement. Responses include
a content revision checked by the launch worker. Metadata and revision come
from one parsed keyfile read, so atomic replacement cannot attach a new revision
to an old label or prevent the next refresh from retiring its id. An injected
replacement integration test covers this exact race. They are not persisted file
ids. File catalog_gen and desktop publication are independent.

Entries keep `StartupWMClass` as `wm_class` when it is present and at most 255
bytes; longer values are treated as absent. Results carry it so the popup can
list the application's open windows ([ADR 0035](../../adr/0035-open-windows-under-applications.md)).

Bounds: 8,192 encountered desktop ids (including hidden overrides), 16 nested
directories, 4,096-byte fields/filenames and 64 KiB desktop files. Directory
symlinks are not traversed. Failed snapshot builds retain the prior catalog.
Changes are found by polling stat data rather than inotify, avoiding extra
watch pressure. Application install/remove lag is parse/build time plus at most
one second. See [ADR 0015](../../adr/0015-m3-desktop-catalog-and-launcher.md) and
[review fixes](../../adr/0017-m3-snapshot-cancellation-and-acceptance.md).

## M4 metadata integration

The service can copy sorted entry names, generic names, keywords, icons and
revisions under the existing exclusive lease. desktop_gen identifies its view;
owned semantic metadata is used after releasing this lease, before inference.

## Personal boosts (M5)

`desktop_query_boosted` boosts entries by sorted-id position in the leased
snapshot (`desktop_entry`), through `lexical_workspace_boost`. Callers remap
positions when `desktop_gen` changes; the daemon's
[personal](../personal/README.md) state matches application usage by desktop
id. Each query replaces the previous boosts, so `desktop_query` stays
unboosted. Editing a `.desktop` file gives its entry a new session id, which
sorts last, so its position changes; tests in `test_personal.c` and
`test_desktop.py` check that boosts follow it.

## Change-stamped refresh (M7)

Until M7 the worker re-read and re-parsed every `.desktop` file each second.
On a stock Cinnamon desktop that is 171 files and about 1 MB of localized
keys: 1.4% of a core around the clock, with about 1.6 MiB/s of reads, and 87%
of the whole daemon's idle CPU. The M7 benchmark never saw it because its
daemons ran with an empty `XDG_DATA_DIRS`.

Now each check (`check_ms`, default one second) computes a **change stamp**:
it walks the applications trees exactly as discovery does (one shared walker:
real directories are entered, symlinked ones are not, names ending in
`.desktop` are entries) and folds in, for every entry, its path and its `stat`
dev, inode, mode, size, mtime and ctime (following symlinks, like the parse),
plus a marker for each directory it cannot list. Per-item hashes are added, so
listing order does not matter. That is about one `stat` per entry and no
reads. A discovery computes the same stamp of what it actually read, and the
worker keeps that one: a change the parse may have half seen (a directory
unreadable for a moment, a file replaced mid-walk) differs from the next
check's stamp even if it was reverted since. A failed parse records the
check's stamp instead, so an unreadable tree is not parsed every second.

The worker parses when the stamp differs, when asked (`desktop_refresh`, as
`torchlight reconcile` does), once more `settle_ms` (default 5 s) after the
last change, and at least every `rescan_ms` (default 10 min). The last two
exist because one input has no stamp: GIO hides an entry whose `TryExec`
binary is not on `PATH`, and that binary may appear after its `.desktop`
file. Non-`.desktop` files (`mimeinfo.cache`) and empty directories do not
change the stamp. `desktop_create(options, out)` takes `tl_desktop_options`
(NULL for defaults); `desktop_parses` counts completed discoveries for tests.
The condition variable waits on `CLOCK_MONOTONIC`. The worker thread is named
`tl-desktop`.

`change_stamps` in `test_desktop.c` uses 10 ms checks: no parse while nothing
changes (there was one per check before), none for `mimeinfo.cache` or an
empty new directory, exactly one plus the settle parse for an entry replaced by
rename, entries in a new subdirectory, explicit refreshes, and a directory
that becomes unlistable and listable again. `half_seen_directory` makes a
forced parse long (3,000 filler entries ahead of the system trees, one-second
checks) and makes a directory unreadable only while that parse runs: the entry
must come back. Recording the check's stamp instead fails it in 6 of 8 runs.
See [ADR 0042](../../adr/0042-desktop-change-stamps.md).

## The desktop's own menu (ADR 0057)

An entry is listed when the running desktop's application menu lists it:
`desktop_shown_in` decides from its `OnlyShowIn`, `NotShowIn` and `Exec` and the
`XDG_CURRENT_DESKTOP` value. The rule is GIO's (`g_desktop_app_info_get_show_in`: the
first current desktop that either list names decides; otherwise only entries without
`OnlyShowIn` are listed), except on Cinnamon, whose menu (libcinnamon-menu) also lists
entries restricted to GNOME and never lists `gnome-control-center` panels. That is why
GNOME Terminal (`OnlyShowIn=GNOME;Unity;` in Ubuntu's package) appears on Linux Mint;
GIO alone hid it, and Power Statistics with it. The popup's launch check applies the
same rule to the entry it is about to start. The rule takes the desktop list as an
argument rather than using GIO's once-per-process copy of the environment, so tests
judge several desktops in one process; `tests/unit/test_desktop.c` covers it with a
stand-in `gnome-control-center` on `PATH`, so GIO accepts the panel and only the rule can
hide it.

## Named applications (ADR 0058)

`desktop_named` says, for the results of a query on the current lease, whether the
query names each application: every word of the query (split at whitespace, as the
lexical engine splits) starts the application's display name, a word of it or its
initials. That is `prefix_score` over the normalized name, kept per entry at discovery
(`tokenize_create` of the search key's basename), against the query normalized into a
scratch buffer the catalog owns, so it allocates nothing; only the lease holder uses
it. It is the same evidence the name bonus weights: generic names, keywords, typos
and letters scattered through a name do not name an application. The daemon lists
named applications right after exact paths and before every other file, including
files named exactly like the query. A name the tokenizer rejects is still searched; it
just never counts as named. On a stock Mint desktop (112 entries) querying every match
and naming them costs at most 13 µs a keystroke (for "te"), against 6 µs for the
top ten alone.

## Query evidence (ADR 0061)

`desktop_evidence` classifies each result of a query on the current lease by what the
query's words find in it: *named* (as `desktop_named`), *described* (each word that does
not start the name is a whole word of the generic name or keywords, the tags a settings
panel carries: "audio" for Sound, "wifi" for Network), *word start* (some word only starts
such a word, as "aud" does) or *fuzzy* (a typo, shared trigrams or scattered letters). The
generic field also holds the untranslated name, so an English name still describes an
application under another locale. Generic names and keywords are tokenized at discovery
like the name, with the same `tokenize_create` the lexical engine uses for these fields,
so the evidence agrees with what search matches; a word's evidence comes from
`prefix_score` at token strength or above (a field's initials are not its words), and a
whole word is a complete token. Each entry also keeps a mask of the symbols that start a
word of its fields, so a word starting with none of them is fuzzy without scoring. The
daemon lists described applications after exact file names and before the rest, and for a
one-word query drops fuzzy matches once anything matches by a word start (see
[daemon](../daemon/README.md)). On the live Mint desktop (112 entries) classifying every
match costs 20 to 28 µs for a one-letter query (naming alone 10 to 12 µs) and 6 to 7 µs
for a whole word such as "sound".
