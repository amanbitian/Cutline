#pragma once

// Thin RAII wrappers over the SQLite C API.
//
// Every statement in the project store goes through these types rather than
// string concatenation. Two reasons: user text (media paths, marker labels,
// effect presets) never becomes SQL syntax, and statements can be prepared once
// and reused, which matters when loading a sequence with thousands of clips.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace cutline::db {

// Thrown for any SQLite failure. Carries the SQLite result code so callers can
// distinguish contention (SQLITE_BUSY) from genuine corruption.
class SqlError final : public std::runtime_error {
 public:
  SqlError(int code, std::string message) : std::runtime_error(std::move(message)), code_(code) {}
  [[nodiscard]] int code() const noexcept { return code_; }

 private:
  int code_;
};

[[noreturn]] void ThrowSqlError(sqlite3* database, std::string_view context);
void Check(sqlite3* database, int result, std::string_view context);

class Statement final {
 public:
  Statement(sqlite3* database, std::string_view sql);
  ~Statement();
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  Statement(Statement&& other) noexcept;
  Statement& operator=(Statement&& other) noexcept;

  Statement& Bind(int index, std::int64_t value);
  Statement& Bind(int index, int value) { return Bind(index, static_cast<std::int64_t>(value)); }
  Statement& Bind(int index, bool value) { return Bind(index, static_cast<std::int64_t>(value ? 1 : 0)); }
  Statement& Bind(int index, double value);
  Statement& Bind(int index, std::string_view value);
  Statement& Bind(int index, const char* value) { return Bind(index, std::string_view(value)); }
  Statement& BindBlob(int index, const std::vector<std::byte>& value);
  Statement& BindNull(int index);
  // Binds an optional string as TEXT or NULL, which the schema uses for the
  // nullable parent/source references.
  Statement& BindOptional(int index, const std::optional<std::string>& value);

  // Advances the cursor. Returns true while a row is available.
  [[nodiscard]] bool Step();
  // Runs a statement that must not return rows.
  void Run();
  void Reset();

  [[nodiscard]] std::int64_t ColumnInt(int index) const;
  [[nodiscard]] double ColumnDouble(int index) const;
  [[nodiscard]] std::string ColumnText(int index) const;
  [[nodiscard]] std::optional<std::string> ColumnOptionalText(int index) const;
  [[nodiscard]] bool ColumnIsNull(int index) const;
  [[nodiscard]] int ColumnCount() const;

 private:
  sqlite3* database_{nullptr};
  sqlite3_stmt* statement_{nullptr};
};

// Executes one or more semicolon-separated statements with no bound values.
// Reserved for fixed schema DDL, never for anything carrying user text.
void Execute(sqlite3* database, std::string_view sql);

[[nodiscard]] std::int64_t ScalarInt(sqlite3* database, std::string_view sql);
[[nodiscard]] std::string ScalarText(sqlite3* database, std::string_view sql);

// Scoped transaction. Rolls back unless Commit() is called, so a throwing
// mutation can never leave a half-applied command behind.
class Transaction final {
 public:
  explicit Transaction(sqlite3* database);
  ~Transaction();
  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;
  void Commit();

 private:
  sqlite3* database_{nullptr};
  bool open_{false};
};

}  // namespace cutline::db
