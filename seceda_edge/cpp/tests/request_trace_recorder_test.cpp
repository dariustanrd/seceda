#include "telemetry/request_trace_recorder.hpp"

#include <nlohmann/json.hpp>

#include <iostream>

namespace {

using namespace seceda::edge;
using json = nlohmann::json;

bool require(bool condition, const char * message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        return false;
    }
    return true;
}

InferenceRequest make_request() {
    InferenceRequest request;
    request.seceda.request_id = "chatcmpl-seceda-trace-recorder";
    request.seceda.transport = "chat_completions";
    request.messages.push_back({"user", "hello", {}, {}, {}});
    refresh_request_views(request);
    return request;
}

}  // namespace

int main() {
    TraceRegistry traces(16);
    RequestTraceRecorder recorder(traces);
    const InferenceRequest request = make_request();

    RouteDecision decision;
    decision.target = RouteTarget::kCloud;
    decision.reason = "complexity_hint";
    decision.matched_rules = {"explain"};
    decision.estimated_tokens = 42;
    recorder.record_route_selected(request, decision);

    StreamedChatDelta delta;
    delta.content = "partial";
    recorder.record_stream_started(request);
    recorder.record_stream_delta(request, delta);

    InferenceResponse response;
    response.ok = true;
    response.request_id = request.seceda.request_id;
    response.requested_target = RouteTarget::kAuto;
    response.initial_target = RouteTarget::kCloud;
    response.final_target = RouteTarget::kCloud;
    response.route_reason = "complexity_hint";
    response.message.role = "assistant";
    response.message.content = "done";
    response.finish_reason = "stop";
    response.total_timing.prompt_tokens = 2;
    response.total_timing.generated_tokens = 1;
    recorder.record_response(request, response);

    const auto batch = traces.get_events(0, 16, request.seceda.request_id);
    if (!require(batch.events.size() == 5, "recorder should emit five trace events")) {
        return 1;
    }
    if (!require(batch.events[0].phase == "route_selected", "first event should record route")) {
        return 1;
    }
    if (!require(batch.events[1].phase == "stream_started", "second event should record stream start")) {
        return 1;
    }
    if (!require(batch.events[2].phase == "stream_delta", "third event should record stream delta")) {
        return 1;
    }
    if (!require(batch.events[3].phase == "output_item", "fourth event should record output item")) {
        return 1;
    }
    if (!require(batch.events[4].phase == "request_complete", "last event should record completion")) {
        return 1;
    }

    const json route_payload = json::parse(batch.events[0].payload_json);
    if (!require(route_payload["target"] == "cloud", "route payload should include target")) {
        return 1;
    }
    if (!require(route_payload["estimated_tokens"] == 42, "route payload should include token estimate")) {
        return 1;
    }

    const json response_payload = json::parse(batch.events[4].payload_json);
    if (!require(response_payload["ok"] == true, "response payload should include ok status")) {
        return 1;
    }
    if (!require(
            response_payload["timing"]["total_tokens"] == 3,
            "response payload should include total token count")) {
        return 1;
    }

    return 0;
}
