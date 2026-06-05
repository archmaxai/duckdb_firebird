#include "storage/firebird_table_entry.hpp"

#include "storage/firebird_catalog.hpp"
#include "firebird_scanner.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"

namespace duckdb {

FirebirdTableEntry::FirebirdTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info)
    : TableCatalogEntry(catalog, schema, info) {
}

unique_ptr<BaseStatistics> FirebirdTableEntry::GetStatistics(ClientContext &context, column_t column_id) {
	return nullptr;
}

TableFunction FirebirdTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	auto result = make_uniq<FirebirdBindData>();
	auto &fb_catalog = catalog.Cast<FirebirdCatalog>();
	result->params = fb_catalog.params;
	result->table_name = name;
	for (auto &col : columns.Logical()) {
		result->names.push_back(col.GetName());
		result->types.push_back(col.GetType());
	}
	bind_data = std::move(result);
	return FirebirdScanFunction();
}

TableStorageInfo FirebirdTableEntry::GetStorageInfo(ClientContext &context) {
	TableStorageInfo result;
	return result;
}

void FirebirdTableEntry::BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj,
                                               LogicalUpdate &update, ClientContext &context) {
}

} // namespace duckdb
