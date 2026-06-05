//===----------------------------------------------------------------------===//
//                         DuckDB - Firebird
//
// storage/firebird_catalog.hpp
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb/catalog/catalog.hpp"
#include "firebird_connection.hpp"

namespace duckdb {
class FirebirdSchemaEntry;

class FirebirdCatalog : public Catalog {
public:
	FirebirdCatalog(AttachedDatabase &db_p, FirebirdConnectionParameters params, AccessMode access_mode);
	~FirebirdCatalog() override;

	FirebirdConnectionParameters params;
	AccessMode access_mode;

public:
	void Initialize(bool load_builtin) override;
	string GetCatalogType() override {
		return "firebird";
	}

	optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) override;
	void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) override;
	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction, const EntryLookupInfo &schema_lookup,
	                                              OnEntryNotFound if_not_found) override;

	FirebirdSchemaEntry &GetMainSchema() {
		return *main_schema;
	}

	PhysicalOperator &PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner, LogicalCreateTable &op,
	                                    PhysicalOperator &plan) override;
	PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
	                             optional_ptr<PhysicalOperator> plan) override;
	PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
	                             PhysicalOperator &plan) override;
	PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
	                             PhysicalOperator &plan) override;
	unique_ptr<LogicalOperator> BindCreateIndex(Binder &binder, CreateStatement &stmt, TableCatalogEntry &table,
	                                            unique_ptr<LogicalOperator> plan) override;

	DatabaseSize GetDatabaseSize(ClientContext &context) override;

	bool InMemory() override {
		return false;
	}
	string GetDBPath() override {
		return params.database;
	}

private:
	void DropSchema(ClientContext &context, DropInfo &info) override;

private:
	unique_ptr<FirebirdSchemaEntry> main_schema;
};

} // namespace duckdb
