#pragma once

#include "benchforge/adapter.hpp"
#include "benchforge/config.hpp"
#include "benchforge/environment.hpp"
#include "benchforge/metrics.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace benchforge {

struct RunSummary {
    std::string run_id;
    std::string started_at_utc;
    std::string adapter;
    std::string adapter_version;
    std::string endpoint;
    std::string storage_configuration;
    std::string cleanup_status;
    std::string mode;
    std::uint64_t offered_rate_ops_sec{0};
    TransportCalibration transport_calibration;
    EnvironmentInfo environment;
    bool valid{true};
    std::vector<std::string> invalid_reasons;
    std::uint64_t telemetry_dropped{0};
    Scenario scenario{Scenario::Normal};
    std::uint64_t seed{0};
    std::uint32_t workers{0};
    std::uint64_t warmup_ms{0};
    std::uint64_t measured_ms{0};
    std::array<OperationMetrics, kOperationCount> operations;

    std::uint64_t TotalOperations() const noexcept;
    std::uint64_t TotalErrors() const noexcept;
    std::uint64_t TotalTimeouts() const noexcept;
};

RunSummary RunBenchmark(const RunConfig& config,
                        std::function<bool()> cancelled = {},
                        std::function<void()> acquired = {});

} // namespace benchforge
