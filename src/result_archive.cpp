#include "result_archive.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <vector>
namespace benchforge::detail {
namespace {
using Json = nlohmann::json;
constexpr std::uintmax_t kMaximumBytes = 1024U * 1024U;
bool Identifier(const std::string& value) {
    return !value.empty() && value.size() <= 64 && std::all_of(value.begin(),value.end(),[](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c=='_' || c=='-';
    });
}
bool Directory(const std::filesystem::path& path) {
    const auto status=std::filesystem::symlink_status(path);
    return !std::filesystem::is_symlink(status) && std::filesystem::is_directory(status);
}
Json Read(const std::filesystem::path& path) {
    const auto status=std::filesystem::symlink_status(path / "summary.json");
    if (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status))
        throw std::runtime_error("saved result is not a regular file");
    if (std::filesystem::file_size(path / "summary.json") > kMaximumBytes)
        throw std::runtime_error("saved result exceeds 1 MiB");
    std::ifstream file(path / "summary.json",std::ios::binary);
    // Bound the actual read as well as the preflight size, including a concurrently growing file.
    std::string text; text.resize(static_cast<std::size_t>(kMaximumBytes)+1U);
    file.read(text.data(),static_cast<std::streamsize>(text.size()));
    const auto bytes=file.gcount();
    if (bytes < 0 || static_cast<std::uintmax_t>(bytes)>kMaximumBytes || file.bad())
        throw std::runtime_error("could not read bounded saved result");
    text.resize(static_cast<std::size_t>(bytes));
    auto result=Json::parse(text);
    if (!result.is_object() || result.value("schema_version",0)!=2 ||
        result.at("run_id").get<std::string>() != path.filename().string() ||
        !result.at("adapter").is_string() || !result.at("scenario").is_string() ||
        !result.at("started_at_utc").is_string() || !result.at("valid").is_boolean() ||
        !result.at("operations").is_array() || !result.at("config").is_object())
        throw std::runtime_error("saved result schema is incomplete or unsupported");
    if (result.at("adapter").get_ref<const std::string&>().size()>32 ||
        result.at("scenario").get_ref<const std::string&>().size()>32 ||
        result.at("started_at_utc").get_ref<const std::string&>().size()>64)
        throw std::runtime_error("saved result catalog metadata exceeds limits");
    for (const auto& key : {"experiment_id", "experiment_status", "experiment_profile", "resource_profile", "measurement_method"}) {
        if (result.contains(key) && (!result.at(key).is_string() || result.at(key).get_ref<const std::string&>().size() > 128))
            throw std::runtime_error("experiment catalog metadata exceeds limits");
    }
    return result;
}
}
Json ReadArchivedResult(const std::filesystem::path& root,const std::string& key) {
    const auto separator=key.find('~');
    if (separator==std::string::npos) throw std::runtime_error("invalid saved result key");
    const auto bucket=key.substr(0,separator), id=key.substr(separator+1);
    if ((!bucket.empty() && !Identifier(bucket)) || !Identifier(id) || !Directory(root))
        throw std::runtime_error("invalid saved result key");
    const auto parent=bucket.empty() ? root : root / bucket;
    const auto path=parent / id;
    if (!Directory(parent) || !Directory(path)) throw std::runtime_error("saved result was not found");
    return Read(path);
}
Json ListArchivedResults(const std::filesystem::path& root) {
    Json response{{"results",Json::array()},{"skipped",0},{"truncated",false}};
    if (!std::filesystem::exists(root)) return response;
    if (!Directory(root)) throw std::runtime_error("result root is not a regular directory");
    std::vector<Json> results;
    std::size_t scanned=0,skipped=0;
    std::uintmax_t bytes_scanned=0;
    auto inspect=[&](const std::filesystem::path& path,const std::string& bucket) {
        if (++scanned>5000) { response["truncated"]=true; return; }
        if (!Identifier(path.filename().string()) || !Directory(path)) return;
        if (!std::filesystem::exists(path / "summary.json")) return;
        try {
            const auto bytes=std::filesystem::file_size(path / "summary.json");
            if (bytes>kMaximumBytes) { ++skipped; return; }
            bytes_scanned+=bytes;
            if (bytes_scanned>32U*1024U*1024U) { response["truncated"]=true; scanned=5001; return; }
            const auto summary=Read(path);
            results.push_back({{"key",bucket+"~"+path.filename().string()},
                {"runId",summary.at("run_id")},{"adapter",summary.at("adapter")},
                {"scenario",summary.at("scenario")},{"startedAtUtc",summary.at("started_at_utc")},
                {"valid",summary.at("valid")},{"source",bucket.empty() ? "runs" : bucket},
                {"experimentId",summary.value("experiment_id",std::string{})},
                {"experimentProfile",summary.value("experiment_profile",std::string{})}});
        } catch (const std::exception&) { ++skipped; }
    };
    for (const auto& entry:std::filesystem::directory_iterator(root)) {
        if (++scanned>5000) { response["truncated"]=true; break; }
        const auto path=entry.path();
        if (!Identifier(path.filename().string()) || !Directory(path)) continue;
        if (std::filesystem::exists(path / "summary.json")) inspect(path,"");
        else for (const auto& child:std::filesystem::directory_iterator(path)) {
            inspect(child.path(),path.filename().string());
            if (scanned>5000) break;
        }
    }
    std::sort(results.begin(),results.end(),[](const Json& a,const Json& b) {
        return a.at("startedAtUtc")!=b.at("startedAtUtc") ? a.at("startedAtUtc")>b.at("startedAtUtc") : a.at("key")<b.at("key");
    });
    if (results.size()>500) { results.resize(500); response["truncated"]=true; }
    response["results"]=results; response["skipped"]=skipped;
    return response;
}
} // namespace benchforge::detail
