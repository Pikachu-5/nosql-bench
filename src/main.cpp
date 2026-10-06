#include "benchforge/adapter.hpp"
#include "benchforge/config.hpp"
#include "benchforge/control_api.hpp"
#include "benchforge/results.hpp"
#include "benchforge/runner.hpp"
#include "feedkv_server.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

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
            for (int i = 2; i < argc; ++i) {
                const std::string option = argv[i];
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
            return benchforge::RunFeedKvServerImpl(port);
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
            const auto config = LoadAndValidate(arguments);
            const auto summary = benchforge::RunBenchmark(config);
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
            return summary.TotalErrors() == 0 ? 0 : 1;
        }

        PrintUsage(std::cerr);
        throw std::invalid_argument("unknown command: " + command);
    } catch (const std::exception& error) {
        std::cerr << "benchforge: " << error.what() << '\n';
        return 1;
    }
}
