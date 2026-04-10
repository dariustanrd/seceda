#include "telemetry/metrics_registry.hpp"

#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace seceda::edge {

MetricsRegistry::MetricsRegistry(std::size_t event_capacity)
    : event_capacity_(event_capacity == 0 ? 1 : event_capacity) {}

void MetricsRegistry::request_started() {
    requests_in_flight_.fetch_add(1);
}

void MetricsRegistry::request_finished(const InferenceResponse & response) {
    requests_total_.fetch_add(1);
    requests_in_flight_.fetch_sub(1);

    total_latency_ms_sum_.fetch_add(
        static_cast<std::uint64_t>(std::llround(response.total_timing.total_latency_ms)));

    if (response.final_target == RouteTarget::kLocal) {
        local_requests_total_.fetch_add(1);
    } else if (response.final_target == RouteTarget::kCloud) {
        cloud_requests_total_.fetch_add(1);
    }

    if (response.fallback_used) {
        fallback_requests_total_.fetch_add(1);
    }

    if (!response.ok) {
        errors_total_.fetch_add(1);
    }

    if (response.local_timing.has_value()) {
        local_latency_ms_sum_.fetch_add(
            static_cast<std::uint64_t>(std::llround(response.local_timing->total_latency_ms)));
    }
    if (response.cloud_timing.has_value()) {
        cloud_latency_ms_sum_.fetch_add(
            static_cast<std::uint64_t>(std::llround(response.cloud_timing->total_latency_ms)));
    }

    push_event(response);
}

std::string MetricsRegistry::render_prometheus() const {
    std::ostringstream out;
    out << "# HELP seceda_edge_requests_total Total inference requests\n";
    out << "# TYPE seceda_edge_requests_total counter\n";
    out << "seceda_edge_requests_total " << requests_total_.load() << "\n\n";

    out << "# HELP seceda_edge_requests_by_target_total Total completed requests by final target\n";
    out << "# TYPE seceda_edge_requests_by_target_total counter\n";
    out << "seceda_edge_requests_by_target_total{target=\"local\"} " << local_requests_total_.load() << "\n";
    out << "seceda_edge_requests_by_target_total{target=\"cloud\"} " << cloud_requests_total_.load() << "\n\n";

    out << "# HELP seceda_edge_fallback_requests_total Total requests that used fallback\n";
    out << "# TYPE seceda_edge_fallback_requests_total counter\n";
    out << "seceda_edge_fallback_requests_total " << fallback_requests_total_.load() << "\n\n";

    out << "# HELP seceda_edge_errors_total Total failed requests\n";
    out << "# TYPE seceda_edge_errors_total counter\n";
    out << "seceda_edge_errors_total " << errors_total_.load() << "\n\n";

    out << "# HELP seceda_edge_latency_ms_sum Cumulative total request latency in milliseconds\n";
    out << "# TYPE seceda_edge_latency_ms_sum counter\n";
    out << "seceda_edge_latency_ms_sum " << total_latency_ms_sum_.load() << "\n\n";

    out << "# HELP seceda_edge_component_latency_ms_sum Cumulative component latency in milliseconds\n";
    out << "# TYPE seceda_edge_component_latency_ms_sum counter\n";
    out << "seceda_edge_component_latency_ms_sum{component=\"local\"} " << local_latency_ms_sum_.load() << "\n";
    out << "seceda_edge_component_latency_ms_sum{component=\"cloud\"} " << cloud_latency_ms_sum_.load() << "\n\n";

    out << "# HELP seceda_edge_requests_in_flight Requests currently in flight\n";
    out << "# TYPE seceda_edge_requests_in_flight gauge\n";
    out << "seceda_edge_requests_in_flight " << requests_in_flight_.load() << "\n";

    return out.str();
}

EventBatch MetricsRegistry::get_events(std::uint64_t since_id, std::size_t limit) const {
    EventBatch batch;
    batch.since_id = since_id;
    limit = limit == 0 ? 1000 : limit;

    std::lock_guard<std::mutex> lock(events_mutex_);
    if (!events_.empty()) {
        batch.latest_id = events_.back().id;
    }

    for (const auto & event : events_) {
        if (event.id <= since_id) {
            continue;
        }
        batch.events.push_back(event);
        if (batch.events.size() >= limit) {
            break;
        }
    }

    return batch;
}

std::string MetricsRegistry::now_utc() const {
    const std::time_t now = std::time(nullptr);
    std::tm utc_tm {};
#if defined(_WIN32)
    gmtime_s(&utc_tm, &now);
#else
    gmtime_r(&now, &utc_tm);
#endif

    std::ostringstream out;
    out << std::put_time(&utc_tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

void MetricsRegistry::push_event(const InferenceResponse & response) {
    InferenceEvent event;
    event.id = next_event_id_.fetch_add(1);
    event.timestamp_utc = now_utc();
    event.initial_target = response.initial_target;
    event.final_target = response.final_target;
    event.ok = response.ok;
    event.fallback_used = response.fallback_used;
    event.error_kind = response.error_kind;
    event.route_reason = response.route_reason;
    event.fallback_reason = response.fallback_reason;
    event.total_latency_ms = response.total_timing.total_latency_ms;
    if (response.local_timing.has_value()) {
        event.local_latency_ms = response.local_timing->total_latency_ms;
    }
    if (response.cloud_timing.has_value()) {
        event.cloud_latency_ms = response.cloud_timing->total_latency_ms;
    }

    std::lock_guard<std::mutex> lock(events_mutex_);
    events_.push_back(std::move(event));
    while (events_.size() > event_capacity_) {
        events_.pop_front();
    }
}

}  // namespace seceda::edge
