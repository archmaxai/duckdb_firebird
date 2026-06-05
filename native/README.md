# DuckDB Firebird Catalog Extension (native `ATTACH`)

A **real DuckDB storage/catalog extension** that lets you attach a Firebird
database and query its tables with native SQL — no `firebird_query(...)` table
function required:

```sql
INSTALL firebird FROM 'https://<your-pages-host>/firebird';   -- once
LOAD firebird;

ATTACH 'firebird://user:password@host:3050//path/to/db.fdb' AS fb (TYPE firebird);

-- Firebird tables now behave like native DuckDB tables:
SHOW ALL TABLES;
SELECT * FROM fb.EMPLOYEES WHERE ACTIVE;
SELECT d.NAME, count(*)
  FROM fb.PROJECT p JOIN fb.DEPARTMENT d ON p.DEPT_ID = d.ID
  GROUP BY d.NAME;
```

Unlike the table-function approach (on the `main` branch), this extension
registers a Firebird **catalog**: tables and their column types are discovered
from the Firebird system tables, appear in `SHOW ALL TABLES` /
`information_schema`, and the query optimizer pushes **column projection**,
**`WHERE` filters**, and **`LIMIT`** down into the Firebird query.

### Filter pushdown

Simple predicates are translated into the Firebird `SELECT` so that filtering
happens server-side and fewer rows cross the network. Supported: comparisons
(`= <> < <= > >=`), `IS [NOT] NULL`, `IN (...)`, `BETWEEN`, and `AND`/`OR`
combinations of these on a single column. You can confirm what was pushed with
`EXPLAIN` — pushed predicates appear under `Filters:` on the `FIREBIRD_SCAN`
node. Anything not pushable (e.g. `LIKE`, expressions like `upper(col)=...`,
**joins and aggregations**) is still evaluated by DuckDB after the scan.

### Limit pushdown

A `LIMIT [OFFSET]` sitting directly above a scan (through column projections
only) is appended to the Firebird query as a `ROWS` clause, so Firebird stops
producing rows early. `count + offset` rows are requested and DuckDB still
applies the exact `LIMIT`/`OFFSET` on top — the pushdown only reduces transfer.
It is intentionally skipped when a residual filter (e.g. `LIKE`) sits between the
limit and the scan, or when the limit is paired with `ORDER BY` (a `TOP_N` that
needs the full input). Implemented as an optimizer extension, since DuckDB has no
table-function flag for limit pushdown.

> Joins/aggregations are **not** pushed to Firebird — like every DuckDB scanner
> extension (`postgres`/`mysql`/`sqlite`), DuckDB scans the base tables and runs
> joins/aggregations in its own engine. For a heavy join over a high-latency
> link, prefer pre-filtering (so projection + filter pushdown shrink the rows) or
> run the join as one native Firebird query on the `main` branch's
> `firebird_query(...)` function.

> Status: **read-only**. `SELECT` and joins work; `INSERT`/`UPDATE`/`DELETE`/DDL
> are intentionally rejected.

## How it works

| Layer | Implementation |
| ----- | -------------- |
| Firebird wire protocol | `rust/` — a pure-Rust client (`rsfbclient`, no native `fbclient` needed) compiled to a C-ABI static library (`libfirebird_client.a`). |
| DuckDB catalog + scan | `src/` — a C++ storage extension (catalog / schema / table entries / transaction manager / scan table function) built against the DuckDB C++ API, modelled on `sqlite_scanner`. |

### Network performance

Upstream `rsfbclient` fetches **one row per network round-trip**, which is
catastrophic over real (high-latency) links — a metadata scan that takes
milliseconds on localhost could take minutes over a VPN/relay. We ship a small
[vendored patch](rust/vendor/rsfbclient-rust/PATCHES.md) that:

- **batches rows** (≈200 per `op_fetch`), and
- the catalog loads **all column metadata in a single query** instead of one
  query per table.

Combined, attaching + `SHOW ALL TABLES` over a ~30 ms relay dropped from
"appears to hang" to ~3 s, and `SELECT`s stream at thousands of rows/second.

Socket timeouts make genuine connectivity problems fail fast instead of hanging:

| Knob | Env var | Default |
| ---- | ------- | ------- |
| connect timeout | `FIREBIRD_CONNECT_TIMEOUT_SECS` | `15` |
| read/write timeout | `FIREBIRD_SOCKET_TIMEOUT_SECS` (`0` disables) | `60` |

Because it links DuckDB's internal C++ API, the binary is **version-locked**:
this build targets **DuckDB v1.5.3**.

## Connection parameters

The ATTACH path is a DSN; individual fields can be overridden with ATTACH
options, and anything left unset falls back to environment variables.

Precedence: **ATTACH option → DSN component → environment variable → default**.

DSN: `firebird://[user[:password]@]host[:port]/database[?charset=UTF8]`

| Field | ATTACH option | Env var | Default |
| ----- | ------------- | ------- | ------- |
| host | `HOST` | `FIREBIRD_HOST` | `localhost` |
| port | `PORT` | `FIREBIRD_PORT` | `3050` |
| user | `USER` / `USERNAME` | `FIREBIRD_USER` | `SYSDBA` |
| password | `PASSWORD` / `PASS` | `FIREBIRD_PASSWORD` | (empty) |
| database | `DATABASE` / `DB` | `FIREBIRD_DATABASE` | (required) |
| charset | `CHARSET` | `FIREBIRD_CHARSET` | `UTF8` |

Examples:

```sql
-- everything in the DSN
ATTACH 'firebird://archmax_readonly:secret@10.0.0.5:3050//var/lib/firebird/data/test.fdb' AS fb (TYPE firebird);

-- DSN + option overrides
ATTACH 'firebird://10.0.0.5/test.fdb' AS fb (TYPE firebird, USER 'sysdba', PASSWORD 'masterkey');

-- rely entirely on FIREBIRD_* environment variables
ATTACH '' AS fb (TYPE firebird);
```

### Notes

- **Usernames** are upper-cased (Firebird stores SRP verifiers under the
  upper-cased login name). Wrap a name in double quotes to keep it verbatim.
- **Windows database paths** (`C:\...\db.fdb`) contain backslashes; prefer
  passing them via the `DATABASE` option or `FIREBIRD_DATABASE` to avoid shell
  mangling.
- **Absolute Unix paths**: use a double slash after the host, e.g.
  `firebird://host:3050//var/lib/firebird/data/test.fdb`.

## Type mapping

| Firebird | DuckDB |
| -------- | ------ |
| SMALLINT / INTEGER / BIGINT | SMALLINT / INTEGER / BIGINT |
| NUMERIC / DECIMAL | DOUBLE (scaled value; precision not preserved) |
| FLOAT | FLOAT |
| DOUBLE PRECISION | DOUBLE |
| DATE | DATE |
| TIME | TIME |
| TIMESTAMP | TIMESTAMP |
| CHAR / VARCHAR | VARCHAR |
| BLOB SUB_TYPE TEXT | VARCHAR |
| BLOB (binary) | BLOB |
| BOOLEAN | BOOLEAN |

## Building from source

Requirements: a C++ toolchain, `cmake`, `ninja` (optional), and a Rust
toolchain (`cargo`).

```bash
cd native
./setup.sh                 # clones DuckDB v1.5.3 + extension-ci-tools
GEN=ninja make release     # builds DuckDB + the extension (slow first time)
```

Artifacts:
- `build/release/extension/firebird/firebird.duckdb_extension` — the loadable extension
- `build/release/duckdb` — a DuckDB CLI with the extension statically linked

> CMake 4.x users: export `CMAKE_POLICY_VERSION_MINIMUM=3.5` before building.

### Run the local tests

```bash
docker compose up -d        # from the repo root, starts a seeded Firebird

native/test_local.sh        # quick smoke test (catalog + a few scans)

# Full self-checking read-feature suite: joins (inner/left/right/full/cross/
# self/3-table), aggregates, GROUP BY/HAVING, window functions, CTEs, subqueries
# (IN/EXISTS/correlated/scalar), set ops (UNION/INTERSECT/EXCEPT), filter and
# limit pushdown edge cases, cross-database joins, type fidelity, and read-only
# enforcement.
FRESH=1 native/test_queries.sh   # FRESH=1 recreates the container to re-seed
```

## Limitations / roadmap

- Read-only (no writes / DDL).
- Single-threaded streaming scan per table (one dedicated connection per scan).
- Projection, simple-`WHERE` filter, and `LIMIT` pushdown are implemented; joins
  and aggregations are not pushed down (DuckDB evaluates them after the scan).
- `NUMERIC`/`DECIMAL` surface as `DOUBLE`.
