# Third-party code

Vendored directories are copied verbatim from upstream and never edited in
place. Update one by replacing the whole directory with a new upstream release
and recording the new tag and commit here.

## frizbee

| Field | Value |
|---|---|
| Upstream | https://github.com/saghen/frizbee |
| Tag | `v0.13.0` |
| Commit | `d608923f526c57c7ac90b79c0cb1b8619b449179` |
| License | MIT (`frizbee/LICENSE`) |
| Copied | 2026-10-07, every file except `.git/` |
| Used for | Production fuzzy scoring through the C binding (`bindings/frizbee-c`), see [ADR 0029](../docs/adr/0029-m6-frizbee-scoring-and-candidate-volume.md) |

`frizbee-build/` is **not** vendored. It holds two Torchlight-owned Cargo
manifests that compile the vendored sources without editing them:

- `frizbee-build/core/Cargo.toml` builds `frizbee/src` as the `frizbee` crate.
  It mirrors upstream's manifest but omits the optional `serde` dependency, so
  the build needs no crate registry and works offline with an empty Cargo home.
- `frizbee-build/Cargo.toml` builds `frizbee/bindings/frizbee-c/src` as the
  static library `libfrizbee.a` (with `panic = "abort"`, since unwinding into C
  is undefined).

`make` runs `cargo build --release --offline --locked` on these manifests into
`build/frizbee/`. Rust (1.89 or newer) is therefore a build-time dependency of
every binary; the C header is used directly from
`frizbee/bindings/frizbee-c/include/frizbee.h`. Frizbee selects AVX-512, AVX2,
SSE4.1 or scalar kernels at run time, so the library needs no target-CPU flags.

To update: replace `frizbee/` with the new release, compare its `Cargo.toml`
features and edition with `frizbee-build/core/Cargo.toml`, run
`cargo generate-lockfile --offline --manifest-path third_party/frizbee-build/Cargo.toml`,
then `make test` and `make bench`.
