#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace testforge::db {

class Connection;

/// RAII wrapper around a prepared statement.
///
/// Every query in TestForge goes through this type, and it has no interface
/// for splicing text into SQL. That is the point: values are bound as
/// parameters, so a test name containing an apostrophe (or a semicolon and a
/// DROP TABLE) is data and can never become syntax. See docs/security.md.
class Statement {
 public:
    Statement(Connection& connection, std::string_view sql);

    ~Statement();

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;

    // --- binding: 1-based indices, matching SQLite's own convention --------
    Statement& bind(int index, std::nullptr_t);
    Statement& bind(int index, std::int64_t value);
    Statement& bind(int index, int value);
    Statement& bind(int index, double value);
    Statement& bind(int index, bool value);
    Statement& bind(int index, std::string_view value);
    Statement& bind(int index, const std::string& value);
    Statement& bind(int index, const char* value);

    /// Binds an optional: nullopt becomes SQL NULL.
    template<typename T>
    Statement& bind(int index, const std::optional<T>& value) {
        if (value.has_value()) {
            return bind(index, *value);
        }
        return bind(index, nullptr);
    }

    /// Binds every argument in order starting at index 1.
    template<typename... Args>
    Statement& bindAll(const Args&... args) {
        int index = 0;
        (bind(++index, args), ...);
        return *this;
    }

    /// Advances to the next row. Returns false when the result set is done.
    bool step();

    /// Runs a statement that returns no rows. Throws if it does.
    void execute();

    void reset();

    void clearBindings();

    // --- reading: 0-based indices, matching SQLite ------------------------
    [[nodiscard]] int columnCount() const;
    [[nodiscard]] std::string columnName(int index) const;
    [[nodiscard]] bool isNull(int index) const;
    [[nodiscard]] std::int64_t getInt(int index) const;
    [[nodiscard]] double getDouble(int index) const;
    [[nodiscard]] std::string getText(int index) const;
    [[nodiscard]] bool getBool(int index) const;

    [[nodiscard]] sqlite3_stmt* handle() const noexcept { return statement_; }

 private:
    void checkBindResult(int code, int index) const;

    sqlite3* database_ = nullptr;
    sqlite3_stmt* statement_ = nullptr;
    std::string sql_;
};

/// RAII transaction. Rolls back unless commit() was called.
///
/// Grouping the writes of one test run into a transaction turns thousands of
/// individual fsyncs into one, and — more importantly — means a crash mid-run
/// cannot leave a half-written result row behind.
class Transaction {
 public:
    explicit Transaction(Connection& connection);

    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = delete;
    Transaction& operator=(Transaction&&) = delete;

    void commit();

    void rollback();

 private:
    Connection* connection_;
    bool finished_ = false;
};

/// RAII wrapper around an open SQLite database.
class Connection {
 public:
    /// Opens (creating if necessary). Pass ":memory:" for an in-memory
    /// database, which is what the unit tests use.
    /// Throws PersistenceError on failure.
    Connection(const std::string& path, std::int64_t busyTimeoutMs = 5000);

    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;

    /// Runs one or more statements with no parameters and no results.
    void executeScript(std::string_view sql);

    [[nodiscard]] Statement prepare(std::string_view sql);

    [[nodiscard]] std::int64_t lastInsertRowId() const;

    [[nodiscard]] int changes() const;

    [[nodiscard]] sqlite3* handle() const noexcept { return database_; }

    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    /// Current value of a PRAGMA, for diagnostics.
    [[nodiscard]] std::string pragma(const std::string& name);

    /// Runs "PRAGMA integrity_check" and returns "ok" or the problems found.
    [[nodiscard]] std::string integrityCheck();

    /// The SQLite library version this binary is linked against.
    [[nodiscard]] static std::string libraryVersion();

 private:
    sqlite3* database_ = nullptr;
    std::string path_;
};

}  // namespace testforge::db
