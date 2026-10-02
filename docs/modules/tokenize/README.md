# tokenize

> **Status:** Implemented (M1), contract `utf8proc-nfc-casefold-opaque-v2`
> **Source:** `src/index/tokenize.c` · **Header:** `include/torchlight/tokenize.h`
> **Tests:** `tests/unit/test_tokenize.c`

## Behavior and ownership

Normalization uses approved utf8proc 2.9.0: Unicode case-folding plus canonical
NFC composition of valid grapheme clusters. Each malformed byte becomes
`TOKENIZE_OPAQUE_BASE + byte`, never U+FFFD in matching keys. Original path bytes
are held separately by the engine/store. `TOKENIZE_VERSION` versions these rules;
changing Unicode data or boundary behavior requires rebuilding resident keys.
An ASCII fast path skips the general decoder and grapheme-break checks for runs
of ASCII (any two ASCII characters except CR LF form separate clusters).

Owned tokenized paths contain scalar/opaque symbols, basename position, word
boundary flags, original grapheme byte offsets, and a conservative 64-bit mask.
v2 word boundaries: after a separator (`/ _ - . space`), lower→upper case
(`fooBar`), the last capital of an acronym before lowercase (`HTML|Parser`) and
letter↔digit changes (`report|2024`). Mask bits: `a`–`z` and `0`–`9` have
dedicated bits (the filter is exact for them); every other symbol hashes into
the remaining 28 bits, where collisions only admit extra candidates.

`tokenize_into` uses caller buffers so query normalization does not allocate;
insufficient space returns TL_LIMIT. `tokenize_create/destroy` own path metadata.
Display creation preserves original valid UTF-8, replaces malformed bytes with
U+FFFD, escapes ASCII controls/backslashes, and must never supply action paths.

Tests cover canonical equivalents, sharp-s expansion, Greek case-folding,
Unicode camel boundaries, acronym and digit boundaries, distinct ASCII mask bits,
opaque-byte identity, malformed encodings, NUL rejection, short buffers and safe
display.

## Related

- [Implementation increment ADR](../../adr/0006-m1-prefix-subsequence-baseline.md)
- [M1 completion ADR](../../adr/0008-m1-completion-channels-directories-config.md)
- Public headers document parameters, lifetimes and error contracts.
