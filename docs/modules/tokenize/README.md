# tokenize

> **Status:** Implemented for first M1 increment
> **Source:** `src/index/tokenize.c` · **Header:** `include/torchlight/tokenize.h`
> **Tests:** `tests/unit/test_tokenize.c`

## Behavior and ownership

Normalization uses approved utf8proc 2.9.0: Unicode case-folding plus canonical
NFC composition of valid grapheme clusters. Each malformed byte becomes
`TOKENIZE_OPAQUE_BASE + byte`, never U+FFFD in matching keys. Original path bytes
are held separately by the engine/store. `TOKENIZE_VERSION` versions these rules;
changing Unicode data or boundary behavior requires rebuilding resident keys.

Owned tokenized paths contain scalar/opaque symbols, basename position,
separator/camelCase boundary flags, original grapheme byte offsets, and a
conservative 64-bit mask. Folded expansions share their source cluster offset.
`tokenize_into` uses caller buffers so query normalization does not allocate;
insufficient space returns TL_LIMIT. `tokenize_create/destroy` own path metadata.

Display creation preserves original valid UTF-8, replaces malformed bytes with
U+FFFD, escapes ASCII controls/backslashes, and must never supply action paths.
Tests cover canonical equivalents, sharp-s expansion, Greek case-folding,
Unicode camel boundaries, opaque-byte identity, malformed encodings, NUL
rejection, short buffers and safe display. Trigram byte-key emission is planned.

## Related

- [Implementation increment ADR](../../adr/0006-m1-prefix-subsequence-baseline.md)
- [Evaluation](../../evaluation.md)
- Public headers document parameters, lifetimes and error contracts.
