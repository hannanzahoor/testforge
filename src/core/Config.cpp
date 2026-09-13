#include "testforge/core/Config.hpp"

#include "testforge/core/Environment.hpp"
#include "testforge/core/Exceptions.hpp"
#include "testforge/core/StringUtils.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <thread>

namespace testforge {
namespace {

const json::Value* section(const json::Value& root, std::string_view name) {
    const json::Value* found = root.find(name);
    return (found != nullptr && found->isObject()) ? found : nullptr;
}

void readBool(const json::Value* obj, std::string_view key, bool& out) {
    if (obj == nullptr) {
        return;
    }
    if (const json::Value* value = obj->find(key); value != nullptr && value->isBool()) {
        out = value->asBool();
    }
}

void readInt(const json::Value* obj, std::string_view key, std::int64_t& out) {
    if (obj == nullptr) {
        return;
    }
    if (const json::Value* value = obj->find(key); value != nullptr && value->isNumber()) {
        out = value->asInt();
    }
}

void readIntNarrow(const json::Value* obj, std::string_view key, int& out) {
    std::int64_t wide = out;
    readInt(obj, key, wide);
    out = static_cast<int>(wide);
}

void readString(const json::Value* obj, std::string_view key, std::string& out) {
    if (obj == nullptr) {
        return;
    }
    if (const json::Value* value = obj->find(key); value != nullptr && value->isString()) {
        out = value->asString();
    }
}

void readStringList(const json::Value* obj, std::string_view key, std::vector<std::string>& out) {
    if (obj == nullptr) {
        return;
    }
    const json::Value* value = obj->find(key);
    if (value == nullptr || !value->isArray()) {
        return;
    }
    out.clear();
    for (const json::Value& item : value->asArray()) {
        if (item.isString()) {
            out.push_back(item.asString());
        }
    }
}

json::Value headersToJson(const std::vector<json::Member>& headers) {
    json::Value out = json::Value::object();
    for (const json::Member& header : headers) {
        out.set(header.first, header.second);
    }
    return out;
}

}  // namespace

Config Config::loadFromFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        throw ConfigurationError("cannot open configuration file: " + path);
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();

    std::string parseError;
    const std::optional<json::Value> parsed = json::tryParse(buffer.str(), &parseError);
    if (!parsed.has_value()) {
        throw ConfigurationError("invalid JSON in " + path + ": " + parseError);
    }
    if (!parsed->isObject()) {
        throw ConfigurationError("configuration root must be a JSON object: " + path);
    }

    Config config = fromJson(*parsed);
    config.sourcePath_ = path;
    return config;
}

Config Config::fromJson(const json::Value& root) {
    Config config;

    if (const json::Value* exec = section(root, "execution"); exec != nullptr) {
        readIntNarrow(exec, "workers", config.execution.workers);
        readInt(exec, "default_timeout_ms", config.execution.defaultTimeoutMs);
        readInt(exec, "cancellation_grace_ms", config.execution.cancellationGraceMs);
        readBool(exec, "fail_fast", config.execution.failFast);
        readIntNarrow(exec, "retry_failed", config.execution.retryFailed);
        readBool(exec, "shuffle", config.execution.shuffle);
        std::int64_t seed = static_cast<std::int64_t>(config.execution.shuffleSeed);
        readInt(exec, "shuffle_seed", seed);
        config.execution.shuffleSeed = static_cast<std::uint64_t>(seed);
        readStringList(exec, "exclude_suites_by_default", config.execution.excludeSuitesByDefault);
    }

    if (const json::Value* log = section(root, "logging"); log != nullptr) {
        std::string level = std::string(toString(config.logging.level));
        readString(log, "level", level);
        if (const std::optional<LogLevel> parsed = logLevelFromString(level); parsed.has_value()) {
            config.logging.level = *parsed;
        }
        readBool(log, "json", config.logging.json);
        readBool(log, "color", config.logging.color);
        readString(log, "file", config.logging.file);
    }

    if (const json::Value* db = section(root, "database"); db != nullptr) {
        readBool(db, "enabled", config.database.enabled);
        readString(db, "path", config.database.path);
        readIntNarrow(db, "retention_days", config.database.retentionDays);
        readInt(db, "busy_timeout_ms", config.database.busyTimeoutMs);
    }

    if (const json::Value* rep = section(root, "reporting"); rep != nullptr) {
        readString(rep, "output_directory", config.reporting.outputDirectory);
        readBool(rep, "console", config.reporting.console);
        readBool(rep, "json", config.reporting.json);
        readBool(rep, "html", config.reporting.html);
        readBool(rep, "junit", config.reporting.junit);
        readBool(rep, "verbose_failures", config.reporting.verboseFailures);
    }

    if (const json::Value* api = section(root, "api"); api != nullptr) {
        readString(api, "base_url", config.api.baseUrl);
        readInt(api, "timeout_ms", config.api.timeoutMs);
        readInt(api, "sla_ms", config.api.slaMs);
        if (const json::Value* headers = api->find("default_headers");
            headers != nullptr && headers->isObject()) {
            config.api.defaultHeaders = headers->asObject();
        }
    }

    if (const json::Value* ai = section(root, "ai"); ai != nullptr) {
        readBool(ai, "enabled", config.ai.enabled);
        readString(ai, "endpoint", config.ai.endpoint);
        readString(ai, "model", config.ai.model);
        readInt(ai, "timeout_ms", config.ai.timeoutMs);
        readIntNarrow(ai, "max_generated_tests", config.ai.maxGeneratedTests);
        readInt(ai, "max_response_bytes", config.ai.maxResponseBytes);
        readStringList(ai, "allowed_target_hosts", config.ai.allowedTargetHosts);
    }

    if (const json::Value* diag = section(root, "diagnostics"); diag != nullptr) {
        readBool(diag, "collect_on_failure", config.diagnostics.collectOnFailure);
        readBool(diag, "include_gpu", config.diagnostics.includeGpu);
        readBool(diag, "include_processes", config.diagnostics.includeProcesses);
        readBool(diag, "include_system_logs", config.diagnostics.includeSystemLogs);
        readIntNarrow(diag, "top_process_count", config.diagnostics.topProcessCount);
        readInt(diag, "command_timeout_ms", config.diagnostics.commandTimeoutMs);
    }

    if (const json::Value* server = section(root, "server"); server != nullptr) {
        readString(server, "host", config.server.host);
        readIntNarrow(server, "port", config.server.port);
        readIntNarrow(server, "workers", config.server.workers);
        readString(server, "static_directory", config.server.staticDirectory);
        readInt(server, "max_request_bytes", config.server.maxRequestBytes);
        readIntNarrow(server, "max_pending_connections", config.server.maxPendingConnections);
    }

    if (const json::Value* custom = root.find("custom"); custom != nullptr) {
        config.custom = *custom;
    }

    return config;
}

void Config::applyEnvironmentOverrides() {
    execution.workers = static_cast<int>(env::getInt("TESTFORGE_WORKERS", execution.workers));
    execution.defaultTimeoutMs = env::getInt("TESTFORGE_TIMEOUT_MS", execution.defaultTimeoutMs);
    execution.failFast = env::getBool("TESTFORGE_FAIL_FAST", execution.failFast);
    execution.retryFailed =
        static_cast<int>(env::getInt("TESTFORGE_RETRY_FAILED", execution.retryFailed));

    if (const std::optional<std::string> level = env::get("TESTFORGE_LOG_LEVEL");
        level.has_value()) {
        if (const std::optional<LogLevel> parsed = logLevelFromString(*level); parsed.has_value()) {
            logging.level = *parsed;
        }
    }
    logging.json = env::getBool("TESTFORGE_LOG_JSON", logging.json);
    logging.file = env::getOr("TESTFORGE_LOG_FILE", logging.file);

    // NO_COLOR is a de facto standard honoured by well-behaved CLIs.
    if (env::isSet("NO_COLOR")) {
        logging.color = false;
    }

    database.enabled = env::getBool("TESTFORGE_DB_ENABLED", database.enabled);
    database.path = env::getOr("TESTFORGE_DB_PATH", database.path);

    reporting.outputDirectory = env::getOr("TESTFORGE_REPORT_DIR", reporting.outputDirectory);

    api.baseUrl = env::getOr("TESTFORGE_API_BASE_URL", api.baseUrl);
    api.timeoutMs = env::getInt("TESTFORGE_API_TIMEOUT_MS", api.timeoutMs);

    ai.enabled = env::getBool("TESTFORGE_AI_ENABLED", ai.enabled);
    ai.endpoint = env::getOr("TESTFORGE_AI_ENDPOINT", ai.endpoint);
    ai.model = env::getOr("TESTFORGE_AI_MODEL", ai.model);
    ai.timeoutMs = env::getInt("TESTFORGE_AI_TIMEOUT_MS", ai.timeoutMs);

    diagnostics.collectOnFailure =
        env::getBool("TESTFORGE_DIAG_ON_FAILURE", diagnostics.collectOnFailure);
    diagnostics.includeGpu = env::getBool("TESTFORGE_DIAG_GPU", diagnostics.includeGpu);

    server.host = env::getOr("TESTFORGE_SERVER_HOST", server.host);
    server.port = static_cast<int>(env::getInt("TESTFORGE_SERVER_PORT", server.port));
}

void Config::validate() const {
    std::vector<std::string> problems;

    if (execution.workers < 0 || execution.workers > 512) {
        problems.emplace_back("execution.workers must be between 0 and 512");
    }
    if (execution.defaultTimeoutMs < 0) {
        problems.emplace_back("execution.default_timeout_ms must not be negative");
    }
    if (execution.retryFailed < 0 || execution.retryFailed > 10) {
        problems.emplace_back("execution.retry_failed must be between 0 and 10");
    }
    if (database.enabled && database.path.empty()) {
        problems.emplace_back("database.path must not be empty when the database is enabled");
    }
    if (database.retentionDays < 0) {
        problems.emplace_back("database.retention_days must not be negative");
    }
    if (api.timeoutMs <= 0) {
        problems.emplace_back("api.timeout_ms must be positive");
    }
    if (!api.baseUrl.empty() && !strings::startsWith(api.baseUrl, "http://") &&
        !strings::startsWith(api.baseUrl, "https://")) {
        problems.emplace_back("api.base_url must start with http:// or https://");
    }
    if (ai.enabled && ai.endpoint.empty()) {
        problems.emplace_back("ai.endpoint must be set when ai.enabled is true");
    }
    if (ai.maxGeneratedTests <= 0 || ai.maxGeneratedTests > 500) {
        problems.emplace_back("ai.max_generated_tests must be between 1 and 500");
    }
    if (server.port < 1 || server.port > 65535) {
        problems.emplace_back("server.port must be between 1 and 65535");
    }
    if (server.workers < 1 || server.workers > 128) {
        problems.emplace_back("server.workers must be between 1 and 128");
    }
    if (server.maxPendingConnections < 1) {
        problems.emplace_back("server.max_pending_connections must be at least 1");
    }

    if (!problems.empty()) {
        std::ostringstream os;
        os << "invalid configuration";
        if (!sourcePath_.empty()) {
            os << " (" << sourcePath_ << ")";
        }
        os << ":";
        for (const std::string& problem : problems) {
            os << "\n  - " << problem;
        }
        throw ConfigurationError(os.str());
    }
}

int Config::effectiveWorkers() const {
    if (execution.workers > 0) {
        return execution.workers;
    }
    const unsigned hardware = std::thread::hardware_concurrency();
    // hardware_concurrency() is allowed to return 0 when it cannot tell.
    return hardware == 0 ? 4 : static_cast<int>(std::min(hardware, 32u));
}

json::Value Config::toJson() const {
    json::Value root = json::Value::object();

    json::Value exec = json::Value::object();
    exec.set("workers", execution.workers);
    exec.set("effective_workers", effectiveWorkers());
    exec.set("default_timeout_ms", execution.defaultTimeoutMs);
    exec.set("cancellation_grace_ms", execution.cancellationGraceMs);
    exec.set("fail_fast", execution.failFast);
    exec.set("retry_failed", execution.retryFailed);
    exec.set("shuffle", execution.shuffle);
    exec.set("shuffle_seed", static_cast<std::int64_t>(execution.shuffleSeed));
    json::Value excluded = json::Value::array();
    for (const std::string& suite : execution.excludeSuitesByDefault) {
        excluded.push(suite);
    }
    exec.set("exclude_suites_by_default", excluded);
    root.set("execution", exec);

    json::Value log = json::Value::object();
    log.set("level", std::string(toString(logging.level)));
    log.set("json", logging.json);
    log.set("color", logging.color);
    log.set("file", logging.file);
    root.set("logging", log);

    json::Value db = json::Value::object();
    db.set("enabled", database.enabled);
    db.set("path", database.path);
    db.set("retention_days", database.retentionDays);
    db.set("busy_timeout_ms", database.busyTimeoutMs);
    root.set("database", db);

    json::Value rep = json::Value::object();
    rep.set("output_directory", reporting.outputDirectory);
    rep.set("console", reporting.console);
    rep.set("json", reporting.json);
    rep.set("html", reporting.html);
    rep.set("junit", reporting.junit);
    rep.set("verbose_failures", reporting.verboseFailures);
    root.set("reporting", rep);

    json::Value apiJson = json::Value::object();
    apiJson.set("base_url", api.baseUrl);
    apiJson.set("timeout_ms", api.timeoutMs);
    apiJson.set("sla_ms", api.slaMs);
    apiJson.set("default_headers", headersToJson(api.defaultHeaders));
    root.set("api", apiJson);

    json::Value aiJson = json::Value::object();
    aiJson.set("enabled", ai.enabled);
    aiJson.set("endpoint", ai.endpoint);
    aiJson.set("model", ai.model);
    aiJson.set("timeout_ms", ai.timeoutMs);
    aiJson.set("max_generated_tests", ai.maxGeneratedTests);
    aiJson.set("max_response_bytes", ai.maxResponseBytes);
    json::Value hosts = json::Value::array();
    for (const std::string& host : ai.allowedTargetHosts) {
        hosts.push(host);
    }
    aiJson.set("allowed_target_hosts", hosts);
    root.set("ai", aiJson);

    json::Value diag = json::Value::object();
    diag.set("collect_on_failure", diagnostics.collectOnFailure);
    diag.set("include_gpu", diagnostics.includeGpu);
    diag.set("include_processes", diagnostics.includeProcesses);
    diag.set("include_system_logs", diagnostics.includeSystemLogs);
    diag.set("top_process_count", diagnostics.topProcessCount);
    diag.set("command_timeout_ms", diagnostics.commandTimeoutMs);
    root.set("diagnostics", diag);

    json::Value srv = json::Value::object();
    srv.set("host", server.host);
    srv.set("port", server.port);
    srv.set("workers", server.workers);
    srv.set("static_directory", server.staticDirectory);
    srv.set("max_request_bytes", server.maxRequestBytes);
    srv.set("max_pending_connections", server.maxPendingConnections);
    root.set("server", srv);

    root.set("custom", custom);
    if (!sourcePath_.empty()) {
        root.set("source_path", sourcePath_);
    }
    return root;
}

}  // namespace testforge
