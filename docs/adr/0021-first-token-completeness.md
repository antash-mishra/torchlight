# 0021. Preserve first-token completion at basename strength

- **Status:** Accepted
- **Date:** 2026-10-05

## Context

M3 Part 2 adds 128 points for complete basename tokens, but a token starting
the basename receives only 5128 while the same entry's whole-basename prefix
receives 6000. Keeping the strongest hit discards completion evidence. Searching
`project` therefore ranks `projectile.txt` (6050) before `project notes.txt`
(6047), because the shorter filename wins the remaining tie.

## Decision

Give the existing token key at `text.basename` basename-prefix strength. Store
completed token scores in the existing score member and remove the 128-point
bonus for shorter query fragments. Complete first tokens now score 6128;
later basename tokens remain 5128. Whole-basename keys, initials and parent
tokens retain their previous behavior. No keys, per-key flags or query buffers
are added.

Auxiliary-field mapping recognizes completion at either token tier, then uses
the existing generic-name/keyword weight plus 128. The lexical query bounds
already include the completion bonus, including exact-match priority and the
seal-time one-symbol cache.

## Alternatives considered

Increasing every completed token above the basename tier would also promote
later tokens over basename prefixes. An additional first-token key or per-key
flag would grow the index unnecessarily. Applying the fix only in lexical
ranking would leave the standalone prefix channel discarding the bonus.

## Consequences

`project notes.txt` scores 6175 and outranks `projectile.txt` at 6050. Shorter
fragments keep their previous scores. Regressions cover separator, camelCase,
acronym, case-folded and one-symbol queries, result capacities, exact-name
priority and auxiliary-field completion. Query allocation and storage layouts
remain unchanged. Favoring complete first tokens also shifts ambiguous synthetic
rankings; the 50k parent-query known-item top-ten metric decreases while the mixed
graded fixture retains its results. Validation and 50k/500k measurements are recorded in
[M3 Part 2 verification](../m3-part2-completion.md).
