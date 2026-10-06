#include "feedkv_server.hpp"

#include <algorithm>
#include <charconv>
#include <climits>
#include <cstdint>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace benchforge {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
void CloseSocket(NativeSocket value) {
    if (value != kInvalidSocket) closesocket(value);
}
int SocketError() { return WSAGetLastError(); }
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
void CloseSocket(NativeSocket value) {
    if (value != kInvalidSocket) close(value);
}
int SocketError() { return 0; }
#endif

std::string Bulk(const std::string& value) {
    return "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
}

std::string Array(const std::vector<std::string>& values) {
    std::string response = "*" + std::to_string(values.size()) + "\r\n";
    for (const auto& value : values) response += Bulk(value);
    return response;
}

std::string Error(const std::string& message) {
    return "-ERR " + message + "\r\n";
}

std::int64_t ParseInteger(const std::string& value) {
    std::int64_t parsed_value{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(),
                                        parsed_value);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        throw std::invalid_argument("expected an integer");
    }
    return parsed_value;
}

class CommandReader {
public:
    explicit CommandReader(NativeSocket socket) : socket_(socket) {}

    bool Read(std::vector<std::string>& command) {
        std::string line;
        if (!ReadLine(line)) return false;
        if (line.empty() || line.front() != '*') {
            throw std::invalid_argument("commands must use RESP arrays");
        }
        const auto count = ParseInteger(line.substr(1));
        if (count < 1 || count > 128) {
            throw std::invalid_argument("command argument count is out of range");
        }
        command.clear();
        command.reserve(static_cast<std::size_t>(count));
        for (std::int64_t i = 0; i < count; ++i) {
            if (!ReadLine(line) || line.empty() || line.front() != '$') {
                throw std::invalid_argument("expected a RESP bulk argument");
            }
            const auto length = ParseInteger(line.substr(1));
            if (length < 0 || length > 1024 * 1024) {
                throw std::invalid_argument("argument size is out of range");
            }
            std::string value;
            if (!ReadExact(static_cast<std::size_t>(length), value)) {
                throw std::invalid_argument("incomplete RESP bulk argument");
            }
            command.push_back(std::move(value));
        }
        return true;
    }

private:
    NativeSocket socket_;
    std::string input_;

    bool Fill() {
        char buffer[8192];
#ifdef _WIN32
        const auto count = recv(socket_, buffer, sizeof(buffer), 0);
#else
        const auto count = recv(socket_, buffer, sizeof(buffer), 0);
#endif
        if (count <= 0) return false;
        input_.append(buffer, static_cast<std::size_t>(count));
        return true;
    }

    bool ReadLine(std::string& line) {
        while (true) {
            const auto end = input_.find("\r\n");
            if (end != std::string::npos) {
                line = input_.substr(0, end);
                input_.erase(0, end + 2U);
                return true;
            }
            if (input_.size() > 1024U * 1024U || !Fill()) return false;
        }
    }

    bool ReadExact(std::size_t length, std::string& value) {
        while (input_.size() < length + 2U) {
            if (!Fill()) return false;
        }
        if (input_[length] != '\r' || input_[length + 1U] != '\n') return false;
        value = input_.substr(0, length);
        input_.erase(0, length + 2U);
        return true;
    }
};

struct Entry {
    enum class Type { String, Set, SortedSet } type{Type::String};
    std::string string_value;
    std::unordered_set<std::string> set_value;
    std::unordered_map<std::string, std::int64_t> scores;
    std::map<std::int64_t, std::set<std::string>> sorted_value;
};

class FeedKvStore {
public:
    std::string Execute(const std::vector<std::string>& args) {
        const auto command = Upper(args[0]);
        std::lock_guard lock(mutex_);
        if (command == "PING" && args.size() == 1U) return "+PONG\r\n";
        if (command == "INFO" && args.size() == 2U) {
            if (args[1] == "server") {
                return Bulk("# Server\r\nredis_version:feedkv-0.1\r\n");
            }
            if (args[1] == "persistence") {
                return Bulk("# Persistence\r\naof_enabled:0\r\nrdb_last_save_time:0\r\n");
            }
            return Error("unsupported INFO section");
        }
        if ((command == "SET" || command == "SETNX") && args.size() == 3U) {
            if (command == "SETNX" && entries_.contains(args[1])) return ":0\r\n";
            Entry entry;
            entry.type = Entry::Type::String;
            entry.string_value = args[2];
            entries_[args[1]] = std::move(entry);
            return command == "SETNX" ? ":1\r\n" : "+OK\r\n";
        }
        if (command == "GET" && args.size() == 2U) {
            const auto found = entries_.find(args[1]);
            if (found == entries_.end()) return "$-1\r\n";
            if (found->second.type != Entry::Type::String) {
                return Error("WRONGTYPE Operation against a key holding the wrong kind of value");
            }
            return Bulk(found->second.string_value);
        }
        if (command == "SADD" && args.size() >= 3U) {
            auto& entry = GetEntry(args[1], Entry::Type::Set);
            std::int64_t added = 0;
            for (std::size_t i = 2; i < args.size(); ++i) {
                added += entry.set_value.insert(args[i]).second ? 1 : 0;
            }
            return ":" + std::to_string(added) + "\r\n";
        }
        if (command == "SISMEMBER" && args.size() == 3U) {
            const auto found = entries_.find(args[1]);
            if (found == entries_.end()) return ":0\r\n";
            if (found->second.type != Entry::Type::Set) {
                return Error("WRONGTYPE Operation against a key holding the wrong kind of value");
            }
            return found->second.set_value.contains(args[2]) ? ":1\r\n" : ":0\r\n";
        }
        if (command == "SCARD" && args.size() == 2U) {
            const auto found = entries_.find(args[1]);
            if (found == entries_.end()) return ":0\r\n";
            if (found->second.type != Entry::Type::Set) {
                return Error("WRONGTYPE Operation against a key holding the wrong kind of value");
            }
            return ":" + std::to_string(found->second.set_value.size()) + "\r\n";
        }
        if (command == "SMEMBERS" && args.size() == 2U) {
            const auto found = entries_.find(args[1]);
            if (found == entries_.end()) return "*0\r\n";
            if (found->second.type != Entry::Type::Set) {
                return Error("WRONGTYPE Operation against a key holding the wrong kind of value");
            }
            std::vector<std::string> members(found->second.set_value.begin(),
                                             found->second.set_value.end());
            std::sort(members.begin(), members.end());
            return Array(members);
        }
        if (command == "ZADD" && args.size() == 4U) {
            const auto score = ParseInteger(args[2]);
            auto& entry = GetEntry(args[1], Entry::Type::SortedSet);
            const auto existing = entry.scores.find(args[3]);
            if (existing != entry.scores.end()) {
                if (existing->second != score) {
                    auto& old_group = entry.sorted_value[existing->second];
                    old_group.erase(args[3]);
                    if (old_group.empty()) entry.sorted_value.erase(existing->second);
                    entry.sorted_value[score].insert(args[3]);
                    existing->second = score;
                }
                return ":0\r\n";
            }
            entry.scores.emplace(args[3], score);
            entry.sorted_value[score].insert(args[3]);
            return ":1\r\n";
        }
        if (command == "ZSCORE" && args.size() == 3U) {
            const auto found = entries_.find(args[1]);
            if (found == entries_.end()) return "$-1\r\n";
            if (found->second.type != Entry::Type::SortedSet) {
                return Error("WRONGTYPE Operation against a key holding the wrong kind of value");
            }
            const auto score = found->second.scores.find(args[2]);
            if (score == found->second.scores.end()) return "$-1\r\n";
            return Bulk(std::to_string(score->second));
        }
        if (command == "ZREVRANGE" &&
            (args.size() == 4U ||
             (args.size() == 5U && Upper(args[4]) == "WITHSCORES"))) {
            const auto start = ParseInteger(args[2]);
            const auto stop = ParseInteger(args[3]);
            const auto found = entries_.find(args[1]);
            if (found == entries_.end()) return "*0\r\n";
            if (found->second.type != Entry::Type::SortedSet) {
                return Error("WRONGTYPE Operation against a key holding the wrong kind of value");
            }
            const auto length = static_cast<std::int64_t>(found->second.scores.size());
            auto first = start < 0 ? std::max<std::int64_t>(length + start, 0) : start;
            auto last = stop < 0 ? length + stop : stop;
            first = std::min(first, length);
            last = std::min(last, length - 1);
            std::vector<std::string> members;
            if (first <= last && length != 0) {
                std::int64_t index = 0;
                for (auto score = found->second.sorted_value.rbegin();
                     score != found->second.sorted_value.rend(); ++score) {
                    for (auto member = score->second.rbegin();
                         member != score->second.rend(); ++member) {
                        if (index >= first && index <= last) {
                            members.push_back(*member);
                            if (args.size() == 5U) {
                                members.push_back(std::to_string(score->first));
                            }
                        }
                        if (index++ >= last) break;
                    }
                    if (index > last) break;
                }
            }
            return Array(members);
        }
        if (command == "ZREMRANGEBYRANK" && args.size() == 4U) {
            const auto start = ParseInteger(args[2]);
            const auto stop = ParseInteger(args[3]);
            const auto found = entries_.find(args[1]);
            if (found == entries_.end()) return ":0\r\n";
            if (found->second.type != Entry::Type::SortedSet) {
                return Error("WRONGTYPE Operation against a key holding the wrong kind of value");
            }
            auto& entry = found->second;
            const auto length = static_cast<std::int64_t>(entry.scores.size());
            const auto first = start < 0 ? std::max<std::int64_t>(length + start, 0)
                                         : std::min(start, length);
            const auto last = stop < 0 ? length + stop : std::min(stop, length - 1);
            if (first > last || length == 0) return ":0\r\n";
            std::vector<std::pair<std::int64_t, std::string>> removed;
            std::int64_t index = 0;
            for (const auto& [score, members] : entry.sorted_value) {
                for (const auto& member : members) {
                    if (index >= first && index <= last) {
                        removed.emplace_back(score, member);
                    }
                    ++index;
                    if (index > last) break;
                }
                if (index > last) break;
            }
            for (const auto& [score, member] : removed) {
                entry.scores.erase(member);
                auto group = entry.sorted_value.find(score);
                group->second.erase(member);
                if (group->second.empty()) entry.sorted_value.erase(group);
            }
            return ":" + std::to_string(removed.size()) + "\r\n";
        }
        if (command == "SCAN" && args.size() == 6U && Upper(args[2]) == "MATCH" &&
            Upper(args[4]) == "COUNT") {
            const auto cursor = ParseInteger(args[1]);
            const auto count = std::clamp<std::int64_t>(ParseInteger(args[5]), 1, 10000);
            const auto pattern = args[3];
            if (cursor < 0 || pattern.empty() || pattern.back() != '*' ||
                pattern.find_first_of("?[]") != std::string::npos ||
                pattern.find('*') != pattern.size() - 1U) {
                return Error("FeedKV SCAN supports a prefix followed by one wildcard");
            }
            const auto prefix = pattern.substr(0, pattern.size() - 1U);
            std::vector<std::string> keys;
            for (const auto& [key, unused] : entries_) {
                (void)unused;
                if (key.rfind(prefix, 0) == 0) keys.push_back(key);
            }
            std::sort(keys.begin(), keys.end());
            if (static_cast<std::uint64_t>(cursor) > keys.size()) {
                return Error("invalid cursor");
            }
            const auto begin = static_cast<std::size_t>(cursor);
            const auto end = std::min(keys.size(), begin + static_cast<std::size_t>(count));
            std::vector<std::string> page(keys.begin() + begin, keys.begin() + end);
            const auto next_cursor = end == keys.size() ? 0 : end;
            return "*2\r\n" + Bulk(std::to_string(next_cursor)) + Array(page);
        }
        if (command == "DEL" && args.size() >= 2U) {
            std::int64_t removed = 0;
            for (std::size_t i = 1; i < args.size(); ++i) {
                removed += entries_.erase(args[i]) == 1U ? 1 : 0;
            }
            return ":" + std::to_string(removed) + "\r\n";
        }
        return Error("unsupported command or invalid arguments");
    }

private:
    std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;

    static std::string Upper(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c);
        });
        return value;
    }

    Entry& GetEntry(const std::string& key, Entry::Type type) {
        auto [found, inserted] = entries_.try_emplace(key);
        if (inserted) found->second.type = type;
        if (found->second.type != type) {
            throw std::invalid_argument("WRONGTYPE Operation against a key holding the wrong kind of value");
        }
        return found->second;
    }
};

FeedKvStore& Store() {
    static FeedKvStore store;
    return store;
}

bool SendAll(NativeSocket socket, const std::string& response) {
    std::size_t sent = 0;
    while (sent < response.size()) {
#ifdef _WIN32
        const auto remaining = static_cast<int>(std::min<std::size_t>(
            response.size() - sent, static_cast<std::size_t>(INT_MAX)));
        const auto count = send(socket, response.data() + sent, remaining, 0);
#else
        const auto count = send(socket, response.data() + sent,
                                response.size() - sent, 0);
#endif
        if (count <= 0) return false;
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

void ServeConnection(NativeSocket socket) {
    CommandReader reader(socket);
    try {
        std::vector<std::string> command;
        while (reader.Read(command)) {
            std::string response;
            try {
                response = Store().Execute(command);
            } catch (const std::exception& error) {
                response = Error(error.what());
            }
            if (!SendAll(socket, response)) break;
        }
    } catch (...) {
        // A malformed client is disconnected without affecting other sessions.
    }
    CloseSocket(socket);
}

} // namespace

int RunFeedKvServerImpl(std::uint16_t port, bool container_listen) {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        throw std::runtime_error("could not initialize Winsock");
    }
#endif
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* addresses = nullptr;
    const auto service = std::to_string(port);
    const auto address_text = container_listen ? "0.0.0.0" : "127.0.0.1";
    const auto status = getaddrinfo(address_text, service.c_str(), &hints, &addresses);
    if (status != 0) throw std::runtime_error("could not resolve FeedKV loopback address");

    NativeSocket listener = kInvalidSocket;
    for (auto* address = addresses; address != nullptr; address = address->ai_next) {
        const auto candidate = socket(address->ai_family, address->ai_socktype,
                                      address->ai_protocol);
        if (candidate == kInvalidSocket) continue;
        int reuse = 1;
#ifdef _WIN32
        setsockopt(candidate, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
        setsockopt(candidate, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
        if (bind(candidate, address->ai_addr,
                 static_cast<int>(address->ai_addrlen)) == 0 && listen(candidate, 64) == 0) {
            listener = candidate;
            break;
        }
        CloseSocket(candidate);
    }
    freeaddrinfo(addresses);
    if (listener == kInvalidSocket) {
        throw std::runtime_error("could not bind FeedKV to 127.0.0.1:" + service +
                                 " (socket error " + std::to_string(SocketError()) + ")");
    }

    std::cout << "FeedKV listening on " << address_text << ":" << port
              << " (volatile RESP2 subset; press Ctrl+C to stop)\n";
    while (true) {
        const auto client = accept(listener, nullptr, nullptr);
        if (client == kInvalidSocket) continue;
        std::thread(ServeConnection, client).detach();
    }
}

} // namespace benchforge
