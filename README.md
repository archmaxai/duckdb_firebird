# duckdb_firebird

A [DuckDB](https://duckdb.org) **storage/catalog extension** that attaches a
**Firebird** database and lets you query its tables with native SQL, with
**no native `fbclient` required**. It speaks the Firebird wire
protocol in pure Rust (via [`rsfbclient`](https://crates.io/crates/rsfbclient)),
compiled into a C++ extension that registers a real DuckDB catalog.

```sql
ATTACH 'firebird://user:password@host:3050//path/to/db.fdb' AS fb (TYPE firebird);

-- Firebird tables behave like native DuckDB tables:
SHOW ALL TABLES;
SELECT * FROM fb.EMPLOYEES WHERE ACTIVE;
SELECT d.NAME, count(*) FROM fb.PROJECTS p JOIN fb.DEPARTMENTS d ON p.DEPT_ID = d.ID GROUP BY d.NAME;
```

Tables and column types are discovered from the Firebird system catalog and show
up in `SHOW ALL TABLES` / `information_schema`. The optimizer pushes **column
projection**, **`WHERE` filters**, and **`LIMIT`** down into the Firebird query.

> Status: **read-only**. `SELECT` and joins work; `INSERT`/`UPDATE`/`DELETE`/DDL
> are intentionally rejected.

Because it links DuckDB's internal C++ API, the binary is **version-locked to
DuckDB v1.5.3** and must be loaded by a matching client.

## Install from GitHub (no build required)

CI builds the extension for `linux_amd64`, `linux_arm64`, and `osx_arm64` and
publishes a DuckDB **custom extension repository** to GitHub Pages (see
[`.github/workflows/catalog-extension.yml`](.github/workflows/catalog-extension.yml)).
Install it straight from DuckDB v1.5.3 — no local build, no clone:

```bash
duckdb -unsigned          # unsigned extensions must be allowed at startup
```

```sql
SET custom_extension_repository = 'https://<owner>.github.io/<repo>';
INSTALL firebird;          -- downloads the right platform binary
LOAD firebird;

ATTACH 'firebird://user:pass@host:3050//path/db.fdb' AS fb (TYPE firebird);
```

From Python:

```python
import duckdb
con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.execute("SET custom_extension_repository = 'https://<owner>.github.io/<repo>'")
con.execute("INSTALL firebird")
con.execute("LOAD firebird")
```

> The published binaries are **unsigned**, so `allow_unsigned_extensions` (or the
> `-unsigned` CLI flag) is required. Tagged releases (`catalog-vX.Y.Z`)
> additionally attach the raw `firebird.<platform>.duckdb_extension` files as
> GitHub Release assets, which you can download and `LOAD '<path>'` directly.
>
> Enable publishing once in the repo: **Settings → Pages → Build and deployment
> → Source: GitHub Actions**. The repository URL to use is printed on the
> published Pages site's landing page.

## Connection parameters

The `ATTACH` target is a DSN, and each connection field is resolved in this
order:

1. the matching component of the DSN, then
2. the corresponding `FIREBIRD_*` environment variable, then
3. a built-in default.

| Field      | Env fallback        | Default     |
|------------|---------------------|-------------|
| host       | `FIREBIRD_HOST`     | `localhost` |
| port       | `FIREBIRD_PORT`     | `3050`      |
| user       | `FIREBIRD_USER`     | `SYSDBA`    |
| password   | `FIREBIRD_PASSWORD` | `masterkey` |
| database   | `FIREBIRD_DATABASE` | *(required)*|
| charset    | `FIREBIRD_CHARSET`  | `UTF-8`     |

### DSN form

```
firebird://user:password@host:port/database?charset=UTF8
```

* **IPv6 hosts** use bracket notation: `firebird://user:password@[2001:db8::1]:3050/db.fdb`
* **Windows paths** are taken verbatim: `.../C:\firebird\data\example.fdb`
* **Absolute Unix paths** need a double slash (the first `/` is the
  authority/path separator, as in JDBC Firebird):
  `firebird://SYSDBA:masterkey@127.0.0.1:3050//var/lib/firebird/data/test.fdb`

### A note on usernames

Firebird treats unquoted login names case-insensitively and stores the SRP
password verifier under the **upper-cased** name. The extension upper-cases the
username for you, so `myuser` and `MYUSER` both work.

## Type mapping

| Firebird                        | DuckDB      |
|---------------------------------|-------------|
| `SMALLINT` / `INTEGER` / `BIGINT` | `SMALLINT` / `INTEGER` / `BIGINT` |
| `FLOAT`                         | `FLOAT`     |
| `DOUBLE PRECISION`              | `DOUBLE`    |
| `NUMERIC` / `DECIMAL`           | `DOUBLE`*   |
| `CHAR` / `VARCHAR`              | `VARCHAR`   |
| `BLOB SUB_TYPE TEXT`            | `VARCHAR`   |
| `BLOB SUB_TYPE BINARY`          | `BLOB`      |
| `BOOLEAN`                       | `BOOLEAN`   |
| `DATE` / `TIME` / `TIMESTAMP`   | `DATE` / `TIME` / `TIMESTAMP` |

\* `NUMERIC`/`DECIMAL` surface as `DOUBLE`, so very large/high-scale values may
lose precision; cast on the Firebird side (e.g. `CAST(col AS VARCHAR)`) if you
need exact decimals.

## Building & internals

The extension lives in [`native/`](native/). See
[`native/README.md`](native/README.md) for build instructions, architecture, the
pushdown details (projection / filter / limit), and the network-performance
notes (batched fetch, bulk metadata, socket timeouts).

```bash
native/setup.sh              # fetch the pinned DuckDB v1.5.3 source + ci-tools
cd native && GEN=ninja make release   # -> build/release/extension/firebird/firebird.duckdb_extension
```

## Development

A throwaway Firebird 5 server with seed data is provided via Docker:

```bash
docker compose up -d     # starts firebird on localhost:3050, seeds test.fdb
```

Seed schema lives in `test/initdb/01_schema.sql` (`EMPLOYEES`, `DEPARTMENTS`,
`PROJECTS` with foreign keys, plus a type-coverage table). The self-checking
read-feature suite runs the full matrix of joins, aggregates, subqueries, window
functions, set ops, filter/limit pushdown, and read-only enforcement:

```bash
native/test_queries.sh   # FRESH=1 recreates the container to re-seed
```

## License

MIT
