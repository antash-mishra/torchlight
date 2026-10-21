# 0061. Word matches first; near matches only when nothing else matches

- **Status:** Accepted; revises the metadata ranking of [ADR 0018](0018-application-name-ranking.md)
  and extends the tiers of [ADR 0058](0058-applications-the-query-names-come-first.md)
- **Date:** 2026-10-21

## Context

On the live desktop the person typed everyday words and found lists they called garbage.
[ADR 0062](0062-folders-tools-manage.md) removes the biggest source, folders that tools
manage, but two ranking rules then still filled the popup's ten rows badly:

- **Near-match padding.** A query lists whatever matches at all, and the typo, trigram and
  scattered-letter channels match almost anything. With the SDK gone, "sound" listed the
  person's `Sound.ts` and `Sound.test.ts`, then `GenBoundingBoxesProcess.h` (a typo of the
  start "bound"), `round_grill.png` (a typo), `test_sample_boundaries.py` (scattered
  letters). "battery" listed ten files sharing letter groups with it (`TinyFormatter.h`
  shares "att", "tte" and "ter"). These channels exist for misspelled queries, but they ran
  just as much when the word matched real names.
- **Tagged settings out of reach.** A settings panel or application tagged with the word
  (its keywords or generic name) scored 2,600 (2,800 for generic names), below every file
  whose name starts with the word (6,000). On the live desktop "audio" put Sound settings,
  VLC and Rhythmbox at ranks 731 to 733, "wifi" put Network at 254 and "microphone" put
  Sound at 23. ADR 0018 kept metadata at ordinary strength so that weak metadata matches
  could not hide an exact file; it did not tell a whole tag from part of one or from a typo.

## Decision

1. **Word-start evidence.** A match is a *word start* when each query word starts the
   entry's name, a word of the name (whole camelCase words included, ADR 0060) or its
   initials, or is its exact name or path; for applications, also when it starts a word of
   the generic name or keywords. Anything else is a *near match*: a typo, shared trigrams,
   scattered letters or a parent folder's name alone. `lexical_word_start` tells them apart
   for a file result of a one-word query by score (word starts score at least the initials
   score less the largest place penalty, 4,244; a typo, the strongest near match, at most
   3,564, a gap a static assertion keeps; only scattered letters of a word of 34 or more
   symbols, nearly contiguous by then, can reach the line). `desktop_evidence` classifies each application
   per word with `prefix_score`: *named* (ADR 0058), *described* (each other word is a whole
   word of the generic name or keywords), *word start* or *fuzzy*.
2. **No near-match padding for one word.** For a one-word query without `/`, once any file
   or application matches by a word start, the daemon drops the near matches. Results the
   person opened before (personally boosted) stay, but a boost that lifts a near match over
   the line does not make it a word start that drops the others: the daemon judges each
   file on its score less its boost. When nothing matches by a word start,
   near matches remain the fallback, so typos still find things. Queries of two or more
   words keep near matches: each word must match anyway, and their summed scores cannot
   tell the evidence of each word apart.
3. **Tagged applications after exact names.** The daemon's tiers become: exact raw paths,
   named applications, exact file names, described applications, then the rest by score.
   Part of a tag ("aud" for "audio") keeps ordinary metadata strength. Semantic fusion gets
   `RANK_DESCRIBED` after exact basenames, so its final phase keeps the order.

## Alternatives considered

- **A score floor on every query.** A two-word score sums its words, so a floor would cut
  good matches whose words mix strong and parent evidence and keep weak ones that sum high.
- **Cutting inside the lexical engine.** The decision needs every file segment (base and
  delta) and the desktop catalog together, and each engine's exact pruning bounds would have
  to agree with a new rule. The daemon already merges all of them.
- **Ordering multiword results by strength first.** Right in principle, but it changes every
  pruning bound of the engine; left for later, since every word must match anyway.
- **All applications before files** (rejected by ADR 0018 and ADR 0058). A described
  application needs the whole tag, and fuzzy metadata matches are dropped, which removes the
  reason for the rejection; partial tags still rank by score.
- **Reserving popup rows for applications** (rejected by ADR 0058): they would still sort
  after every exact file.

## Consequences

- On the live desktop (with ADR 0062): "sound" lists Sound, System Info, `Sound.ts` and
  `Sound.test.ts` and nothing else; "wifi" lists Network; "battery" Power Statistics;
  "microphone" Sound; "audio" the person's `audio` folders, then Sound, VLC and Rhythmbox,
  then their audio files. Across 35 everyday words the popup showed 246 rows instead of
  350, 79 of them applications or settings instead of 36. Typing any keyword of any of the
  111 listed applications now shows it among the ten results 426 times out of 428 (147
  before); see [the evaluation](../evaluation.md#word-matches-first-and-folders-tools-manage-2026-10-21).
- Lists get shorter. Two rows is the honest answer when two things match.
- A file containing the word inside another word ("playsound.wav" for "sound") is not shown
  for that one word while a word start exists; typing a second word ("sound wav") or having
  opened it before brings it back.
- `tests/test_desktop.py` checks the cut, the typo fallback, a second word keeping near
  matches, an opened near match that keeps the other near matches, and the tagged tier ("browser" now lists Google Chrome, whose generic name is
  "Web Browser", first; "brow" still lists the file). Unit tests cover `desktop_evidence`,
  `lexical_word_start` and the fusion tier.
- The mixed relevance evaluator merges the same way; on `mixed.json` this change and the
  revision before it both score 1.0 on every metric of every labeled query, so its gate
  (which requires improving on the recorded baseline) is unchanged.
- Cost: `desktop_evidence` checks the name, generic name and keywords for each word of each
  application match, skipping the fields when no word of them starts with the word's first
  symbol. On the live desktop (112 applications) it takes 20 to 28 µs for a one-letter query,
  where naming alone took 10 to 12 µs, and 6 to 7 µs for a whole word. The cut is linear in
  the results.
