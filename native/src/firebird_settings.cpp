#include "firebird_settings.hpp"

#include "duckdb/common/string_util.hpp"

#include <cstdlib>

namespace duckdb {

// StringUtil::Trim mutates in place and returns void, so wrap it for value use.
static string Trimmed(string s) {
	StringUtil::Trim(s);
	return s;
}

string FirebirdSettings::NormalizeUser(const string &user) {
	auto t = Trimmed(user);
	if (t.size() >= 2 && t.front() == '"' && t.back() == '"') {
		return t.substr(1, t.size() - 2);
	}
	return StringUtil::Upper(t);
}

bool FirebirdSettings::GetEnv(const char *name, string &out) {
	auto value = std::getenv(name);
	if (!value) {
		return false;
	}
	out = string(value);
	return !out.empty();
}

FirebirdDSN FirebirdSettings::ParseDSN(const string &input_p) {
	FirebirdDSN dsn;
	string input = Trimmed(input_p);
	if (input.empty()) {
		return dsn;
	}

	// Strip the firebird:// scheme if present.
	for (auto &scheme : {string("firebird://"), string("fb://")}) {
		if (StringUtil::StartsWith(StringUtil::Lower(input), scheme)) {
			input = input.substr(scheme.size());
			break;
		}
	}

	// Split off the query string (?charset=...).
	auto qpos = input.find('?');
	string query;
	if (qpos != string::npos) {
		query = input.substr(qpos + 1);
		input = input.substr(0, qpos);
	}

	// Split off user[:password]@ credentials (use the last '@' before any path).
	auto at_pos = input.find('@');
	if (at_pos != string::npos) {
		auto creds = input.substr(0, at_pos);
		input = input.substr(at_pos + 1);
		auto colon = creds.find(':');
		if (colon != string::npos) {
			dsn.user = creds.substr(0, colon);
			dsn.password = creds.substr(colon + 1);
			dsn.has_user = !dsn.user.empty();
			dsn.has_password = true;
		} else if (!creds.empty()) {
			dsn.user = creds;
			dsn.has_user = true;
		}
	}

	// Remaining: host[:port]/database. Handle IPv6 [::1] host bracket notation.
	string hostport;
	string database;
	if (!input.empty() && input.front() == '[') {
		auto close = input.find(']');
		if (close != string::npos) {
			dsn.host = input.substr(1, close - 1);
			dsn.has_host = true;
			auto rest = input.substr(close + 1);
			// rest may be :port/db or /db
			auto slash = rest.find('/');
			string after_host = slash == string::npos ? rest : rest.substr(0, slash);
			if (!after_host.empty() && after_host.front() == ':') {
				dsn.port = (uint16_t)std::stoi(after_host.substr(1));
				dsn.has_port = true;
			}
			if (slash != string::npos) {
				database = rest.substr(slash + 1);
			}
		}
	} else {
		auto slash = input.find('/');
		if (slash == string::npos) {
			// No '/': the whole thing is host:port OR a database/path.
			// If it contains a ':' that is not a Windows drive, treat as host:port.
			hostport = input;
			auto colon = hostport.find(':');
			bool windows_drive = colon == 1; // e.g. C:\path
			if (colon != string::npos && !windows_drive) {
				dsn.host = hostport.substr(0, colon);
				dsn.has_host = !dsn.host.empty();
				dsn.port = (uint16_t)std::stoi(hostport.substr(colon + 1));
				dsn.has_port = true;
			} else {
				database = hostport;
			}
		} else {
			hostport = input.substr(0, slash);
			database = input.substr(slash + 1);
			auto colon = hostport.find(':');
			bool windows_drive = colon == 1;
			if (colon != string::npos && !windows_drive) {
				dsn.host = hostport.substr(0, colon);
				dsn.has_host = !dsn.host.empty();
				dsn.port = (uint16_t)std::stoi(hostport.substr(colon + 1));
				dsn.has_port = true;
			} else if (!hostport.empty()) {
				dsn.host = hostport;
				dsn.has_host = true;
			}
		}
	}

	if (!database.empty()) {
		dsn.database = database;
		dsn.has_database = true;
	}

	// Parse query parameters.
	if (!query.empty()) {
		for (auto &kv : StringUtil::Split(query, '&')) {
			auto eq = kv.find('=');
			if (eq == string::npos) {
				continue;
			}
			auto key = StringUtil::Lower(Trimmed(kv.substr(0, eq)));
			auto value = Trimmed(kv.substr(eq + 1));
			if (key == "charset") {
				dsn.charset = value;
				dsn.has_charset = true;
			} else if (key == "user") {
				dsn.user = value;
				dsn.has_user = true;
			} else if (key == "password") {
				dsn.password = value;
				dsn.has_password = true;
			} else if (key == "port") {
				dsn.port = (uint16_t)std::stoi(value);
				dsn.has_port = true;
			}
		}
	}

	return dsn;
}

} // namespace duckdb
