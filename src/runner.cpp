#include "benchforge/runner.hpp"

#include "benchforge/adapter.hpp"
#include "benchforge/workload.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <latch>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace benchforge {
namespace {

using Clock = std::chrono::steady_clock;

void WaitUntil(std::chrono::steady_clock::time_point scheduled, const std::function<bool()>& cancelled) {
    // Some Windows timer implementations wake just before the requested deadline.
    // Recheck the steady clock so requests are never dispatched ahead of schedule.
    while (std::chrono::steady_clock::now() < scheduled && !cancelled()) {
        std::this_thread::sleep_until(std::min(scheduled, Clock::now() + std::chrono::milliseconds(20)));
    }
}

std::string FormatUtc(std::chrono::system_clock::time_point time_point,
                      bool include_fraction) {
    const auto time = std::chrono::system_clock::to_time_t(time_point);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S");
    if (include_fraction) {
        const auto since_epoch =
            std::chrono::duration_cast<std::chrono::microseconds>(
                time_point.time_since_epoch());
        const auto fraction = static_cast<std::uint64_t>(
            since_epoch.count() % 1000000);
        stream << '.' << std::setw(6) << std::setfill('0') << fraction;
    }
    stream << 'Z';
    return stream.str();
}

std::string MakeRunId(std::chrono::system_clock::time_point now) {
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    const auto since_epoch =
        std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch());
    const auto fraction = static_cast<std::uint64_t>(
        since_epoch.count() % 1000000);
    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y%m%dT%H%M%S") << '_'
           << std::setw(6) << std::setfill('0') << fraction << 'Z';
    return stream.str();
}

std::size_t IndexOf(OperationType type) {
    return static_cast<std::size_t>(type);
}

} // namespace

std::uint64_t RunSummary::TotalOperations() const noexcept {
    std::uint64_t total = 0;
    for (const auto& operation : operations) {
        total += operation.count;
    }
    return total;
}

std::uint64_t RunSummary::TotalErrors() const noexcept {
    std::uint64_t total = 0;
    for (const auto& operation : operations) {
        total += operation.errors;
    }
    return total;
}

std::uint64_t RunSummary::TotalTimeouts() const noexcept {
    std::uint64_t total = 0;
    for (const auto& operation : operations) total += operation.timeouts;
    return total;
}

RunSummary RunBenchmark(const RunConfig& config, std::function<bool()> cancelled,
                        std::function<void()> acquired) {
    if (!cancelled) cancelled = [] { return false; };
    const auto validation_errors = ValidateConfig(config);
    if (!validation_errors.empty()) {
        throw std::invalid_argument(validation_errors.front());
    }

    struct WorkerMetrics {
        std::array<OperationMetrics, kOperationCount> operations;
    };

    RunSummary summary;
    summary.run_id = config.run_id.empty()
                         ? MakeRunId(std::chrono::system_clock::now())
                         : config.run_id;
    summary.adapter = config.adapter;
    summary.scenario = config.scenario;
    summary.seed = config.seed;
    summary.workers = config.workers;
    summary.warmup_ms = config.warmup_ms;
    summary.mode = ToString(config.mode);
    summary.offered_rate_ops_sec = config.offered_rate_ops_sec;

    const auto key_prefix = "benchforge:" + summary.run_id + ":";
    std::unique_ptr<DatabaseAdapter> loader;
    summary.started_at_utc = FormatUtc(std::chrono::system_clock::now(), true);
    summary.environment = CaptureEnvironment();
    auto clean = [&] {
        if (!loader) return std::string("namespace not acquired");
        loader->SetCancellationCheck({});
        return loader->Cleanup();
    };
    try {
        loader = CreateAdapter(config.adapter, config, key_prefix);
        loader->SetCancellationCheck(cancelled);
        loader->SetOwnershipCallback(std::move(acquired));
        loader->LoadDataset(config);
        summary.transport_calibration = loader->Calibrate();
    } catch (const std::exception& error) {
        summary.valid = false;
        summary.invalid_reasons.emplace_back(error.what());
        if (loader) {
            summary.adapter_version = loader->Version(); summary.endpoint = loader->Endpoint();
            summary.storage_configuration = loader->StorageConfiguration();
        } else {
            summary.adapter_version = "not available; connection failed before loading";
            summary.storage_configuration = "not observed; connection failed before loading";
        }
        summary.cleanup_status = clean();
        return summary;
    }
    summary.adapter_version = loader->Version();
    summary.endpoint = loader->Endpoint();
    summary.storage_configuration = loader->StorageConfiguration();
    summary.environment = CaptureEnvironment();

    std::vector<WorkerMetrics> worker_metrics(config.workers);
    std::vector<std::unique_ptr<DatabaseAdapter>> adapters;
    try {
        adapters.reserve(config.workers);
        for (std::uint32_t worker = 0; worker < config.workers; ++worker) {
            adapters.push_back(CreateAdapter(config.adapter, config, key_prefix));
            adapters.back()->PrepareWorker();
        }
    } catch (const std::exception& error) {
        summary.valid = false;
        summary.invalid_reasons.emplace_back(error.what());
        summary.adapter_version = loader->Version(); summary.endpoint = loader->Endpoint();
        summary.storage_configuration = loader->StorageConfiguration();
        summary.cleanup_status = clean();
        return summary;
    }

    std::latch ready(static_cast<std::ptrdiff_t>(config.workers));
    std::latch start_gate(1);
    std::vector<std::thread> threads;
    threads.reserve(config.workers);

    Clock::time_point run_start{};

    for (std::uint32_t worker = 0; worker < config.workers; ++worker) {
        const auto worker_seed =
            config.seed ^ (static_cast<std::uint64_t>(worker + 1U) *
                           0x9E3779B97F4A7C15ULL);
        threads.emplace_back(
            [&, worker, worker_seed, adapter = std::move(adapters[worker])]() mutable {
                WorkloadGenerator generator(config, worker_seed, worker,
                                            config.workers);
                ready.count_down();
                start_gate.wait();

                const auto measured_start =
                    run_start + std::chrono::milliseconds(config.warmup_ms);
                const auto measured_end =
                    measured_start + std::chrono::milliseconds(config.duration_ms);

                auto execute_warmup = [&] {
                    try {
                        auto operation = generator.Next();
                        // Warm cache/transport without changing the shared seeded state.
                        // Keep measurement IDs and RNG independent of warm-up speed.
                        operation.type = operation.sequence % 2U == 0 ? OperationType::TimelineRead : OperationType::UserProfileRead;
                        adapter->Execute(operation);
                    } catch (...) {
                        // Warm-up errors do not enter measured operation counts.
                    }
                };

                if (config.mode == RunMode::ClosedLoop) {
                    while (Clock::now() < measured_start && !cancelled()) execute_warmup();
                } else {
                    const auto interval = std::chrono::duration_cast<Clock::duration>(
                        std::chrono::duration<double>(
                            static_cast<double>(config.workers) /
                            static_cast<double>(config.offered_rate_ops_sec)));
                    const auto phase = std::chrono::duration_cast<Clock::duration>(
                        std::chrono::duration<double>(
                            static_cast<double>(worker) /
                            static_cast<double>(config.offered_rate_ops_sec)));
                    auto scheduled = run_start + phase;
                    while (scheduled < measured_start && !cancelled()) {
                        WaitUntil(scheduled, cancelled);
                        if (cancelled()) break;
                        execute_warmup();
                        scheduled += interval;
                    }
                }

                generator = WorkloadGenerator(config, worker_seed, worker, config.workers);
                auto& metrics = worker_metrics[worker].operations;
                if (config.mode == RunMode::ClosedLoop) {
                    while (Clock::now() < measured_end && !cancelled()) {
                        const auto operation = generator.Next();
                        const auto operation_start = Clock::now();
                        bool success = true;
                        bool timed_out = false;
                        try {
                            adapter->Execute(operation);
                        } catch (const AdapterTimeout&) {
                            success = false;
                            timed_out = true;
                        } catch (...) {
                            success = false;
                        }
                        const auto operation_end = Clock::now();
                        const auto latency = std::chrono::duration_cast<
                            std::chrono::nanoseconds>(operation_end - operation_start);
                        metrics[IndexOf(operation.type)].Record(
                            static_cast<std::uint64_t>(latency.count()), success,
                            timed_out);
                    }
                } else {
                    const auto interval = std::chrono::duration_cast<Clock::duration>(
                        std::chrono::duration<double>(
                            static_cast<double>(config.workers) /
                            static_cast<double>(config.offered_rate_ops_sec)));
                    const auto phase = std::chrono::duration_cast<Clock::duration>(
                        std::chrono::duration<double>(
                            static_cast<double>(worker) /
                            static_cast<double>(config.offered_rate_ops_sec)));
                    auto scheduled = measured_start + phase;
                    while (scheduled < measured_end && !cancelled()) {
                        WaitUntil(scheduled, cancelled);
                        if (cancelled()) break;
                        const auto operation = generator.Next();
                        const auto operation_start = Clock::now();
                        const auto lag = std::chrono::duration_cast<
                            std::chrono::nanoseconds>(operation_start - scheduled);
                        bool success = true;
                        bool timed_out = false;
                        try {
                            adapter->Execute(operation);
                        } catch (const AdapterTimeout&) {
                            success = false;
                            timed_out = true;
                        } catch (...) {
                            success = false;
                        }
                        const auto operation_end = Clock::now();
                        const auto latency = std::chrono::duration_cast<
                            std::chrono::nanoseconds>(operation_end - operation_start);
                        metrics[IndexOf(operation.type)].Record(
                            static_cast<std::uint64_t>(latency.count()), success,
                            timed_out, static_cast<std::uint64_t>(
                                std::max<std::int64_t>(0, lag.count())));
                        scheduled += interval;
                    }
                }
            });
    }

    ready.wait();
    run_start = Clock::now();
    summary.started_at_utc = FormatUtc(std::chrono::system_clock::now(), true);
    start_gate.count_down();

    for (auto& thread : threads) {
        thread.join();
    }

    for (const auto& worker : worker_metrics) {
        for (std::size_t i = 0; i < kOperationCount; ++i) {
            summary.operations[i].Merge(worker.operations[i]);
        }
    }
    const auto actual = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - run_start - std::chrono::milliseconds(config.warmup_ms)).count();
    summary.measured_ms = static_cast<std::uint64_t>(std::max<std::int64_t>(0, actual));
    if (!cancelled()) summary.measured_ms = std::max<std::uint64_t>(config.duration_ms, summary.measured_ms);
    if (cancelled()) { summary.valid = false; summary.invalid_reasons.emplace_back("run cancelled"); }
    for (const auto& operation : summary.operations) {
        if (operation.count != operation.latency.Count() ||
            operation.count != operation.send_lag.Count() ||
            operation.errors > operation.count ||
            operation.timeouts > operation.errors) {
            summary.valid = false;
            summary.invalid_reasons.emplace_back(
                "operation count, histogram, error, or timeout totals are inconsistent");
            break;
        }
    }
    if (summary.telemetry_dropped != 0) {
        summary.valid = false;
        summary.invalid_reasons.emplace_back("telemetry samples were dropped");
    }
    if (summary.TotalOperations() == 0) {
        summary.valid = false;
        summary.invalid_reasons.emplace_back("no measured operations completed");
    }
    if (summary.TotalTimeouts() != 0) {
        summary.valid = false;
        summary.invalid_reasons.emplace_back("one or more measured operations timed out");
    }
    if (summary.TotalErrors() > summary.TotalTimeouts()) {
        summary.valid = false;
        summary.invalid_reasons.emplace_back("one or more measured operations failed");
    }
    summary.cleanup_status = clean();
    if (summary.cleanup_status.rfind("cleanup failed:", 0) == 0) {
        summary.valid = false;
        summary.invalid_reasons.emplace_back("database cleanup failed after measurement");
    }
    return summary;
}

} // namespace benchforge
