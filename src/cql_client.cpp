#include "cql_client.hpp"
#include "benchforge/adapter.hpp"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <limits>
#include <mutex>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
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
constexpr std::size_t kMaxFrame = 64U * 1024U * 1024U;
#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr auto kInvalidSocket = INVALID_SOCKET;
void CloseSocket(std::intptr_t value) { if (value != -1) closesocket(static_cast<SOCKET>(value)); }
#else
using NativeSocket = int;
constexpr auto kInvalidSocket = -1;
void CloseSocket(std::intptr_t value) { if (value != -1) close(static_cast<int>(value)); }
#endif

void Append(std::string& out, std::uint64_t value, unsigned size) {
    for (unsigned i = size; i != 0; --i) {
        out.push_back(static_cast<char>((value >> ((i - 1U) * 8U)) & 0xffU));
    }
}
void ShortString(std::string& out, const std::string& value) {
    if (value.size() > 65535U) throw std::length_error("CQL short string too large");
    Append(out, value.size(), 2);
    out += value;
}
void Bytes(std::string& out, const std::string& value) {
    if (value.size() > kMaxFrame) throw std::length_error("CQL bytes too large");
    Append(out, value.size(), 4);
    out += value;
}
void Values(std::string& out, const std::vector<std::string>& values) {
    if (values.size() > 65535U) throw std::length_error("too many CQL values");
    Append(out, values.size(), 2);
    for (const auto& value : values) Bytes(out, value);
}

class Reader {
public:
    explicit Reader(const std::string& bytes) : bytes_(bytes) {}
    std::uint64_t Number(unsigned size) {
        Need(size);
        std::uint64_t result = 0;
        while (size-- != 0) result = (result << 8U) | static_cast<unsigned char>(bytes_[offset_++]);
        return result;
    }
    std::string Take(std::size_t size) {
        Need(size);
        auto result = bytes_.substr(offset_, size);
        offset_ += size;
        return result;
    }
    std::string String() { return Take(static_cast<std::size_t>(Number(2))); }
    std::optional<std::string> Value() {
        const auto size = static_cast<std::uint32_t>(Number(4));
        if (size == 0xffffffffU) return std::nullopt;
        if (size > kMaxFrame) throw std::runtime_error("invalid CQL value length");
        return Take(size);
    }
    std::string Rest() { return Take(bytes_.size() - offset_); }
    std::size_t Remaining() const { return bytes_.size() - offset_; }
    void Type(unsigned depth = 0) {
        if (depth > 32U) throw std::runtime_error("CQL type nesting too deep");
        const auto type = Number(2);
        if (type == 0) { (void)String(); }
        else if (type == 0x20 || type == 0x22) { Type(depth + 1U); }
        else if (type == 0x21) { Type(depth + 1U); Type(depth + 1U); }
        else if (type == 0x30 || type == 0x31) {
            if (type == 0x30) { (void)String(); (void)String(); }
            const auto count = Number(2);
            for (std::uint64_t i = 0; i < count; ++i) {
                if (type == 0x30) (void)String();
                Type(depth + 1U);
            }
        } else if (type > 0x15) throw std::runtime_error("unknown CQL column type");
    }
private:
    const std::string& bytes_;
    std::size_t offset_{0};
    void Need(std::size_t size) const {
        if (size > bytes_.size() - offset_) throw std::runtime_error("truncated CQL response");
    }
};

[[noreturn]] void SocketError(const char* operation) {
#ifdef _WIN32
    const auto timed_out = WSAGetLastError() == WSAETIMEDOUT;
#else
    const auto timed_out = errno == EAGAIN || errno == EWOULDBLOCK;
#endif
    if (timed_out) throw AdapterTimeout(std::string("Cassandra socket ") + operation + " timed out");
    throw std::runtime_error(std::string("Cassandra socket ") + operation + " failed");
}
} // namespace

std::string CqlBigint(std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument("Cassandra ID exceeds signed bigint range");
    }
    std::string result;
    Append(result, value, 8);
    return result;
}
std::int64_t CqlReadBigint(const std::optional<std::string>& value) {
    if (!value || value->size() != 8U) throw std::runtime_error("expected CQL bigint");
    Reader reader(*value);
    return std::bit_cast<std::int64_t>(reader.Number(8));
}

CqlResult ParseCqlResult(const std::string& body) {
    Reader reader(body);
    const auto kind = reader.Number(4);
    CqlResult result;
    if (kind == 1 || kind == 3 || kind == 5) return result;
    if (kind != 2) throw std::runtime_error("unexpected CQL result kind");
    const auto flags = reader.Number(4);
    const auto columns = reader.Number(4);
    if (columns > 1024U || (flags & ~7ULL) != 0) {
        throw std::runtime_error("invalid CQL row metadata");
    }
    if ((flags & 2U) != 0) {
        const auto state = reader.Value();
        if (!state || state->empty()) throw std::runtime_error("CQL paging state missing");
        result.paging_state = *state;
    }
    // This client always requests metadata, including for prepared executions.
    if ((flags & 4U) != 0 && columns != 0) throw std::runtime_error("CQL omitted requested metadata");
    if ((flags & 4U) == 0) {
        if ((flags & 1U) != 0) { (void)reader.String(); (void)reader.String(); }
        for (std::uint64_t i = 0; i < columns; ++i) {
            if ((flags & 1U) == 0) { (void)reader.String(); (void)reader.String(); }
            result.columns.push_back(reader.String());
            reader.Type();
        }
    }
    const auto rows = reader.Number(4);
    if (rows > 1000000U || (columns != 0 && rows > reader.Remaining() / (columns * 4U))) {
        throw std::runtime_error("invalid CQL row count");
    }
    for (std::uint64_t i = 0; i < rows; ++i) {
        CqlRow row;
        for (std::uint64_t j = 0; j < columns; ++j) row.push_back(reader.Value());
        result.rows.push_back(std::move(row));
    }
    return result;
}

CqlClient::CqlClient(const std::string& host, std::uint16_t port) {
#ifdef _WIN32
    static std::once_flag initialized;
    static int startup = 0;
    std::call_once(initialized, [] { WSADATA data{}; startup = WSAStartup(MAKEWORD(2, 2), &data); });
    if (startup != 0) throw std::runtime_error("could not initialize Winsock");
#endif
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    const auto service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses) != 0) {
        throw std::runtime_error("could not resolve Cassandra host");
    }
    for (auto* address = addresses; address != nullptr; address = address->ai_next) {
        const auto candidate = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate == kInvalidSocket) continue;
#ifdef _WIN32
        const DWORD timeout = 10000;
        setsockopt(candidate, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(candidate, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
        const timeval timeout{10, 0};
        setsockopt(candidate, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(candidate, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
        if (connect(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) {
            socket_ = static_cast<std::intptr_t>(candidate);
            break;
        }
        CloseSocket(static_cast<std::intptr_t>(candidate));
    }
    freeaddrinfo(addresses);
    if (socket_ == -1) throw std::runtime_error("could not connect to Cassandra at " + host + ":" + service);
    try {
        std::string options;
        Append(options, 1, 2);
        ShortString(options, "CQL_VERSION");
        ShortString(options, "3.0.0");
        (void)Exchange(1, options, 2);
    } catch (...) { CloseSocket(socket_); socket_ = -1; throw; }
}
CqlClient::~CqlClient() { CloseSocket(socket_); }

void CqlClient::Send(const std::string& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto size = static_cast<int>(std::min(bytes.size() - offset, kMaxFrame));
        const auto count = send(static_cast<NativeSocket>(socket_), bytes.data() + offset, size, 0);
        if (count < 0) SocketError("write");
        if (count == 0) throw std::runtime_error("Cassandra connection closed");
        offset += static_cast<std::size_t>(count);
    }
}
std::string CqlClient::ReadExact(std::size_t size) {
    if (size > kMaxFrame) throw std::runtime_error("CQL frame exceeds 64 MiB");
    std::string bytes(size, '\0');
    std::size_t offset = 0;
    while (offset < size) {
        const auto count = recv(static_cast<NativeSocket>(socket_), bytes.data() + offset,
                                static_cast<int>(size - offset), 0);
        if (count < 0) SocketError("read");
        if (count == 0) throw std::runtime_error("Cassandra connection closed");
        offset += static_cast<std::size_t>(count);
    }
    return bytes;
}
std::string CqlClient::Exchange(std::uint8_t opcode, const std::string& body,
                               std::uint8_t expected_opcode) {
    if (socket_ == -1) throw std::runtime_error("Cassandra connection is closed");
    if (body.size() > kMaxFrame) throw std::length_error("CQL request exceeds 64 MiB");
    try {
        std::string frame;
        frame.push_back('\x04'); frame.push_back('\0');
        Append(frame, 0, 2); // one outstanding request; stream zero
        frame.push_back(static_cast<char>(opcode));
        Append(frame, body.size(), 4);
        frame += body;
        Send(frame);
        const auto header = ReadExact(9);
        Reader h(header);
        if (h.Number(1) != 0x84) throw std::runtime_error("expected Cassandra protocol v4");
        const auto flags = h.Number(1);
        if ((flags & ~0x0eULL) != 0 || h.Number(2) != 0) {
            throw std::runtime_error("unexpected Cassandra frame flags or stream");
        }
        const auto response_opcode = h.Number(1);
        const auto payload = ReadExact(static_cast<std::size_t>(h.Number(4)));
        Reader response(payload);
        if ((flags & 2U) != 0) (void)response.Take(16);
        if ((flags & 8U) != 0) {
            const auto count = response.Number(2);
            for (std::uint64_t i = 0; i < count; ++i) (void)response.String();
        }
        if ((flags & 4U) != 0) {
            const auto count = response.Number(2);
            for (std::uint64_t i = 0; i < count; ++i) { (void)response.String(); (void)response.Value(); }
        }
        if (response_opcode == 0) {
            const auto code = response.Number(4);
            const auto message = response.String();
            if (code == 0x1100 || code == 0x1200) throw AdapterTimeout("Cassandra server timeout: " + message);
            throw std::runtime_error("Cassandra error " + std::to_string(code) + ": " + message);
        }
        if (response_opcode == 3) throw std::runtime_error("Cassandra adapter requires local authentication disabled");
        if (response_opcode != expected_opcode) throw std::runtime_error("unexpected Cassandra response opcode");
        return response.Rest();
    } catch (...) {
        // A timeout or malformed frame must never leave stale replies for the next operation.
        CloseSocket(socket_); socket_ = -1;
        throw;
    }
}
CqlResult CqlClient::Query(const std::string& query) {
    std::string body;
    Bytes(body, query);
    Append(body, 1, 2); // consistency ONE
    body.push_back('\0');
    return ParseCqlResult(Exchange(7, body, 8));
}
std::string CqlClient::Prepare(const std::string& query) {
    std::string body;
    Bytes(body, query);
    const auto reply = Exchange(9, body, 8);
    Reader reader(reply);
    if (reader.Number(4) != 4) throw std::runtime_error("Cassandra PREPARE returned no statement");
    const auto id = reader.String();
    if (id.empty()) throw std::runtime_error("Cassandra statement ID is empty");
    return id;
}
CqlResult CqlClient::Execute(const std::string& id, const std::vector<std::string>& values,
                            const std::string& paging_state) {
    std::string body;
    ShortString(body, id);
    Append(body, 1, 2);
    body.push_back(static_cast<char>(0x05U | (paging_state.empty() ? 0U : 0x08U)));
    Values(body, values);
    Append(body, 256, 4);
    if (!paging_state.empty()) Bytes(body, paging_state);
    return ParseCqlResult(Exchange(10, body, 8));
}
void CqlClient::Batch(const std::vector<CqlStatement>& statements) {
    if (statements.empty() || statements.size() > 256U) throw std::invalid_argument("CQL batch must contain 1 to 256 statements");
    std::string body(1, '\0'); // logged batch
    Append(body, statements.size(), 2);
    for (const auto& statement : statements) {
        body.push_back('\x01');
        ShortString(body, statement.id);
        Values(body, statement.values);
    }
    Append(body, 1, 2);
    body.push_back('\0');
    (void)ParseCqlResult(Exchange(13, body, 8));
}
void CqlClient::Probe() { (void)Exchange(5, {}, 6); }

} // namespace benchforge::detail
