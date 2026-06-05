#include "storage/firebird_schema_entry.hpp"

#include "storage/firebird_transaction.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"

namespace duckdb {

FirebirdSchemaEntry::FirebirdSchemaEntry(Catalog &catalog, CreateSchemaInfo &info)
    : SchemaCatalogEntry(catalog, info) {
}

static void ThrowReadOnly() {
	throw NotImplementedException(
	    "The Firebird catalog extension is currently read-only and cannot modify the Firebird database");
}

optional_ptr<CatalogEntry> FirebirdSchemaEntry::CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) {
	ThrowReadOnly();
}
optional_ptr<CatalogEntry> FirebirdSchemaEntry::CreateFunction(CatalogTransaction transaction,
                                                               CreateFunctionInfo &info) {
	ThrowReadOnly();
}
optional_ptr<CatalogEntry> FirebirdSchemaEntry::CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
                                                            TableCatalogEntry &table) {
	ThrowReadOnly();
}
optional_ptr<CatalogEntry> FirebirdSchemaEntry::CreateView(CatalogTransaction transaction, CreateViewInfo &info) {
	ThrowReadOnly();
}
optional_ptr<CatalogEntry> FirebirdSchemaEntry::CreateSequence(CatalogTransaction transaction,
                                                               CreateSequenceInfo &info) {
	ThrowReadOnly();
}
optional_ptr<CatalogEntry> FirebirdSchemaEntry::CreateTableFunction(CatalogTransaction transaction,
                                                                    CreateTableFunctionInfo &info) {
	ThrowReadOnly();
}
optional_ptr<CatalogEntry> FirebirdSchemaEntry::CreateCopyFunction(CatalogTransaction transaction,
                                                                   CreateCopyFunctionInfo &info) {
	ThrowReadOnly();
}
optional_ptr<CatalogEntry> FirebirdSchemaEntry::CreatePragmaFunction(CatalogTransaction transaction,
                                                                     CreatePragmaFunctionInfo &info) {
	ThrowReadOnly();
}
optional_ptr<CatalogEntry> FirebirdSchemaEntry::CreateCollation(CatalogTransaction transaction,
                                                                CreateCollationInfo &info) {
	ThrowReadOnly();
}
optional_ptr<CatalogEntry> FirebirdSchemaEntry::CreateType(CatalogTransaction transaction, CreateTypeInfo &info) {
	ThrowReadOnly();
}
void FirebirdSchemaEntry::Alter(CatalogTransaction transaction, AlterInfo &info) {
	ThrowReadOnly();
}
void FirebirdSchemaEntry::DropEntry(ClientContext &context, DropInfo &info) {
	ThrowReadOnly();
}

void FirebirdSchemaEntry::Scan(ClientContext &context, CatalogType type,
                               const std::function<void(CatalogEntry &)> &callback) {
	if (type != CatalogType::TABLE_ENTRY) {
		return;
	}
	auto &transaction = FirebirdTransaction::Get(context, catalog);
	// Bulk-load every table's columns in a single round-trip rather than issuing
	// one query per table (catastrophic over high-latency links).
	vector<string> tables;
	transaction.LoadAllEntries(tables);
	for (auto &table_name : tables) {
		auto entry = transaction.GetCatalogEntry(table_name);
		if (entry) {
			callback(*entry);
		}
	}
}

void FirebirdSchemaEntry::Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
	throw InternalException("Scan without context not supported for Firebird catalog");
}

optional_ptr<CatalogEntry> FirebirdSchemaEntry::LookupEntry(CatalogTransaction transaction,
                                                            const EntryLookupInfo &lookup_info) {
	if (lookup_info.GetCatalogType() != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}
	auto &fb_transaction = transaction.transaction->Cast<FirebirdTransaction>();
	return fb_transaction.GetCatalogEntry(lookup_info.GetEntryName());
}

} // namespace duckdb
