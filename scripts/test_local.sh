#!/usr/bin/env bash
# End-to-end smoke test against the local dockerized Firebird server.
# Brings up the container (if needed), builds the extension, and runs queries.
set -euo pipefail

cd "$(dirname "$0")/.."

DB="/var/lib/firebird/data/test.fdb"
HOST="127.0.0.1"

echo "==> Ensuring test Firebird container is running"
docker compose up -d
# Wait for the server to accept connections.
for i in $(seq 1 30); do
  if docker exec duckdb_firebird_test sh -lc \
      "printf 'SELECT 1 FROM RDB\$DATABASE;\n' | /opt/firebird/bin/isql -user SYSDBA -password masterkey $DB" \
      >/dev/null 2>&1; then
    echo "    Firebird is ready."
    break
  fi
  sleep 1
done

echo "==> Building extension"
./scripts/build.sh >/dev/null

echo "==> firebird_tables()"
duckdb -unsigned -box -c "
LOAD 'build/firebird.duckdb_extension';
SELECT * FROM firebird_tables(host => '$HOST', user => 'SYSDBA', password => 'masterkey', database => '$DB');
"

echo "==> firebird_query() EMPLOYEES"
duckdb -unsigned -box -c "
LOAD 'build/firebird.duckdb_extension';
SELECT * FROM firebird_query(
  'SELECT * FROM EMPLOYEES ORDER BY ID',
  host => '$HOST', user => 'SYSDBA', password => 'masterkey', database => '$DB');
"

echo "==> Column type inference over TYPE_GALLERY"
duckdb -unsigned -box -c "
LOAD 'build/firebird.duckdb_extension';
DESCRIBE SELECT * FROM firebird_query(
  'SELECT * FROM TYPE_GALLERY',
  host => '$HOST', user => 'SYSDBA', password => 'masterkey', database => '$DB');
"

echo "==> All local tests passed."
