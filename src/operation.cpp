#include "benchforge/operation.hpp"

namespace benchforge {

std::string_view ToString(OperationType type) noexcept {
    switch (type) {
    case OperationType::TimelineRead:
        return "timeline_read";
    case OperationType::UserProfileRead:
        return "user_profile_read";
    case OperationType::PostLike:
        return "post_like";
    case OperationType::PostCreate:
        return "post_create";
    case OperationType::HashtagSearch:
        return "hashtag_search";
    case OperationType::UserFollow:
        return "user_follow";
    case OperationType::Count:
        break;
    }
    return "unknown";
}

bool ParseOperationType(std::string_view text, OperationType& result) noexcept {
    for (const auto type : kOperationTypes) {
        if (text == ToString(type)) {
            result = type;
            return true;
        }
    }
    return false;
}

} // namespace benchforge
