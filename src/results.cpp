#include "benchforge/results.hpp"

#include "benchforge/operation.hpp"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace benchforge {
namespace {

std::string JsonEscape(const std::string& value) {
    std::ostringstream escaped;
    for (const unsigned char character : value) {
        switch (character) {
        case '"':
            escaped << "\\\"";
            break;
        case '\\':
            escaped << "\\\\";
            break;
        case '\b':
            escaped << "\\b";
            break;
        case '\f':
            escaped << "\\f";
            break;
        case '\n':
            escaped << "\\n";
            break;
        case '\r':
            escaped << "\\r";
            break;
        case '\t':
            escaped << "\\t";
            break;
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

std::uint64_t MinimumLatency(const OperationMetrics& metrics) {
    return metrics.count == 0 ? 0 : metrics.min_latency_ns;
}

double Throughput(const OperationMetrics& metrics, std::uint64_t elapsed_ms) {
    if (elapsed_ms == 0) {
        return 0.0;
    }
    return static_cast<double>(metrics.count) * 1000.0 /
           static_cast<double>(elapsed_ms);
}

void WriteSummaryJson(const std::filesystem::path& path, const RunConfig& config,
                      const RunSummary& summary) {
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("could not write " + path.string());
    }
    output << "{\n"
           << "  \"schema_version\": 2,\n"
           << "  \"run_id\": \"" << JsonEscape(summary.run_id) << "\",\n"
           << "  \"started_at_utc\": \"" << JsonEscape(summary.started_at_utc)
           << "\",\n"
           << "  \"adapter\": \"" << JsonEscape(summary.adapter) << "\",\n"
           << "  \"adapter_version\": \""
           << JsonEscape(summary.adapter_version) << "\",\n"
           << "  \"endpoint\": \"" << JsonEscape(summary.endpoint) << "\",\n"
           << "  \"storage_configuration\": \""
           << JsonEscape(summary.storage_configuration) << "\",\n"
           << "  \"cleanup_status\": \""
           << JsonEscape(summary.cleanup_status) << "\",\n"
           << "  \"scenario\": \"" << ToString(summary.scenario) << "\",\n"
           << "  \"mode\": \"" << JsonEscape(summary.mode) << "\",\n"
           << "  \"offered_rate_ops_sec\": "
           << summary.offered_rate_ops_sec << ",\n"
           << "  \"seed\": " << summary.seed << ",\n"
           << "  \"workers\": " << summary.workers << ",\n"
           << "  \"warmup_ms\": " << summary.warmup_ms << ",\n"
           << "  \"measured_ms\": " << summary.measured_ms << ",\n"
           << "  \"total_operations\": " << summary.TotalOperations() << ",\n"
           << "  \"total_errors\": " << summary.TotalErrors() << ",\n"
           << "  \"total_timeouts\": " << summary.TotalTimeouts() << ",\n"
           << "  \"valid\": " << (summary.valid ? "true" : "false") << ",\n"
           << "  \"invalid_reasons\": [";
    for (std::size_t i = 0; i < summary.invalid_reasons.size(); ++i) {
        if (i != 0) output << ", ";
        output << "\"" << JsonEscape(summary.invalid_reasons[i]) << "\"";
    }
    output << "],\n"
           << "  \"telemetry_dropped\": " << summary.telemetry_dropped << ",\n"
           << "  \"histogram_resolution\": "
              "\"16 subdivisions per power-of-two bucket; approximate\",\n"
           << "  \"environment\": {\n"
           << "    \"host_name\": \"" << JsonEscape(summary.environment.host_name) << "\",\n"
           << "    \"operating_system\": \""
           << JsonEscape(summary.environment.operating_system) << "\",\n"
           << "    \"architecture\": \""
           << JsonEscape(summary.environment.architecture) << "\",\n"
           << "    \"logical_processors\": "
           << summary.environment.logical_processors << ",\n"
           << "    \"total_memory_bytes\": "
           << summary.environment.total_memory_bytes << "\n"
           << "  },\n"
           << "  \"transport_calibration\": {\n"
           << "    \"kind\": \""
           << JsonEscape(summary.transport_calibration.kind) << "\",\n"
           << "    \"samples\": " << summary.transport_calibration.samples << ",\n"
           << "    \"p50_ns\": " << summary.transport_calibration.p50_ns << ",\n"
           << "    \"p95_ns\": " << summary.transport_calibration.p95_ns << ",\n"
           << "    \"mean_ns\": " << summary.transport_calibration.mean_ns << "\n"
           << "  },\n"
           << "  \"config\": {\n"
           << "    \"database_host\": \""
           << JsonEscape(config.database_host) << "\",\n"
           << "    \"database_port\": "
           << (config.database_port == 0
                   ? DefaultDatabasePort(config.adapter) : config.database_port)
           << ",\n"
           << "    \"mode\": \"" << ToString(config.mode) << "\",\n"
           << "    \"offered_rate_ops_sec\": "
           << config.offered_rate_ops_sec << ",\n"
           << "    \"duration_ms\": " << config.duration_ms << ",\n"
           << "    \"users\": " << config.users << ",\n"
           << "    \"posts\": " << config.posts << ",\n"
           << "    \"follows\": " << config.follows << ",\n"
           << "    \"hashtags\": " << config.hashtags << ",\n"
           << "    \"celebrity_post_percent\": "
           << static_cast<unsigned>(config.celebrity_post_percent) << ",\n"
           << "    \"operation_weights\": {";
    for (std::size_t i = 0; i < kOperationCount; ++i) {
        if (i != 0) output << ", ";
        output << "\"" << ToString(kOperationTypes[i]) << "\": "
               << static_cast<unsigned>(config.weights[i]);
    }
    output << "}\n"
           << "  },\n"
           << "  \"operations\": [\n";
    for (std::size_t i = 0; i < kOperationCount; ++i) {
        const auto& metrics = summary.operations[i];
        output << "    {\"type\": \"" << ToString(kOperationTypes[i])
               << "\", \"count\": " << metrics.count
               << ", \"errors\": " << metrics.errors
               << ", \"timeouts\": " << metrics.timeouts
               << ", \"ops_per_sec\": "
               << std::fixed << std::setprecision(3)
               << Throughput(metrics, summary.measured_ms)
               << ", \"p50_ns\": " << metrics.latency.Percentile(0.50)
               << ", \"p95_ns\": " << metrics.latency.Percentile(0.95)
               << ", \"p99_ns\": " << metrics.latency.Percentile(0.99)
               << ", \"p999_ns\": " << metrics.latency.Percentile(0.999)
               << ", \"min_ns\": " << MinimumLatency(metrics)
               << ", \"max_ns\": " << metrics.max_latency_ns
               << ", \"mean_ns\": " << metrics.MeanLatencyNs()
               << ", \"send_lag_p50_ns\": "
               << metrics.send_lag.Percentile(0.50)
               << ", \"send_lag_p95_ns\": "
               << metrics.send_lag.Percentile(0.95)
               << ", \"send_lag_p99_ns\": "
               << metrics.send_lag.Percentile(0.99)
               << ", \"max_send_lag_ns\": "
               << metrics.max_send_lag_ns << "}";
        if (i + 1U != kOperationCount) {
            output << ',';
        }
        output << '\n';
    }
    output << "  ]\n}\n";
    if (!output) {
        throw std::runtime_error("failed while writing " + path.string());
    }
}

void WriteOperationsCsv(const std::filesystem::path& path,
                        const RunSummary& summary) {
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("could not write " + path.string());
    }
    output << "operation,count,errors,timeouts,ops_per_sec,p50_ns,p95_ns,p99_ns,p999_ns,"
              "min_ns,max_ns,mean_ns,send_lag_p50_ns,send_lag_p95_ns,"
              "send_lag_p99_ns,max_send_lag_ns\n";
    output << std::fixed << std::setprecision(3);
    for (std::size_t i = 0; i < kOperationCount; ++i) {
        const auto& metrics = summary.operations[i];
        output << ToString(kOperationTypes[i]) << ',' << metrics.count << ','
               << metrics.errors << ',' << metrics.timeouts << ','
               << Throughput(metrics, summary.measured_ms) << ','
               << metrics.latency.Percentile(0.50) << ','
               << metrics.latency.Percentile(0.95) << ','
               << metrics.latency.Percentile(0.99) << ','
               << metrics.latency.Percentile(0.999) << ','
               << MinimumLatency(metrics) << ',' << metrics.max_latency_ns << ','
               << metrics.MeanLatencyNs() << ','
               << metrics.send_lag.Percentile(0.50) << ','
               << metrics.send_lag.Percentile(0.95) << ','
               << metrics.send_lag.Percentile(0.99) << ','
               << metrics.max_send_lag_ns << '\n';
    }
    if (!output) {
        throw std::runtime_error("failed while writing " + path.string());
    }
}

} // namespace

std::filesystem::path WriteResults(const RunConfig& config,
                                   const RunSummary& summary) {
    const std::filesystem::path run_directory =
        std::filesystem::path(config.output_dir) / summary.run_id;
    std::filesystem::create_directories(run_directory);
    WriteSummaryJson(run_directory / "summary.json", config, summary);
    WriteOperationsCsv(run_directory / "operations.csv", summary);
    return run_directory;
}

} // namespace benchforge
