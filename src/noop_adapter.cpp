#include "benchforge/adapter.hpp"
#include "database_adapter.hpp"

#include <stdexcept>

namespace benchforge {
namespace {

class NoopAdapter final : public DatabaseAdapter {
public:
    std::string Name() const override {
        return "noop";
    }

    std::string Version() const override {
        return "BenchForge noop 0.1";
    }

    std::string StorageConfiguration() const override {
        return "No database; harness-only calibration";
    }

    std::string Endpoint() const override {
        return "none";
    }

    void LoadDataset(const RunConfig& config) override {
        (void)config;
    }

    TransportCalibration Calibrate() override { return {}; }

    void Execute(const Operation& operation) override {
        (void)operation;
    }

    std::string Cleanup() noexcept override { return "not applicable"; }
};

} // namespace

std::unique_ptr<DatabaseAdapter> CreateAdapter(const std::string& name,
                                               const RunConfig& config,
                                               const std::string& key_prefix) {
    (void)config;
    (void)key_prefix;
    if (name == "noop") {
        return std::make_unique<NoopAdapter>();
    }
    if (name == "feedkv" || name == "redis") {
        return detail::CreateNetworkAdapter(name, config, key_prefix);
    }
    if (name == "mongo") {
        return detail::CreateMongoAdapter(config, key_prefix);
    }
    if (name == "cassandra") {
        return detail::CreateCassandraAdapter(config, key_prefix);
    }
    if (name == "neo4j") {
        return detail::CreateNeo4jAdapter(config, key_prefix);
    }
    throw std::invalid_argument("adapter is not available: " + name);
}

std::vector<std::string> AvailableAdapters() {
    return {"cassandra", "feedkv", "mongo", "neo4j", "noop", "redis"};
}

} // namespace benchforge
