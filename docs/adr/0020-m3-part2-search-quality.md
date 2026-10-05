# 0020. Explicit fields, prefix edits and optimal fuzzy alignment

- **Status:** Accepted
- **Date:** 2026-10-05

## Context

ADR 0018 fixes Chrome name priority, but an unfinished transposition such as
`proej` still cannot retrieve `projectNotes.md`. Desktop metadata encoded as
parent directories conflates keywords, generic names and filesystem context.
Greedy character alignment can hide a better abbreviation match later in a name.

## Decision

Keep the existing complete-token deletion index and lexicographically order its
distinct terms. Their contiguous prefix ranges form an implicit trie. Walk it
with clipped optimal-string-alignment rows, pruning branches beyond one edit;
expand every accepted subtree without a candidate cutoff. Prefix editing starts
at five normalized symbols, complete-token editing at three, both ending at 32.
Exclude numeric-only query words and tokens. Adjacent swaps cost one edit.
No extra persistent trie or prefix-deletion table is required. Indexed terms
remain limited to 32 symbols; longer terms retain the other lexical channels.

Add `lexical_add_fields` to copy independent generic-name and keyword fields.
Names remain basenames; filesystem folders remain interned directory context.
Indexed field prefixes score 2800/2600 respectively, versus folder prefixes at
2500 and name tokens at 5000. Complete name/auxiliary tokens gain 128. Desktop
name prefixes retain their existing 2000 bonus. Every query word remains required;
one-edit evidence stays below exact prefix evidence. Fuzzy scoring rewards
boundary alignment and character proximity without treating metadata as folders.
Auxiliary fuzzy matching scans the explicit fields, intended for the bounded
application catalog; file catalogs with no auxiliary fields incur no field scan.

Use optimal alignment for text up to 512 normalized symbols, with fixed stack
rows and running maxima for capped gaps. Complexity is O(query × text), with an
ordered-membership rejection pass. Larger text retains the greedy fallback.
Keep `fuzzy_score_greedy` as a comparison API. The existing score upper bound
remains valid; token-completeness bounds are updated before pruning and sealing
one-symbol caches. Queries still allocate nothing and perform no I/O.

## Alternatives considered

A separate token trie/FST would require additional persistent nodes or a new
library. An index of every prefix deletion would multiply memory quadratically
in token length. Sorted-term range traversal reuses the existing dictionary.
The labeled evaluator also compares exact-token field-weighted BM25 (k1=1.2,
b=0.75); it provides corpus weighting but cannot retrieve unfinished edits or
abbreviations without separate expansion. Optimal and greedy scoring share the
same bonuses, isolating alignment rather than adopting unrelated fzf constants.

## Consequences

Independent exhaustive alignment and full OSA matrix tests check the algorithms.
Allocator interposition, cold/warm and top-k checks protect query contracts.
The mixed fixture separates target families between tuning and held-out queries,
labels ambiguous alternatives, and records per-class recall, first-useful rank,
top-ten success and graded nDCG. Held-out relevance improves over revision
`7ace90f0a3d1474a0594b08ee8db9fde72e0f713`. The fixture is deliberately small;
it does not establish universal relevance or personal usage quality.

Optimal alignment and complete prefix-edit expansion cost additional query work.
The [completion report](../m3-part2-completion.md) records measurements and limits;
5 ms optimization and whole-engine rebuild improvements remain separate work.
No dependencies or persistence/IPC schema changed.
