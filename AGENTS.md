# CLAUDE.md

Torchlight is a Spotlight-style launcher for Linux, written in C, that
searches filenames and paths.

**Read first:** `docs/README.md` (docs index), `docs/architecture.md`
(components, flows, dependency direction), and `PLAN.md` (design, schema,
milestones).

## Layout

```
include/torchlight/   public headers, one per module
src/core/             generic reusable utilities (no Torchlight domain knowledge)
src/index/            tokenize, dirtree, lexical (prefix, trigram, subseq, typo),
                      fuzzy, embed, vector, rank
src/storage/          store.c, the ONLY place SQL lives
src/fs/               crawl, watch
src/ipc/              socket protocol
src/service/          resident daemon loop and background writer orchestration
src/bin/              thin executables that wire modules together, no algorithms
ui/                   gtk/ popup, optional tui/ (daemon communication via ipc only)
tests/                unit/test_<module>.c, test_cli.py, bench/, fixtures/
docs/                 architecture, glossary, evaluation, adr/, modules/<module>/README.md
models/ scripts/ third_party/ (vendored, never edited in place)
```

## Commands

```sh
make            # debug build
make test       # unit tests (ASan + UBSan)
make lint       # clang-tidy + cppcheck
make format     # clang-format
make bench      # latency + ranking benchmarks (BENCH_PATHS=file adds a real corpus)
```

`make` also compiles the vendored Frizbee matcher (`third_party/frizbee`) with
`cargo`, offline, into `build/frizbee/` once; Rust 1.89+ is a build dependency.

## Code rules

**Readable**
- Use clear, boring code and descriptive names (`trigram_index_add`, not
  `tia`).
- Keep functions under ~50 lines with one responsibility. Return early, and
  keep nesting to 3 levels or fewer. Use named constants, not magic numbers.

**Commented**
- Every file starts with a header comment saying what the module does.
- Every public function gets a doc comment covering purpose, params, return,
  pointer ownership/lifetime, and errors.
- Inline comments explain *why* (algorithms, constants, invariants), not
  *what*.

**Modular and reusable**
- Each module has an opaque type plus `<module>_create`, `<module>_destroy`
  and a small API. Callers never touch internals.
- No global mutable state. Pass context explicitly so any component can be
  instantiated many times.
- Swappable parts are interfaces (function-pointer structs), e.g.
  `tl_embedder`.
- Shared helpers (arena, vec, hashmap) live once in `src/core/`. Don't
  re-implement them.
- Every module must be unit-testable in isolation.

**C**
- C17, `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`.
- `snake_case`, with public symbols prefixed by the module name. Macros and
  enum constants are `UPPER_SNAKE`. Internal helpers are `static`.
- Fallible functions return `tl_status` and pass outputs through pointers.
  Never ignore a status, and never `exit`/`abort` in library code.
- Whoever allocates provides the free function. Use `goto cleanup` for
  multi-resource functions, and arenas for per-query temporaries.
- No `strcpy`, `sprintf` or `gets`. Validate all external input and check
  size arithmetic for overflow.
- Headers use include guards, are self-contained, and are const-correct.

**Performance and SQLite**
- The lexical/ranking path does no filesystem I/O, SQL, or global heap
  allocation.
- Paths are raw bytes, never assumed to be UTF-8.
- Say `catalog_gen` or `emb_gen`, never a bare "generation".
- SQL uses prepared statements only, never string-built. Schema changes go
  through migrations.

**Dependencies:** discuss before adding any.

## Docs

Follow "Keeping docs current" in `docs/README.md`. In short:
- Update `docs/modules/<module>/README.md` (including Status) in the same
  change as the code.
- Write an ADR in `docs/adr/` for design decisions.
- Add new terms to `docs/glossary.md`.
- Header comments are the API source of truth. Module docs explain why and
  how.

## Definition of done

- [ ] `make test` and `make lint` pass with zero warnings.
- [ ] Doc comments, tests (with a regression test for each fix) and the
      module doc are updated.
- [ ] An ADR is added if a design decision was made, and `make bench` was run
      if the query path changed.
- [ ] `CLAUDE.md` and `AGENTS.md` are identical.
