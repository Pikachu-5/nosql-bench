#pragma once

#include "benchforge/operation.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace benchforge {

enum class Scenario {
    Normal,
    Celebrity
};

enum class RunMode {
    ClosedLoop,
    OpenLoop
};

struct RunConfig {
    std::string adapter{"noop"};
    std::string database_host{"127.0.0.1"};
    std::uint16_t database_port{0};
    RunMode mode{RunMode::ClosedLoop};
    std::uint64_t offered_rate_ops_sec{1000};
    Scenario scenario{Scenario::Normal};
    std::uint64_t seed{42};
    std::uint32_t workers{4};
    std::uint32_t warmup_ms{250};
    std::uint32_t duration_ms{1000};
    std::uint64_t users{10000};
    std::uint64_t posts{100000};
    std::uint64_t follows{200000};
    std::uint64_t hashtags{1000};
    std::uint8_t celebrity_post_percent{10};
    std::string output_dir{"runs"};
    std::string run_id;
    std::array<std::uint8_t, kOperationCount> weights{40, 25, 15, 10, 5, 5};
};

std::uint16_t DefaultDatabasePort(const std::string& adapter) noexcept;

std::string ToString(Scenario scenario);
bool ParseScenario(const std::string& text, Scenario& scenario);
std::string ToString(RunMode mode);
bool ParseRunMode(const std::string& text, RunMode& mode);
RunConfig LoadConfig(const std::filesystem::path& path);
std::vector<std::string> ValidateConfig(const RunConfig& config);

} // namespace benchforge
