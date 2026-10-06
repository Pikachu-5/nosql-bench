#pragma once

#include <cstdint>
#include <string>

namespace benchforge {

struct EnvironmentInfo {
    std::string host_name;
    std::string operating_system;
    std::string architecture;
    std::uint32_t logical_processors{0};
    std::uint64_t total_memory_bytes{0};
};

EnvironmentInfo CaptureEnvironment();

} // namespace benchforge
