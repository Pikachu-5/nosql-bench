#include "benchforge/control_api.hpp"

#include "benchforge/adapter.hpp"
#include "benchforge/config.hpp"
#include "child_process.hpp"
#include "result_archive.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace benchforge {
namespace {

constexpr std::size_t kMaximumHeaderBytes = 16U * 1024U;
constexpr std::size_t kMaximumBodyBytes = 32U * 1024U;
constexpr std::size_t kMaximumResultBytes = 1024U * 1024U;

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
void CloseSocket(Socket socket) noexcept { closesocket(socket); }
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
void CloseSocket(Socket socket) noexcept { close(socket); }
#endif

struct SocketGuard {
    explicit SocketGuard(Socket value) : socket(value) {}
    ~SocketGuard() {
        if (socket != kInvalidSocket) {
            CloseSocket(socket);
        }
    }
    Socket socket;
};

struct SocketRuntime {
    SocketRuntime() {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("could not initialize Windows sockets");
        }
#endif
    }
    ~SocketRuntime() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

struct HttpRequest {
    std::string method;
    std::string target;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    int status{200};
    std::string content_type{"application/json; charset=utf-8"};
    std::string body;
};

class HttpError : public std::runtime_error {
public:
    HttpError(int status_code, const std::string& message)
        : std::runtime_error(message), status(status_code) {}
    int status;
};

std::string_view Trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1U);
}

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool ReceiveBytes(Socket socket, std::string& buffer) {
    std::array<char, 4096> chunk{};
#ifdef _WIN32
    const int received = recv(socket, chunk.data(), static_cast<int>(chunk.size()), 0);
#else
    const auto received = recv(socket, chunk.data(), chunk.size(), 0);
#endif
    if (received <= 0) {
        return false;
    }
    buffer.append(chunk.data(), static_cast<std::size_t>(received));
    return true;
}

HttpRequest ReadRequest(Socket socket) {
    std::string raw;
    std::size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        if (raw.size() >= kMaximumHeaderBytes) {
            throw HttpError(413, "request headers are too large");
        }
        if (!ReceiveBytes(socket, raw)) {
            throw HttpError(400, "incomplete request headers");
        }
        header_end = raw.find("\r\n\r\n");
        if (header_end == std::string::npos && raw.size() > kMaximumHeaderBytes) {
            throw HttpError(413, "request headers are too large");
        }
    }

    HttpRequest request;
    const std::string header_text = raw.substr(0, header_end);
    std::istringstream lines(header_text);
    std::string line;
    if (!std::getline(lines, line)) {
        throw HttpError(400, "missing request line");
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    std::istringstream request_line(line);
    std::string version;
    if (!(request_line >> request.method >> request.target >> version) ||
        version != "HTTP/1.1") {
        throw HttpError(400, "expected an HTTP/1.1 request line");
    }
    std::string unexpected_token;
    if (request_line >> unexpected_token) {
        throw HttpError(400, "malformed request line");
    }

    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            throw HttpError(400, "malformed request header");
        }
        const auto key_view = Trim(std::string_view(line).substr(0, colon));
        const auto value_view = Trim(std::string_view(line).substr(colon + 1U));
        if (key_view.empty()) {
            throw HttpError(400, "empty request header name");
        }
        const std::string key = LowerAscii(std::string(key_view));
        if (!request.headers.emplace(key, std::string(value_view)).second) {
            throw HttpError(400, "duplicate request header");
        }
    }

    if (request.headers.contains("transfer-encoding")) {
        throw HttpError(400, "chunked request bodies are not supported");
    }
    std::size_t content_length = 0;
    if (const auto it = request.headers.find("content-length");
        it != request.headers.end()) {
        const auto parsed = std::from_chars(
            it->second.data(), it->second.data() + it->second.size(), content_length);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != it->second.data() + it->second.size()) {
            throw HttpError(400, "invalid Content-Length");
        }
    }
    if (content_length > kMaximumBodyBytes) {
        throw HttpError(413, "request body is too large");
    }
    const auto body_start = header_end + 4U;
    if (raw.size() > body_start + content_length) {
        throw HttpError(400, "multiple requests per connection are not supported");
    }
    while (raw.size() < body_start + content_length) {
        if (!ReceiveBytes(socket, raw)) {
            throw HttpError(400, "incomplete request body");
        }
        if (raw.size() > body_start + content_length) {
            throw HttpError(400, "multiple requests per connection are not supported");
        }
    }
    request.body = raw.substr(body_start, content_length);
    return request;
}

std::string JsonEscape(std::string_view value) {
    std::ostringstream escaped;
    for (const unsigned char character : value) {
        switch (character) {
        case '"': escaped << "\\\""; break;
        case '\\': escaped << "\\\\"; break;
        case '\b': escaped << "\\b"; break;
        case '\f': escaped << "\\f"; break;
        case '\n': escaped << "\\n"; break;
        case '\r': escaped << "\\r"; break;
        case '\t': escaped << "\\t"; break;
        default:
            if (character < 0x20U) {
                escaped << "\\u00" << std::hex << std::setw(2)
                        << std::setfill('0')
                        << static_cast<unsigned int>(character) << std::dec;
            } else {
                escaped << static_cast<char>(character);
            }
        }
    }
    return escaped.str();
}

std::string JsonError(std::string_view message) {
    return "{\"error\":\"" + JsonEscape(message) + "\"}";
}

std::string StatusText(int status) {
    switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Error";
    }
}

bool SendAll(Socket socket, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
#ifdef _WIN32
        const int count = send(socket, data.data() + sent,
                               static_cast<int>(data.size() - sent), 0);
#else
        const auto count = send(socket, data.data() + sent,
                                data.size() - sent, MSG_NOSIGNAL);
#endif
        if (count <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

void SendResponse(Socket socket, const HttpResponse& response,
                  std::string_view allowed_origin = {}) {
    std::ostringstream headers;
    headers << "HTTP/1.1 " << response.status << ' '
            << StatusText(response.status) << "\r\n"
            << "Content-Type: " << response.content_type << "\r\n"
            << "Content-Length: " << response.body.size() << "\r\n"
            << "Connection: close\r\n"
            << "Cache-Control: no-store\r\n"
            << "X-Content-Type-Options: nosniff\r\n";
    if (!allowed_origin.empty()) {
        headers << "Access-Control-Allow-Origin: " << allowed_origin << "\r\n"
                << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                << "Access-Control-Allow-Headers: Content-Type\r\n"
                << "Access-Control-Max-Age: 600\r\n"
                << "Vary: Origin\r\n";
    }
    headers << "\r\n";
    (void)SendAll(socket, headers.str());
    (void)SendAll(socket, response.body);
}

std::string UrlDecode(std::string_view value) {
    std::string decoded;
    decoded.reserve(value.size());
    auto hex_value = [](char character) -> int {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        if (character >= 'A' && character <= 'F') return character - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+') {
            decoded.push_back(' ');
        } else if (value[i] == '%') {
            if (i + 2U >= value.size()) {
                throw HttpError(400, "malformed form encoding");
            }
            const int high = hex_value(value[i + 1U]);
            const int low = hex_value(value[i + 2U]);
            if (high < 0 || low < 0) {
                throw HttpError(400, "malformed form encoding");
            }
            const char decoded_character = static_cast<char>((high << 4) | low);
            if (decoded_character == '\0' || decoded_character == '\r' ||
                decoded_character == '\n') {
                throw HttpError(400, "control characters are not allowed in form values");
            }
            decoded.push_back(decoded_character);
            i += 2U;
        } else {
            decoded.push_back(value[i]);
        }
    }
    return decoded;
}

std::map<std::string, std::string> ParseForm(std::string_view body) {
    std::map<std::string, std::string> values;
    std::size_t position = 0;
    while (position <= body.size()) {
        const auto ampersand = body.find('&', position);
        const auto end = ampersand == std::string_view::npos ? body.size() : ampersand;
        const auto entry = body.substr(position, end - position);
        const auto equals = entry.find('=');
        if (equals == std::string_view::npos) {
            throw HttpError(400, "form fields must use key=value syntax");
        }
        const auto key = UrlDecode(entry.substr(0, equals));
        const auto value = UrlDecode(entry.substr(equals + 1U));
        if (key.empty() || !values.emplace(key, value).second) {
            throw HttpError(400, "form field names must be non-empty and unique");
        }
        if (ampersand == std::string_view::npos) {
            break;
        }
        position = ampersand + 1U;
    }
    return values;
}

template <typename T>
T ParseUnsigned(const std::string& key, const std::string& text) {
    T result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        throw HttpError(400, "invalid integer in field '" + key + "'");
    }
    return result;
}

RunConfig ParseRunConfig(const HttpRequest& request) {
    const auto content_type = request.headers.find("content-type");
    if (content_type == request.headers.end()) {
        throw HttpError(415, "use application/x-www-form-urlencoded");
    }
    const auto content_type_end = content_type->second.find(';');
    const auto media_type = LowerAscii(std::string(Trim(std::string_view(
        content_type->second).substr(0, content_type_end))));
    if (media_type != "application/x-www-form-urlencoded") {
        throw HttpError(415, "use application/x-www-form-urlencoded");
    }

    const auto fields = ParseForm(request.body);
    RunConfig config;
    for (const auto& [key, value] : fields) {
        if (key == "adapter") {
            config.adapter = value;
        } else if (key == "database_host") {
            config.database_host = value;
        } else if (key == "database_port") {
            config.database_port = ParseUnsigned<std::uint16_t>(key, value);
        } else if (key == "mode") {
            if (!ParseRunMode(value, config.mode)) {
                throw HttpError(400, "mode must be closed_loop or open_loop");
            }
        } else if (key == "offered_rate_ops_sec") {
            config.offered_rate_ops_sec = ParseUnsigned<std::uint64_t>(key, value);
        } else if (key == "scenario") {
            if (!ParseScenario(value, config.scenario)) {
                throw HttpError(400, "scenario must be normal or celebrity");
            }
        } else if (key == "seed") {
            config.seed = ParseUnsigned<std::uint64_t>(key, value);
        } else if (key == "workers") {
            config.workers = ParseUnsigned<std::uint32_t>(key, value);
        } else if (key == "warmup_ms") {
            config.warmup_ms = ParseUnsigned<std::uint32_t>(key, value);
        } else if (key == "duration_ms") {
            config.duration_ms = ParseUnsigned<std::uint32_t>(key, value);
        } else if (key == "users") {
            config.users = ParseUnsigned<std::uint64_t>(key, value);
        } else if (key == "posts") {
            config.posts = ParseUnsigned<std::uint64_t>(key, value);
        } else if (key == "follows") {
            config.follows = ParseUnsigned<std::uint64_t>(key, value);
        } else if (key == "hashtags") {
            config.hashtags = ParseUnsigned<std::uint64_t>(key, value);
        } else if (key == "celebrity_post_percent") {
            config.celebrity_post_percent = ParseUnsigned<std::uint8_t>(key, value);
        } else if (key.rfind("weight.", 0) == 0) {
            OperationType type{};
            if (!ParseOperationType(std::string_view(key).substr(7), type)) {
                throw HttpError(400, "unknown operation weight field '" + key + "'");
            }
            config.weights[static_cast<std::size_t>(type)] =
                ParseUnsigned<std::uint8_t>(key, value);
        } else {
            throw HttpError(400, "unknown or disallowed field '" + key + "'");
        }
    }
    const auto errors = ValidateConfig(config);
    if (!errors.empty()) {
        std::string message;
        for (std::size_t i = 0; i < errors.size(); ++i) {
            if (i != 0) message += "; ";
            message += errors[i];
        }
        throw HttpError(400, message);
    }
    const auto adapters = AvailableAdapters();
    if (std::find(adapters.begin(), adapters.end(), config.adapter) == adapters.end()) {
        throw HttpError(400, "adapter is not available");
    }
    return config;
}

void WriteWorkerConfig(const std::filesystem::path& path, const RunConfig& config) {
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        throw std::runtime_error("could not write worker configuration");
    }
    output << "adapter = " << config.adapter << '\n'
           << "database_host = " << config.database_host << '\n'
           << "database_port = " << config.database_port << '\n'
           << "mode = " << ToString(config.mode) << '\n'
           << "offered_rate_ops_sec = " << config.offered_rate_ops_sec << '\n'
           << "scenario = " << ToString(config.scenario) << '\n'
           << "seed = " << config.seed << '\n'
           << "workers = " << config.workers << '\n'
           << "warmup_ms = " << config.warmup_ms << '\n'
           << "duration_ms = " << config.duration_ms << '\n'
           << "users = " << config.users << '\n'
           << "posts = " << config.posts << '\n'
           << "follows = " << config.follows << '\n'
           << "hashtags = " << config.hashtags << '\n'
           << "celebrity_post_percent = "
           << static_cast<unsigned>(config.celebrity_post_percent) << '\n'
           << "output_dir = " << config.output_dir << '\n';
    for (std::size_t i = 0; i < kOperationCount; ++i) {
        output << "weight." << ToString(kOperationTypes[i]) << " = "
               << static_cast<unsigned>(config.weights[i]) << '\n';
    }
    if (!output) {
        throw std::runtime_error("failed while writing worker configuration");
    }
}

std::string NewRunId() {
    static std::atomic<std::uint64_t> sequence{0};
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return "run_" + std::to_string(now) + "_" +
           std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

std::string CurrentUtc() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

struct RunRecord {
    std::string id;
    std::string adapter;
    std::string scenario;
    std::string status{"running"};
    std::string error;
    std::string created_at_utc;
    std::uint32_t workers{0};
    std::uint32_t duration_ms{0};
    std::optional<int> exit_code;
    bool cancel_requested{false};
    std::filesystem::path result_directory;
    std::shared_ptr<detail::ChildProcess> process;
};

struct RunSnapshot {
    std::string id;
    std::string adapter;
    std::string scenario;
    std::string status;
    std::string error;
    std::string created_at_utc;
    std::uint32_t workers{0};
    std::uint32_t duration_ms{0};
    std::optional<int> exit_code;
};

RunSnapshot Snapshot(const RunRecord& run) {
    return {run.id, run.adapter, run.scenario,
            run.cancel_requested && run.status == "running"
                ? "cancelling" : run.status,
            run.error, run.created_at_utc, run.workers, run.duration_ms,
            run.exit_code};
}

std::string SerializeRun(const RunSnapshot& run) {
    std::ostringstream output;
    output << "{\"runId\":\"" << JsonEscape(run.id)
           << "\",\"status\":\"" << JsonEscape(run.status)
           << "\",\"adapter\":\"" << JsonEscape(run.adapter)
           << "\",\"scenario\":\"" << JsonEscape(run.scenario)
           << "\",\"createdAtUtc\":\"" << JsonEscape(run.created_at_utc)
           << "\",\"workers\":" << run.workers
           << ",\"durationMs\":" << run.duration_ms
           << ",\"exitCode\":";
    if (run.exit_code) output << *run.exit_code;
    else output << "null";
    output << ",\"error\":\"" << JsonEscape(run.error) << "\"}";
    return output.str();
}

class RunManager {
public:
    std::filesystem::path ArchiveRoot() const { return output_root_.parent_path(); }
    RunManager(std::filesystem::path executable, std::filesystem::path workspace)
        : executable_(std::filesystem::absolute(std::move(executable))),
          output_root_(std::filesystem::absolute(workspace / "runs" / "api")) {}

    ~RunManager() {
        {
            std::lock_guard lock(mutex_);
            for (const auto& run : runs_) {
                if (run->status == "running" && run->process) {
                    run->process->Terminate();
                }
            }
        }
        for (auto& monitor : monitors_) {
            if (monitor.joinable()) monitor.join();
        }
    }

    RunSnapshot Start(RunConfig config) {
        std::lock_guard lock(mutex_);
        for (const auto& existing : runs_) {
            if (existing->status == "running") {
                throw HttpError(409, "a benchmark run is already active");
            }
        }
        if (runs_.size() >= 500) {
            throw HttpError(503, "this control session has reached its run history limit");
        }

        do {
            config.run_id = NewRunId();
        } while (std::filesystem::exists(output_root_ / config.run_id));
        config.output_dir = output_root_.string();
        const auto errors = ValidateConfig(config);
        if (!errors.empty()) {
            throw HttpError(400, errors.front());
        }
        const auto result_directory = output_root_ / config.run_id;
        std::filesystem::create_directories(result_directory);
        const auto config_path = result_directory / "worker.conf";
        WriteWorkerConfig(config_path, config);
        auto process = detail::ChildProcess::Start(executable_, config_path,
                                                   config.run_id);

        auto run = std::make_shared<RunRecord>();
        run->id = config.run_id;
        run->adapter = config.adapter;
        run->scenario = ToString(config.scenario);
        run->created_at_utc = CurrentUtc();
        run->workers = config.workers;
        run->duration_ms = config.duration_ms;
        run->result_directory = result_directory;
        run->process = std::move(process);
        runs_.push_back(run);
        monitors_.emplace_back([this, run] { Monitor(run); });
        return Snapshot(*run);
    }

    std::vector<RunSnapshot> List() const {
        std::lock_guard lock(mutex_);
        std::vector<RunSnapshot> snapshots;
        snapshots.reserve(runs_.size());
        for (const auto& run : runs_) snapshots.push_back(Snapshot(*run));
        return snapshots;
    }

    std::optional<RunSnapshot> Find(std::string_view id) const {
        std::lock_guard lock(mutex_);
        const auto found = std::find_if(runs_.begin(), runs_.end(),
            [id](const auto& run) { return run->id == id; });
        if (found == runs_.end()) return std::nullopt;
        return Snapshot(**found);
    }

    RunSnapshot Cancel(std::string_view id) {
        std::lock_guard lock(mutex_);
        const auto found = std::find_if(runs_.begin(), runs_.end(),
            [id](const auto& run) { return run->id == id; });
        if (found == runs_.end()) {
            throw HttpError(404, "run was not found");
        }
        const auto& run = *found;
        if (run->status != "running") {
            throw HttpError(409, "run is no longer active");
        }
        run->cancel_requested = true;
        if (run->process) run->process->Terminate();
        return Snapshot(*run);
    }

    std::filesystem::path ResultDirectory(std::string_view id) const {
        std::lock_guard lock(mutex_);
        const auto found = std::find_if(runs_.begin(), runs_.end(),
            [id](const auto& run) { return run->id == id; });
        if (found == runs_.end()) {
            throw HttpError(404, "run was not found");
        }
        if ((*found)->status != "succeeded") {
            throw HttpError(409, "results are available only after a successful run");
        }
        return (*found)->result_directory;
    }

private:
    void Monitor(const std::shared_ptr<RunRecord>& run) {
        int exit_code = 1;
        std::string failure;
        try {
            exit_code = run->process->Wait();
        } catch (const std::exception& error) {
            failure = error.what();
        }
        std::error_code filesystem_error;
        const bool has_summary = std::filesystem::exists(
            run->result_directory / "summary.json", filesystem_error);
        if (filesystem_error && failure.empty()) {
            failure = filesystem_error.message();
        }
        std::lock_guard lock(mutex_);
        run->exit_code = exit_code;
        if (run->cancel_requested) {
            run->status = "cancelled";
        } else if (failure.empty() && exit_code == 0 && has_summary) {
            run->status = "succeeded";
        } else {
            run->status = "failed";
            run->error = failure.empty()
                ? "worker exited without a successful result (exit " +
                      std::to_string(exit_code) + ")"
                : failure;
        }
    }

    std::filesystem::path executable_;
    std::filesystem::path output_root_;
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<RunRecord>> runs_;
    std::vector<std::thread> monitors_;
};

bool IsRunId(std::string_view id) {
    return !id.empty() && id.size() <= 64 &&
        std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return std::isalnum(c) != 0 || c == '_' || c == '-';
        });
}

std::string ReadResult(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw HttpError(404, "run results are not available");
    }
    input.seekg(0, std::ios::end);
    const auto length = input.tellg();
    if (length < 0 || static_cast<std::uint64_t>(length) > kMaximumResultBytes) {
        throw HttpError(413, "result file exceeds API response limit");
    }
    input.seekg(0, std::ios::beg);
    std::string data(static_cast<std::size_t>(length), '\0');
    input.read(data.data(), static_cast<std::streamsize>(data.size()));
    if (!input && !data.empty()) {
        throw HttpError(500, "could not read result file");
    }
    return data;
}

HttpResponse HandleApiRequest(const HttpRequest& request, RunManager& runs,
                              std::uint16_t port) {
    if (request.target.find('?') != std::string::npos ||
        request.target.find('#') != std::string::npos) {
        throw HttpError(400, "query strings and fragments are not supported");
    }
    const auto host = request.headers.find("host");
    const auto expected_port = ":" + std::to_string(port);
    if (host == request.headers.end() ||
        (host->second != "127.0.0.1" + expected_port &&
         host->second != "localhost" + expected_port)) {
        throw HttpError(403, "Host header is not a local address and port");
    }

    if (request.method == "OPTIONS") {
        return {204, "application/json; charset=utf-8", {}};
    }

    if (request.method == "GET" && request.target == "/health") {
        return {200, "application/json; charset=utf-8",
                "{\"status\":\"ok\",\"service\":\"benchforge-control\"}"};
    }
    if (request.method == "GET" && request.target == "/api/adapters") {
        std::ostringstream body;
        body << "{\"adapters\":[";
        const auto adapters = AvailableAdapters();
        for (std::size_t i = 0; i < adapters.size(); ++i) {
            if (i != 0) body << ',';
            body << "{\"name\":\"" << JsonEscape(adapters[i]) << "\"}";
        }
        body << "]}";
        return {200, "application/json; charset=utf-8", body.str()};
    }
    if (request.method == "GET" && request.target == "/api/runs") {
        std::ostringstream body;
        body << "{\"runs\":[";
        const auto history = runs.List();
        for (std::size_t i = 0; i < history.size(); ++i) {
            if (i != 0) body << ',';
            body << SerializeRun(history[i]);
        }
        body << "]}";
        return {200, "application/json; charset=utf-8", body.str()};
    }
    if (request.method == "GET" && request.target == "/api/results") {
        return {200, "application/json; charset=utf-8", detail::ListArchivedResults(runs.ArchiveRoot()).dump()};
    }
    constexpr std::string_view archive_prefix = "/api/results/";
    if (request.method == "GET" && request.target.rfind(archive_prefix, 0) == 0) {
        try {
            return {200, "application/json; charset=utf-8", detail::ReadArchivedResult(
                runs.ArchiveRoot(), request.target.substr(archive_prefix.size())).dump()};
        } catch (const std::exception&) { throw HttpError(404, "saved result was not found or is unreadable"); }
    }
    if (request.method == "POST" && request.target == "/api/runs") {
        const auto run = runs.Start(ParseRunConfig(request));
        return {202, "application/json; charset=utf-8", SerializeRun(run)};
    }

    constexpr std::string_view prefix = "/api/runs/";
    if (request.target.rfind(prefix, 0) == 0) {
        const auto suffix = std::string_view(request.target).substr(prefix.size());
        const auto slash = suffix.find('/');
        const auto id = suffix.substr(0, slash);
        if (!IsRunId(id)) {
            throw HttpError(404, "run was not found");
        }
        const auto run = runs.Find(id);
        if (!run) {
            throw HttpError(404, "run was not found");
        }
        const auto tail = slash == std::string_view::npos
            ? std::string_view{} : suffix.substr(slash + 1U);
        if (request.method == "GET" && tail.empty()) {
            return {200, "application/json; charset=utf-8", SerializeRun(*run)};
        }
        if (request.method == "POST" && tail == "cancel") {
            return {202, "application/json; charset=utf-8", SerializeRun(runs.Cancel(id))};
        }
        if (request.method == "GET" && tail == "results") {
            return {200, "application/json; charset=utf-8",
                    ReadResult(runs.ResultDirectory(id) / "summary.json")};
        }
        throw HttpError(404, "API route was not found");
    }
    throw HttpError(404, "API route was not found");
}

bool IsAllowedOrigin(std::string_view origin) {
    return origin == "http://localhost:5180" ||
           origin == "http://127.0.0.1:5180";
}

void SetReceiveTimeout(Socket socket) {
#ifdef _WIN32
    const DWORD timeout_ms = 10000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    const timeval timeout{10, 0};
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
}

void HandleConnection(Socket socket, RunManager& runs, std::uint16_t port) {
    SetReceiveTimeout(socket);
    std::string origin;
    try {
        const auto request = ReadRequest(socket);
        if (const auto found = request.headers.find("origin");
            found != request.headers.end()) {
            origin = found->second;
            if (!IsAllowedOrigin(origin)) {
                throw HttpError(403, "browser origin is not allowed");
            }
        }
        const auto response = HandleApiRequest(request, runs, port);
        SendResponse(socket, response, origin);
    } catch (const HttpError& error) {
        SendResponse(socket, {error.status, "application/json; charset=utf-8",
                              JsonError(error.what())}, origin);
    } catch (const std::exception& error) {
        SendResponse(socket, {500, "application/json; charset=utf-8",
                              JsonError(error.what())}, origin);
    }
}

} // namespace

int RunControlApi(const std::filesystem::path& executable_path,
                  std::uint16_t port) {
    SocketRuntime runtime;
    auto executable = std::filesystem::absolute(executable_path);
    auto workspace = executable.parent_path().parent_path();
    if (!std::filesystem::exists(workspace / "config" / "default.conf")) {
        workspace = executable.parent_path();
    }

    const Socket listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == kInvalidSocket) {
        throw std::runtime_error("could not create control API socket");
    }
    SocketGuard listener_guard(listener);
    int reuse_address = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse_address), sizeof(reuse_address));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
        throw std::runtime_error("could not parse loopback address");
    }
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) != 0) {
        throw std::runtime_error("could not bind control API to 127.0.0.1:" +
                                 std::to_string(port));
    }
    if (listen(listener, 16) != 0) {
        throw std::runtime_error("could not listen for control API requests");
    }

    RunManager runs(std::filesystem::absolute(executable), workspace);
    std::cout << "BenchForge control API listening at http://127.0.0.1:"
              << port << " (loopback only)\n";
    while (true) {
        sockaddr_in client_address{};
#ifdef _WIN32
        int client_length = sizeof(client_address);
#else
        socklen_t client_length = sizeof(client_address);
#endif
        const Socket client = accept(listener,
            reinterpret_cast<sockaddr*>(&client_address), &client_length);
        if (client == kInvalidSocket) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEINTR) continue;
#else
            if (errno == EINTR) continue;
#endif
            throw std::runtime_error("control API accept failed");
        }
        SocketGuard client_guard(client);
        HandleConnection(client, runs, port);
    }
}

} // namespace benchforge
