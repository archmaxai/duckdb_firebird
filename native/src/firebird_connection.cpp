#include "firebird_connection.hpp"

#include "duckdb/common/exception.hpp"

namespace duckdb {

// Firebird internal field types (RDB$FIELDS.RDB$FIELD_TYPE).
enum FirebirdFieldType : int {
	FB_TYPE_SHORT = 7,
	FB_TYPE_LONG = 8,
	FB_TYPE_QUAD = 9,
	FB_TYPE_FLOAT = 10,
	FB_TYPE_DOUBLE = 27,
	FB_TYPE_D_FLOAT = 11,
	FB_TYPE_DATE = 12,
	FB_TYPE_TIME = 13,
	FB_TYPE_TIMESTAMP = 35,
	FB_TYPE_CHAR = 14,
	FB_TYPE_VARCHAR = 37,
	FB_TYPE_CSTRING = 40,
	FB_TYPE_BLOB = 261,
	FB_TYPE_BOOLEAN = 23,
	FB_TYPE_INT128 = 26
};

LogicalType FirebirdFieldToLogicalType(int field_type, int sub_type, int scale, int length, int precision) {
	switch (field_type) {
	case FB_TYPE_SHORT:
		return scale < 0 ? LogicalType::DOUBLE : LogicalType::SMALLINT;
	case FB_TYPE_LONG:
		return scale < 0 ? LogicalType::DOUBLE : LogicalType::INTEGER;
	case FB_TYPE_INT128:
	case FB_TYPE_QUAD:
	case FB_TYPE_DOUBLE:
		// NUMERIC/DECIMAL with INT64/INT128 backing arrives from the wire protocol
		// as a scaled double, so we surface DOUBLE for fidelity with the scan.
		return field_type == FB_TYPE_QUAD ? LogicalType::BIGINT : LogicalType::DOUBLE;
	case 16: // INT64
		return scale < 0 ? LogicalType::DOUBLE : LogicalType::BIGINT;
	case FB_TYPE_FLOAT:
		return LogicalType::FLOAT;
	case FB_TYPE_D_FLOAT:
		return LogicalType::DOUBLE;
	case FB_TYPE_DATE:
		return LogicalType::DATE;
	case FB_TYPE_TIME:
		return LogicalType::TIME;
	case FB_TYPE_TIMESTAMP:
		return LogicalType::TIMESTAMP;
	case FB_TYPE_CHAR:
	case FB_TYPE_VARCHAR:
	case FB_TYPE_CSTRING:
		return LogicalType::VARCHAR;
	case FB_TYPE_BLOB:
		// sub_type 1 == text blob, everything else is binary.
		return sub_type == 1 ? LogicalType::VARCHAR : LogicalType::BLOB;
	case FB_TYPE_BOOLEAN:
		return LogicalType::BOOLEAN;
	default:
		return LogicalType::VARCHAR;
	}
}

//===----------------------------------------------------------------------===//
// FirebirdConnection
//===----------------------------------------------------------------------===//

static string TakeError(char *err) {
	if (!err) {
		return "unknown Firebird error";
	}
	string msg(err);
	fb_free_string(err);
	return msg;
}

FirebirdConnection::FirebirdConnection(const FirebirdConnectionParameters &params) {
	char *err = nullptr;
	conn = fb_connect(params.host.c_str(), params.port, params.user.c_str(), params.password.c_str(),
	                  params.database.c_str(), params.charset.c_str(), &err);
	if (!conn) {
		throw IOException("Failed to connect to Firebird database \"%s\": %s", params.database, TakeError(err));
	}
}

FirebirdConnection::~FirebirdConnection() {
	Close();
}

FirebirdConnection::FirebirdConnection(FirebirdConnection &&other) noexcept {
	conn = other.conn;
	other.conn = nullptr;
}

FirebirdConnection &FirebirdConnection::operator=(FirebirdConnection &&other) noexcept {
	if (this != &other) {
		Close();
		conn = other.conn;
		other.conn = nullptr;
	}
	return *this;
}

void FirebirdConnection::Close() {
	if (conn) {
		fb_disconnect(conn);
		conn = nullptr;
	}
}

vector<string> FirebirdConnection::GetTables() {
	if (!conn) {
		throw InternalException("GetTables called on closed Firebird connection");
	}
	char *err = nullptr;
	auto list = fb_list_tables(conn, &err);
	if (!list) {
		throw IOException("Failed to list Firebird tables: %s", TakeError(err));
	}
	vector<string> result;
	auto count = fb_strlist_len(list);
	result.reserve(count);
	for (int i = 0; i < count; i++) {
		auto name = fb_strlist_get(list, i);
		if (name) {
			result.emplace_back(name);
		}
	}
	fb_strlist_free(list);
	return result;
}

void FirebirdConnection::GetTableColumns(const string &table_name, vector<string> &names,
                                         vector<LogicalType> &types) {
	if (!conn) {
		throw InternalException("GetTableColumns called on closed Firebird connection");
	}
	char *err = nullptr;
	auto list = fb_table_columns(conn, table_name.c_str(), &err);
	if (!list) {
		throw IOException("Failed to read columns for Firebird table \"%s\": %s", table_name, TakeError(err));
	}
	auto count = fb_collist_len(list);
	for (int i = 0; i < count; i++) {
		auto name = fb_collist_name(list, i);
		auto field_type = fb_collist_field_type(list, i);
		auto sub_type = fb_collist_sub_type(list, i);
		auto scale = fb_collist_scale(list, i);
		auto length = fb_collist_length(list, i);
		auto precision = fb_collist_precision(list, i);
		names.emplace_back(name ? name : "");
		types.emplace_back(FirebirdFieldToLogicalType(field_type, sub_type, scale, length, precision));
	}
	fb_collist_free(list);
}

case_insensitive_map_t<FirebirdTableColumns> FirebirdConnection::GetAllTableColumns() {
	if (!conn) {
		throw InternalException("GetAllTableColumns called on closed Firebird connection");
	}
	char *err = nullptr;
	auto list = fb_all_columns(conn, &err);
	if (!list) {
		throw IOException("Failed to read Firebird column metadata: %s", TakeError(err));
	}
	case_insensitive_map_t<FirebirdTableColumns> result;
	auto count = fb_collist_len(list);
	for (int i = 0; i < count; i++) {
		auto table = fb_collist_table(list, i);
		if (!table) {
			continue;
		}
		auto name = fb_collist_name(list, i);
		auto field_type = fb_collist_field_type(list, i);
		auto sub_type = fb_collist_sub_type(list, i);
		auto scale = fb_collist_scale(list, i);
		auto length = fb_collist_length(list, i);
		auto precision = fb_collist_precision(list, i);
		auto &cols = result[table];
		cols.names.emplace_back(name ? name : "");
		cols.types.emplace_back(FirebirdFieldToLogicalType(field_type, sub_type, scale, length, precision));
	}
	fb_collist_free(list);
	return result;
}

//===----------------------------------------------------------------------===//
// FirebirdCursor
//===----------------------------------------------------------------------===//

FirebirdCursor::FirebirdCursor(const FirebirdConnectionParameters &params, const string &sql) {
	char *err = nullptr;
	cursor = fb_cursor_open(params.host.c_str(), params.port, params.user.c_str(), params.password.c_str(),
	                        params.database.c_str(), params.charset.c_str(), sql.c_str(), &err);
	if (!cursor) {
		throw IOException("Failed to execute Firebird query [%s]: %s", sql, TakeError(err));
	}
}

FirebirdCursor::~FirebirdCursor() {
	if (cursor) {
		fb_cursor_free(cursor);
		cursor = nullptr;
	}
}

bool FirebirdCursor::Next() {
	char *err = nullptr;
	auto rc = fb_cursor_next(cursor, &err);
	if (rc < 0) {
		throw IOException("Error while fetching from Firebird: %s", TakeError(err));
	}
	return rc == 1;
}

} // namespace duckdb
