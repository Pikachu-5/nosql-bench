#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <optional>

namespace benchforge::detail {

class ChildProcess {
public:
    static std::shared_ptr<ChildProcess> Start(
        const std::filesystem::path& executable,
        const std::filesystem::path& config_path,
        const std::string& run_id);

    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    int Wait();
    std::optional<int> Poll();
    void Terminate() noexcept;

private:
    ChildProcess() = default;

#ifdef _WIN32
    void* process_handle_{nullptr};
#else
    int process_id_{-1};
#endif
};

} // namespace benchforge::detail
