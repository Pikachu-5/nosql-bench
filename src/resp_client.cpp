#include "resp_client.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace benchforge::detail {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
NativeSocket ToNative(std::intptr_t value) {
    return static_cast<NativeSocket>(value);
}
std::intptr_t ToStored(NativeSocket value) {
    return static_cast<std::intptr_t>(value);
}
void CloseSocket(NativeSocket value) {
    if (value != kInvalidSocket) closesocket(value);
}
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
NativeSocket ToNative(std::intptr_t value) { return static_cast<int>(value); }
std::intptr_t ToStored(NativeSocket value) { return value; }
void CloseSocket(NativeSocket value) {
    if (value != kInvalidSocket) close(value);
}
#endif

[[noreturn]] void ThrowSocketFailure(const char* context) {
#ifdef _WIN32
    if (WSAGetLastError() == WSAETIMEDOUT) {
        throw AdapterTimeout(std::string("database socket ") + context + " timed out");
    }
#else
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        throw AdapterTimeout(std::string("database socket ") + context + " timed out");
    }
#endif
    throw std::runtime_error(std::string("database socket ") + context + " failed");
}

void EnsureNetworkRuntime() {
#ifdef _WIN32
    static std::once_flag initialized;
    static int startup_result = 0;
    std::call_once(initialized, [] {
        WSADATA data{};
        startup_result = WSAStartup(MAKEWORD(2, 2), &data);
    });
    if (startup_result != 0) {
        throw std::runtime_error("could not initialize Winsock");
    }
#endif
}

std::int64_t ParseSigned(const std::string& text, const char* label) {
    std::int64_t value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        throw std::runtime_error(std::string("invalid RESP ") + label);
    }
    return value;
}

} // namespace

std::string EncodeCommand(const RespCommand& command) {
    if (command.empty() || command.size() > 128) {
        throw std::invalid_argument("RESP command must contain 1 to 128 arguments");
    }
    std::string output = "*" + std::to_string(command.size()) + "\r\n";
    for (const auto& argument : command) {
        output += "$" + std::to_string(argument.size()) + "\r\n";
        output.append(argument.data(), argument.size());
        output += "\r\n";
    }
    return output;
}

RespClient::RespClient(const std::string& host, std::uint16_t port) {
    EnsureNetworkRuntime();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    const auto service = std::to_string(port);
    const auto status = getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses);
    if (status != 0) {
#ifdef _WIN32
        throw std::runtime_error("could not resolve database host: " + host);
#else
        throw std::runtime_error("could not resolve database host " + host +
                                 ": " + gai_strerror(status));
#endif
    }

    NativeSocket connected = kInvalidSocket;
    for (auto* address = addresses; address != nullptr; address = address->ai_next) {
        const auto candidate = socket(address->ai_family, address->ai_socktype,
                                      address->ai_protocol);
        if (candidate == kInvalidSocket) continue;
        const int nodelay = 1;
        if (setsockopt(candidate, IPPROTO_TCP, TCP_NODELAY,
            reinterpret_cast<const char*>(&nodelay), sizeof(nodelay)) != 0) {
            CloseSocket(candidate); continue;
        }
#ifdef _WIN32
        const DWORD timeout_ms = 10000;
        setsockopt(candidate, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
        setsockopt(candidate, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
        const timeval timeout{10, 0};
        setsockopt(candidate, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(candidate, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
        if (connect(candidate, address->ai_addr,
                    static_cast<int>(address->ai_addrlen)) == 0) {
            connected = candidate;
            break;
        }
        CloseSocket(candidate);
    }
    freeaddrinfo(addresses);
    if (connected == kInvalidSocket) {
        throw std::runtime_error("could not connect to database at " + host + ":" +
                                 service);
    }
    socket_ = ToStored(connected);
}

RespClient::~RespClient() {
    CloseSocket(ToNative(socket_));
}

RespClient::RespClient(RespClient&& other) noexcept
    : socket_(other.socket_), input_(std::move(other.input_)) {
    other.socket_ = -1;
}

RespClient& RespClient::operator=(RespClient&& other) noexcept {
    if (this != &other) {
        CloseSocket(ToNative(socket_));
        socket_ = other.socket_;
        input_ = std::move(other.input_);
        other.socket_ = -1;
    }
    return *this;
}

void RespClient::Send(const std::string& bytes) {
    const auto socket_value = ToNative(socket_);
    std::size_t sent = 0;
    while (sent < bytes.size()) {
#ifdef _WIN32
        const auto remaining = static_cast<int>(std::min<std::size_t>(
            bytes.size() - sent, static_cast<std::size_t>(std::numeric_limits<int>::max())));
        const auto count = send(socket_value, bytes.data() + sent, remaining, 0);
#else
        const auto count = send(socket_value, bytes.data() + sent,
                                bytes.size() - sent, 0);
#endif
        if (count < 0) ThrowSocketFailure("write");
        if (count == 0) throw std::runtime_error("database socket write closed");
        sent += static_cast<std::size_t>(count);
    }
}

std::string RespClient::ReadLine() {
    while (true) {
        const auto end = input_.find("\r\n");
        if (end != std::string::npos) {
            auto line = input_.substr(0, end);
            input_.erase(0, end + 2);
            return line;
        }
        if (input_.size() > 1024U * 1024U) {
            throw std::runtime_error("RESP line exceeds 1 MiB");
        }
        char buffer[8192];
#ifdef _WIN32
        const auto count = recv(ToNative(socket_), buffer, sizeof(buffer), 0);
#else
        const auto count = recv(ToNative(socket_), buffer, sizeof(buffer), 0);
#endif
        if (count < 0) ThrowSocketFailure("read");
        if (count == 0) throw std::runtime_error("database socket closed");
        input_.append(buffer, static_cast<std::size_t>(count));
    }
}

std::string RespClient::ReadExact(std::size_t length) {
    if (length > 64U * 1024U * 1024U) {
        throw std::runtime_error("RESP bulk value exceeds 64 MiB");
    }
    while (input_.size() < length + 2U) {
        char buffer[8192];
        const auto count = recv(ToNative(socket_), buffer, sizeof(buffer), 0);
        if (count < 0) ThrowSocketFailure("read");
        if (count == 0) throw std::runtime_error("database socket closed");
        input_.append(buffer, static_cast<std::size_t>(count));
    }
    if (input_[length] != '\r' || input_[length + 1U] != '\n') {
        throw std::runtime_error("malformed RESP bulk value terminator");
    }
    auto value = input_.substr(0, length);
    input_.erase(0, length + 2U);
    return value;
}

RespValue RespClient::ReadValue() {
    std::function<RespValue(unsigned)> read = [&](unsigned depth) -> RespValue {
        if (depth > 16U) throw std::runtime_error("RESP nesting is too deep");
        while (input_.empty()) {
            char buffer[8192];
            const auto count = recv(ToNative(socket_), buffer, sizeof(buffer), 0);
            if (count < 0) ThrowSocketFailure("read");
            if (count == 0) throw std::runtime_error("database socket closed");
            input_.append(buffer, static_cast<std::size_t>(count));
        }
        const auto type = input_.front();
        input_.erase(0, 1);
        const auto line = ReadLine();
        RespValue value;
        switch (type) {
        case '+': value.kind = RespValue::Kind::Simple; value.text = line; return value;
        case '-': value.kind = RespValue::Kind::Error; value.text = line; return value;
        case ':': value.kind = RespValue::Kind::Integer;
                  value.integer = ParseSigned(line, "integer"); return value;
        case '$': {
            const auto length = ParseSigned(line, "bulk length");
            if (length == -1) return value;
            if (length < 0) throw std::runtime_error("invalid RESP bulk length");
            value.kind = RespValue::Kind::Bulk;
            value.text = ReadExact(static_cast<std::size_t>(length));
            return value;
        }
        case '*': {
            const auto count = ParseSigned(line, "array length");
            if (count == -1) return value;
            if (count < 0 || count > 1000000) {
                throw std::runtime_error("invalid RESP array length");
            }
            value.kind = RespValue::Kind::Array;
            value.elements.reserve(static_cast<std::size_t>(count));
            for (std::int64_t i = 0; i < count; ++i) {
                value.elements.push_back(read(depth + 1U));
            }
            return value;
        }
        default: throw std::runtime_error("unsupported RESP response type");
        }
    };
    return read(0);
}

RespValue RespClient::Command(const RespCommand& command) {
    Send(EncodeCommand(command));
    auto response = ReadValue();
    if (response.kind == RespValue::Kind::Error) {
        throw std::runtime_error("database returned RESP error: " + response.text);
    }
    return response;
}

std::vector<RespValue> RespClient::Pipeline(
    const std::vector<RespCommand>& commands) {
    if (commands.empty()) return {};
    std::string bytes;
    for (const auto& command : commands) bytes += EncodeCommand(command);
    Send(bytes);
    std::vector<RespValue> responses;
    responses.reserve(commands.size());
    for (std::size_t i = 0; i < commands.size(); ++i) {
        auto response = ReadValue();
        if (response.kind == RespValue::Kind::Error) {
            throw std::runtime_error("database returned RESP error: " + response.text);
        }
        responses.push_back(std::move(response));
    }
    return responses;
}

std::string RequireText(const RespValue& value, const std::string& context) {
    if (value.kind != RespValue::Kind::Bulk && value.kind != RespValue::Kind::Simple) {
        throw std::runtime_error(context + " returned an unexpected RESP type");
    }
    return value.text;
}

std::int64_t RequireInteger(const RespValue& value, const std::string& context) {
    if (value.kind != RespValue::Kind::Integer) {
        throw std::runtime_error(context + " returned an unexpected RESP type");
    }
    return value.integer;
}

void RequireArray(const RespValue& value, const std::string& context) {
    if (value.kind != RespValue::Kind::Array) {
        throw std::runtime_error(context + " returned an unexpected RESP type");
    }
}

} // namespace benchforge::detail
