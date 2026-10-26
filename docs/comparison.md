# How Torchlight compares

Torchlight measured against the launchers people would use instead: GNOME Shell's
search, Ulauncher, rofi, fzf and plocate. Every launcher gets the same apps, the same
files and the same queries, is graded by the same scorer, and runs with its own
defaults. The [results](#results) include where Torchlight loses.

## What is measured

Three numbers per launcher:

- **Found first**: the share of queries whose right answer is the first result.
- **Found in the top 5**: the share whose right answer is among the first five.
  Every launcher's window shows at least five.
- **Time per query**: median and 95th percentile, in milliseconds, measured the way
  each launcher answers (below). Torchlight's daemon stays running, so its memory
  is reported too.

Torchlight is scored on what its popup shows: groups (apps, settings, folders,
files) placed where their best result ranks, ten rows in all.

### Queries

- **Apps** (`tests/bench/compare/apps/`): the 89 applications GLib shows in a GNOME
  session on the reference desktop, from system folders only, stripped to their
  untranslated search and visibility keys. Each app gets three generated queries:
  its whole name, its first three letters, and the name with one typo in its longest
  word of five or more letters (the four edit kinds of `tests/bench/queries.c`,
  never on the first letter), from the held-out seed 2. Any app with the same name
  counts as right, as duplicate file names do.
- **Everyday app queries** (`tests/bench/compare/everyday_queries.tsv`): 36 queries
  written by hand, such as `term`, `chrome`, `fierfox`, `browser` and `task manager`,
  each with every app that counts as right. They were committed before any launcher
  ran and were not changed afterwards.
- **Files**: the synthetic 50,000-path home of `make bench`, created as empty files,
  and its held-out queries (seed 2, 200 of each kind): exact name, prefix,
  abbreviation, one typo, a partial word with a typo, and folder plus name. A result
  is right when it has the target's file name (duplicates count) and, for folder
  queries, the same parent folder. None of these queries were used to tune Torchlight.

All queries are lowercase, as people type into a launcher.

### Launchers

| Launcher | Apps | Files | How it is asked |
|---|---|---|---|
| Torchlight | ✓ | ✓ | Its release daemon over its socket, history off, indexing the home and the app fixture |
| GNOME Shell search | ✓ | | `g_desktop_app_info_search`, the call `Shell.AppSystem.search` makes, with Shell's visibility filter; with no usage history Shell keeps GLib's order |
| Ulauncher 5.16.2 | ✓ | | Its own app search code: `SortedList` with `fuzzy_search.get_score` on the name and the executable, a minimum score of 75 and nine results, over the apps in the order its own reader finds them, as `AppDb.find` does |
| rofi 1.7.5 | ✓ | ✓ | `rofi -dmenu -i -format i -filter Q -dump` under Xvfb, with its default matching; apps are one line each of what `drun` matches by default (name, generic name, command, categories, keywords), in `drun`'s order |
| fzf 0.74.5 | ✓ | ✓ | `fzf --filter Q` with its defaults; app lines are names |
| plocate 1.1.19 | | ✓ | Its own index of the home (default pruning), every word a case-insensitive pattern |

rofi and fzf are filters: a launcher built on them pipes the list in each time it
opens, so their time includes reading the list. GNOME and Ulauncher are asked in
process, with no window to draw, Torchlight over its socket, plocate as a command.

Some details decide results and were checked against the real windows:
`box_spotcheck.sh` screenshots rofi's `drun` and Ulauncher under Xvfb for five
everyday queries, and the lists compared (`term` and `settings` in rofi; `term`,
`settings` and `calc` in Ulauncher) matched the adapters' rows exactly. The first
Ulauncher adapter did not: it ordered apps by id, which is what led to the tie
rule below.

- Only rofi's dmenu mode can print results without a window, so apps are fed as
  `drun` would match them. dmenu mode is case-sensitive unless given `-i`, which
  `drun` does not need. `-dump` prints matches in input order and ignores `-sort`;
  that is also what the window shows by default, but it means rofi's sorted fuzzy
  setting cannot be measured.
- Ulauncher caps scores at 100, and equal scores keep the order its database got
  the apps in, which is the order its reader found the files on disk. `term` scores
  100 for both Terminal and Thunderbird Mail, so which comes first depends on the
  folder's order on that machine.
- GLib loads an entry only when its program exists; the images create a do-nothing
  program for each one, as installed apps have.
- Torchlight skips hidden folders, build output and package folders by default
  (ADR 0062). Queries whose file sits in one stay in the score and are also counted
  separately.

Not measured: Albert, KRunner, Walker and Vicinae, which cannot be asked without
their window; rofi's sorted fuzzy setting (above); GNOME's file search (Tracker),
which needs a session's indexer.

### How it runs

`tests/bench/compare/boat_compare.sh` runs everything on Boat cloud boxes (Ubuntu
24.04, 4 vCPU); nothing runs on the developer's computer. A base box builds
Torchlight, runs `make test` and `make lint`, builds one Docker image per launcher
from the same inputs (`Dockerfile.*`, downloads checked against
`downloads.sha256`) and smoke-tests each. It is then forked into one box per
launcher. On each, that launcher and Torchlight take turns over three rounds, who
goes first alternating, in containers limited to the same two CPUs and 4 GiB with
no network; each section starts with 20 untimed queries. Search quality does not
depend on the machine, so `report.py` fails unless every round of a launcher and
Torchlight on every box rank every query identically. Speeds are compared only
within a box.

`tests/bench/compare/local_compare.sh` runs the same inputs on a developer's
computer, for timings on real desktop hardware. Docker needs root there, so the
launchers run on the host inside an unprivileged user and mount namespace whose
private `/home` holds the synthetic home at `/home/user`, as in the images, pinned
to the same two CPUs; the real home is hidden from them.

## Results

### 2026-10-27

Torchlight 0.1.6 (the release daemon built at `3a858d4`) on Boat boxes (4 vCPU AMD
Ryzen 9 9950X or Ryzen 7 9700X, Ubuntu 24.04), and the same binaries on the reference
desktop (Intel Core i7-8700K, Linux Mint 22.2, GNOME on X11, with the person's own
debug daemon running). Raw summaries and every query's rank:
[Boat](../tests/bench/results/2026-10-27-launcher-comparison-boat.md)
([ranks](../tests/bench/results/2026-10-27-launcher-comparison-boat-ranks.tsv)),
[desktop](../tests/bench/results/2026-10-27-launcher-comparison-local.md)
([ranks](../tests/bench/results/2026-10-27-launcher-comparison-local-ranks.tsv)).

Every launcher ranked every query identically in all three rounds, in a first and a
second full run on different boxes (Ulauncher's adapter was corrected between them;
above), and on the desktop, with two exceptions: GNOME and Ulauncher break ties
between equal matches by the order files sit on disk, so 29 and 35 of their 297 app
answers came out in a different order on the desktop than in the images. Their
figures below are given as the range of the two.

**Apps** (261 generated queries; found first / top 5):

| Launcher | Found first | Top 5 | Full name | First letters | One typo |
|---|---|---|---|---|---|
| Torchlight | 92% | 99% | 100% / 100% | 78% / 99% | 99% / 99% |
| Ulauncher | 86–89% | 99% | 96–97% / 100% | 66–72% / 99% | 98% / 99% |
| fzf | 65% | 74% | 99% / 100% | 74% / 99% | 20% / 20% |
| GNOME Shell search | 58–60% | 69% | 98% / 100% | 68–74% / 99% | 5% / 5% |
| rofi | 45% | 63% | 89% / 99% | 39% / 82% | 5% / 5% |

**Everyday app queries** (36):

| Launcher | Found first | Top 5 |
|---|---|---|
| Torchlight | 94% | 97% |
| GNOME Shell search | 83–86% | 89% |
| fzf | 83% | 83% |
| Ulauncher | 81% | 86% |
| rofi | 69% | 86% |

**Files** (1,200 queries in a 50,000-path home; found first / top 5):

| Launcher | Found first | Top 5 | Exact name | Prefix | Abbreviation | One typo | Partial typo | Folder + name |
|---|---|---|---|---|---|---|---|---|
| Torchlight | 45% | 58% | 96% / 100% | 22% / 42% | 41% / 63% | 73% / 86% | 6% / 7% | 30% / 50% |
| fzf | 35% | 46% | 90% / 94% | 22% / 32% | 32% / 55% | 27% / 34% | 1% / 2% | 36% / 60% |
| plocate | 25% | 31% | 86% / 94% | 24% / 34% | 1% / 2% | 1% / 1% | 0% / 0% | 39% / 53% |
| rofi | 24% | 29% | 88% / 94% | 20% / 28% | 2% / 4% | 1% / 1% | 0% / 1% | 35% / 47% |

One of the 1,200 targets sits in a folder Torchlight skips by default (a `build`
folder); it counts as a miss above.

**Time per query** (median / 95th percentile, ms). Each launcher is compared with
Torchlight on the same machine; Torchlight's own times on every box and the desktop
were 0.1–0.35 ms median and 0.5–2.3 ms at the 95th percentile.

| Launcher | Apps, Boat | Apps, desktop | Files, Boat | Files, desktop |
|---|---|---|---|---|
| Torchlight (on the desktop) | | 0.23 / 0.89 | | 0.23 / 0.88 |
| GNOME Shell search | 0.007 / 0.017 | 0.009 / 0.017 | | |
| Ulauncher | 0.24 / 0.31 | 0.44 / 0.54 | | |
| fzf | 1.3 / 5.0 | 2.0 / 2.5 | 16 / 51 | 17 / 55 |
| plocate | | | 0.9 / 1.6 | 2.0 / 3.8 |
| rofi | 17 / 25 | 63 / 74 | 33 / 48 | 88 / 100 |

Torchlight's daemon held 41–50 MiB (peak 44–56 MiB) with the 89 apps and 50,000
paths and answered 0.4–1.1 s after starting. rofi, fzf and plocate keep nothing
running between searches; GNOME's search runs inside the shell and Ulauncher's in
its own process, neither measured here.

### What the numbers say

- **Apps.** Torchlight finds the right app first most often: 92% of generated and
  94% of everyday queries. Ulauncher is close on generated ones because it also
  forgives typos. GNOME, rofi and fzf find a mistyped name 5–20% of the time.
- **Files.** Torchlight finds the right file first for 45% of queries, against 35%
  for fzf and about 25% for plocate and rofi. It leads on exact names (96%), typos
  (73%, against 27% at best) and abbreviations (41%, against 32%). Prefixes and
  partial words are often ambiguous in this corpus (hundreds of names start with
  `screens…`), which keeps everyone's numbers low there.
- **Speed.** Torchlight answers in about a quarter of a millisecond over its socket,
  with 50,000 files. GNOME's in-process GLib call is faster for its 89 apps and does
  no files. fzf, plocate and rofi start a process per search, and rofi's start is the
  slowest: 17–88 ms.

### Where Torchlight loses

64 of the 1,497 queries were found first by another launcher and not by Torchlight:

- **Folder plus name** (31 queries). plocate, fzf and rofi find 35–39% of these first
  against Torchlight's 30%, and fzf has more in its top five (60% against 50%).
- **File prefixes** (16), mostly lost to plocate, whose plain index order happens to
  put the folder of that name first, and **abbreviations** (8), mostly to fzf. The
  other four file losses are two exact names and two typos.
- **Apps** (5): `cliron` for CLion is found only by Ulauncher; `chrm` puts Google
  Chrome tenth, under files, where fzf puts it first; `code` lists OpenCode before
  Visual Studio Code; and the prefixes `dis` and `fir` list Disks before Disk Usage
  Analyzer and Firefox before Firewall Configuration.
- **Partial words with a typo** (`projc` for `project…`) are rarely found by anyone;
  Torchlight's 6% is the best of a low field.
