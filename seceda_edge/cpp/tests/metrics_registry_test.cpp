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
    response.request_id = "chatcmpl-seceda-test-1";
    response.requested_target = RouteTarget::kAuto;
    response.initial_target = RouteTarget::kLocal;
    response.final_target = RouteTarget::kCloud;
    response.finish_reason = "stop";
    response.route_reason = "named_backend";
    response.matched_rules = {"named_backend", "freshness"};
    response.active_model_path = "local.gguf";
    response.total_timing.total_latency_ms = 42.0;
    response.total_timing.has_ttft = true;
    response.total_timing.ttft_ms = 12.0;
    response.total_timing.prompt_tokens = 11;
    response.total_timing.generated_tokens = 7;
    response.cloud_timing = TimingInfo{};
    response.cloud_timing->total_latency_ms = 42.0;
    response.cloud_timing->prompt_tokens = 11;
    response.cloud_timing->generated_tokens = 7;
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
    if (!require(
            event.request_id == "chatcmpl-seceda-test-1",
            "request id should be recorded")) {
        return 1;
    }
    if (!require(
            event.requested_target == RouteTarget::kAuto,
            "requested target should be recorded")) {
        return 1;
    }
    if (!require(event.finish_reason == "stop", "finish reason should be recorded")) {
        return 1;
    }
    if (!require(
            event.matched_rules.size() == 2 && event.matched_rules.front() == "named_backend",
            "matched rules should be recorded")) {
        return 1;
    }
    if (!require(event.active_model_path == "local.gguf", "active model path should be recorded")) {
        return 1;
    }
    if (!require(
            event.timing.prompt_tokens == 11 && event.timing.generated_tokens == 7,
            "aggregate token usage should be recorded")) {
        return 1;
    }
    if (!require(
            event.cloud_timing.has_value() && event.cloud_timing->generated_tokens == 7,
            "component timing should be recorded")) {
        return 1;
    }

    const auto filtered = metrics.get_events(0, 8, "chatcmpl-seceda-test-1");
    if (!require(filtered.events.size() == 1, "request id filter should return matching events")) {
        return 1;
    }

    const auto missing = metrics.get_events(0, 8, "chatcmpl-seceda-missing");
    if (!require(missing.events.empty(), "request id filter should exclude non-matching events")) {
        return 1;
    }

    return 0;
}
