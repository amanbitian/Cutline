#include "core/db/Sql.h"

#include "sqlite3.h"

#include <stdexcept>
#include <utility>

namespace cutline::db {

void ThrowSqlError(sqlite3* database, std::string_view context) {
  const auto code = database != nullptr ? sqlite3_extended_errcode(database) : SQLITE_ERROR;
  const char* detail = database != nullptr ? sqlite3_errmsg(database) : "no database handle";
  throw SqlError(code, std::string(context) + ": " + (detail != nullptr ? detail : "unknown SQLite error"));
}

void Check(sqlite3* database, int result, std::string_view context) {
  if (result != SQLITE_OK) ThrowSqlError(database, context);
}

Statement::Statement(sqlite3* database, std::string_view sql) : database_(database) {
  if (database_ == nullptr) throw std::logic_error("Statement requires an open database");
  const auto result = sqlite3_prepare_v2(database_, sql.data(), static_cast<int>(sql.size()), &statement_, nullptr);
  if (result != SQLITE_OK) {
    statement_ = nullptr;
    ThrowSqlError(database_, "Unable to prepare statement");
  }
}

Statement::~Statement() {
  if (statement_ != nullptr) sqlite3_finalize(statement_);
}

Statement::Statement(Statement&& other) noexcept
    : database_(std::exchange(other.database_, nullptr)), statement_(std::exchange(other.statement_, nullptr)) {}

Statement& Statement::operator=(Statement&& other) noexcept {
  if (this != &other) {
    if (statement_ != nullptr) sqlite3_finalize(statement_);
    database_ = std::exchange(other.database_, nullptr);
    statement_ = std::exchange(other.statement_, nullptr);
  }
  return *this;
}

Statement& Statement::Bind(int index, std::int64_t value) {
  Check(database_, sqlite3_bind_int64(statement_, index, value), "Unable to bind integer");
  return *this;
}

Statement& Statement::Bind(int index, double value) {
  Check(database_, sqlite3_bind_double(statement_, index, value), "Unable to bind real");
  return *this;
}

Statement& Statement::Bind(int index, std::string_view value) {
  // SQLITE_TRANSIENT: SQLite copies the bytes, so callers may bind temporaries.
  Check(database_, sqlite3_bind_text(statement_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT),
        "Unable to bind text");
  return *this;
}

Statement& Statement::BindBlob(int index, const std::vector<std::byte>& value) {
  if (value.empty()) return BindNull(index);
  Check(database_, sqlite3_bind_blob64(statement_, index, value.data(), value.size(), SQLITE_TRANSIENT),
        "Unable to bind blob");
  return *this;
}

Statement& Statement::BindNull(int index) {
  Check(database_, sqlite3_bind_null(statement_, index), "Unable to bind null");
  return *this;
}

Statement& Statement::BindOptional(int index, const std::optional<std::string>& value) {
  return value.has_value() ? Bind(index, *value) : BindNull(index);
}

bool Statement::Step() {
  const auto result = sqlite3_step(statement_);
  if (result == SQLITE_ROW) return true;
  if (result == SQLITE_DONE) return false;
  ThrowSqlError(database_, "Statement failed during execution");
}

void Statement::Run() {
  if (Step()) throw std::logic_error("Statement returned rows where none were expected");
}

void Statement::Reset() {
  sqlite3_reset(statement_);
  sqlite3_clear_bindings(statement_);
}

std::int64_t Statement::ColumnInt(int index) const { return sqlite3_column_int64(statement_, index); }
double Statement::ColumnDouble(int index) const { return sqlite3_column_double(statement_, index); }

std::string Statement::ColumnText(int index) const {
  const auto* text = sqlite3_column_text(statement_, index);
  if (text == nullptr) return {};
  // sqlite3_column_bytes must follow column_text so the length matches the UTF-8
  // conversion; this preserves embedded NULs that a strlen-based read would cut.
  const auto size = static_cast<std::size_t>(sqlite3_column_bytes(statement_, index));
  return std::string(reinterpret_cast<const char*>(text), size);
}

std::optional<std::string> Statement::ColumnOptionalText(int index) const {
  if (ColumnIsNull(index)) return std::nullopt;
  return ColumnText(index);
}

bool Statement::ColumnIsNull(int index) const { return sqlite3_column_type(statement_, index) == SQLITE_NULL; }
int Statement::ColumnCount() const { return sqlite3_column_count(statement_); }

void Execute(sqlite3* database, std::string_view sql) {
  char* message = nullptr;
  const std::string owned(sql);
  if (sqlite3_exec(database, owned.c_str(), nullptr, nullptr, &message) != SQLITE_OK) {
    const auto code = sqlite3_extended_errcode(database);
    const std::string detail = message != nullptr ? message : "unknown SQLite error";
    sqlite3_free(message);
    throw SqlError(code, "Unable to execute statement: " + detail);
  }
}

std::int64_t ScalarInt(sqlite3* database, std::string_view sql) {
  Statement statement(database, sql);
  return statement.Step() ? statement.ColumnInt(0) : 0;
}

std::string ScalarText(sqlite3* database, std::string_view sql) {
  Statement statement(database, sql);
  return statement.Step() ? statement.ColumnText(0) : std::string{};
}

Transaction::Transaction(sqlite3* database) : database_(database) {
  // IMMEDIATE takes the write lock up front so two concurrent writers fail fast
  // instead of deadlocking at COMMIT time.
  Execute(database_, "BEGIN IMMEDIATE;");
  open_ = true;
}

Transaction::~Transaction() {
  if (!open_) return;
  try {
    Execute(database_, "ROLLBACK;");
  } catch (...) {
    // A failed rollback during unwinding is not recoverable here; the next
    // statement on this connection will surface it.
  }
}

void Transaction::Commit() {
  if (!open_) throw std::logic_error("Transaction has already finished");
  Execute(database_, "COMMIT;");
  open_ = false;
}

}  // namespace cutline::db
