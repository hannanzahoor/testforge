#pragma once

#include <string>
#include <vector>

namespace testforge::schema {

/// One forward-only schema step.
struct Migration {
    int version = 0;
    std::string description;
    std::string sql;
};

/// The version a freshly created database ends up at.
int currentVersion();

/// Every migration, in ascending version order.
///
/// Migrations are append-only: shipped SQL is never edited, because a database
/// that already applied it will never see the change.
std::vector<Migration> migrations();

}  // namespace testforge::schema
