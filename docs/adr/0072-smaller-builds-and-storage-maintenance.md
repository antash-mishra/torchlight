# 0072: Smaller release builds and explicit storage maintenance

Status: Accepted (2026-11-01)

The footprint investigation found 33 MiB of installed executables and a 492 MiB
semantic cache while this workstation runs lexical-only search. The cache's
actual record payload is about 113 MiB. See [the measurements](../footprint-plan.md).

Release builds use function/data sections and linker garbage collection, retaining
portable static dependencies and offline CLI functionality. Source installations
use stripped release binaries; unstripped build outputs retain debugging data.
Ordinary `make` remains a debug build. Makefile changes invalidate release outputs.

Schema v5 moves embedding records from a WITHOUT ROWID index B-tree to an ordinary
rowid table with the same unique `(emb_gen,text)` key. The 1 KiB vector payload
fits table leaf pages without the existing layout's mostly empty overflow pages.
Migration copies bytes and touched flags inside the schema transaction, retaining
all model descriptors, file identities and history. Activation already retires
other model generations and untouched vectors, bounding successfully activated
caches to the current prepared texts. Disabling semantics does not silently purge
a reusable cache; an explicit maintenance operation handles that lifecycle.

`torchlight storage` reports an existing database through a read-only connection,
without creating or migrating it. `torchlight maintenance --purge-semantic`
requires the database singleton lock, removes only semantic data transactionally,
then compacts. Maintenance also works directly on v4 so an unwanted cache need
not first be copied to v5. The next ordinary open performs schema migration.

Compaction uses SQLite's journaled VACUUM under an exclusive connection lock,
with integrity/foreign-key checks before and after. SQLite keeps the original
database recoverable until its compaction commit; there is no application-level
rename or WAL-sidecar replacement protocol. The purge and compaction are separate
transactions: on compaction failure the catalog stays usable, and repeating
maintenance safely finishes reclamation. Running maintenance without the purge
flag compacts while retaining semantics. Neither operation runs during ordinary
search/startup. External model files are left to explicit user-directed removal;
Torchlight cannot infer whether another catalog references a model path.

Validation covers existing-schema migration and rollback, vector equality and
uniqueness, preservation of raw paths/IDs/history/catalog_gen, singleton/read-lock
contention, read-only reporting, interrupted/failed maintenance, semantic reuse,
and packaged release actions. No new dependency is introduced.
