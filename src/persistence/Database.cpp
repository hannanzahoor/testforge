#include "testforge/persistence/Database.hpp"

#include "testforge/core/Exceptions.hpp"
#include "testforge/core/StringUtils.hpp"

#include <sqlite3.h>

#include <sstream>
#include <utility>

namespace testforge::db {
namespace {

[[noreturn]] void throwSqliteError(sqlite3* database, const std::string& context) {
    std::ostringstream os;
    os << context;
    if (database != nullptr) {
        os << ": " << sqlite3_errmsg(database) << " (code " << sqlite3_extended_errcode(database)
           << ")";
    }
    throw PersistenceError(os.str());
}

}  // namespace

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

Connection::Connection(const std::string& path, std::int64_t busyTimeoutMs) : path_(path) {
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(path.c_str(), &database_, flags, nullptr) != SQLITE_OK) {
        const std::string message =
            database_ != nullptr ? sqlite3_errmsg(database_) : "out of memory";
        sqlite3_close(database_);
        database_ = nullptr;
        throw PersistenceError("cannot open database '" + path + "': " + message);
    }

    // Wait rather than fail when another connection holds the write lock. A
    // CLI run and the REST server may share the same file.
    sqlite3_busy_timeout(database_, static_cast<int>(busyTimeoutMs));

    // WAL lets readers (the dashboard, `testforge history`) proceed while a run
    // is writing results. It is the single most useful pragma for this
    // workload. It is unsupported for :memory:, where the call is harmless.
    executeScript("PRAGMA journal_mode = WAL;");
    // NORMAL trades a tiny durability window on power loss for a large write
    // speedup; test results are reproducible, so that is the right trade.
    executeScript("PRAGMA synchronous = NORMAL;");
    executeScript("PRAGMA foreign_keys = ON;");
    executeScript("PRAGMA temp_store = MEMORY;");
}

Connection::~Connection() {
    if (database_ != nullptr) {
        sqlite3_close_v2(database_);
        database_ = nullptr;
    }
}

Connection::Connection(Connection&& other) noexcept
    : database_(other.database_), path_(std::move(other.path_)) {
    other.database_ = nullptr;
}

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        if (database_ != nullptr) {
            sqlite3_close_v2(database_);
        }
        database_ = other.database_;
        path_ = std::move(other.path_);
        other.database_ = nullptr;
    }
    return *this;
}

void Connection::executeScript(std::string_view sql) {
    const std::string text(sql);
    char* error = nullptr;
    if (sqlite3_exec(database_, text.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error != nullptr ? error : "unknown error";
        sqlite3_free(error);
        throw PersistenceError("SQL script failed: " + message);
    }
}

Statement Connection::prepare(std::string_view sql) {
    return Statement(*this, sql);
}

std::int64_t Connection::lastInsertRowId() const {
    return sqlite3_last_insert_rowid(database_);
}

int Connection::changes() const {
    return sqlite3_changes(database_);
}

std::string Connection::pragma(const std::string& name) {
    // The name is not user input — every call site passes a literal — but it
    // still cannot be bound as a parameter, so validate it rather than trust
    // the call site forever.
    for (const char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '_') {
            throw PersistenceError("invalid pragma name: " + name);
        }
    }
    Statement statement(*this, "PRAGMA " + name + ";");
    if (statement.step()) {
        return statement.getText(0);
    }
    return {};
}

std::string Connection::integrityCheck() {
    Statement statement(*this, "PRAGMA integrity_check;");
    std::ostringstream os;
    while (statement.step()) {
        if (os.tellp() > 0) {
            os << "; ";
        }
        os << statement.getText(0);
    }
    return os.str();
}

std::string Connection::libraryVersion() {
    return sqlite3_libversion();
}

// ---------------------------------------------------------------------------
// Statement
// ---------------------------------------------------------------------------

Statement::Statement(Connection& connection, std::string_view sql)
    : database_(connection.handle()), sql_(sql) {
    if (sqlite3_prepare_v2(
            database_, sql_.c_str(), static_cast<int>(sql_.size()), &statement_, nullptr) !=
        SQLITE_OK) {
        throwSqliteError(database_,
                         "cannot prepare statement [" + strings::truncate(sql_, 200) + "]");
    }
}

Statement::~Statement() {
    if (statement_ != nullptr) {
        sqlite3_finalize(statement_);
        statement_ = nullptr;
    }
}

Statement::Statement(Statement&& other) noexcept
    : database_(other.database_), statement_(other.statement_), sql_(std::move(other.sql_)) {
    other.statement_ = nullptr;
    other.database_ = nullptr;
}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        if (statement_ != nullptr) {
            sqlite3_finalize(statement_);
        }
        database_ = other.database_;
        statement_ = other.statement_;
        sql_ = std::move(other.sql_);
        other.statement_ = nullptr;
        other.database_ = nullptr;
    }
    return *this;
}

void Statement::checkBindResult(int code, int index) const {
    if (code != SQLITE_OK) {
        throwSqliteError(database_,
                         "cannot bind parameter " + std::to_string(index) + " of [" +
                             strings::truncate(sql_, 120) + "]");
    }
}

Statement& Statement::bind(int index, std::nullptr_t) {
    checkBindResult(sqlite3_bind_null(statement_, index), index);
    return *this;
}

Statement& Statement::bind(int index, std::int64_t value) {
    checkBindResult(sqlite3_bind_int64(statement_, index, value), index);
    return *this;
}

Statement& Statement::bind(int index, int value) {
    return bind(index, static_cast<std::int64_t>(value));
}

Statement& Statement::bind(int index, double value) {
    checkBindResult(sqlite3_bind_double(statement_, index, value), index);
    return *this;
}

Statement& Statement::bind(int index, bool value) {
    return bind(index, static_cast<std::int64_t>(value ? 1 : 0));
}

Statement& Statement::bind(int index, std::string_view value) {
    // SQLITE_TRANSIENT: SQLite copies the bytes, so the caller's buffer does
    // not have to outlive the statement. Getting this wrong is the classic
    // source of garbage-in-the-database bugs.
    checkBindResult(
        sqlite3_bind_text(
            statement_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT),
        index);
    return *this;
}

Statement& Statement::bind(int index, const std::string& value) {
    return bind(index, std::string_view(value));
}

Statement& Statement::bind(int index, const char* value) {
    if (value == nullptr) {
        return bind(index, nullptr);
    }
    return bind(index, std::string_view(value));
}

bool Statement::step() {
    const int code = sqlite3_step(statement_);
    if (code == SQLITE_ROW) {
        return true;
    }
    if (code == SQLITE_DONE) {
        return false;
    }
    throwSqliteError(database_, "statement failed [" + strings::truncate(sql_, 200) + "]");
}

void Statement::execute() {
    if (step()) {
        // A statement bound with execute() is not expected to return rows;
        // silently discarding them would hide a mistake in the caller.
        throw PersistenceError("execute() used on a statement that returns rows: " +
                               strings::truncate(sql_, 200));
    }
    reset();
}

void Statement::reset() {
    sqlite3_reset(statement_);
}

void Statement::clearBindings() {
    sqlite3_clear_bindings(statement_);
}

int Statement::columnCount() const {
    return sqlite3_column_count(statement_);
}

std::string Statement::columnName(int index) const {
    const char* name = sqlite3_column_name(statement_, index);
    return name != nullptr ? name : "";
}

bool Statement::isNull(int index) const {
    return sqlite3_column_type(statement_, index) == SQLITE_NULL;
}

std::int64_t Statement::getInt(int index) const {
    return sqlite3_column_int64(statement_, index);
}

double Statement::getDouble(int index) const {
    return sqlite3_column_double(statement_, index);
}

std::string Statement::getText(int index) const {
    const auto* text = sqlite3_column_text(statement_, index);
    if (text == nullptr) {
        return {};
    }
    const int size = sqlite3_column_bytes(statement_, index);
    return {reinterpret_cast<const char*>(text), static_cast<std::size_t>(size)};
}

bool Statement::getBool(int index) const {
    return getInt(index) != 0;
}

// ---------------------------------------------------------------------------
// Transaction
// ---------------------------------------------------------------------------

Transaction::Transaction(Connection& connection) : connection_(&connection) {
    connection_->executeScript("BEGIN IMMEDIATE;");
}

Transaction::~Transaction() {
    if (!finished_) {
        // Destructors must not throw; a rollback that fails during unwinding
        // has nowhere useful to report to, and the connection is about to be
        // closed anyway.
        try {
            connection_->executeScript("ROLLBACK;");
        } catch (...) {
        }
    }
}

void Transaction::commit() {
    if (finished_) {
        return;
    }
    finished_ = true;
    connection_->executeScript("COMMIT;");
}

void Transaction::rollback() {
    if (finished_) {
        return;
    }
    finished_ = true;
    connection_->executeScript("ROLLBACK;");
}

}  // namespace testforge::db
