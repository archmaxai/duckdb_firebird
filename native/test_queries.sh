#!/usr/bin/env bash
# Comprehensive read-feature test suite for the Firebird catalog extension,
# run against the local dockerized Firebird test server (../docker-compose.yml).
#
# It verifies that a wide variety of read query types work through a native
# ATTACH (joins, aggregates, subqueries, CTEs, window functions, set operations,
# cross-database joins, type fidelity) and that all write/DDL operations are
# correctly rejected (the extension is read-only).
#
# Usage:
#   native/test_queries.sh            # uses an already-running container
#   FRESH=1 native/test_queries.sh    # recreate the container (re-seed schema)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

DUCKDB_BIN="${DUCKDB_BIN:-$SCRIPT_DIR/build/release/duckdb}"
DSN="${FIREBIRD_DSN:-firebird://testuser:testpass@localhost:3050//var/lib/firebird/data/test.fdb}"
QUERIES="$SCRIPT_DIR/test/queries.sql"

if [ ! -x "$DUCKDB_BIN" ]; then
	echo "!! $DUCKDB_BIN not found - run 'make release' in $SCRIPT_DIR first" >&2
	exit 1
fi

if [ "${FRESH:-0}" = "1" ]; then
	echo ">> Recreating local Firebird test server (fresh seed) ..."
	(cd "$REPO_DIR" && docker compose down -v >/dev/null 2>&1 || true)
fi
echo ">> Starting local Firebird test server ..."
(cd "$REPO_DIR" && docker compose up -d >/dev/null)

echo ">> Waiting for Firebird to accept connections ..."
for _ in $(seq 1 40); do
	if docker exec duckdb_firebird_test sh -lc \
		"printf 'SELECT 1 FROM RDB\$DATABASE;\n' | /opt/firebird/bin/isql -user SYSDBA -password masterkey /var/lib/firebird/data/test.fdb" \
		>/dev/null 2>&1; then
		break
	fi
	sleep 1
done

pass=0
fail=0
failed_names=()

echo ">> Running positive read-feature checks ..."
output="$({ echo "ATTACH '$DSN' AS fb (TYPE firebird);"; cat "$QUERIES"; } | "$DUCKDB_BIN" 2>&1)"

# Each check prints a `name|PASS` or `name|FAIL` line.
while IFS='|' read -r name result; do
	case "$result" in
	PASS) pass=$((pass + 1)); printf '   \033[32mPASS\033[0m %s\n' "$name" ;;
	FAIL) fail=$((fail + 1)); failed_names+=("$name"); printf '   \033[31mFAIL\033[0m %s\n' "$name" ;;
	esac
done < <(printf '%s\n' "$output" | grep -E '\|(PASS|FAIL)$')

# If the SQL aborted (e.g. ATTACH failed) we may have produced no checks.
if [ $((pass + fail)) -eq 0 ]; then
	echo "!! No checks ran. Raw output:" >&2
	printf '%s\n' "$output" >&2
	exit 1
fi

echo ">> Running read-only enforcement checks (writes must be rejected) ..."
ro_checks=(
	"INSERT INTO fb.EMPLOYEES (ID, FIRST_NAME) VALUES (99, 'Nope')"
	"UPDATE fb.EMPLOYEES SET FIRST_NAME='X' WHERE ID=1"
	"DELETE FROM fb.EMPLOYEES WHERE ID=1"
	"CREATE TABLE fb.NEW_TABLE (X INTEGER)"
	"DROP TABLE fb.EMPLOYEES"
	"CREATE SCHEMA fb.new_schema"
)
for stmt in "${ro_checks[@]}"; do
	label="reject: ${stmt:0:24}..."
	out="$("$DUCKDB_BIN" 2>&1 <<SQL
ATTACH '$DSN' AS fb (TYPE firebird);
$stmt;
SQL
)"
	# A correctly rejected write surfaces an error mentioning read-only / not
	# implemented / a binder error.
	if printf '%s' "$out" | grep -qiE 'read-only|not implemented|cannot modify|Error'; then
		pass=$((pass + 1)); printf '   \033[32mPASS\033[0m %s\n' "$label"
	else
		fail=$((fail + 1)); failed_names+=("$label"); printf '   \033[31mFAIL\033[0m %s (write was NOT rejected)\n' "$label"
	fi
done

echo
echo ">> Results: $pass passed, $fail failed"
if [ "$fail" -ne 0 ]; then
	echo ">> Failed: ${failed_names[*]}"
	exit 1
fi
echo ">> All read-feature tests passed."
