//===----------------------------------------------------------------------===//
//                         DuckDB - Firebird
//
// firebird_scanner.hpp
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb.hpp"
#include "firebird_connection.hpp"

namespace duckdb {

struct FirebirdBindData : public TableFunctionData {
	FirebirdConnectionParameters params;
	string table_name;
	vector<string> names;
	vector<LogicalType> types;
	//! Row cap pushed down from a LIMIT above the scan (count + offset). Only set
	//! by the limit-pushdown optimizer extension; the LIMIT operator is retained
	//! so DuckDB still enforces exact LIMIT/OFFSET semantics.
	bool has_limit = false;
	idx_t limit_rows = 0;
};

//! Builds the (internal) table function used to scan a Firebird table. The bind
//! data is supplied by FirebirdTableEntry::GetScanFunction.
TableFunction FirebirdScanFunction();

//! Registers an optimizer extension that pushes a LIMIT (and OFFSET) above a
//! Firebird scan down into the generated SQL as a Firebird `ROWS` clause.
void RegisterFirebirdLimitPushdown(DBConfig &config);

} // namespace duckdb
