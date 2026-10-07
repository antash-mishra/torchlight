# embed

> **Status:** M4 adapter implemented; native Potion backend and preprocessing available
> **Source:** `src/index/embed.c` · **Header:** `include/torchlight/embed.h`
> **Tests:** `tests/unit/test_embed.c`

## Purpose

Owns a swappable local `tl_embedder` backend, immutable model metadata and
validated float output. It supplies the interface for model comparison; the
separate Potion module loads and runs the trained English backend through it. See [M4 implementation](../../m4-implementation.md) and
[ADR 0022](../../adr/0022-m4-semantic-foundation.md).

## Public API and ownership

`embedder_create` copies the model descriptor and callback table. The backend
context transfers only on success; failed creation leaves it with the caller.
`embedder_destroy` calls the backend destructor exactly once. The descriptor
returned by `embedder_model` borrows the adapter's lifetime.

`embedder_encode` distinguishes query and document roles so a backend can use
the model's correct prompts. It accepts nonempty prepared UTF-8 text up to
16,384 bytes, calls the backend and L2-normalizes its finite nonzero vector.
Invalid backend output becomes `TL_STATE`; backend errors propagate. Output is
cleared on failure when the adapter, output buffer and dimension are valid.
The wrapper uses no heap allocation or I/O, but a backend may allocate.

## Model identity and invariants

The owned descriptor records `emb_gen`, model id/revision, tokenizer version,
preprocessing version, dimension, projection version and quantization version.
All versions must be explicit, including `none` for an unused projection.
`nested_prefixes` declares that leading components form a usable smaller
embedding (Potion sets it: its exports truncate a Matryoshka-trained model). It
describes the model, not stored vectors, so it is not part of `emb_gen`; the
semantic service uses it to choose the prefix-shortlist vector search
([ADR 0031](../../adr/0031-m4-prefix-shortlist-vector-search.md)).
The caller assigns a nonzero `emb_gen` uniquely for the complete descriptor;
persisted identity and replacement publication belong to later service/storage
work. There is no model activation or snapshot registry in this adapter.

Each adapter has one inference caller at a time. Independent adapters can use
independent contexts; the interface assumes neither backend thread safety nor
preemption. Queuing and cancellation belong to the implemented semantic service.

## Text preparation and model selection

Raw filesystem paths retain their original byte identity in lexical search and
actions. They cannot be passed directly to inference when malformed UTF-8 is
present. The native Potion backend provides `launcher-text-1` preprocessing and
pinned-reference inference. Names/extensions/nearby scoped folders and app metadata
are evaluated with English queries. [Model evaluation](../../m4-model-evaluation.md)
records the selected provisional backend, parity and remaining comparison gates.
The adapter itself still owns no activation registry; the service provides it.

## Testing

Fixture callbacks test metadata copying (including `nested_prefixes`), query/document roles, Unicode input,
failure ownership, destructor calls, malformed/oversized text, backend errors,
zero/NaN/infinite vectors, normalization and reuse after failure. They test the
adapter contract and do not establish trained-model parity or semantic quality.

## Related

- [vector](../vector/README.md)
- [rank](../rank/README.md)
- [M4 work remaining](../../m4-implementation.md)

## M4 integration

See [native backend/service decision](../../adr/0023-m4-native-potion-and-two-phase-search.md)
and [measured model evaluation](../../m4-model-evaluation.md).
