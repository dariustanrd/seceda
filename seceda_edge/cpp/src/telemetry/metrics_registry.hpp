#pragma once

#include "runtime/contracts.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>

namespace seceda::edge {

class MetricsRegistry {
public:
    explicit MetricsRegistry(std::size_t event_capacity = 2048);

    void request_started();
    void request_finished(const InferenceResponse & response);

    std::string render_prometheus() const;
    EventBatch get_events(std::uint64_t since_id, std::size_t limit) const;

private:
    std::string now_utc() const;
    void push_event(const InferenceResponse & response);

    std::size_t event_capacity_ = 0;
    std::atomic<std::uint64_t> requests_total_{0};
    std::atomic<std::uint64_t> local_requests_total_{0};
    std::atomic<std::uint64_t> cloud_requests_total_{0};
    std::atomic<std::uint64_t> fallback_requests_total_{0};
    std::atomic<std::uint64_t> errors_total_{0};
    std::atomic<std::uint64_t> total_latency_ms_sum_{0};
    std::atomic<std::uint64_t> local_latency_ms_sum_{0};
    std::atomic<std::uint64_t> cloud_latency_ms_sum_{0};
    std::atomic<std::int64_t> requests_in_flight_{0};
    std::atomic<std::uint64_t> next_event_id_{1};

    mutable std::mutex labeled_metrics_mutex_;
    std::map<std::string, std::uint64_t> requests_by_backend_total_;
    std::map<std::string, std::uint64_t> requests_by_engine_total_;
    std::map<std::string, std::uint64_t> requests_by_model_alias_total_;
    std::map<std::string, std::uint64_t> latency_ms_sum_by_backend_;

    mutable std::mutex events_mutex_;
    std::deque<InferenceEvent> events_;
};

}  // namespace seceda::edge
