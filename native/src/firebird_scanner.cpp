#include "firebird_scanner.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/null_filter.hpp"
#include "duckdb/planner/filter/in_filter.hpp"
#include "duckdb/planner/filter/optional_filter.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

namespace duckdb {

struct FirebirdGlobalState : public GlobalTableFunctionState {
	string sql;
	bool claimed = false;
	mutex lock;

	idx_t MaxThreads() const override {
		return 1;
	}
};

struct FirebirdLocalState : public LocalTableFunctionState {
	unique_ptr<FirebirdCursor> cursor;
	bool done = false;
};

static string QuoteIdentifier(const string &name) {
	return "\"" + StringUtil::Replace(name, "\"", "\"\"") + "\"";
}

//===----------------------------------------------------------------------===//
// Filter pushdown
//
// DuckDB consumes filters it pushes into a table scan (PhysicalTableScan does
// not re-apply them), so we must translate every *required* filter exactly into
// the Firebird SELECT. Optimization-only filters (OPTIONAL/DYNAMIC/BLOOM) may be
// skipped safely. Anything we cannot represent exactly throws rather than risk
// silently returning wrong rows -- but given this function sets neither
// pushdown_expression nor produces STRUCT columns, those cases are unreachable
// in practice.
//===----------------------------------------------------------------------===//

static string FirebirdCompareOp(ExpressionType type) {
	switch (type) {
	case ExpressionType::COMPARE_EQUAL:
		return "=";
	case ExpressionType::COMPARE_NOTEQUAL:
		return "<>";
	case ExpressionType::COMPARE_LESSTHAN:
		return "<";
	case ExpressionType::COMPARE_GREATERTHAN:
		return ">";
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return "<=";
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return ">=";
	default:
		return "";
	}
}

//! Format a DuckDB Value as a Firebird SQL literal. Returns false if the value
//! type cannot be represented safely.
static bool FirebirdFormatLiteral(const Value &value, string &out) {
	if (value.IsNull()) {
		return false;
	}
	switch (value.type().id()) {
	case LogicalTypeId::BOOLEAN:
		out = value.GetValue<bool>() ? "TRUE" : "FALSE";
		return true;
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL:
		out = value.ToString();
		return true;
	case LogicalTypeId::VARCHAR:
		out = "'" + StringUtil::Replace(value.GetValue<string>(), "'", "''") + "'";
		return true;
	case LogicalTypeId::DATE:
		out = "DATE '" + value.ToString() + "'";
		return true;
	case LogicalTypeId::TIME:
		out = "TIME '" + value.ToString() + "'";
		return true;
	case LogicalTypeId::TIMESTAMP:
		out = "TIMESTAMP '" + value.ToString() + "'";
		return true;
	default:
		return false;
	}
}

//! Translate a single-column TableFilter to a Firebird WHERE fragment.
//! Returns true and sets `out` if a clause was produced; returns false if the
//! filter is optimization-only and was safely skipped; throws if the filter is
//! required but cannot be represented.
static bool TranslateFilter(const TableFilter &filter, const string &col, string &out) {
	switch (filter.filter_type) {
	case TableFilterType::CONSTANT_COMPARISON: {
		auto &cf = filter.Cast<ConstantFilter>();
		auto op = FirebirdCompareOp(cf.comparison_type);
		string literal;
		if (op.empty() || !FirebirdFormatLiteral(cf.constant, literal)) {
			throw NotImplementedException("Firebird filter pushdown: unsupported comparison on column %s", col);
		}
		out = col + " " + op + " " + literal;
		return true;
	}
	case TableFilterType::IS_NULL:
		out = col + " IS NULL";
		return true;
	case TableFilterType::IS_NOT_NULL:
		out = col + " IS NOT NULL";
		return true;
	case TableFilterType::IN_FILTER: {
		auto &in = filter.Cast<InFilter>();
		if (in.values.empty()) {
			out = "1 = 0";
			return true;
		}
		vector<string> literals;
		for (auto &v : in.values) {
			string literal;
			if (!FirebirdFormatLiteral(v, literal)) {
				throw NotImplementedException("Firebird filter pushdown: unsupported IN value on column %s", col);
			}
			literals.push_back(literal);
		}
		out = col + " IN (" + StringUtil::Join(literals, ", ") + ")";
		return true;
	}
	case TableFilterType::CONJUNCTION_AND: {
		auto &conj = filter.Cast<ConjunctionAndFilter>();
		vector<string> parts;
		for (auto &child : conj.child_filters) {
			string part;
			// Dropping a skipped (optional) AND-term only widens the result for an
			// optimization-only term, which is safe.
			if (TranslateFilter(*child, col, part)) {
				parts.push_back("(" + part + ")");
			}
		}
		if (parts.empty()) {
			return false;
		}
		out = StringUtil::Join(parts, " AND ");
		return true;
	}
	case TableFilterType::CONJUNCTION_OR: {
		auto &conj = filter.Cast<ConjunctionOrFilter>();
		vector<string> parts;
		for (auto &child : conj.child_filters) {
			string part;
			// An OR must be represented in full: dropping a branch would wrongly
			// narrow the result, so refuse if any branch is not representable.
			if (!TranslateFilter(*child, col, part)) {
				throw NotImplementedException("Firebird filter pushdown: cannot represent OR filter on column %s", col);
			}
			parts.push_back("(" + part + ")");
		}
		if (parts.empty()) {
			return false;
		}
		out = StringUtil::Join(parts, " OR ");
		return true;
	}
	case TableFilterType::OPTIONAL_FILTER:
	case TableFilterType::DYNAMIC_FILTER:
	case TableFilterType::BLOOM_FILTER:
		// Optimization-only: not required for correctness, safe to skip.
		return false;
	default:
		throw NotImplementedException("Firebird filter pushdown: unsupported filter type");
	}
}

static string BuildWhereClause(const FirebirdBindData &bind_data, const vector<column_t> &column_ids,
                               optional_ptr<TableFilterSet> filters) {
	if (!filters || filters->filters.empty()) {
		return string();
	}
	vector<string> clauses;
	for (auto &entry : filters->filters) {
		// The filter key is an index into column_ids (the projection), not the
		// table column index. Resolve to the real column id, then its name.
		auto proj_index = entry.first;
		if (proj_index >= column_ids.size()) {
			throw NotImplementedException("Firebird filter pushdown: filter references unknown column");
		}
		auto col_id = column_ids[proj_index];
		if (IsRowIdColumnId(col_id) || col_id >= bind_data.names.size()) {
			// We expose no row id / virtual column to filter on.
			throw NotImplementedException("Firebird filter pushdown: filter on unsupported virtual column");
		}
		auto col = QuoteIdentifier(bind_data.names[col_id]);
		string clause;
		if (TranslateFilter(*entry.second, col, clause)) {
			clauses.push_back(clause);
		}
	}
	if (clauses.empty()) {
		return string();
	}
	return " WHERE " + StringUtil::Join(clauses, " AND ");
}

static unique_ptr<GlobalTableFunctionState> FirebirdInitGlobal(ClientContext &context,
                                                               TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<FirebirdBindData>();
	auto result = make_uniq<FirebirdGlobalState>();

	vector<string> select_cols;
	for (auto col_id : input.column_ids) {
		if (IsRowIdColumnId(col_id) || col_id >= bind_data.names.size()) {
			// We do not expose a row id; emit a constant placeholder.
			select_cols.push_back("1");
		} else {
			select_cols.push_back(QuoteIdentifier(bind_data.names[col_id]));
		}
	}
	if (select_cols.empty()) {
		// e.g. COUNT(*) - we still need rows to count.
		select_cols.push_back("1");
	}

	result->sql = "SELECT " + StringUtil::Join(select_cols, ", ") + " FROM " + QuoteIdentifier(bind_data.table_name) +
	              BuildWhereClause(bind_data, input.column_ids, input.filters);
	if (bind_data.has_limit) {
		// Firebird caps the result set with a trailing ROWS clause. DuckDB still
		// applies the real LIMIT/OFFSET on top, so this only reduces transfer.
		result->sql += " ROWS " + to_string(bind_data.limit_rows);
	}
	return std::move(result);
}

static unique_ptr<LocalTableFunctionState>
FirebirdInitLocal(ExecutionContext &context, TableFunctionInitInput &input, GlobalTableFunctionState *global_state) {
	auto &bind_data = input.bind_data->Cast<FirebirdBindData>();
	auto &gstate = global_state->Cast<FirebirdGlobalState>();
	auto result = make_uniq<FirebirdLocalState>();
	{
		lock_guard<mutex> l(gstate.lock);
		if (gstate.claimed) {
			result->done = true;
			return std::move(result);
		}
		gstate.claimed = true;
	}
	result->cursor = make_uniq<FirebirdCursor>(bind_data.params, gstate.sql);
	return std::move(result);
}

static void FirebirdScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &lstate = data.local_state->Cast<FirebirdLocalState>();
	if (lstate.done || !lstate.cursor) {
		output.SetCardinality(0);
		return;
	}

	auto handle = lstate.cursor->handle();
	idx_t out_idx = 0;
	while (out_idx < STANDARD_VECTOR_SIZE) {
		if (!lstate.cursor->Next()) {
			lstate.done = true;
			break;
		}
		for (idx_t c = 0; c < output.ColumnCount(); c++) {
			auto &vec = output.data[c];
			auto col = (int)c;
			if (fb_cell_is_null(handle, col)) {
				FlatVector::SetNull(vec, out_idx, true);
				continue;
			}
			switch (vec.GetType().id()) {
			case LogicalTypeId::SMALLINT:
				FlatVector::GetData<int16_t>(vec)[out_idx] = (int16_t)fb_cell_i64(handle, col);
				break;
			case LogicalTypeId::INTEGER:
				FlatVector::GetData<int32_t>(vec)[out_idx] = (int32_t)fb_cell_i64(handle, col);
				break;
			case LogicalTypeId::BIGINT:
				FlatVector::GetData<int64_t>(vec)[out_idx] = fb_cell_i64(handle, col);
				break;
			case LogicalTypeId::FLOAT:
				FlatVector::GetData<float>(vec)[out_idx] = (float)fb_cell_f64(handle, col);
				break;
			case LogicalTypeId::DOUBLE:
				FlatVector::GetData<double>(vec)[out_idx] = fb_cell_f64(handle, col);
				break;
			case LogicalTypeId::BOOLEAN:
				FlatVector::GetData<bool>(vec)[out_idx] = fb_cell_bool(handle, col) != 0;
				break;
			case LogicalTypeId::DATE:
				FlatVector::GetData<date_t>(vec)[out_idx] = date_t(fb_cell_date_days(handle, col));
				break;
			case LogicalTypeId::TIME:
				FlatVector::GetData<dtime_t>(vec)[out_idx] = dtime_t(fb_cell_time_micros(handle, col));
				break;
			case LogicalTypeId::TIMESTAMP:
				FlatVector::GetData<timestamp_t>(vec)[out_idx] = timestamp_t(fb_cell_ts_micros(handle, col));
				break;
			case LogicalTypeId::VARCHAR: {
				size_t len = 0;
				auto ptr = fb_cell_text(handle, col, &len);
				FlatVector::GetData<string_t>(vec)[out_idx] =
				    StringVector::AddString(vec, reinterpret_cast<const char *>(ptr), len);
				break;
			}
			case LogicalTypeId::BLOB: {
				size_t len = 0;
				auto ptr = fb_cell_blob(handle, col, &len);
				FlatVector::GetData<string_t>(vec)[out_idx] =
				    StringVector::AddStringOrBlob(vec, reinterpret_cast<const char *>(ptr), len);
				break;
			}
			default:
				throw NotImplementedException("Firebird scan: unsupported output type %s",
				                              vec.GetType().ToString());
			}
		}
		out_idx++;
	}
	output.SetCardinality(out_idx);
}

TableFunction FirebirdScanFunction() {
	TableFunction function("firebird_scan", {}, FirebirdScan, nullptr, FirebirdInitGlobal, FirebirdInitLocal);
	function.projection_pushdown = true;
	function.filter_pushdown = true;
	return function;
}

//===----------------------------------------------------------------------===//
// Limit pushdown (optimizer extension)
//
// DuckDB has no flag to hand a LIMIT to a table function, so we register an
// optimizer pass that runs after the built-in optimizers. When a LIMIT sits
// directly above a Firebird scan (through only row-count/order-preserving
// projections), we record count+offset on the scan's bind data so the generated
// SQL can append a Firebird `ROWS` clause. We never remove the LIMIT operator,
// so DuckDB still enforces exact LIMIT/OFFSET semantics -- the pushdown is purely
// a transfer-reduction optimization.
//===----------------------------------------------------------------------===//

//! Follow a chain of single-child, row-count- and order-preserving operators
//! (projections) down to a Firebird scan, if one is reachable.
static optional_ptr<LogicalGet> FindFirebirdGet(LogicalOperator &op) {
	LogicalOperator *current = &op;
	while (true) {
		switch (current->type) {
		case LogicalOperatorType::LOGICAL_PROJECTION:
			if (current->children.size() != 1) {
				return nullptr;
			}
			current = current->children[0].get();
			break;
		case LogicalOperatorType::LOGICAL_GET: {
			auto &get = current->Cast<LogicalGet>();
			if (get.function.name == "firebird_scan" && get.bind_data) {
				return &get;
			}
			return nullptr;
		}
		default:
			// Anything else (FILTER, ORDER_BY, JOIN, AGGREGATE, ...) changes the
			// row count or order, so the limit cannot be safely pushed past it.
			return nullptr;
		}
	}
}

static void FirebirdPushLimit(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_LIMIT && op.children.size() == 1) {
		auto &limit = op.Cast<LogicalLimit>();
		bool offset_ok = limit.offset_val.Type() == LimitNodeType::UNSET ||
		                 limit.offset_val.Type() == LimitNodeType::CONSTANT_VALUE;
		if (limit.limit_val.Type() == LimitNodeType::CONSTANT_VALUE && offset_ok) {
			idx_t count = limit.limit_val.GetConstantValue();
			idx_t offset =
			    limit.offset_val.Type() == LimitNodeType::CONSTANT_VALUE ? limit.offset_val.GetConstantValue() : 0;
			// Fetch enough rows for DuckDB's own LIMIT/OFFSET to operate on. Guard
			// against overflow; if it would overflow, simply skip the pushdown.
			if (offset <= NumericLimits<idx_t>::Maximum() - count) {
				auto get = FindFirebirdGet(*op.children[0]);
				if (get) {
					auto &bind = get->bind_data->Cast<FirebirdBindData>();
					idx_t total = count + offset;
					if (!bind.has_limit || total < bind.limit_rows) {
						bind.has_limit = true;
						bind.limit_rows = total;
					}
				}
			}
		}
	}
	for (auto &child : op.children) {
		FirebirdPushLimit(*child);
	}
}

static void FirebirdLimitOptimizeFunction(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	if (plan) {
		FirebirdPushLimit(*plan);
	}
}

void RegisterFirebirdLimitPushdown(DBConfig &config) {
	OptimizerExtension extension;
	extension.optimize_function = FirebirdLimitOptimizeFunction;
	OptimizerExtension::Register(config, std::move(extension));
}

} // namespace duckdb
