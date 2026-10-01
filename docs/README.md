# Torchlight docs

Start here. Read in this order:

1. [`architecture.md`](architecture.md): how the pieces fit together and how a query flows.
2. [`glossary.md`](glossary.md): terms used throughout the code and docs.
3. [`evaluation.md`](evaluation.md): labeled queries, metrics, regression scenarios, recorded results.
4. [`modules/`](modules/): one folder per module, each with a `README.md`.
5. [`adr/`](adr/): Architecture Decision Records, explaining *why* things are the way they are.

## Implementation status

M1's first prefix/subsequence increment is implemented: local index/query CLI,
SQLite catalog, crawler, Unicode/raw-byte tokenization and warm-engine benchmark.
Trigram/typo retrieval and performance acceptance remain open. See the root
[README](../README.md) for commands and [0006](adr/0006-m1-prefix-subsequence-baseline.md)
for the incremental scope.

## Module index

| Layer | Module | Doc |
|---|---|---|
| core | Utilities (arena, vec, hashmap, log) | [core](modules/core/README.md) |
| core | Configuration | [config](modules/config/README.md) |
| index | Path tokenizer | [tokenize](modules/tokenize/README.md) |
| index | Lexical candidate orchestrator | [lexical](modules/lexical/README.md) |
| index | Exact/prefix channel | [prefix](modules/prefix/README.md) |
| index | Trigram channel | [trigram](modules/trigram/README.md) |
| index | Subsequence channel | [subseq](modules/subseq/README.md) |
| index | Typo channel | [typo](modules/typo/README.md) |
| index | Fuzzy/edit scorer | [fuzzy](modules/fuzzy/README.md) |
| index | Swappable embedder | [embed](modules/embed/README.md) |
| index | Vector search | [vector](modules/vector/README.md) |
| index | Ranking & fusion | [rank](modules/rank/README.md) |
| storage | SQLite store | [store](modules/store/README.md) |
| fs | Crawler | [crawl](modules/crawl/README.md) |
| fs | inotify watcher | [watch](modules/watch/README.md) |
| ipc | Socket protocol | [ipc](modules/ipc/README.md) |
| bin | Daemon | [daemon](modules/daemon/README.md) |
| bin | CLI client | [cli](modules/cli/README.md) |
| ui | GTK4 popup & optional TUI | [ui](modules/ui/README.md) |

## Current decisions

- [0003: retrieval, consistency, and measured milestones](adr/0003-retrieval-consistency-and-measured-milestones.md)
  supersedes the initial lexical and embedding proposals in 0001/0002.
- [0004: lexical module split, byte paths, two-phase responses, resident vectors](adr/0004-lexical-split-byte-paths-two-phase.md)
  refines 0003.
- [0005: candidate caches, response completion, and snapshot lifetimes](adr/0005-cache-and-response-correctness.md)
  refines 0003/0004; resolves later correctness and memory-accounting gaps.

- [0006: first M1 prefix/subsequence baseline](adr/0006-m1-prefix-subsequence-baseline.md)
  records the initial implementation and remaining M1 gates.
- [0007: partial scans, nested roots and component-scoped parent matching](adr/0007-partial-scans-and-component-parent-matching.md)
  refines 0006 after the first M1 review.

## Keeping docs current

- Adding a module? Copy [`modules/_TEMPLATE.md`](modules/_TEMPLATE.md) to
  `modules/<name>/README.md` and add it to the table above.
- Changing a module's API or behavior? Update its doc **in the same change**.
- Making a significant design decision? Add an ADR using
  [`adr/0000-template.md`](adr/0000-template.md).
