# Potion

> **Status:** Implemented for M4; pinned Potion retrieval 32M, native CPU inference
> and model loading restricted to regular files.

Public contract: [potion.h](../../../include/torchlight/potion.h).

The adapter loads owned tables, checks size arithmetic, SHA256 payload integrity,
UTF-8 vocabulary offsets, unique tokens and finite weights. TLSTAT01 contains
little-endian counts/offsets/float32 components, source revision and source hashes.
The header is 220 bytes; the payload contains count+1 offsets, NUL-terminated
vocabulary and a row-major table. No mappings, Python, network or ONNX runtime
are used by C inference. Python export/reference evaluation dependencies are
isolated and optional. Large model artifacts stay in ignored `build/models/`.

Model inputs must be regular files; symlinks to regular files remain supported.
The loader opens with nonblocking flags, then checks the actual descriptor's
type before reading. FIFOs, devices and directories return `TL_IO`, so a FIFO
without a writer cannot block semantic-worker shutdown. Checking the opened
descriptor also avoids a path-replacement race between validation and reading.
Unit tests cover regular symlink targets and directories; a daemon regression
covers unavailable semantics and bounded shutdown with a FIFO model path.

BERT normalization lowercases, strips accents and controls, separates Chinese
characters and punctuation, then performs longest-match WordPiece with the
100-character bound. Literal special tokens retain reference behavior. Unknown
words are discarded in their entirety, including partially matched prefixes.
Mean pooling followed by L2 normalization matches Model2Vec; CLS/SEP are not
inserted. Scratch belongs to one serialized adapter and encoding allocates
nothing. Matryoshka dimensional truncation receives a distinct emb_gen.

`launcher-text-1` uses names, extensions and at most two nearby parents. The
service clips context at the nearest indexed root's basename; host-specific
parents outside indexing scope cannot add semantic evidence. Camel-case and
path separators become spaces; malformed path bytes become spaces while the
result identity retains the original bytes. Applications use display name,
generic name and keywords. File contents are not opened.

See [model evaluation](../../m4-model-evaluation.md) for the pinned export,
108-case trained-model parity and analytic sanitizer loader/tokenizer tests.
