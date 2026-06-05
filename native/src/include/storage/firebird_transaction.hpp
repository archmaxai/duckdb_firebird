//===----------------------------------------------------------------------===//
//                         DuckDB - Firebird
//
// storage/firebird_transaction.hpp
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb/transaction/transaction.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "firebird_connection.hpp"

namespace duckdb {
class FirebirdCatalog;

class FirebirdTransaction : public Transaction {
public:
	FirebirdTransaction(FirebirdCatalog &firebird_catalog, TransactionManager &manager, ClientContext &context);
	~FirebirdTransaction() override;

	static FirebirdTransaction &Get(ClientContext &context, Catalog &catalog);

	//! Introspection connection, opened lazily on first use.
	FirebirdConnection &GetConnection();

	//! Look up (and cache) a table entry by name. Returns nullptr if not found.
	optional_ptr<CatalogEntry> GetCatalogEntry(const string &table_name);
	void ClearTableEntry(const string &table_name);

	//! Eagerly load every table's columns in one query and cache the entries.
	//! Used by catalog scans (e.g. SHOW ALL TABLES) to avoid an N+1 round-trip
	//! storm. `table_names` receives the table list in catalog order.
	void LoadAllEntries(vector<string> &table_names);

private:
	//! Build and cache a table entry from already-known column metadata.
	//! Caller must hold `entry_lock`.
	CatalogEntry &MakeTableEntry(const string &table_name, const vector<string> &names,
	                             const vector<LogicalType> &types);

	FirebirdCatalog &firebird_catalog;
	FirebirdConnection connection;
	mutex entry_lock;
	case_insensitive_map_t<unique_ptr<CatalogEntry>> catalog_entries;
};

} // namespace duckdb
