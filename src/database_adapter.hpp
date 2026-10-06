#pragma once

#include "benchforge/adapter.hpp"

namespace benchforge::detail {

std::unique_ptr<DatabaseAdapter> CreateNetworkAdapter(
    const std::string& name, const RunConfig& config,
    const std::string& key_prefix);
std::unique_ptr<DatabaseAdapter> CreateMongoAdapter(
    const RunConfig& config, const std::string& key_prefix);
std::unique_ptr<DatabaseAdapter> CreateCassandraAdapter(
    const RunConfig& config, const std::string& key_prefix);

} // namespace benchforge::detail
