#pragma once

#include "benchforge/operation.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace benchforge {

// Fixed-size logarithmic histogram. Sixteen sub-buckets per power of two keep
// storage bounded while giving useful approximate latency percentiles.
class LatencyHistogram {
public:
    static constexpr std::size_t kSubBuckets = 16;
    static constexpr std::size_t kBucketCount = 1 + 64 * kSubBuckets;

    void Record(std::uint64_t nanoseconds) noexcept {
        ++buckets_[BucketFor(nanoseconds)];
        ++count_;
    }

    void Merge(const LatencyHistogram& other) noexcept {
        for (std::size_t i = 0; i < buckets_.size(); ++i) {
            buckets_[i] += other.buckets_[i];
        }
        count_ += other.count_;
    }

    std::uint64_t Count() const noexcept {
        return count_;
    }

    std::uint64_t Percentile(double percentile) const noexcept {
        if (count_ == 0) {
            return 0;
        }
        const double bounded = std::clamp(percentile, 0.0, 1.0);
        const auto rank = static_cast<std::uint64_t>(
            std::ceil(bounded * static_cast<double>(count_)));
        const auto target = std::max<std::uint64_t>(rank, 1);
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < buckets_.size(); ++i) {
            seen += buckets_[i];
            if (seen >= target) {
                return UpperBound(i);
            }
        }
        return std::numeric_limits<std::uint64_t>::max();
    }

private:
    static std::size_t BucketFor(std::uint64_t value) noexcept {
        if (value == 0) {
            return 0;
        }
        const auto exponent = static_cast<unsigned>(std::bit_width(value) - 1U);
        const auto base = std::uint64_t{1} << exponent;
        unsigned subdivision = 0;
        if (exponent >= 4U) {
            const auto quantum = base / kSubBuckets;
            subdivision = static_cast<unsigned>((value - base) / quantum);
            subdivision = std::min(subdivision, static_cast<unsigned>(kSubBuckets - 1));
        }
        return 1U + static_cast<std::size_t>(exponent) * kSubBuckets + subdivision;
    }

    static std::uint64_t UpperBound(std::size_t index) noexcept {
        if (index == 0) {
            return 0;
        }
        const auto bucket = index - 1U;
        const auto exponent = static_cast<unsigned>(bucket / kSubBuckets);
        const auto subdivision = static_cast<unsigned>(bucket % kSubBuckets);
        const auto base = std::uint64_t{1} << exponent;
        if (exponent < 4U) {
            return (base * 2U) - 1U;
        }
        const auto quantum = base / kSubBuckets;
        const auto increment = static_cast<std::uint64_t>(subdivision + 1U) * quantum;
        const auto max_value = std::numeric_limits<std::uint64_t>::max();
        if (increment > max_value - base) {
            return max_value;
        }
        return base + increment - 1U;
    }

    std::array<std::uint64_t, kBucketCount> buckets_{};
    std::uint64_t count_{0};
};

struct OperationMetrics {
    std::uint64_t count{0};
    std::uint64_t errors{0};
    std::uint64_t timeouts{0};
    std::uint64_t total_latency_ns{0};
    std::uint64_t min_latency_ns{std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t max_latency_ns{0};
    LatencyHistogram latency;
    LatencyHistogram send_lag;
    std::uint64_t max_send_lag_ns{0};

    void Record(std::uint64_t nanoseconds, bool success, bool timed_out = false,
                std::uint64_t send_lag_ns = 0) noexcept {
        ++count;
        if (!success) {
            ++errors;
        }
        if (timed_out) ++timeouts;
        total_latency_ns += nanoseconds;
        min_latency_ns = std::min(min_latency_ns, nanoseconds);
        max_latency_ns = std::max(max_latency_ns, nanoseconds);
        latency.Record(nanoseconds);
        send_lag.Record(send_lag_ns);
        max_send_lag_ns = std::max(max_send_lag_ns, send_lag_ns);
    }

    void Merge(const OperationMetrics& other) noexcept {
        count += other.count;
        errors += other.errors;
        timeouts += other.timeouts;
        total_latency_ns += other.total_latency_ns;
        if (other.count > 0) {
            min_latency_ns = std::min(min_latency_ns, other.min_latency_ns);
            max_latency_ns = std::max(max_latency_ns, other.max_latency_ns);
        }
        latency.Merge(other.latency);
        send_lag.Merge(other.send_lag);
        max_send_lag_ns = std::max(max_send_lag_ns, other.max_send_lag_ns);
    }

    double MeanLatencyNs() const noexcept {
        return count == 0 ? 0.0
                          : static_cast<double>(total_latency_ns) /
                                static_cast<double>(count);
    }
};

} // namespace benchforge
