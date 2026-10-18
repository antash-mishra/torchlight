# 0058. Applications the query names come before files

- **Status:** Accepted; revises the exact-file priority of
  [ADR 0018](0018-application-name-ranking.md)
- **Date:** 2026-10-17

## Context

The popup asks the daemon for ten results, and the daemon merged applications and files
by lexical score. A file whose name equals the query scores in an exact tier
(10,000,000 less its place penalty), far above any application matched by a word of its
name (about 8,000 with the desktop engine's name bonus, ADR 0018). So whenever ten files
or folders were named exactly like a typed word, an application that word names left the
popup entirely. A home folder with SDKs, Go module caches and source checkouts has
folders named `google`, `time`, `system`, `net`, `cli` and `code` by the dozen: typing
"google" listed ten folders from `~/go/pkg/mod` and no Google Chrome, "time" no Date &
Time, "cli" no CLion. Place ranking cannot help: its penalty is at most 256, inside the
exact tier.

ADR 0018 kept exact files first so that weak application matches (generic names,
keywords, typos) could not hide an exact file query. That reasoning holds for weak
matches, not for a word that starts an application's name.

## Decision

1. **A query names an application** when every word of it (split at whitespace, as the
   lexical engine splits) starts the application's name, a word of the name, or the
   name's initials. This is exactly the evidence the desktop engine's name bonus weights,
   decided by the same `prefix_score` over the normalized name (`desktop_named`).
   Generic names, keywords, typos and letters scattered through the name do not name it.
2. **The daemon lists results in three tiers:** files whose raw path the query is,
   then the applications the query names in the desktop engine's order, then every other
   result by score, applications before equally scored files (ADR 0018). Exact file
   names come after named applications and before everything else.
3. **The daemon weighs every desktop match,** not only as many as the request's limit, so
   a named application that a boosted multiword match ranks below an unnamed one in the
   desktop engine still comes first. The desktop catalog is small; the extra work is at
   most a few microseconds per keystroke on a stock desktop (112 entries: 13 µs instead
   of 6 µs for "te", under a microsecond for longer words).
4. **Semantic fusion keeps the tier.** The lexical pool handed to the semantic service
   flags named applications, and fusion has a tier for them (`RANK_NAMED`) after exact
   paths and before exact basenames, so the final phase does not undo the lexical one.

## Alternatives considered

- **Reserve a few slots for applications:** they would still sort below every exact
  file, at the bottom of the popup, after the folders group.
- **Put every application before files:** promotes weak metadata matches, which is what
  ADR 0018 rightly rejected.
- **Raise the name bonus:** the exact tier is far above the bonus's 4,096 limit, and a
  score threshold cannot tell name matches from others once a query has three words.
- **Exempt only files in dependencies:** the place penalty is internal to the score;
  exposing it couples the daemon to score encoding, and a person's own `google` folder
  is still rarely what "google" means in a launcher.

## Consequences

- Typing a word that starts an application's name lists that application first even
  when many files carry the word as their whole name; those files follow it. A typed path
  still lists its file first.
- With many applications sharing a short prefix ("s"), named applications can fill the
  popup's ten rows; an exact file of that one-letter name is then not shown.
- `tests/test_desktop.py` checks Google Chrome first with twelve folders named `google`
  at limits 1, 10 and 1000, the exact file `chrome` right after the application, and an
  exact path first. `tests/unit/test_desktop.c` covers `desktop_named`, and
  `tests/unit/test_rank.c` the fusion tier. The mixed relevance evaluator merges as the
  daemon now does when the library has `desktop_named`.
- On a real Linux Mint home (213k paths), checking every installed application's name,
  name words and typed prefixes in the top ten: name failures went from 2 to 0, name-word
  failures from 54 to 7 (all shared words such as "manager", where other named
  applications rank first) and typed-prefix failures from 26 to 2.
