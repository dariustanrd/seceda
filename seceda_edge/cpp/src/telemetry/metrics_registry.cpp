#include "telemetry/metrics_registry.hpp"

#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace seceda::edge {
namespace {

const std::string & non_empty_label_value(const std::string & value) {
    static const std::string kUnknown = "unknown";
    return value.empty() ? kUnknown : value;
}

std::string prometheus_escape_label_value(const std::string & value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value) {
        if (ch == '\\' || ch == '"') {
            escaped.push_back('\\');
        }
        escaped.push_back(ch);
    }
    return escaped;
}

}  // namespace

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

    {
        std::lock_guard<std::mutex> lock(labeled_metrics_mutex_);
        const std::string backend_id = non_empty_label_value(response.final_identity.backend_id);
        const std::string engine_id = non_empty_label_value(response.final_identity.engine_id);
        const std::string model_alias = non_empty_label_value(response.final_identity.model_alias);

        ++requests_by_backend_total_[backend_id];
        ++requests_by_engine_total_[engine_id];
        ++requests_by_model_alias_total_[model_alias];
        latency_ms_sum_by_backend_[backend_id] +=
            static_cast<std::uint64_t>(std::llround(response.total_timing.total_latency_ms));
    }

    push_event(response);
}

std::string MetricsRegistry::render_prometheus() const {
    std::ostringstream out;
    out << "# HELP seceda_edge_requests_total Total chat completion requests\n";
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

    {
        std::lock_guard<std::mutex> lock(labeled_metrics_mutex_);

        out << "# HELP seceda_edge_requests_by_backend_total Total completed requests by backend identity\n";
        out << "# TYPE seceda_edge_requests_by_backend_total counter\n";
        for (const auto & [backend_id, count] : requests_by_backend_total_) {
            out << "seceda_edge_requests_by_backend_total{backend_id=\""
                << prometheus_escape_label_value(backend_id) << "\"} " << count << "\n";
        }
        out << "\n";

        out << "# HELP seceda_edge_requests_by_engine_total Total completed requests by engine identity\n";
        out << "# TYPE seceda_edge_requests_by_engine_total counter\n";
        for (const auto & [engine_id, count] : requests_by_engine_total_) {
            out << "seceda_edge_requests_by_engine_total{engine_id=\""
                << prometheus_escape_label_value(engine_id) << "\"} " << count << "\n";
        }
        out << "\n";

        out << "# HELP seceda_edge_requests_by_model_alias_total Total completed requests by public model alias\n";
        out << "# TYPE seceda_edge_requests_by_model_alias_total counter\n";
        for (const auto & [model_alias, count] : requests_by_model_alias_total_) {
            out << "seceda_edge_requests_by_model_alias_total{model_alias=\""
                << prometheus_escape_label_value(model_alias) << "\"} " << count << "\n";
        }
        out << "\n";

        out << "# HELP seceda_edge_latency_ms_sum_by_backend Cumulative request latency in milliseconds by backend identity\n";
        out << "# TYPE seceda_edge_latency_ms_sum_by_backend counter\n";
        for (const auto & [backend_id, total_ms] : latency_ms_sum_by_backend_) {
            out << "seceda_edge_latency_ms_sum_by_backend{backend_id=\""
                << prometheus_escape_label_value(backend_id) << "\"} " << total_ms << "\n";
        }
        out << "\n";
    }

    out << "# HELP seceda_edge_requests_in_flight Requests currently in flight\n";
    out << "# TYPE seceda_edge_requests_in_flight gauge\n";
    out << "seceda_edge_requests_in_flight " << requests_in_flight_.load() << "\n";

    return out.str();
}

EventBatch MetricsRegistry::get_events(std::uint64_t since_id, std::size_t limit, std::string request_id) const {
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
        if (!request_id.empty() && event.request_id != request_id) {
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
    event.request_id = response.request_id;
    event.timestamp_utc = now_utc();
    event.requested_target = response.requested_target;
    event.initial_target = response.initial_target;
    event.final_target = response.final_target;
    event.ok = response.ok;
    event.fallback_used = response.fallback_used;
    event.error_kind = response.error_kind;
    event.error = response.error;
    event.finish_reason = response.finish_reason;
    event.route_reason = response.route_reason;
    event.fallback_reason = response.fallback_reason;
    event.matched_rules = response.matched_rules;
    event.initial_engine_id = response.initial_identity.engine_id;
    event.initial_backend_id = response.initial_identity.backend_id;
    event.initial_model_id = response.initial_identity.model_id;
    event.initial_model_alias = response.initial_identity.model_alias;
    event.initial_execution_mode = response.initial_identity.execution_mode;
    event.engine_id = response.final_identity.engine_id;
    event.backend_id = response.final_identity.backend_id;
    event.model_id = response.final_identity.model_id;
    event.model_alias = response.final_identity.model_alias;
    event.display_name = response.final_identity.display_name;
    event.execution_mode = response.final_identity.execution_mode;
    event.active_model_path = response.active_model_path;
    event.timing = response.total_timing;
    if (response.local_timing.has_value()) {
        event.local_timing = response.local_timing;
    }
    if (response.cloud_timing.has_value()) {
        event.cloud_timing = response.cloud_timing;
    }

    std::lock_guard<std::mutex> lock(events_mutex_);
    events_.push_back(std::move(event));
    while (events_.size() > event_capacity_) {
        events_.pop_front();
    }
}

}  // namespace seceda::edge
