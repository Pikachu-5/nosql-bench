#pragma once
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
namespace benchforge::detail {
// Read-only archive, bounded to runs/<id> and runs/<bucket>/<id>.
nlohmann::json ListArchivedResults(const std::filesystem::path& root);
nlohmann::json ReadArchivedResult(const std::filesystem::path& root, const std::string& key);
}
