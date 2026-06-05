#!/usr/bin/env bash
# End-to-end smoke test of the Firebird catalog extension against the local
# docker test server (see ../docker-compose.yml).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

DUCKDB_BIN="${DUCKDB_BIN:-$SCRIPT_DIR/build/release/duckdb}"
DSN="firebird://testuser:testpass@localhost:3050//var/lib/firebird/data/test.fdb"

if [ ! -x "$DUCKDB_BIN" ]; then
	echo "!! $DUCKDB_BIN not found - run 'make release' in $SCRIPT_DIR first" >&2
	exit 1
fi

echo ">> Starting local Firebird test server ..."
(cd "$REPO_DIR" && docker compose up -d)

echo ">> Waiting for Firebird to accept connections ..."
for _ in $(seq 1 30); do
	if nc -z localhost 3050 2>/dev/null; then
		break
	fi
	sleep 1
done

echo ">> Running catalog + scan tests ..."
"$DUCKDB_BIN" -box <<SQL
ATTACH '$DSN' AS fb (TYPE firebird);
.print '--- catalog ---'
SHOW ALL TABLES;
.print '--- native select ---'
SELECT * FROM fb.EMPLOYEES ORDER BY ID;
.print '--- aggregate + filter (pushed projection) ---'
SELECT DEPARTMENT, count(*) AS n, round(avg(SALARY), 2) AS avg_sal
  FROM fb.EMPLOYEES WHERE ACTIVE GROUP BY DEPARTMENT ORDER BY n DESC;
.print '--- type gallery ---'
SELECT C_SMALLINT, C_BIGINT, C_FLOAT, C_NUMERIC, C_DATE, C_TIME, C_TIMESTAMP, C_BOOLEAN, C_BLOB_TEXT
  FROM fb.TYPE_GALLERY;
SQL

echo ">> OK"
