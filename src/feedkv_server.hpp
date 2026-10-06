#pragma once

#include <cstdint>

namespace benchforge {

int RunFeedKvServerImpl(std::uint16_t port, bool container_listen = false);

} // namespace benchforge
