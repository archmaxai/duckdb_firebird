#include "storage/firebird_transaction_manager.hpp"

#include "duckdb/main/attached_database.hpp"

namespace duckdb {

FirebirdTransactionManager::FirebirdTransactionManager(AttachedDatabase &db_p, FirebirdCatalog &firebird_catalog)
    : TransactionManager(db_p), firebird_catalog(firebird_catalog) {
}

Transaction &FirebirdTransactionManager::StartTransaction(ClientContext &context) {
	auto transaction = make_uniq<FirebirdTransaction>(firebird_catalog, *this, context);
	auto &result = *transaction;
	lock_guard<mutex> l(transaction_lock);
	transactions[result] = std::move(transaction);
	return result;
}

ErrorData FirebirdTransactionManager::CommitTransaction(ClientContext &context, Transaction &transaction) {
	lock_guard<mutex> l(transaction_lock);
	transactions.erase(transaction);
	return ErrorData();
}

void FirebirdTransactionManager::RollbackTransaction(Transaction &transaction) {
	lock_guard<mutex> l(transaction_lock);
	transactions.erase(transaction);
}

void FirebirdTransactionManager::Checkpoint(ClientContext &context, bool force) {
	// Read-only remote catalog: nothing to checkpoint.
}

} // namespace duckdb
