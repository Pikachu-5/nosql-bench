#include "neo4j_client.hpp"
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <map>
#include <mutex>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace benchforge::detail {
namespace {
constexpr std::size_t kLimit = 4U * 1024U * 1024U;
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalid = INVALID_SOCKET;
void Close(Socket socket) { if (socket != kInvalid) closesocket(socket); }
#else
using Socket = int;
constexpr Socket kInvalid = -1;
void Close(Socket socket) { if (socket != kInvalid) close(socket); }
#endif
[[noreturn]] void Failure() {
#ifdef _WIN32
    if (WSAGetLastError() == WSAETIMEDOUT) throw AdapterTimeout("Neo4j HTTP socket timed out");
#else
    if (errno == EAGAIN || errno == EWOULDBLOCK) throw AdapterTimeout("Neo4j HTTP socket timed out");
#endif
    throw std::runtime_error("Neo4j HTTP socket failed");
}
std::size_t Size(const std::string& text, int base = 10) {
    std::size_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value > kLimit)
        throw std::runtime_error("invalid or oversized Neo4j HTTP response length");
    return value;
}
std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}
}
Neo4jClient::Neo4jClient(const std::string& host, std::uint16_t port)
    : host_(host + ":" + std::to_string(port)) {
#ifdef _WIN32
    static std::once_flag once;
    static int startup = 0;
    std::call_once(once, [] { WSADATA data{}; startup = WSAStartup(MAKEWORD(2,2), &data); });
    if (startup != 0) throw std::runtime_error("Winsock initialization failed");
#endif
    addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &addresses) != 0)
        throw std::runtime_error("Neo4j host resolution failed");
    for (auto* address = addresses; address; address = address->ai_next) {
        const auto candidate = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate == kInvalid) continue;
#ifdef _WIN32
        const DWORD timeout = 10000;
        setsockopt(candidate, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(candidate, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
        const timeval timeout{10,0};
        setsockopt(candidate, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(candidate, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
        if (connect(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) {
            socket_ = static_cast<std::intptr_t>(candidate); break;
        }
        Close(candidate);
    }
    freeaddrinfo(addresses);
    if (socket_ == -1) throw std::runtime_error("could not connect to Neo4j at " + host_);
}
Neo4jClient::~Neo4jClient() { Close(static_cast<Socket>(socket_)); }
void Neo4jClient::Send(const std::string& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const auto count = send(static_cast<Socket>(socket_), bytes.data() + sent,
            static_cast<int>(std::min<std::size_t>(bytes.size() - sent, 65536)), 0);
        if (count < 0) Failure();
        if (count == 0) throw std::runtime_error("Neo4j HTTP connection closed on write");
        sent += static_cast<std::size_t>(count);
    }
}
void Neo4jClient::Receive() {
    char bytes[8192];
    const auto count = recv(static_cast<Socket>(socket_), bytes, sizeof(bytes), 0);
    if (count < 0) Failure();
    if (count == 0) throw std::runtime_error("Neo4j HTTP connection closed on read");
    input_.append(bytes, static_cast<std::size_t>(count));
    if (input_.size() > kLimit + 8192) throw std::runtime_error("Neo4j HTTP input exceeds limit");
}
std::string Neo4jClient::Line() {
    while (input_.find("\r\n") == std::string::npos) {
        if (input_.size() > 16384) throw std::runtime_error("Neo4j HTTP line too long");
        Receive();
    }
    const auto end = input_.find("\r\n");
    if (end > 16384) throw std::runtime_error("Neo4j HTTP line too long");
    auto result = input_.substr(0, end); input_.erase(0, end + 2); return result;
}
std::string Neo4jClient::Exact(std::size_t size) {
    if (size > kLimit) throw std::runtime_error("Neo4j response too large");
    while (input_.size() < size) Receive();
    auto result = input_.substr(0, size); input_.erase(0, size); return result;
}
nlohmann::json ParseNeo4jResponse(const std::string& body) {
    const auto result = nlohmann::json::parse(body);
    if (!result.is_object()) throw std::runtime_error("invalid Neo4j response object");
    if (result.contains("errors")) {
        const auto& errors = result.at("errors");
        if (!errors.is_array()) throw std::runtime_error("invalid Neo4j errors field");
        if (!errors.empty()) throw Neo4jQueryError(errors.front().value("code", "unknown"), "Neo4j query error: " + errors.front().dump());
    }
    if (!result.contains("data") || !result.at("data").is_object() ||
        !result.at("data").contains("values") || !result.at("data").at("values").is_array())
        throw std::runtime_error("Neo4j response data missing");
    return result;
}
nlohmann::json Neo4jClient::Query(const std::string& statement, const nlohmann::json& parameters) {
    const auto body = nlohmann::json{{"statement", statement}, {"parameters", parameters}}.dump();
    Send("POST /db/neo4j/query/v2 HTTP/1.1\r\nHost: " + host_ +
        "\r\nContent-Type: application/json\r\nAccept: application/json\r\nConnection: keep-alive\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body);
    const auto status = Line();
    std::map<std::string, std::string> headers;
    std::size_t header_bytes = status.size();
    while (true) {
        auto line = Line(); if (line.empty()) break;
        header_bytes += line.size();
        if (header_bytes > 32768) throw std::runtime_error("Neo4j HTTP headers too large");
        const auto colon = line.find(':');
        if (colon == std::string::npos) throw std::runtime_error("invalid Neo4j HTTP header");
        auto value = line.substr(colon + 1);
        const auto first = value.find_first_not_of(" \t");
        value = first == std::string::npos ? "" : value.substr(first);
        if (!headers.emplace(Lower(line.substr(0, colon)), Lower(value)).second)
            throw std::runtime_error("duplicate Neo4j HTTP header");
    }
    std::string response;
    if (headers.contains("transfer-encoding")) {
        if (headers.at("transfer-encoding") != "chunked" || headers.contains("content-length"))
            throw std::runtime_error("unsupported Neo4j HTTP framing");
        while (true) {
            const auto chunk = Line(); const auto length = Size(chunk.substr(0, chunk.find(';')), 16);
            if (length == 0) {
                std::size_t trailers = 0;
                while (true) { const auto trailer = Line(); if (trailer.empty()) break;
                    trailers += trailer.size(); if (trailers > 32768) throw std::runtime_error("Neo4j HTTP trailers too large"); }
                break;
            }
            if (response.size() + length > kLimit) throw std::runtime_error("Neo4j response too large");
            response += Exact(length);
            if (Exact(2) != "\r\n") throw std::runtime_error("invalid Neo4j HTTP chunk");
        }
    } else if (headers.contains("content-length")) response = Exact(Size(headers.at("content-length")));
    else throw std::runtime_error("Neo4j HTTP response has no bounded framing");
    if (status.rfind("HTTP/1.1 202 ", 0) != 0 && status.rfind("HTTP/1.1 200 ", 0) != 0)
        throw std::runtime_error("Neo4j HTTP failure: " + status + " " + response.substr(0, 512));
    return ParseNeo4jResponse(response);
}
} // namespace benchforge::detail
