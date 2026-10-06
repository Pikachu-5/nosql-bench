#include "child_process.hpp"

#include <stdexcept>
#include <system_error>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace benchforge::detail {
namespace {

#ifdef _WIN32
std::wstring QuoteWindowsArgument(const std::wstring& value) {
    std::wstring result{L"\""};
    std::size_t backslashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            result.append(backslashes * 2U + 1U, L'\\');
            result.push_back(L'"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(character);
    }
    result.append(backslashes * 2U, L'\\');
    result.push_back(L'"');
    return result;
}
#endif

} // namespace

std::shared_ptr<ChildProcess> ChildProcess::Start(
    const std::filesystem::path& executable,
    const std::filesystem::path& config_path,
    const std::string& run_id) {
    auto child = std::shared_ptr<ChildProcess>(new ChildProcess());
#ifdef _WIN32
    const auto executable_string = std::filesystem::absolute(executable).wstring();
    const auto config_string = std::filesystem::absolute(config_path).wstring();
    std::wstring command_line = QuoteWindowsArgument(executable_string) +
                               L" worker --config " +
                               QuoteWindowsArgument(config_string) +
                               L" --run-id " +
                               QuoteWindowsArgument(
                                   std::wstring(run_id.begin(), run_id.end()));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable_string.c_str(), command_line.data(), nullptr,
                        nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &process)) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(), "could not start worker");
    }
    CloseHandle(process.hThread);
    child->process_handle_ = process.hProcess;
#else
    const auto executable_string = std::filesystem::absolute(executable).string();
    const auto config_string = std::filesystem::absolute(config_path).string();
    const pid_t process_id = fork();
    if (process_id < 0) {
        throw std::system_error(errno, std::generic_category(), "could not fork worker");
    }
    if (process_id == 0) {
        execl(executable_string.c_str(), executable_string.c_str(), "worker",
              "--config", config_string.c_str(), "--run-id", run_id.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    child->process_id_ = static_cast<int>(process_id);
#endif
    return child;
}

ChildProcess::~ChildProcess() {
#ifdef _WIN32
    if (process_handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(process_handle_));
    }
#endif
}

int ChildProcess::Wait() {
#ifdef _WIN32
    const auto process = static_cast<HANDLE>(process_handle_);
    if (process == nullptr) {
        throw std::logic_error("worker process handle is not initialized");
    }
    const DWORD wait_result = WaitForSingleObject(process, INFINITE);
    if (wait_result != WAIT_OBJECT_0) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(), "could not wait for worker");
    }
    DWORD exit_code = 1;
    if (!GetExitCodeProcess(process, &exit_code)) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(), "could not read worker exit code");
    }
    return static_cast<int>(exit_code);
#else
    int status = 0;
    pid_t result = -1;
    do {
        result = waitpid(static_cast<pid_t>(process_id_), &status, 0);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        throw std::system_error(errno, std::generic_category(), "could not wait for worker");
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
#endif
}

void ChildProcess::Terminate() noexcept {
#ifdef _WIN32
    if (process_handle_ != nullptr) {
        TerminateProcess(static_cast<HANDLE>(process_handle_), 1);
    }
#else
    if (process_id_ > 0) {
        kill(static_cast<pid_t>(process_id_), SIGTERM);
    }
#endif
}

} // namespace benchforge::detail
