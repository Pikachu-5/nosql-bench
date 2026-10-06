#pragma once

#include <cstdint>
#include <filesystem>

namespace benchforge {

int RunControlApi(const std::filesystem::path& executable_path,
                  std::uint16_t port);
int RunFeedKvServer(std::uint16_t port);

} // namespace benchforge
