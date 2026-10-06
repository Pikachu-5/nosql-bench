#pragma once

#include "benchforge/config.hpp"
#include "benchforge/operation.hpp"

#include <cstdint>
#include <random>

namespace benchforge {

class WorkloadGenerator {
public:
    WorkloadGenerator(const RunConfig& config, std::uint64_t worker_seed,
                      std::uint32_t worker_index = 0,
                      std::uint32_t worker_count = 1);

    Operation Next();

private:
    std::uint64_t RandomBelow(std::uint64_t exclusive_upper_bound);
    OperationType ChooseOperation();

    RunConfig config_;
    std::mt19937_64 random_;
    std::uint64_t sequence_{0};
    std::uint32_t worker_index_{0};
    std::uint32_t worker_count_{1};
};

} // namespace benchforge
