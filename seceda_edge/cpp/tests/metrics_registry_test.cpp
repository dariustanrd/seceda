#include "telemetry/metrics_registry.hpp"

#include <iostream>
#include <string>

namespace {

using namespace seceda::edge;

bool require(bool condition, const char * message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        return false;
    }
    return true;
}

bool require_contains(
    const std::string & haystack,
    const std::string & needle,
    const char * message) {
    return require(haystack.find(needle) != std::string::npos, message);
}

}  // namespace

int main() {
    MetricsRegistry metrics(8);
    metrics.request_started();

    InferenceResponse response;
    response.ok = true;
    response.initial_target = RouteTarget::kLocal;
    response.final_target = RouteTarget::kCloud;
    response.route_reason = "named_backend";
    response.total_timing.total_latency_ms = 42.0;
    response.cloud_timing = TimingInfo{};
    response.cloud_timing->total_latency_ms = 42.0;
    response.initial_identity.engine_id = "local/llama.cpp";
    response.initial_identity.backend_id = "local";
    response.initial_identity.model_id = "local-default";
    response.initial_identity.model_alias = "local/default";
    response.initial_identity.execution_mode = "in_process";
    response.final_identity.engine_id = "cloud/openai";
    response.final_identity.backend_id = "remote/alt";
    response.final_identity.model_id = "remote-alt-model";
    response.final_identity.model_alias = "remote/alt";
    response.final_identity.display_name = "Remote Alt";
    response.final_identity.execution_mode = "remote_service";

    metrics.request_finished(response);

    const std::string rendered = metrics.render_prometheus();
    if (!require_contains(
            rendered,
            "seceda_edge_requests_by_backend_total{backend_id=\"remote/alt\"} 1",
            "backend identity metric should be emitted")) {
        return 1;
    }
    if (!require_contains(
            rendered,
            "seceda_edge_requests_by_engine_total{engine_id=\"cloud/openai\"} 1",
            "engine identity metric should be emitted")) {
        return 1;
    }
    if (!require_contains(
            rendered,
            "seceda_edge_requests_by_model_alias_total{model_alias=\"remote/alt\"} 1",
            "model alias identity metric should be emitted")) {
        return 1;
    }

    const auto batch = metrics.get_events(0, 8);
    if (!require(batch.events.size() == 1, "one inference event should be recorded")) {
        return 1;
    }
    const auto & event = batch.events.front();
    if (!require(event.initial_backend_id == "local", "initial backend identity should be recorded")) {
        return 1;
    }
    if (!require(event.backend_id == "remote/alt", "final backend identity should be recorded")) {
        return 1;
    }
    if (!require(event.display_name == "Remote Alt", "final display name should be recorded")) {
        return 1;
    }
    if (!require(
            event.execution_mode == "remote_service",
            "final execution mode should be recorded")) {
        return 1;
    }

    return 0;
}
