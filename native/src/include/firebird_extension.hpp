#ifndef DUCKDB_BUILD_LOADABLE_EXTENSION
#define DUCKDB_BUILD_LOADABLE_EXTENSION
#endif
#include "duckdb.hpp"

namespace duckdb {

class FirebirdExtension : public Extension {
public:
	std::string Name() override {
		return "firebird";
	}
	void Load(ExtensionLoader &loader) override;
};

} // namespace duckdb
