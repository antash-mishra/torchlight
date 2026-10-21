# 0062. Leave out folders that tools manage

- **Status:** Accepted; revises the rejected default exclusions of
  [ADR 0047](0047-place-ranking-and-excluded-folders.md)
- **Date:** 2026-10-21

## Context

Torchlight indexes the whole home folder. On the live desktop 66% of the 213,520 entries
were inside `~/Android/Sdk`, 17.5% in Go's module cache and about 15,000 in one Python
virtual environment. Every everyday word matched hundreds of their files, mostly in pairs:
the SDK keeps versions side by side (`sources/android-35` and `android-36`; NDK 27.0 and 27.1
hold 9,165 identical entries each). Typing "camera" listed six `camera` folders from the
SDK, the NDK and a cloned library and four `camera.h` files; the person's own camera code
started at rank 9. Across 35 everyday words, 254 of the popup's 350 rows were inside
third-party trees (SDKs, caches and cloned libraries) and 72 were a second copy of a row
already shown.

ADR 0047's place penalty (at most 256) cannot fix this: it reorders matches of similar
strength, and a folder whose name is exactly the word (`…/sysroot/usr/include/camera`,
10,000,000 less 256) outranks `~/workspace/engine/src/camera.cpp` (6,182). ADR 0047 offered
`exclude`, but each person would have to find and list their own SDKs and caches. The
crawler already skipped hidden folders and `node_modules`, `target`, `build` and
`__pycache__` for everyone.

## Decision

Extend that built-in rule to folders only tools make and folders a tool marks as its own.
The folder and everything in it are not indexed and not watched:

- **Names:** `bower_components`, `site-packages`, `dist-packages` (dependency installs) and
  `CMakeFiles` (CMake's build internals), beside the existing ones.
- **Markers a tool writes into the folder it manages:** a regular `pyvenv.cfg` (Python
  virtual environments from venv, virtualenv or uv, whatever the folder is called), a
  `conda-meta` directory (Conda environments and installations), a regular
  `.knownPackages` (an Android SDK, written by its package manager wherever it lives), a
  regular `CACHEDIR.TAG` that starts with the
  [Cache Directory Tagging](https://bford.info/cachedir/) signature (caches and build
  outputs that follow the standard), and a folder named `mod` holding `cache/download`
  (Go's module cache).

Explicit roots and the `allow` setting override markers as they override names, and
`exclude` still wins over everything. One decision function serves walks, scoped rescans
and coverage checks. A walk checks a directory it enters against that directory's own
listing, which fts builds anyway and reuses: only a directory holding a marker's name pays
for lookups (type checks, the tag's first 43 bytes). Coverage checks and children-only
rescans, which do not list the directory, look the markers up directly and end in the same
test, so every path agrees.

Markers rather than folder names for SDKs: `sdk`, `vendor`, `third_party` and version
numbers stay place penalties (ADR 0047), because people give their own folders those names.

## Alternatives considered

- **Default exclusions by path** (`~/Android/Sdk`, `~/go/pkg`): wrong wherever a tool was
  configured differently; markers travel with the folder.
- **Folder names such as `sdk` or `vendor`:** a person's own project can use them.
- **Hiding these files at query time:** keeps 88% of this index's entries, memory and
  watches for files a launcher never shows.
- **A larger penalty or a separate tier for dependency trees:** still padding when the
  person has no matching file of their own, and exact names stay above any penalty.
- **`CMakeCache.txt` as a marker:** an in-source build puts it in the project's own root
  (it is in a cloned library's root on the live desktop), which would hide the sources.

## Consequences

- On the live desktop, indexing the same home with an isolated `-O3` daemon: 213,520
  entries became 26,169, inotify watches 34,498 became 3,803, resident memory after the
  first index 176 MiB became 38.5 MiB, and the first index took 1 s and 0.5 CPU-s instead of
  5 s and 3.8 CPU-s. In the 35-word sample (with ADR 0061) third-party rows went from 254 to
  58, all in cloned or unpacked libraries that carry no marker, and no row was a second
  version of another (72 before).
- Files inside these folders cannot be found; `allow = PATH` (or `root = PATH`) brings one
  back.
- A folder that becomes managed after it was indexed (a venv whose `pyvenv.cfg` appears
  after its folder) stops being covered at its next change; the writer then runs a full
  scan, which prunes it. Pip's installs land in `site-packages`, skipped by name meanwhile.
- Cloned libraries and unpacked tools carry no marker and stay (`~/assimp`, two Vulkan SDK
  versions on the live desktop); the place penalty and personal ranking order them.
- The marker list is policy and will need entries for other ecosystems (Wine prefixes,
  Steam libraries, game installs). `tool_managed_folders` in `test_crawl.c` covers each
  marker, look-alikes that stay, coverage, scoped walks, the allowlist and explicit roots;
  `tests/test_cli.py` checks `site-packages` and a venv stay out of the index.
