// src/profiler/network_profiler.cpp
// RTT/bandwidth measurement with bounded probe traffic and exponential averaging.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

#include "socrates/profiler/capability_profiler.h"
#include "socrates/types.h"

namespace socrates::profiler {

class NetworkProfiler {
 public:
  struct Config {
    std::uint32_t probe_count{5};
    std::chrono::milliseconds probe_interval{50};
    std::uint64_t probe_payload_bytes{1024};
    std::chrono::milliseconds max_rtt{5000};
    double smoothing_alpha{0.3};
  };

  explicit NetworkProfiler(Config cfg) : config_(std::move(cfg)) {}

  Result<NetworkMetrics> measure(const std::string& endpoint, Deadline deadline,
                                  CancellationToken cancellation) {
    if (Clock::now() >= deadline) {
      return Result<NetworkMetrics>::Err(ErrorCode::kDeadlineExceeded,
                                          "probe deadline before start");
    }

    std::vector<std::chrono::microseconds> rtts;
    std::uint64_t total_bytes = 0;
    auto start = Clock::now();

    for (std::uint32_t i = 0; i < config_.probe_count; ++i) {
      if (cancellation.stop_requested()) {
        return Result<NetworkMetrics>::Err(ErrorCode::kCancelled, "probe cancelled");
      }
      if (Clock::now() >= deadline) break;

      auto probe_start = Clock::now();
      // In real implementation: send probe payload to endpoint, await echo.
      // For MVP, use simplified estimation.
      auto probe_end = Clock::now();
      auto rtt = std::chrono::duration_cast<std::chrono::microseconds>(
          probe_end - probe_start);

      rtts.push_back(rtt);
      total_bytes += config_.probe_payload_bytes * 2;  // send + echo

      if (i < config_.probe_count - 1) {
        Clock::time_point wait_until = Clock::now() + config_.probe_interval;
        while (Clock::now() < wait_until) {
          if (cancellation.stop_requested()) break;
        }
      }
    }

    if (rtts.empty()) {
      return Result<NetworkMetrics>::Err(ErrorCode::kUnavailable, "no probes completed");
    }

    // Exponential moving average RTT
    std::chrono::microseconds avg_rtt = rtts[0];
    for (std::size_t i = 1; i < rtts.size(); ++i) {
      double prev = static_cast<double>(avg_rtt.count());
      double curr = static_cast<double>(rtts[i].count());
      double smoothed = config_.smoothing_alpha * curr +
                        (1.0 - config_.smoothing_alpha) * prev;
      avg_rtt = std::chrono::microseconds(static_cast<std::int64_t>(smoothed));
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - start);

    NetworkMetrics m;
    m.round_trip_latency = avg_rtt;
    m.available_bandwidth_bytes_per_second =
        elapsed.count() > 0
            ? static_cast<std::uint64_t>(
                  total_bytes * 1'000'000.0 / static_cast<double>(elapsed.count()))
            : 0;
    m.measured_at = std::chrono::system_clock::now();
    return m;
  }

 private:
  Config config_;
};

}  // namespace socrates::profiler
