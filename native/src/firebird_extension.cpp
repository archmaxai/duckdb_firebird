#ifndef DUCKDB_BUILD_LOADABLE_EXTENSION
#define DUCKDB_BUILD_LOADABLE_EXTENSION
#endif
#include "duckdb.hpp"

#include "firebird_extension.hpp"
#include "firebird_storage.hpp"
#include "firebird_scanner.hpp"

#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/storage/storage_extension.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);

	// Register the storage backend so `ATTACH '...' (TYPE firebird)` works.
	StorageExtension::Register(config, "firebird", make_shared_ptr<FirebirdStorageExtension>());

	// Push a LIMIT above a Firebird scan into the generated SQL (ROWS clause).
	RegisterFirebirdLimitPushdown(config);
}

void FirebirdExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(firebird, loader) {
	duckdb::LoadInternal(loader);
}
}
