#include "benchforge/config.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace benchforge {
namespace {

std::string_view Trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1U);
}

template <typename T>
T ParseInteger(std::string_view text, const std::string& key, std::size_t line) {
    T value{};
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        throw std::runtime_error("config line " + std::to_string(line) +
                                 ": invalid integer for " + key);
    }
    return value;
}

void AssignInteger(const std::string& key, std::string_view value,
                   std::size_t line, RunConfig& config) {
    if (key == "seed") {
        config.seed = ParseInteger<std::uint64_t>(value, key, line);
    } else if (key == "workers") {
        config.workers = ParseInteger<std::uint32_t>(value, key, line);
    } else if (key == "warmup_ms") {
        config.warmup_ms = ParseInteger<std::uint32_t>(value, key, line);
    } else if (key == "duration_ms") {
        config.duration_ms = ParseInteger<std::uint32_t>(value, key, line);
    } else if (key == "users") {
        config.users = ParseInteger<std::uint64_t>(value, key, line);
    } else if (key == "posts") {
        config.posts = ParseInteger<std::uint64_t>(value, key, line);
    } else if (key == "follows") {
        config.follows = ParseInteger<std::uint64_t>(value, key, line);
    } else if (key == "hashtags") {
        config.hashtags = ParseInteger<std::uint64_t>(value, key, line);
    } else if (key == "celebrity_post_percent") {
        config.celebrity_post_percent = ParseInteger<std::uint8_t>(value, key, line);
    } else {
        OperationType type{};
        if (key.rfind("weight.", 0) == 0 &&
            ParseOperationType(std::string_view(key).substr(7), type)) {
            config.weights[static_cast<std::size_t>(type)] =
                ParseInteger<std::uint8_t>(value, key, line);
            return;
        }
        throw std::runtime_error("config line " + std::to_string(line) +
                                 ": unknown key " + key);
    }
}

} // namespace

std::string ToString(Scenario scenario) {
    switch (scenario) {
    case Scenario::Normal:
        return "normal";
    case Scenario::Celebrity:
        return "celebrity";
    }
    return "unknown";
}

bool ParseScenario(const std::string& text, Scenario& scenario) {
    if (text == "normal") {
        scenario = Scenario::Normal;
        return true;
    }
    if (text == "celebrity") {
        scenario = Scenario::Celebrity;
        return true;
    }
    return false;
}

std::string ToString(RunMode mode) {
    switch (mode) {
    case RunMode::ClosedLoop: return "closed_loop";
    case RunMode::OpenLoop: return "open_loop";
    }
    return "unknown";
}

bool ParseRunMode(const std::string& text, RunMode& mode) {
    if (text == "closed_loop") {
        mode = RunMode::ClosedLoop;
        return true;
    }
    if (text == "open_loop") {
        mode = RunMode::OpenLoop;
        return true;
    }
    return false;
}

RunConfig LoadConfig(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("could not open config file: " + path.string());
    }

    RunConfig config;
    std::set<std::string> seen_keys;
    std::string raw_line;
    std::size_t line_number = 0;
    while (std::getline(input, raw_line)) {
        ++line_number;
        auto line = Trim(raw_line);
        const auto comment = line.find('#');
        if (comment != std::string_view::npos) {
            line = Trim(line.substr(0, comment));
        }
        if (line.empty()) {
            continue;
        }

        const auto equals = line.find('=');
        if (equals == std::string_view::npos) {
            throw std::runtime_error("config line " + std::to_string(line_number) +
                                     ": expected key = value");
        }
        const std::string key(Trim(line.substr(0, equals)));
        const auto value = Trim(line.substr(equals + 1U));
        if (key.empty() || value.empty()) {
            throw std::runtime_error("config line " + std::to_string(line_number) +
                                     ": key and value must be non-empty");
        }
        if (!seen_keys.insert(key).second) {
            throw std::runtime_error("config line " + std::to_string(line_number) +
                                     ": duplicate key " + key);
        }

        if (key == "adapter") {
            config.adapter = std::string(value);
        } else if (key == "database_host") {
            config.database_host = std::string(value);
        } else if (key == "database_port") {
            config.database_port =
                ParseInteger<std::uint16_t>(value, key, line_number);
        } else if (key == "mode") {
            if (!ParseRunMode(std::string(value), config.mode)) {
                throw std::runtime_error("config line " + std::to_string(line_number) +
                                         ": mode must be closed_loop or open_loop");
            }
        } else if (key == "offered_rate_ops_sec") {
            config.offered_rate_ops_sec =
                ParseInteger<std::uint64_t>(value, key, line_number);
        } else if (key == "scenario") {
            if (!ParseScenario(std::string(value), config.scenario)) {
                throw std::runtime_error("config line " + std::to_string(line_number) +
                                         ": scenario must be normal or celebrity");
            }
        } else if (key == "output_dir") {
            config.output_dir = std::string(value);
        } else {
            AssignInteger(key, value, line_number, config);
        }
    }
    if (!input.eof()) {
        throw std::runtime_error("error reading config file: " + path.string());
    }
    return config;
}

std::vector<std::string> ValidateConfig(const RunConfig& config) {
    std::vector<std::string> errors;
    if (config.adapter.empty()) {
        errors.emplace_back("adapter must not be empty");
    }
    if (config.database_host != "127.0.0.1" &&
        config.database_host != "localhost") {
        errors.emplace_back("database_host must be localhost or 127.0.0.1");
    }
    if (config.workers == 0 || config.workers > 256) {
        errors.emplace_back("workers must be between 1 and 256");
    }
    if (config.duration_ms == 0 || config.duration_ms > 600000) {
        errors.emplace_back("duration_ms must be between 1 and 600000");
    }
    if (config.offered_rate_ops_sec == 0 ||
        config.offered_rate_ops_sec > 1000000) {
        errors.emplace_back("offered_rate_ops_sec must be between 1 and 1000000");
    }
    if (config.warmup_ms > 300000) {
        errors.emplace_back("warmup_ms must not exceed 300000");
    }
    if (config.users == 0 || config.users > 10000000) {
        errors.emplace_back("users must be between 1 and 10000000");
    }
    if (config.posts == 0 || config.posts > 100000000) {
        errors.emplace_back("posts must be between 1 and 100000000");
    }
    if (config.follows > 1000000000) {
        errors.emplace_back("follows must not exceed 1000000000");
    }
    if (config.users != 0 && config.follows > config.users * (config.users - 1U)) {
        errors.emplace_back("follows must not exceed the number of unique directed user pairs");
    }
    if (config.hashtags == 0 || config.hashtags > 1000000) {
        errors.emplace_back("hashtags must be between 1 and 1000000");
    }
    if (config.celebrity_post_percent > 100) {
        errors.emplace_back("celebrity_post_percent must be between 0 and 100");
    }
    if (config.output_dir.empty()) {
        errors.emplace_back("output_dir must not be empty");
    }
    if (config.run_id.size() > 64 ||
        !std::all_of(config.run_id.begin(), config.run_id.end(), [](unsigned char c) {
            return std::isalnum(c) != 0 || c == '_' || c == '-';
        })) {
        errors.emplace_back("run_id may contain only letters, numbers, '_' and '-' (max 64)");
    }

    std::uint32_t weight_total = 0;
    for (const auto weight : config.weights) {
        weight_total += weight;
    }
    if (weight_total != 100) {
        errors.emplace_back("operation weights must sum to 100");
    }
    if (config.users < 2 &&
        config.weights[static_cast<std::size_t>(OperationType::UserFollow)] != 0) {
        errors.emplace_back("user_follow requires at least two users");
    }
    return errors;
}

std::uint16_t DefaultDatabasePort(const std::string& adapter) noexcept {
    if (adapter == "neo4j") return 7474;
    if (adapter == "redis") {
        return 6379;
    }
    if (adapter == "feedkv") {
        return 6380;
    }
    if (adapter == "mongo") {
        return 27017;
    }
    if (adapter == "cassandra") {
        return 9042;
    }
    return 0;
}

} // namespace benchforge
