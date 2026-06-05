//===----------------------------------------------------------------------===//
//                         DuckDB - Firebird
//
// firebird_settings.hpp
//
// Resolution of Firebird connection parameters from the ATTACH string, ATTACH
// named options and environment variables.
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb.hpp"
#include "firebird_connection.hpp"

namespace duckdb {

//! Parsed pieces of a `firebird://` DSN. Empty optionals mean "not specified".
struct FirebirdDSN {
	bool has_host = false;
	string host;
	bool has_port = false;
	uint16_t port = 3050;
	bool has_user = false;
	string user;
	bool has_password = false;
	string password;
	bool has_database = false;
	string database;
	bool has_charset = false;
	string charset;
};

struct FirebirdSettings {
	//! Parse a `firebird://user:pass@host:port/db?charset=X` style string. A bare
	//! path/host is treated as the database. Tolerant: unspecified parts stay unset.
	static FirebirdDSN ParseDSN(const string &input);

	//! Firebird upper-cases unquoted login names for its SRP verifier; a name
	//! wrapped in double quotes is taken verbatim.
	static string NormalizeUser(const string &user);

	//! Read an environment variable; returns true and sets `out` if present.
	static bool GetEnv(const char *name, string &out);
};

} // namespace duckdb
