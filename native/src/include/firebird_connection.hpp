//===----------------------------------------------------------------------===//
//                         DuckDB - Firebird
//
// firebird_connection.hpp
//
// Thin RAII C++ wrappers over the firebird_client C ABI plus Firebird->DuckDB
// type mapping.
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb.hpp"
#include "firebird_client.h"

namespace duckdb {

//! Everything needed to open a Firebird connection.
struct FirebirdConnectionParameters {
	string host = "localhost";
	uint16_t port = 3050;
	string user;
	string password;
	string database;
	string charset = "UTF8";
};

//! Map a Firebird system-catalog field description to a DuckDB LogicalType.
//! `field_type`/`sub_type`/`scale`/`precision` come straight from RDB$FIELDS.
LogicalType FirebirdFieldToLogicalType(int field_type, int sub_type, int scale, int length, int precision);

//! Ordered columns of a single table.
struct FirebirdTableColumns {
	vector<string> names;
	vector<LogicalType> types;
};

//! RAII connection used for catalog introspection.
class FirebirdConnection {
public:
	FirebirdConnection() = default;
	explicit FirebirdConnection(const FirebirdConnectionParameters &params);
	~FirebirdConnection();

	FirebirdConnection(FirebirdConnection &&other) noexcept;
	FirebirdConnection &operator=(FirebirdConnection &&other) noexcept;
	FirebirdConnection(const FirebirdConnection &) = delete;
	FirebirdConnection &operator=(const FirebirdConnection &) = delete;

	bool IsOpen() const {
		return conn != nullptr;
	}
	void Close();

	//! Names of all user tables (and views) in the database.
	vector<string> GetTables();
	//! Populate `names`/`types` with the columns of `table_name`, in order.
	void GetTableColumns(const string &table_name, vector<string> &names, vector<LogicalType> &types);
	//! Columns of every user table, fetched in a single round-trip. Keyed by
	//! table name. Avoids the per-table N+1 query pattern during catalog scans.
	case_insensitive_map_t<FirebirdTableColumns> GetAllTableColumns();

private:
	FbConn *conn = nullptr;
};

//! RAII streaming cursor. Owns a dedicated Firebird connection for the lifetime
//! of the scan it backs.
class FirebirdCursor {
public:
	FirebirdCursor(const FirebirdConnectionParameters &params, const string &sql);
	~FirebirdCursor();

	FirebirdCursor(const FirebirdCursor &) = delete;
	FirebirdCursor &operator=(const FirebirdCursor &) = delete;

	//! Advance to the next row. Returns false at end of data, throws on error.
	bool Next();

	FbCursor *handle() {
		return cursor;
	}

private:
	FbCursor *cursor = nullptr;
};

} // namespace duckdb
