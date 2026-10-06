#pragma once

#include "benchforge/adapter.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <csignal>
#include <thread>
#include <chrono>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#include <cerrno>
#endif

namespace benchforge::detail {
inline std::uint64_t ProcessId() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}
inline bool ProcessAlive(std::uint64_t pid) {
    if (pid == 0 || pid > UINT32_MAX) throw std::runtime_error("invalid journal process ID");
#ifdef _WIN32
    const auto handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!handle) {
        if (GetLastError() == ERROR_INVALID_PARAMETER) return false;
        throw std::runtime_error("cannot verify journal process has exited");
    }
    const bool alive = WaitForSingleObject(handle, 0) != WAIT_OBJECT_0;
    CloseHandle(handle); return alive;
#else
    return kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
#endif
}
inline void WriteJournal(const std::filesystem::path& path, const nlohmann::json& journal) {
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << journal.dump(2) << '\n';
        output.flush();
        if (!output) throw std::runtime_error("could not write recovery journal");
    }
#ifdef _WIN32
    for (unsigned attempt=0;;++attempt) {
        if (MoveFileExW(std::filesystem::path(temporary).c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) break;
        const auto error = GetLastError();
        if (attempt==39 || (error!=ERROR_SHARING_VIOLATION && error!=ERROR_ACCESS_DENIED))
            throw std::runtime_error("could not commit recovery journal (Windows error " + std::to_string(error) + ")");
        // Short-lived readers on Windows may omit FILE_SHARE_DELETE.
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
#else
    std::filesystem::rename(temporary, path);
#endif
}
inline int RecoverRun(const std::filesystem::path& supplied) {
    const auto path = std::filesystem::absolute(supplied);
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(path)) ||
        !std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > 16384)
        throw std::runtime_error("recovery requires a regular journal under 16 KiB");
    std::ifstream input(path);
    auto journal = nlohmann::json::parse(input);
    input.close();
    RunConfig config;
    if (journal.at("schema") != 1) throw std::runtime_error("unsupported recovery journal");
    config.run_id = journal.at("run_id").get<std::string>();
    config.adapter = journal.at("adapter").get<std::string>();
    config.database_host = journal.at("database_host").get<std::string>();
    config.database_port = journal.at("database_port").get<std::uint16_t>();
    if (!ValidateConfig(config).empty() || path.filename() != "recovery.json" ||
        path.parent_path().filename().string() != config.run_id || config.run_id.empty())
        throw std::runtime_error("journal identity or local endpoint is invalid");
    if (ProcessAlive(journal.at("pid").get<std::uint64_t>()))
        throw std::runtime_error("refusing recovery while recorded worker is alive");
    if (!journal.at("namespace_owned").get<bool>()) return 0;
    const auto lock = path.parent_path() / "recovery.lock";
    if (!std::filesystem::create_directory(lock)) throw std::runtime_error("recovery already locked; inspect recovery.lock");
    struct Guard { std::filesystem::path path; ~Guard() { std::error_code error; std::filesystem::remove(path,error); } } guard{lock};
    auto adapter = CreateAdapter(config.adapter, config, "benchforge:" + config.run_id + ":");
    const auto result = adapter->RecoverNamespace();
    journal["cleanup_status"] = result;
    if (result.rfind("cleanup failed:",0) == 0) { WriteJournal(path,journal); throw std::runtime_error(result); }
    journal["namespace_owned"] = false;
    journal["state"] = "recovered";
    WriteJournal(path,journal);
    return 0;
}
}
