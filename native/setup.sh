#!/usr/bin/env bash
# Fetches the DuckDB source and extension build tooling required to build the
# Firebird catalog extension. These are large checkouts and are intentionally
# kept out of git (see ../.gitignore); run this once before `make release`.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

DUCKDB_TAG="${DUCKDB_TAG:-v1.5.3}"
CI_TOOLS_BRANCH="${CI_TOOLS_BRANCH:-v1.5-variegata}"

if [ ! -d duckdb/.git ]; then
	echo ">> Cloning DuckDB ${DUCKDB_TAG} ..."
	rm -rf duckdb
	git clone --depth 1 --branch "${DUCKDB_TAG}" https://github.com/duckdb/duckdb duckdb
else
	echo ">> DuckDB checkout already present (skipping)"
fi

if [ ! -d extension-ci-tools/.git ]; then
	echo ">> Cloning extension-ci-tools ${CI_TOOLS_BRANCH} ..."
	rm -rf extension-ci-tools
	git clone --depth 1 --branch "${CI_TOOLS_BRANCH}" \
		https://github.com/duckdb/extension-ci-tools extension-ci-tools
else
	echo ">> extension-ci-tools checkout already present (skipping)"
fi

echo ">> Done. Build with:  make release  (from $(pwd))"
