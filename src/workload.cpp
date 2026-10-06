#include "benchforge/workload.hpp"

#include <stdexcept>

namespace benchforge {

WorkloadGenerator::WorkloadGenerator(const RunConfig& config,
                                     std::uint64_t worker_seed,
                                     std::uint32_t worker_index,
                                     std::uint32_t worker_count)
    : config_(config), random_(worker_seed), worker_index_(worker_index),
      worker_count_(worker_count == 0 ? 1 : worker_count) {}

std::uint64_t WorkloadGenerator::RandomBelow(std::uint64_t exclusive_upper_bound) {
    if (exclusive_upper_bound == 0) {
        throw std::logic_error("cannot generate an ID from an empty dataset");
    }
    return random_() % exclusive_upper_bound;
}

OperationType WorkloadGenerator::ChooseOperation() {
    const auto ticket = static_cast<std::uint32_t>(random_() % 100U);
    std::uint32_t cumulative = 0;
    for (const auto type : kOperationTypes) {
        cumulative += config_.weights[static_cast<std::size_t>(type)];
        if (ticket < cumulative) {
            return type;
        }
    }
    return kOperationTypes.back();
}

Operation WorkloadGenerator::Next() {
    Operation operation;
    operation.type = ChooseOperation();
    operation.sequence = sequence_;
    operation.user_id = RandomBelow(config_.users);
    operation.target_user_id = RandomBelow(config_.users);
    if (operation.type == OperationType::UserFollow && config_.users > 1 &&
        operation.target_user_id == operation.user_id) {
        operation.target_user_id =
            (operation.target_user_id + 1U) % config_.users;
    }

    if (operation.type == OperationType::PostCreate) {
        if (config_.scenario == Scenario::Celebrity &&
            random_() % 100U < config_.celebrity_post_percent) {
            operation.user_id = 0;
        }
        const auto unique_sequence = sequence_ * worker_count_ + worker_index_;
        operation.post_id = config_.posts + unique_sequence;
        operation.created_at_ms = operation.post_id + 1U;
    } else {
        operation.post_id = RandomBelow(config_.posts);
        operation.created_at_ms = operation.post_id + 1U;
    }
    operation.hashtag_id =
        static_cast<std::uint32_t>(RandomBelow(config_.hashtags));
    ++sequence_;
    return operation;
}

} // namespace benchforge
