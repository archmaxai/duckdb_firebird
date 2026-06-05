//===----------------------------------------------------------------------===//
//                         DuckDB - Firebird
//
// storage/firebird_transaction_manager.hpp
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb/transaction/transaction_manager.hpp"
#include "duckdb/common/reference_map.hpp"
#include "storage/firebird_catalog.hpp"
#include "storage/firebird_transaction.hpp"

namespace duckdb {

class FirebirdTransactionManager : public TransactionManager {
public:
	FirebirdTransactionManager(AttachedDatabase &db_p, FirebirdCatalog &firebird_catalog);

	Transaction &StartTransaction(ClientContext &context) override;
	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override;
	void RollbackTransaction(Transaction &transaction) override;
	void Checkpoint(ClientContext &context, bool force = false) override;

private:
	FirebirdCatalog &firebird_catalog;
	mutex transaction_lock;
	reference_map_t<Transaction, unique_ptr<FirebirdTransaction>> transactions;
};

} // namespace duckdb
