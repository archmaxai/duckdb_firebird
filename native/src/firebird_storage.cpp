#include "firebird_storage.hpp"

#include "firebird_settings.hpp"
#include "storage/firebird_catalog.hpp"
#include "storage/firebird_transaction_manager.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/main/attached_database.hpp"

namespace duckdb {

static bool GetOption(const unordered_map<string, Value> &options, const vector<string> &keys, string &out) {
	for (auto &key : keys) {
		for (auto &entry : options) {
			if (StringUtil::CIEquals(entry.first, key)) {
				out = entry.second.ToString();
				return true;
			}
		}
	}
	return false;
}

static FirebirdConnectionParameters ResolveParameters(AttachInfo &info,
                                                      const unordered_map<string, Value> &options) {
	FirebirdConnectionParameters params;
	auto dsn = FirebirdSettings::ParseDSN(info.path);
	string tmp;

	if (GetOption(options, {"host"}, tmp)) {
		params.host = tmp;
	} else if (dsn.has_host) {
		params.host = dsn.host;
	} else if (FirebirdSettings::GetEnv("FIREBIRD_HOST", tmp)) {
		params.host = tmp;
	}

	if (GetOption(options, {"port"}, tmp)) {
		params.port = (uint16_t)std::stoi(tmp);
	} else if (dsn.has_port) {
		params.port = dsn.port;
	} else if (FirebirdSettings::GetEnv("FIREBIRD_PORT", tmp)) {
		params.port = (uint16_t)std::stoi(tmp);
	}

	string user;
	if (GetOption(options, {"user", "username"}, tmp)) {
		user = tmp;
	} else if (dsn.has_user) {
		user = dsn.user;
	} else if (FirebirdSettings::GetEnv("FIREBIRD_USER", tmp)) {
		user = tmp;
	} else {
		user = "SYSDBA";
	}
	params.user = FirebirdSettings::NormalizeUser(user);

	if (GetOption(options, {"password", "pass"}, tmp)) {
		params.password = tmp;
	} else if (dsn.has_password) {
		params.password = dsn.password;
	} else if (FirebirdSettings::GetEnv("FIREBIRD_PASSWORD", tmp)) {
		params.password = tmp;
	}

	if (GetOption(options, {"database", "db"}, tmp)) {
		params.database = tmp;
	} else if (dsn.has_database) {
		params.database = dsn.database;
	} else if (FirebirdSettings::GetEnv("FIREBIRD_DATABASE", tmp)) {
		params.database = tmp;
	}

	if (GetOption(options, {"charset"}, tmp)) {
		params.charset = tmp;
	} else if (dsn.has_charset) {
		params.charset = dsn.charset;
	} else if (FirebirdSettings::GetEnv("FIREBIRD_CHARSET", tmp)) {
		params.charset = tmp;
	}

	if (params.database.empty()) {
		throw BinderException("Firebird ATTACH requires a database path - provide it in the connection string "
		                      "(e.g. 'firebird://user:pass@host:3050/path/to/db.fdb') or via the DATABASE option");
	}
	return params;
}

static unique_ptr<Catalog> FirebirdAttach(optional_ptr<StorageExtensionInfo> storage_info, ClientContext &context,
                                          AttachedDatabase &db, const string &name, AttachInfo &info,
                                          AttachOptions &attach_options) {
	auto params = ResolveParameters(info, attach_options.options);
	return make_uniq<FirebirdCatalog>(db, std::move(params), attach_options.access_mode);
}

static unique_ptr<TransactionManager>
FirebirdCreateTransactionManager(optional_ptr<StorageExtensionInfo> storage_info, AttachedDatabase &db,
                                 Catalog &catalog) {
	auto &fb_catalog = catalog.Cast<FirebirdCatalog>();
	return make_uniq<FirebirdTransactionManager>(db, fb_catalog);
}

FirebirdStorageExtension::FirebirdStorageExtension() {
	attach = FirebirdAttach;
	create_transaction_manager = FirebirdCreateTransactionManager;
}

} // namespace duckdb
