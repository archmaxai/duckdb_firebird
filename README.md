# duckdb_firebird

A [DuckDB](https://duckdb.org) extension that reads **Firebird** databases
directly, with no MySQL bridge and **no native `fbclient` required**. It speaks
the Firebird wire protocol in pure Rust (via
[`rsfbclient`](https://crates.io/crates/rsfbclient)) and exposes the results as
DuckDB table functions, so you get correct column types instead of everything
arriving as `VARCHAR`.

This is "Option 2" from the bridge-conversion analysis: a real Rust extension
built on the DuckDB C Extension API.

## What you get

Two table functions:

```sql
-- Run arbitrary Firebird SQL and stream the result into DuckDB.
SELECT * FROM firebird_query('SELECT FIRST 10 * FROM ADDRESS', host => '...', ...);

-- List the user tables in the database.
SELECT * FROM firebird_tables(host => '...', ...);
```

DuckDB does the rest — join, aggregate, export to Parquet/CSV, etc., over the
rows pulled from Firebird.

## Install from GitHub (no build required)

CI builds the extension for `linux_amd64`, `linux_arm64`, `osx_amd64`,
`osx_arm64` and `windows_amd64` and publishes a DuckDB **custom extension
repository** to GitHub Pages (see [`.github/workflows/extension.yml`](.github/workflows/extension.yml)).
Once published, install it straight from DuckDB — no local build, no clone:

```bash
duckdb -unsigned          # unsigned extensions must be allowed at startup
```

```sql
SET custom_extension_repository = 'https://<owner>.github.io/<repo>';
INSTALL firebird;          -- downloads the right platform binary
LOAD firebird;

SELECT * FROM firebird_query('SELECT FIRST 5 * FROM ADDRESS', host => '...', ...);
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
> `-unsigned` CLI flag) is required. Tagged releases (`vX.Y.Z`) additionally
> attach the raw `firebird.<platform>.duckdb_extension` files as GitHub Release
> assets, which you can download and `LOAD '<path>'` directly.
>
> Enable publishing once in the repo: **Settings → Pages → Build and deployment
> → Source: GitHub Actions**. The repository URL to use is printed on the
> published Pages site's landing page.

## Quick start (build locally)

```bash
# 1. Build the loadable extension -> build/firebird.duckdb_extension
./scripts/build.sh

# 2. Use it (unsigned extensions must be enabled at startup)
duckdb -unsigned
```

```sql
LOAD 'build/firebird.duckdb_extension';

SELECT *
FROM firebird_query(
  'SELECT FIRST 5 NAME, STANDARDCITY FROM ADDRESS',
  host     => 'fd7a:115c:a1e0::7f38:6a39',   -- IPv6 is fine, no brackets needed here
  user     => 'archmax_readonly',
  password => 't34mw37k_1!',
  database => 'C:\Teamwerk\Vertec\DB\VERTEC.fdb'  -- path as seen on the server
);
```

From Python:

```python
import duckdb
con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.execute("LOAD 'build/firebird.duckdb_extension'")
con.sql("""
  SELECT * FROM firebird_query('SELECT FIRST 5 * FROM ADDRESS',
    host => '...', user => '...', password => '...', database => '...')
""").show()
```

## Connection parameters

Both functions accept the same optional named parameters. Each connection field
is resolved in this order:

1. an explicit named parameter,
2. the matching component of the `dsn` named parameter,
3. the corresponding `FIREBIRD_*` environment variable,
4. a built-in default.

| Named param | Env fallback        | Default     |
|-------------|---------------------|-------------|
| `host`      | `FIREBIRD_HOST`     | `localhost` |
| `port`      | `FIREBIRD_PORT`     | `3050`      |
| `user`      | `FIREBIRD_USER`     | `SYSDBA`    |
| `password`  | `FIREBIRD_PASSWORD` | `masterkey` |
| `database`  | `FIREBIRD_DATABASE` | *(required)*|
| `dsn`       | —                   | —           |
| `charset`   | `FIREBIRD_CHARSET`  | `UTF-8`     |

If every connection field is provided via the environment, calls collapse to
just the SQL:

```sql
SELECT * FROM firebird_query('SELECT FIRST 10 * FROM ADDRESS');
```

### DSN form

```
firebird://user:password@host:port/database?charset=UTF8
```

* **IPv6 hosts** use bracket notation: `firebird://u:p@[fd7a:115c::1]:3050/db.fdb`
* **Windows paths** are taken verbatim: `.../C:\Teamwerk\Vertec\DB\VERTEC.fdb`
* **Absolute Unix paths** need a double slash (the first `/` is the
  authority/path separator, as in JDBC Firebird):
  `firebird://SYSDBA:masterkey@127.0.0.1:3050//var/lib/firebird/data/test.fdb`

Individual named parameters override the corresponding `dsn` component, so you
can keep a base DSN and override just the SQL target database, etc.

> **Tip:** for absolute paths the named `database` parameter is less fiddly than
> the DSN form.

### A note on usernames

Firebird treats unquoted login names case-insensitively and stores the SRP
password verifier under the **upper-cased** name. The extension upper-cases the
username for you, so `archmax_readonly` and `ARCHMAX_READONLY` both work. To use
a genuinely case-sensitive login, wrap it in double quotes: `user => '"MixedCase"'`.

## Type mapping

| Firebird                              | DuckDB      |
|---------------------------------------|-------------|
| `SMALLINT`, `INTEGER`, `BIGINT`       | `BIGINT`    |
| `FLOAT`, `DOUBLE PRECISION`           | `DOUBLE`    |
| `NUMERIC`, `DECIMAL`                  | `DOUBLE`*   |
| `CHAR`, `VARCHAR`                     | `VARCHAR`   |
| `BLOB SUB_TYPE TEXT`                  | `VARCHAR`   |
| `BLOB SUB_TYPE BINARY`               | `BLOB`      |
| `BOOLEAN`                             | `BOOLEAN`   |
| `DATE`, `TIME`, `TIMESTAMP`           | `TIMESTAMP`*|

\* Caveats:
- `NUMERIC`/`DECIMAL` come through as `DOUBLE`, so very large/high-scale values
  may lose precision. Cast on the Firebird side (e.g. `CAST(col AS VARCHAR)`) if
  you need exact decimals.
- `DATE` and `TIME` are projected to `TIMESTAMP`. A bare `TIME` value gets the
  current date attached; select `CAST(col AS VARCHAR)` from Firebird if you need
  the raw time.

Column types are determined from the data; for an all-`NULL` column the Firebird
declared type code is used as a fallback.

## Limitations (v1)

- **Full materialization, no pushdown.** Each call runs your Firebird SQL and
  loads the entire result set into memory before DuckDB sees it. There is no
  projection or predicate pushdown, so `count(*)` over a wide table still pulls
  every column. **Push selectivity into the Firebird SQL** (`FIRST n`, `WHERE`,
  column lists, `COUNT(*)`) — that's where it belongs.
- No `ATTACH ... (TYPE firebird)` catalog integration yet (the C Extension API
  surface used here is table-function based).
- Empty result sets with no derivable column metadata expose a single
  placeholder `column0`.

## Development

### Test Firebird server

A throwaway Firebird 5 server with seed data is provided via Docker:

```bash
docker compose up -d        # starts firebird on localhost:3050, seeds test.fdb
```

Seed schema lives in `test/initdb/01_schema.sql` (an `EMPLOYEES` table and a
`TYPE_GALLERY` table exercising every supported type). Connect as
`SYSDBA` / `masterkey`, database `/var/lib/firebird/data/test.fdb`.

### Build & test

```bash
./scripts/build.sh       # cargo build --release + metadata footer
./scripts/test_local.sh  # bring up docker, build, run end-to-end queries
cargo test               # unit tests (DSN parser)
```

### How the build works

`cargo build --release` produces a raw `target/release/libfirebird.dylib`
(`.so` on Linux). DuckDB will not load a bare shared library — it needs a
metadata footer identifying the platform, C API version and ABI type.
`scripts/append_extension_metadata.py` appends that footer to produce
`build/firebird.duckdb_extension`.

The extension targets **C API `v1.2.0`**, so a single binary works across all
DuckDB releases that share it (1.4.x, 1.5.x, …) — but it is **platform
specific** (build once per `osx_arm64`, `linux_amd64`, …).

## Distribution

* **Just ship the file.** Hand someone `firebird.duckdb_extension`; they
  `LOAD '<path>'` after starting `duckdb -unsigned`.
* **Self-hosted repo.** Lay binaries out at
  `https://host/${duckdb_version}/${platform}/firebird.duckdb_extension.gz` and
  use `SET custom_extension_repository=...; INSTALL firebird;` (still requires
  `SET allow_unsigned_extensions=true`).
* **Community Extensions.** Submit to `duckdb/community-extensions` for signed,
  no-flag `INSTALL firebird FROM community;`.

## License

MIT
