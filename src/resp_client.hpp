#pragma once

#include "benchforge/adapter.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace benchforge::detail {

struct RespValue {
    enum class Kind { Simple, Error, Integer, Bulk, Array, Null } kind{Kind::Null};
    std::string text;
    std::int64_t integer{0};
    std::vector<RespValue> elements;
};

using RespCommand = std::vector<std::string>;

class RespClient {
public:
    RespClient(const std::string& host, std::uint16_t port);
    ~RespClient();

    RespClient(const RespClient&) = delete;
    RespClient& operator=(const RespClient&) = delete;
    RespClient(RespClient&& other) noexcept;
    RespClient& operator=(RespClient&& other) noexcept;

    RespValue Command(const RespCommand& command);
    std::vector<RespValue> Pipeline(const std::vector<RespCommand>& commands);

private:
    std::intptr_t socket_{-1};
    void Send(const std::string& bytes);
    RespValue ReadValue();
    std::string ReadLine();
    std::string ReadExact(std::size_t length);
    std::string input_;
};

std::string EncodeCommand(const RespCommand& command);
std::string RequireText(const RespValue& value, const std::string& context);
std::int64_t RequireInteger(const RespValue& value, const std::string& context);
void RequireArray(const RespValue& value, const std::string& context);

} // namespace benchforge::detail
