# SQLite (amalgamation distribution)

Pinned reference storage engine for Mira's durable state backends
(`docs/plans/m4-context-memory-recovery.md` work items `M4-06`/`M4-09`).

| 字段 | 值 |
| --- | --- |
| Version | 3.53.4 (`SQLITE_VERSION_NUMBER` 3530400) |
| Distribution | `sqlite-amalgamation-3530400.zip` |
| Source | <https://sqlite.org/2026/sqlite-amalgamation-3530400.zip> |
| Archive SHA-256 | `1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d` |
| License | Public Domain ([LICENSE.md](LICENSE.md)) |
| Vendored on | 2026-09-03 |

Only the amalgamation translation unit is used (`sqlite3.c` plus headers);
`shell.c` from the archive is intentionally not vendored. The per-file SHA-256
digests are locked in [`dependencies.lock.json`](../../dependencies.lock.json)
and verified by `tools/check_sbom.py` on every CI run.

Build configuration applied by [`CMakeLists.txt`](CMakeLists.txt):

- `SQLITE_ENABLE_FTS5` — keyword retrieval for the memory reference backend.
- `SQLITE_THREADSAFE=1` (serialized) — required by the threading model below,
  even though Mira serializes access through one worker.
- `SQLITE_OMIT_LOAD_EXTENSION` — no dynamic-loader dependency on any target
  platform (Android/Windows friendly).
- `SQLITE_DQS=0` — double-quoted string literals remain an error.

Threading model: SQLite never spawns threads in this configuration. All
database access is issued from a single dedicated Executor blocking-I/O worker
that owns the only connection (single writer, bounded request channel, WAL
journal mode). See `docs/design/context_and_memory_design.md` §16/§17 and
`AGENTS.md`.
