#include "benchforge/environment.hpp"

#include <thread>
#include <limits>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace benchforge {

EnvironmentInfo CaptureEnvironment() {
    EnvironmentInfo info;
    info.logical_processors = std::thread::hardware_concurrency();
#ifdef _WIN32
    info.operating_system = "Windows";
    char host[256]{};
    DWORD host_size = static_cast<DWORD>(sizeof(host));
    if (GetComputerNameA(host, &host_size)) info.host_name.assign(host, host_size);

    SYSTEM_INFO system_info{};
    GetNativeSystemInfo(&system_info);
    switch (system_info.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64: info.architecture = "x86_64"; break;
    case PROCESSOR_ARCHITECTURE_ARM64: info.architecture = "arm64"; break;
    case PROCESSOR_ARCHITECTURE_INTEL: info.architecture = "x86"; break;
    default: info.architecture = "unknown"; break;
    }
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) info.total_memory_bytes = memory.ullTotalPhys;
#else
    struct utsname system_info{};
    if (uname(&system_info) == 0) {
        info.operating_system = std::string(system_info.sysname) + " " + system_info.release;
        info.architecture = system_info.machine;
    }
    char host[256]{};
    if (gethostname(host, sizeof(host)) == 0) {
        host[sizeof(host) - 1U] = '\0';
        info.host_name = host;
    }
    const long page_count = sysconf(_SC_PHYS_PAGES);
    const long page_size = sysconf(_SC_PAGE_SIZE);
    if (page_count > 0 && page_size > 0) {
        const auto pages = static_cast<std::uint64_t>(page_count);
        const auto bytes_per_page = static_cast<std::uint64_t>(page_size);
        if (pages <= std::numeric_limits<std::uint64_t>::max() / bytes_per_page) {
            info.total_memory_bytes = pages * bytes_per_page;
        }
    }
#endif
    return info;
}

} // namespace benchforge
