#include "storage/firebird_catalog.hpp"

#include "storage/firebird_schema_entry.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/storage/database_size.hpp"

namespace duckdb {

FirebirdCatalog::FirebirdCatalog(AttachedDatabase &db_p, FirebirdConnectionParameters params_p, AccessMode access_mode_p)
    : Catalog(db_p), params(std::move(params_p)), access_mode(access_mode_p) {
}

FirebirdCatalog::~FirebirdCatalog() = default;

void FirebirdCatalog::Initialize(bool load_builtin) {
	CreateSchemaInfo info;
	main_schema = make_uniq<FirebirdSchemaEntry>(*this, info);
}

optional_ptr<CatalogEntry> FirebirdCatalog::CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) {
	throw BinderException("Firebird databases do not support creating new schemas");
}

void FirebirdCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	callback(*main_schema);
}

optional_ptr<SchemaCatalogEntry> FirebirdCatalog::LookupSchema(CatalogTransaction transaction,
                                                               const EntryLookupInfo &schema_lookup,
                                                               OnEntryNotFound if_not_found) {
	auto &schema_name = schema_lookup.GetEntryName();
	if (schema_name == DEFAULT_SCHEMA || schema_name == INVALID_SCHEMA) {
		return main_schema.get();
	}
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		return nullptr;
	}
	throw BinderException("Firebird databases only have a single schema - \"%s\"", string(DEFAULT_SCHEMA));
}

void FirebirdCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	throw BinderException("Firebird databases do not support dropping schemas");
}

DatabaseSize FirebirdCatalog::GetDatabaseSize(ClientContext &context) {
	DatabaseSize result;
	result.total_blocks = 0;
	result.block_size = 0;
	result.free_blocks = 0;
	result.used_blocks = 0;
	result.bytes = 0;
	result.wal_size = idx_t(-1);
	return result;
}

static void ThrowReadOnly() {
	throw NotImplementedException(
	    "The Firebird catalog extension is currently read-only and cannot modify the Firebird database");
}

PhysicalOperator &FirebirdCatalog::PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner,
                                                     LogicalCreateTable &op, PhysicalOperator &plan) {
	ThrowReadOnly();
}

PhysicalOperator &FirebirdCatalog::PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
                                              optional_ptr<PhysicalOperator> plan) {
	ThrowReadOnly();
}

PhysicalOperator &FirebirdCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
                                              PhysicalOperator &plan) {
	ThrowReadOnly();
}

PhysicalOperator &FirebirdCatalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
                                              PhysicalOperator &plan) {
	ThrowReadOnly();
}

unique_ptr<LogicalOperator> FirebirdCatalog::BindCreateIndex(Binder &binder, CreateStatement &stmt,
                                                             TableCatalogEntry &table,
                                                             unique_ptr<LogicalOperator> plan) {
	ThrowReadOnly();
}

} // namespace duckdb
