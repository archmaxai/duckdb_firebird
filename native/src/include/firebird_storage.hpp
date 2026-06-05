//===----------------------------------------------------------------------===//
//                         DuckDB - Firebird
//
// firebird_storage.hpp
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb/storage/storage_extension.hpp"

namespace duckdb {

class FirebirdStorageExtension : public StorageExtension {
public:
	FirebirdStorageExtension();
};

} // namespace duckdb
