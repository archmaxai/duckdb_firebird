#include "storage/firebird_transaction.hpp"

#include "storage/firebird_catalog.hpp"
#include "storage/firebird_schema_entry.hpp"
#include "storage/firebird_table_entry.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"

namespace duckdb {

FirebirdTransaction::FirebirdTransaction(FirebirdCatalog &firebird_catalog, TransactionManager &manager,
                                         ClientContext &context)
    : Transaction(manager, context), firebird_catalog(firebird_catalog) {
}

FirebirdTransaction::~FirebirdTransaction() = default;

FirebirdTransaction &FirebirdTransaction::Get(ClientContext &context, Catalog &catalog) {
	return Transaction::Get(context, catalog).Cast<FirebirdTransaction>();
}

FirebirdConnection &FirebirdTransaction::GetConnection() {
	if (!connection.IsOpen()) {
		connection = FirebirdConnection(firebird_catalog.params);
	}
	return connection;
}

CatalogEntry &FirebirdTransaction::MakeTableEntry(const string &table_name, const vector<string> &names,
                                                 const vector<LogicalType> &types) {
	CreateTableInfo info(firebird_catalog.GetMainSchema(), table_name);
	for (idx_t i = 0; i < names.size(); i++) {
		info.columns.AddColumn(ColumnDefinition(names[i], types[i]));
	}

	auto table_entry = make_uniq<FirebirdTableEntry>(firebird_catalog, firebird_catalog.GetMainSchema(), info);
	auto &result = *table_entry;
	catalog_entries[table_name] = std::move(table_entry);
	return result;
}

optional_ptr<CatalogEntry> FirebirdTransaction::GetCatalogEntry(const string &table_name) {
	lock_guard<mutex> guard(entry_lock);
	auto entry = catalog_entries.find(table_name);
	if (entry != catalog_entries.end()) {
		return entry->second.get();
	}

	vector<string> names;
	vector<LogicalType> types;
	GetConnection().GetTableColumns(table_name, names, types);
	if (names.empty()) {
		// Not a known table.
		return nullptr;
	}

	return &MakeTableEntry(table_name, names, types);
}

void FirebirdTransaction::LoadAllEntries(vector<string> &table_names) {
	lock_guard<mutex> guard(entry_lock);
	auto &conn = GetConnection();
	table_names = conn.GetTables();
	auto all_columns = conn.GetAllTableColumns();
	for (auto &table_name : table_names) {
		if (catalog_entries.find(table_name) != catalog_entries.end()) {
			continue;
		}
		auto cols = all_columns.find(table_name);
		if (cols == all_columns.end() || cols->second.names.empty()) {
			continue;
		}
		MakeTableEntry(table_name, cols->second.names, cols->second.types);
	}
}

void FirebirdTransaction::ClearTableEntry(const string &table_name) {
	lock_guard<mutex> guard(entry_lock);
	catalog_entries.erase(table_name);
}

} // namespace duckdb
