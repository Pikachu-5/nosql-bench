#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace benchforge {

enum class OperationType : std::uint8_t {
    TimelineRead = 0,
    UserProfileRead,
    PostLike,
    PostCreate,
    HashtagSearch,
    UserFollow,
    Count
};

inline constexpr std::size_t kOperationCount =
    static_cast<std::size_t>(OperationType::Count);

inline constexpr std::array<OperationType, kOperationCount> kOperationTypes{
    OperationType::TimelineRead,
    OperationType::UserProfileRead,
    OperationType::PostLike,
    OperationType::PostCreate,
    OperationType::HashtagSearch,
    OperationType::UserFollow
};

struct Operation {
    OperationType type{OperationType::TimelineRead};
    std::uint64_t sequence{0};
    std::uint64_t user_id{0};
    std::uint64_t target_user_id{0};
    std::uint64_t post_id{0};
    std::uint64_t created_at_ms{0};
    std::uint32_t hashtag_id{0};
};

std::string_view ToString(OperationType type) noexcept;
bool ParseOperationType(std::string_view text, OperationType& result) noexcept;

} // namespace benchforge
