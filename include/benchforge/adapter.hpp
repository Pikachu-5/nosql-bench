#pragma once

#include "benchforge/operation.hpp"
#include "benchforge/config.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace benchforge {

class AdapterTimeout : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct TransportCalibration {
    std::string kind{"not applicable"};
    std::uint64_t samples{0};
    std::uint64_t p50_ns{0};
    std::uint64_t p95_ns{0};
    std::uint64_t mean_ns{0};
};

class DatabaseAdapter {
public:
    virtual ~DatabaseAdapter() = default;

    virtual std::string Name() const = 0;
    virtual std::string Version() const = 0;
    virtual std::string StorageConfiguration() const = 0;
    virtual std::string Endpoint() const = 0;
    virtual void LoadDataset(const RunConfig& config) = 0;
    virtual void PrepareWorker() {}
    virtual TransportCalibration Calibrate() = 0;
    virtual void Execute(const Operation& operation) = 0;
    virtual std::string Cleanup() noexcept = 0;
};

std::unique_ptr<DatabaseAdapter> CreateAdapter(const std::string& name,
                                               const RunConfig& config,
                                               const std::string& key_prefix);
std::vector<std::string> AvailableAdapters();

} // namespace benchforge
