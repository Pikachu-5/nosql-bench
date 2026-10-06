#pragma once

#include "benchforge/config.hpp"
#include "benchforge/runner.hpp"

#include <filesystem>

namespace benchforge {

std::filesystem::path WriteResults(const RunConfig& config, const RunSummary& summary);

} // namespace benchforge
