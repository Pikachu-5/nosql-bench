#include "benchforge/adapter.hpp"
#include "benchforge/config.hpp"
#include "benchforge/control_api.hpp"
#include "benchforge/results.hpp"
#include "benchforge/runner.hpp"
#include "feedkv_server.hpp"
#include "run_lifecycle.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <csignal>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void SignalCancellation(int) { interrupted = 1; }

struct Arguments {
    std::filesystem::path config_path;
    std::string output_dir;
    std::string run_id;
};

void PrintUsage(std::ostream& output) {
    output << "BenchForge CLI\n"
           << "Usage:\n"
           << "  benchforge list-adapters\n"
           << "  benchforge validate --config PATH\n"
           << "  benchforge run --config PATH [--output-dir PATH] [--run-id ID]\n"
           << "  benchforge feedkv [--port PORT]\n"
           << "  benchforge serve [--port PORT]\n";
}

Arguments ParseArguments(int argc, char** argv, int first_option) {
    Arguments arguments;
    for (int i = first_option; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--config" || option == "--output-dir" ||
            option == "--run-id") {
            if (i + 1 >= argc) {
                throw std::invalid_argument(option + " requires a value");
            }
            const std::string value = argv[++i];
            if (option == "--config") {
                arguments.config_path = value;
            } else if (option == "--output-dir") {
                arguments.output_dir = value;
            } else {
                arguments.run_id = value;
            }
        } else {
            throw std::invalid_argument("unknown option: " + option);
        }
    }
    if (arguments.config_path.empty()) {
        throw std::invalid_argument("--config PATH is required");
    }
    return arguments;
}

benchforge::RunConfig LoadAndValidate(const Arguments& arguments) {
    auto config = benchforge::LoadConfig(arguments.config_path);
    if (!arguments.output_dir.empty()) {
        config.output_dir = arguments.output_dir;
    }
    config.run_id = arguments.run_id;
    const auto errors = benchforge::ValidateConfig(config);
    if (!errors.empty()) {
        std::string message = "invalid configuration:";
        for (const auto& error : errors) {
            message += "\n  - " + error;
        }
        throw std::invalid_argument(message);
    }
    const auto adapters = benchforge::AvailableAdapters();
    if (std::find(adapters.begin(), adapters.end(), config.adapter) == adapters.end()) {
        throw std::invalid_argument("adapter '" + config.adapter +
                                    "' is not available; run list-adapters");
    }
    return config;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            PrintUsage(std::cerr);
            return 2;
        }
        const std::string command = argv[1];
        if (command == "recover") {
            if (argc != 4 || std::string(argv[2]) != "--manifest")
                throw std::invalid_argument("usage: benchforge recover --manifest PATH/recovery.json");
            return benchforge::detail::RecoverRun(argv[3]);
        }
        if (command == "--help" || command == "help") {
            PrintUsage(std::cout);
            return 0;
        }
        if (command == "list-adapters") {
            for (const auto& adapter : benchforge::AvailableAdapters()) {
                std::cout << adapter << '\n';
            }
            return 0;
        }
        if (command == "feedkv") {
            std::uint16_t port = 6380;
            bool container_listen = false;
            for (int i = 2; i < argc; ++i) {
                const std::string option = argv[i];
                if (option == "--container-listen") { container_listen = true; continue; }
                if (option != "--port" || i + 1 >= argc) {
                    throw std::invalid_argument("usage: benchforge feedkv [--port PORT]");
                }
                unsigned value = 0;
                const std::string raw_port = argv[++i];
                const auto parsed = std::from_chars(
                    raw_port.data(), raw_port.data() + raw_port.size(), value);
                if (parsed.ec != std::errc{} ||
                    parsed.ptr != raw_port.data() + raw_port.size() ||
                    value == 0 || value > 65535) {
                    throw std::invalid_argument("port must be between 1 and 65535");
                }
                port = static_cast<std::uint16_t>(value);
            }
            return benchforge::RunFeedKvServerImpl(port, container_listen);
        }
        if (command == "serve") {
            std::uint16_t port = 8080;
            for (int i = 2; i < argc; ++i) {
                const std::string option = argv[i];
                if (option != "--port" || i + 1 >= argc) {
                    throw std::invalid_argument("usage: benchforge serve [--port PORT]");
                }
                unsigned value = 0;
                const std::string raw_port = argv[++i];
                const auto parsed = std::from_chars(
                    raw_port.data(), raw_port.data() + raw_port.size(), value);
                if (parsed.ec != std::errc{} ||
                    parsed.ptr != raw_port.data() + raw_port.size() ||
                    value == 0 || value > 65535) {
                    throw std::invalid_argument("port must be between 1 and 65535");
                }
                port = static_cast<std::uint16_t>(value);
            }
            return benchforge::RunControlApi(
                std::filesystem::absolute(argv[0]), port);
        }
        if (command == "validate") {
            const auto arguments = ParseArguments(argc, argv, 2);
            const auto config = LoadAndValidate(arguments);
            std::cout << "configuration valid; adapter=" << config.adapter
                      << "; scenario=" << benchforge::ToString(config.scenario)
                      << "; mode=" << benchforge::ToString(config.mode)
                      << "; offered_rate_ops_sec=" << config.offered_rate_ops_sec
                      << "; workers=" << config.workers << '\n';
            return 0;
        }
        if (command == "run" || command == "worker") {
            const auto arguments = ParseArguments(argc, argv, 2);
            auto config = LoadAndValidate(arguments);
            if (config.run_id.empty()) config.run_id = "run_" + std::to_string(
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
            const auto directory = std::filesystem::absolute(std::filesystem::path(config.output_dir) / config.run_id);
            std::filesystem::create_directories(directory);
            const auto manifest = directory / "recovery.json";
            if (std::filesystem::exists(manifest) || std::filesystem::exists(directory / "summary.json"))
                throw std::runtime_error("run ID already has artifacts; choose a new ID");
            nlohmann::json journal{{"schema",1},{"pid",benchforge::detail::ProcessId()},
                {"run_id",config.run_id},{"adapter",config.adapter},{"database_host",config.database_host},
                {"database_port",config.database_port},{"namespace_owned",false},{"state","starting"}};
            benchforge::detail::WriteJournal(manifest,journal);
            std::signal(SIGINT,SignalCancellation); std::signal(SIGTERM,SignalCancellation);
            std::atomic<bool> cancelled{false};
            std::jthread watcher([&](std::stop_token stop) {
                while (!stop.stop_requested()) {
                    std::error_code error;
                    if (interrupted || std::filesystem::exists(directory / "cancel.request",error)) cancelled.store(true);
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            });
            auto summary = benchforge::RunBenchmark(config, [&] { return cancelled.load(); }, [&] {
                journal["namespace_owned"] = true; journal["state"] = "active";
                benchforge::detail::WriteJournal(manifest,journal);
            });
            if (cancelled.load() && summary.valid) { summary.valid = false; summary.invalid_reasons.emplace_back("run cancelled"); }
            journal["cleanup_status"] = summary.cleanup_status;
            if (summary.cleanup_status.rfind("cleanup failed:",0) != 0) journal["namespace_owned"] = false;
            journal["state"] = cancelled.load() ? "cancelled" : (summary.valid ? "completed" : "failed");
            benchforge::detail::WriteJournal(manifest,journal);
            const auto output_directory =
                benchforge::WriteResults(config, summary);
            std::cout << "run_id=" << summary.run_id << '\n'
                      << "adapter=" << summary.adapter << '\n'
                      << "scenario=" << benchforge::ToString(summary.scenario) << '\n'
                      << "mode=" << summary.mode << '\n'
                      << "offered_rate_ops_sec=" << summary.offered_rate_ops_sec << '\n'
                      << "workers=" << summary.workers << '\n'
                      << "operations=" << summary.TotalOperations() << '\n'
                      << "errors=" << summary.TotalErrors() << '\n'
                      << "timeouts=" << summary.TotalTimeouts() << '\n'
                      << "valid=" << (summary.valid ? "true" : "false") << '\n'
                      << "cleanup=" << summary.cleanup_status << '\n'
                      << "measured_ms=" << summary.measured_ms << '\n'
                      << "results=" << output_directory.string() << '\n';
            return cancelled.load() ? 130 : (summary.valid ? 0 : 1);
        }

        PrintUsage(std::cerr);
        throw std::invalid_argument("unknown command: " + command);
    } catch (const std::exception& error) {
        std::cerr << "benchforge: " << error.what() << '\n';
        return 1;
    }
}
