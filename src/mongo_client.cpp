#include "mongo_client.hpp"

#include "benchforge/adapter.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>

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

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
NativeSocket ToNative(std::intptr_t value) { return static_cast<NativeSocket>(value); }
std::intptr_t ToStored(NativeSocket value) { return static_cast<std::intptr_t>(value); }
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

[[noreturn]] void ThrowSocketFailure(const char* context) {
#ifdef _WIN32
    if (WSAGetLastError() == WSAETIMEDOUT) {
        throw AdapterTimeout(std::string("MongoDB socket ") + context + " timed out");
    }
#else
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        throw AdapterTimeout(std::string("MongoDB socket ") + context + " timed out");
    }
#endif
    throw std::runtime_error(std::string("MongoDB socket ") + context + " failed");
}

template <typename T>
void AppendLittleEndian(std::string& output, T value) {
    using Unsigned = std::make_unsigned_t<T>;
    const auto bits = static_cast<Unsigned>(value);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        output.push_back(static_cast<char>((bits >> (i * 8U)) & 0xffU));
    }
}

template <typename T>
T ReadLittleEndian(const std::string& input, std::size_t offset) {
    if (offset > input.size() || input.size() - offset < sizeof(T)) {
        throw std::runtime_error("truncated MongoDB wire message");
    }
    using Unsigned = std::make_unsigned_t<T>;
    Unsigned bits = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        bits |= static_cast<Unsigned>(static_cast<unsigned char>(input[offset + i]))
                << (i * 8U);
    }
    return std::bit_cast<T>(bits);
}

std::string ReadCString(const std::string& bytes, std::size_t& offset,
                        std::size_t end) {
    const auto start = offset;
    while (offset < end && bytes[offset] != '\0') ++offset;
    if (offset >= end) throw std::runtime_error("unterminated BSON field name");
    auto value = bytes.substr(start, offset - start);
    ++offset;
    return value;
}

std::string ReadBsonString(const std::string& bytes, std::size_t& offset,
                           std::size_t end) {
    const auto length = ReadLittleEndian<std::int32_t>(bytes, offset);
    offset += 4U;
    if (length <= 0 || static_cast<std::size_t>(length) > end - offset) {
        throw std::runtime_error("invalid BSON string length");
    }
    if (bytes[offset + static_cast<std::size_t>(length) - 1U] != '\0') {
        throw std::runtime_error("BSON string is missing its terminator");
    }
    auto value = bytes.substr(offset, static_cast<std::size_t>(length) - 1U);
    offset += static_cast<std::size_t>(length);
    return value;
}

MongoValue ParseDocument(const std::string& bytes, std::size_t offset,
                         std::size_t* next_offset, bool is_array,
                         unsigned depth);

void SkipBsonValue(std::uint8_t type, const std::string& bytes,
                   std::size_t& offset, std::size_t end, unsigned depth) {
    auto skip = [&](std::size_t count) {
        if (count > end - offset) throw std::runtime_error("truncated BSON value");
        offset += count;
    };
    switch (type) {
    case 0x01: case 0x09: case 0x11: skip(8); return;
    case 0x02: case 0x0D: case 0x0E: (void)ReadBsonString(bytes, offset, end); return;
    case 0x03: case 0x04: {
        std::size_t after = 0;
        (void)ParseDocument(bytes, offset, &after, type == 0x04, depth + 1U);
        offset = after;
        return;
    }
    case 0x05: {
        const auto length = ReadLittleEndian<std::int32_t>(bytes, offset);
        offset += 4U;
        skip(1);
        if (length < 0) throw std::runtime_error("invalid BSON binary length");
        skip(static_cast<std::size_t>(length));
        return;
    }
    case 0x06: case 0x0A: case 0x7F: case 0xFF: return;
    case 0x07: skip(12); return;
    case 0x08: skip(1); return;
    case 0x0B: (void)ReadCString(bytes, offset, end); (void)ReadCString(bytes, offset, end); return;
    case 0x0C:
        (void)ReadBsonString(bytes, offset, end);
        skip(12);
        return;
    case 0x0F: {
        const auto length = ReadLittleEndian<std::int32_t>(bytes, offset);
        if (length < 4 || static_cast<std::size_t>(length) > end - offset) {
            throw std::runtime_error("invalid BSON code-with-scope length");
        }
        skip(static_cast<std::size_t>(length));
        return;
    }
    case 0x10: skip(4); return;
    case 0x12: case 0x13: skip(type == 0x12 ? 8U : 16U); return;
    default: throw std::runtime_error("unsupported BSON value type");
    }
}

MongoValue ParseDocument(const std::string& bytes, std::size_t offset,
                         std::size_t* next_offset, bool is_array,
                         unsigned depth) {
    if (depth > 64U) throw std::runtime_error("BSON nesting is too deep");
    const auto length = ReadLittleEndian<std::int32_t>(bytes, offset);
    if (length < 5 || static_cast<std::size_t>(length) > bytes.size() - offset) {
        throw std::runtime_error("invalid BSON document length");
    }
    const auto end = offset + static_cast<std::size_t>(length);
    auto position = offset + 4U;
    MongoValue result;
    result.kind = is_array ? MongoValue::Kind::Array : MongoValue::Kind::Document;
    while (position < end - 1U) {
        const auto type = static_cast<std::uint8_t>(bytes[position++]);
        const auto key = ReadCString(bytes, position, end - 1U);
        MongoValue value;
        switch (type) {
        case 0x01:
            value.kind = MongoValue::Kind::Number;
            value.number = std::bit_cast<double>(ReadLittleEndian<std::uint64_t>(bytes, position));
            position += 8U;
            break;
        case 0x02:
            value.kind = MongoValue::Kind::String;
            value.text = ReadBsonString(bytes, position, end - 1U);
            break;
        case 0x03: case 0x04: {
            std::size_t after = 0;
            value = ParseDocument(bytes, position, &after, type == 0x04, depth + 1U);
            position = after;
            break;
        }
        case 0x08:
            if (position >= end - 1U) throw std::runtime_error("truncated BSON boolean");
            value.kind = MongoValue::Kind::Boolean;
            value.boolean = bytes[position++] != 0;
            break;
        case 0x0A:
            value.kind = MongoValue::Kind::Null;
            break;
        case 0x10:
            value.kind = MongoValue::Kind::Number;
            value.integer = ReadLittleEndian<std::int32_t>(bytes, position);
            value.number = static_cast<double>(value.integer);
            value.integer_value = true;
            position += 4U;
            break;
        case 0x12:
            value.kind = MongoValue::Kind::Number;
            value.integer = ReadLittleEndian<std::int64_t>(bytes, position);
            value.number = static_cast<double>(value.integer);
            value.integer_value = true;
            position += 8U;
            break;
        default:
            SkipBsonValue(type, bytes, position, end - 1U, depth);
            break;
        }
        if (is_array) {
            result.elements.push_back(std::move(value));
        } else {
            result.fields.insert_or_assign(key, std::move(value));
        }
    }
    if (position != end - 1U || bytes[position] != '\0') {
        throw std::runtime_error("malformed BSON document terminator");
    }
    if (next_offset != nullptr) *next_offset = end;
    return result;
}

void ValidateFieldName(const std::string& name) {
    if (name.find('\0') != std::string::npos) {
        throw std::invalid_argument("BSON field names may not contain null bytes");
    }
}

} // namespace

const MongoValue* MongoValue::Find(const std::string& name) const noexcept {
    const auto found = fields.find(name);
    return found == fields.end() ? nullptr : &found->second;
}

std::int64_t MongoValue::Integer(const std::string& context) const {
    if (kind != Kind::Number || !integer_value) {
        throw std::runtime_error(context + " was not an integer");
    }
    return integer;
}

const std::string& MongoValue::String(const std::string& context) const {
    if (kind != Kind::String) throw std::runtime_error(context + " was not a string");
    return text;
}

const std::vector<MongoValue>& MongoValue::Array(const std::string& context) const {
    if (kind != Kind::Array) throw std::runtime_error(context + " was not an array");
    return elements;
}

void BsonBuilder::Element(std::uint8_t type, const std::string& name,
                          const std::string& value) {
    ValidateFieldName(name);
    elements_.push_back(static_cast<char>(type));
    elements_.append(name);
    elements_.push_back('\0');
    elements_.append(value);
}

BsonBuilder& BsonBuilder::Int32(const std::string& name, std::int32_t value) {
    std::string encoded;
    AppendLittleEndian(encoded, value);
    Element(0x10, name, encoded);
    return *this;
}

BsonBuilder& BsonBuilder::Int64(const std::string& name, std::int64_t value) {
    std::string encoded;
    AppendLittleEndian(encoded, value);
    Element(0x12, name, encoded);
    return *this;
}

BsonBuilder& BsonBuilder::Boolean(const std::string& name, bool value) {
    Element(0x08, name, std::string(1, value ? '\x01' : '\x00'));
    return *this;
}

BsonBuilder& BsonBuilder::String(const std::string& name,
                                 const std::string& value) {
    std::string encoded;
    if (value.size() >= static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::length_error("BSON string is too large");
    }
    AppendLittleEndian(encoded, static_cast<std::int32_t>(value.size() + 1U));
    encoded.append(value);
    encoded.push_back('\0');
    Element(0x02, name, encoded);
    return *this;
}

BsonBuilder& BsonBuilder::Document(const std::string& name,
                                   const BsonDocument& value) {
    Element(0x03, name, value);
    return *this;
}

BsonBuilder& BsonBuilder::Array(const std::string& name,
                                const BsonDocument& value) {
    Element(0x04, name, value);
    return *this;
}

BsonBuilder& BsonBuilder::RawFields(const BsonDocument& value) {
    if (value.size() < 5U || ReadLittleEndian<std::int32_t>(value, 0) !=
                                 static_cast<std::int32_t>(value.size()) ||
        value.back() != '\0') {
        throw std::invalid_argument("invalid BSON document passed to RawFields");
    }
    elements_.append(value, 4U, value.size() - 5U);
    return *this;
}

BsonDocument BsonBuilder::Finish() const {
    const auto size = elements_.size() + 5U;
    if (size > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::length_error("BSON document is too large");
    }
    BsonDocument result;
    AppendLittleEndian(result, static_cast<std::int32_t>(size));
    result.append(elements_);
    result.push_back('\0');
    return result;
}

BsonDocument ArrayDocuments(const std::vector<BsonDocument>& values) {
    BsonBuilder array;
    for (std::size_t i = 0; i < values.size(); ++i) {
        array.Document(std::to_string(i), values[i]);
    }
    return array.Finish();
}

BsonDocument ArrayInt64(const std::vector<std::int64_t>& values) {
    BsonBuilder array;
    for (std::size_t i = 0; i < values.size(); ++i) {
        array.Int64(std::to_string(i), values[i]);
    }
    return array.Finish();
}

BsonDocument ArrayStrings(const std::vector<std::string>& values) {
    BsonBuilder array;
    for (std::size_t i = 0; i < values.size(); ++i) {
        array.String(std::to_string(i), values[i]);
    }
    return array.Finish();
}

MongoValue ParseBsonDocument(const std::string& bytes, std::size_t offset,
                             std::size_t* next_offset) {
    return ParseDocument(bytes, offset, next_offset, false, 0);
}

MongoClient::MongoClient(const std::string& host, std::uint16_t port) {
    EnsureNetworkRuntime();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    const auto service = std::to_string(port);
    const auto status = getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses);
    if (status != 0) {
#ifdef _WIN32
        throw std::runtime_error("could not resolve MongoDB host: " + host);
#else
        throw std::runtime_error("could not resolve MongoDB host " + host +
                                 ": " + gai_strerror(status));
#endif
    }

    NativeSocket connected = kInvalidSocket;
    for (auto* address = addresses; address != nullptr; address = address->ai_next) {
        const auto candidate = socket(address->ai_family, address->ai_socktype,
                                      address->ai_protocol);
        if (candidate == kInvalidSocket) continue;
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
        throw std::runtime_error("could not connect to MongoDB at " + host + ":" + service);
    }
    socket_ = ToStored(connected);

    BsonBuilder application;
    application.String("name", "BenchForge");
    BsonBuilder driver;
    driver.String("name", "benchforge-native").String("version", "0.1.0");
    BsonBuilder operating_system;
#ifdef _WIN32
    operating_system.String("type", "Windows").String("name", "Windows");
#elif defined(__APPLE__)
    operating_system.String("type", "Darwin").String("name", "macOS");
#else
    operating_system.String("type", "Linux").String("name", "Linux");
#endif
#if defined(_M_X64) || defined(__x86_64__)
    operating_system.String("architecture", "x86_64");
#elif defined(_M_ARM64) || defined(__aarch64__)
    operating_system.String("architecture", "aarch64");
#endif
    BsonBuilder client;
    client.Document("application", application.Finish())
          .Document("driver", driver.Finish())
          .Document("os", operating_system.Finish());
    BsonBuilder hello;
    hello.Int32("hello", 1).Boolean("helloOk", true)
         .Document("client", client.Finish()).String("$db", "admin");
    MongoValue response;
    try {
        response = ParseBsonDocument(Exchange(hello.Finish()));
        RequireMongoOk(response, "MongoDB hello");
    } catch (...) {
        CloseSocket(ToNative(socket_));
        socket_ = -1;
        throw;
    }
}

MongoClient::~MongoClient() {
    CloseSocket(ToNative(socket_));
}

MongoClient::MongoClient(MongoClient&& other) noexcept
    : socket_(other.socket_), request_id_(other.request_id_) {
    other.socket_ = -1;
}

MongoClient& MongoClient::operator=(MongoClient&& other) noexcept {
    if (this != &other) {
        CloseSocket(ToNative(socket_));
        socket_ = other.socket_;
        request_id_ = other.request_id_;
        other.socket_ = -1;
    }
    return *this;
}

void MongoClient::Send(const std::string& bytes) {
    std::size_t sent = 0;
    const auto socket_value = ToNative(socket_);
    while (sent < bytes.size()) {
#ifdef _WIN32
        const auto count = send(socket_value, bytes.data() + sent,
            static_cast<int>(std::min<std::size_t>(bytes.size() - sent,
                static_cast<std::size_t>(std::numeric_limits<int>::max()))), 0);
#else
        const auto count = send(socket_value, bytes.data() + sent,
                                bytes.size() - sent, 0);
#endif
        if (count < 0) ThrowSocketFailure("write");
        if (count == 0) throw std::runtime_error("MongoDB socket write closed");
        sent += static_cast<std::size_t>(count);
    }
}

std::string MongoClient::ReadExact(std::size_t length) {
    if (length > 64U * 1024U * 1024U) {
        throw std::runtime_error("MongoDB wire response exceeds 64 MiB");
    }
    std::string result(length, '\0');
    std::size_t received = 0;
    while (received < length) {
        const auto count = recv(ToNative(socket_), result.data() + received,
                                static_cast<int>(std::min<std::size_t>(
                                    length - received,
                                    static_cast<std::size_t>(std::numeric_limits<int>::max()))),
                                0);
        if (count < 0) ThrowSocketFailure("read");
        if (count == 0) throw std::runtime_error("MongoDB socket closed");
        received += static_cast<std::size_t>(count);
    }
    return result;
}

std::string MongoClient::Exchange(const BsonDocument& command) {
    if (socket_ == -1) throw std::runtime_error("MongoDB connection is closed");
    const auto request_id = ++request_id_;
    std::string message;
    AppendLittleEndian(message, static_cast<std::int32_t>(0));
    AppendLittleEndian(message, request_id);
    AppendLittleEndian(message, static_cast<std::int32_t>(0));
    AppendLittleEndian(message, static_cast<std::int32_t>(2013));
    AppendLittleEndian(message, static_cast<std::int32_t>(0));
    message.push_back('\0');
    message.append(command);
    const auto size = static_cast<std::int32_t>(message.size());
    for (std::size_t i = 0; i < 4U; ++i) {
        message[i] = static_cast<char>((static_cast<std::uint32_t>(size) >> (i * 8U)) & 0xffU);
    }
    Send(message);

    const auto header = ReadExact(16U);
    const auto response_length = ReadLittleEndian<std::int32_t>(header, 0);
    const auto response_to = ReadLittleEndian<std::int32_t>(header, 8U);
    const auto opcode = ReadLittleEndian<std::int32_t>(header, 12U);
    if (response_length < 21 || response_length > 64 * 1024 * 1024) {
        throw std::runtime_error("MongoDB returned an invalid wire message length");
    }
    if (response_to != request_id) {
        throw std::runtime_error("MongoDB response did not match the outstanding request");
    }
    if (opcode != 2013) {
        throw std::runtime_error("MongoDB returned an unsupported wire message opcode");
    }
    const auto payload = ReadExact(static_cast<std::size_t>(response_length - 16));
    if (payload.size() < 5U) {
        throw std::runtime_error("MongoDB returned a truncated OP_MSG response");
    }
    const auto flags = ReadLittleEndian<std::int32_t>(payload, 0);
    auto position = 4U;
    const auto sections_end = payload.size() - ((flags & 1) != 0 ? 4U : 0U);
    while (position < sections_end) {
        const auto kind = static_cast<std::uint8_t>(payload[position++]);
        if (kind == 0) {
            const auto document_length = ReadLittleEndian<std::int32_t>(payload, position);
            if (document_length < 5 ||
                static_cast<std::size_t>(document_length) > sections_end - position) {
                throw std::runtime_error("MongoDB returned an invalid OP_MSG document section");
            }
            return payload.substr(position, static_cast<std::size_t>(document_length));
        }
        if (kind == 1) {
            const auto section_start = position;
            const auto section_length = ReadLittleEndian<std::int32_t>(payload, position);
            if (section_length < 5 ||
                static_cast<std::size_t>(section_length) > sections_end - section_start) {
                throw std::runtime_error("MongoDB returned an invalid OP_MSG sequence section");
            }
            position = section_start + static_cast<std::size_t>(section_length);
            continue;
        }
        throw std::runtime_error("MongoDB returned an unknown OP_MSG section type");
    }
    throw std::runtime_error("MongoDB response contained no command document");
}

MongoValue MongoClient::Command(const std::string& database,
                                const BsonDocument& command) {
    BsonBuilder request;
    request.RawFields(command).String("$db", database);
    const auto reply = ParseBsonDocument(Exchange(request.Finish()));
    RequireMongoOk(reply, "MongoDB command");
    if (const auto* errors = reply.Find("writeErrors");
        errors != nullptr && errors->kind == MongoValue::Kind::Array &&
        !errors->elements.empty()) {
        const auto* message = errors->elements.front().Find("errmsg");
        throw std::runtime_error("MongoDB write failed: " +
            (message != nullptr && message->kind == MongoValue::Kind::String
                ? message->text : "write error"));
    }
    if (const auto* error = reply.Find("writeConcernError");
        error != nullptr && error->kind == MongoValue::Kind::Document) {
        const auto* message = error->Find("errmsg");
        throw std::runtime_error("MongoDB write concern failed: " +
            (message != nullptr && message->kind == MongoValue::Kind::String
                ? message->text : "write concern error"));
    }
    return reply;
}

void RequireMongoOk(const MongoValue& reply, const std::string& context) {
    const auto* ok = reply.Find("ok");
    if (ok == nullptr || ok->kind != MongoValue::Kind::Number || ok->number == 0) {
        const auto* message = reply.Find("errmsg");
        throw std::runtime_error(context + " failed: " +
            (message != nullptr && message->kind == MongoValue::Kind::String
                ? message->text : "server returned ok=0"));
    }
}

} // namespace benchforge::detail
