# 0060. Later CamelCase words match when typed whole

- **Status:** Accepted
- **Date:** 2026-10-17

## Context

The tokenizer splits words at case changes (`SQLite` → `SQ` + `Lite`, `VirtualBox` →
`Virtual` + `Box`), and the prefix channel keeps one key per token plus a key for the
whole basename. A word typed whole therefore matched only when it began the basename
(`libreoffice` → `LibreOffice Writer`, through the basename key). A later word did not:
`sqlite` had no prefix evidence on `DB Browser for SQLite`, only scattered letters, and on
a home folder with SQLite sources that left the application out of the popup's ten
results; `virtualbox` and `Oracle VM VirtualBox` behave the same way. Files share the
problem: `packagename` misses `CMAKE_FIND_PACKAGE_PackageName.rst`. Such a word also
cannot name an application (ADR 0058), whose test is the same prefix evidence.

## Decision

A separator-delimited word of the basename after its first that changes case inside gets
one more prefix key: the whole word, at token strength (5,000, and 5,128 when the query
is the whole word). Its tokens keep their own keys. Only case changes count: a start
between two symbols that are neither digits nor separators. Words joined only through
digits (`v2Final`) get no key, since digits split far more names than case does. The
first word needs none, since the basename key starts with it, and parent folders are
unchanged. The key is produced by the visitor that both `prefix_add` and `prefix_score`
use, so the index and per-text scoring (the side pass, `desktop_named`) agree.

## Alternatives considered

- **Stop splitting tokens at case changes:** loses `parser` → `HTMLParser` and every
  camelCase token match the engine relies on.
- **Make every token key run to the end of its word:** changes completeness for every
  CamelCase name (a typed `foo` would no longer complete `FooBar`'s first token,
  ADR 0021) and reorders many existing results.
- **Add the whole word as a desktop keyword:** fixes applications only, at keyword
  strength, which neither ranks them nor names them.

## Consequences

- `sqlite` names DB Browser for SQLite; `virtualbox` names Oracle VM VirtualBox; files
  match their later CamelCase words typed whole. No existing match loses evidence: the
  new key starts where a token key starts, so it only adds hits for queries that run
  past that token.
- On a real home folder of 213,518 paths, 2,919 basenames (1.4%) gain a key, 2,937 keys
  in all. In three alternating `bench_lexical` runs against the previous build, typing
  stayed the same (p50/p95 0.200/2.364 → 0.199/2.330 ms on that list, 0.695/3.233 →
  0.696/3.219 ms on 500,000 synthetic paths), steady memory grew by 16 KiB, the benchmark's
  labeled quality was unchanged, and building the index took 4.5% longer on the real list
  (0.99 → 1.03 s; 1.2% on the synthetic one) for the extra pass over each basename.
- `tests/unit/test_prefix.c` checks the new keys, their scores and completeness, that
  first words, digit-joined words and fragments past the word get none, and that
  `prefix_score` still reports exactly what `prefix_query` does;
  `tests/unit/test_desktop.c` checks that `sqlite` names DB Browser for SQLite.
