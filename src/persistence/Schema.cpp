// Database schema and migrations.
//
// Migrations are applied by version number, tracked in the `schema_version`
// table. Each migration is append-only: once a version ships, its SQL is never
// edited, because an existing database will never re-run it.

#include "testforge/persistence/Schema.hpp"

#include <array>

namespace testforge::schema {

int currentVersion() {
    return 1;
}

std::vector<Migration> migrations() {
    std::vector<Migration> list;

    list.push_back({1,
                    "initial schema",
                    R"SQL(
CREATE TABLE IF NOT EXISTS test_runs (
    run_id            TEXT PRIMARY KEY,
    label             TEXT NOT NULL DEFAULT '',
    started_at_ms     INTEGER NOT NULL,
    finished_at_ms    INTEGER,
    duration_ms       INTEGER NOT NULL DEFAULT 0,
    workers           INTEGER NOT NULL DEFAULT 1,
    filter            TEXT NOT NULL DEFAULT '',
    git_commit        TEXT NOT NULL DEFAULT '',
    hostname          TEXT NOT NULL DEFAULT '',
    environment_json  TEXT NOT NULL DEFAULT '{}',
    total             INTEGER NOT NULL DEFAULT 0,
    passed            INTEGER NOT NULL DEFAULT 0,
    failed            INTEGER NOT NULL DEFAULT 0,
    skipped           INTEGER NOT NULL DEFAULT 0,
    errors            INTEGER NOT NULL DEFAULT 0,
    timeouts          INTEGER NOT NULL DEFAULT 0,
    completed         INTEGER NOT NULL DEFAULT 0
);

-- Listing runs newest-first is the single most common query in the whole
-- product (CLI history, dashboard, REST). Worth its own index.
CREATE INDEX IF NOT EXISTS idx_test_runs_started
    ON test_runs (started_at_ms DESC);

CREATE TABLE IF NOT EXISTS test_results (
    id                INTEGER PRIMARY KEY AUTOINCREMENT,
    run_id            TEXT NOT NULL,
    test_id           TEXT NOT NULL,
    test_name         TEXT NOT NULL,
    suite_name        TEXT NOT NULL,
    qualified_name    TEXT NOT NULL,
    status            TEXT NOT NULL,
    failure_category  TEXT NOT NULL DEFAULT 'NONE',
    start_time_ms     INTEGER NOT NULL,
    end_time_ms       INTEGER NOT NULL,
    duration_ms       INTEGER NOT NULL DEFAULT 0,
    error_message     TEXT NOT NULL DEFAULT '',
    error_detail      TEXT NOT NULL DEFAULT '',
    logs_json         TEXT NOT NULL DEFAULT '[]',
    tags_json         TEXT NOT NULL DEFAULT '[]',
    metadata_json     TEXT NOT NULL DEFAULT '{}',
    diagnostics_json  TEXT NOT NULL DEFAULT '{}',
    attempt           INTEGER NOT NULL DEFAULT 1,
    flaky_candidate   INTEGER NOT NULL DEFAULT 0,
    worker            TEXT NOT NULL DEFAULT '',
    FOREIGN KEY (run_id) REFERENCES test_runs (run_id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_test_results_run
    ON test_results (run_id);

-- Per-test history: "show me every time api.health_check ran", ordered.
CREATE INDEX IF NOT EXISTS idx_test_results_test
    ON test_results (test_id, start_time_ms DESC);

CREATE INDEX IF NOT EXISTS idx_test_results_status
    ON test_results (status);

CREATE INDEX IF NOT EXISTS idx_test_results_category
    ON test_results (failure_category);

-- Free-form timeline: diagnostics collected, AI analysis requested, retries.
CREATE TABLE IF NOT EXISTS test_events (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    run_id       TEXT NOT NULL,
    created_ms   INTEGER NOT NULL,
    event_type   TEXT NOT NULL,
    payload_json TEXT NOT NULL DEFAULT '{}',
    FOREIGN KEY (run_id) REFERENCES test_runs (run_id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_test_events_run
    ON test_events (run_id, created_ms);
)SQL"});

    return list;
}

}  // namespace testforge::schema
