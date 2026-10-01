# 0001. Trigram + fuzzy scoring instead of BM25 for lexical search

- **Status:** Superseded by 0003
- **Date:** 2026-10-02

## Context
The original Torchlight reportedly used BM25. We currently index filenames and
paths. BM25 can provide term-rarity signals, but needs additional retrieval
strategies for prefixes, abbreviations, and typos.

## Decision
The initial proposal used trigram retrieval and fzf/fzy-style scoring. Review
found that abbreviations can share no trigrams with their targets and subsequence
scoring alone cannot correct arbitrary typos. ADR 0003 replaces this proposal.

## Alternatives considered
- **BM25:** weak signal on short strings; no typo tolerance.
- **SQLite FTS5 trigram tokenizer:** a good M1 shortcut, but every query goes
  through SQLite. We keep it as a fallback option.
- **SPLADE (learned sparse):** strong on documents, but heavy for keystroke
  latency and overkill for filenames.

## Consequences
- Trigram retrieval alone does not guarantee typo, prefix, or abbreviation recall.
- We maintain our own in-memory index and scorer.
