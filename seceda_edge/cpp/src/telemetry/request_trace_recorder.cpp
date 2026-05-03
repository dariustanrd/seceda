#include "telemetry/request_trace_recorder.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace seceda::edge {
namespace {

using json = nlohmann::json;

json timing_trace_json(const TimingInfo & timing) {
    return {
        {"total_latency_ms", timing.total_latency_ms},
        {"has_ttft", timing.has_ttft},
        {"ttft_ms", timing.ttft_ms},
        {"prompt_tokens", timing.prompt_tokens},
        {"generated_tokens", timing.generated_tokens},
        {"total_tokens", total_token_count(timing)},
    };
}

void push_trace_event(
    TraceRegistry & traces,
    const InferenceRequest & request,
    const std::string & phase,
    const std::string & item_type = {},
    const std::string & item_id = {},
    int content_index = -1,
    const std::string & role = {},
    const std::string & tool_name = {},
    const std::string & delta_text = {},
    const std::string & text = {},
    std::optional<json> payload = std::nullopt) {
    PromptTraceEvent event;
    event.request_id = request.seceda.request_id;
    event.transport = request.seceda.transport;
    event.phase = phase;
    event.item_type = item_type;
    event.item_id = item_id;
    event.content_index = content_index;
    event.role = role;
    event.tool_name = tool_name;
    event.delta_text = delta_text;
    event.text = text;
    if (payload.has_value() && !payload->is_null()) {
        event.payload_json = payload->dump();
    }
    traces.push_event(std::move(event));
}

}  // namespace

RequestTraceRecorder::RequestTraceRecorder(TraceRegistry & traces) : traces_(traces) {}

void RequestTraceRecorder::record_route_selected(
    const InferenceRequest & request,
    const RouteDecision & decision) {
    push_trace_event(
        traces_,
        request,
        "route_selected",
        "route",
        "route",
        -1,
        {},
        {},
        {},
        {},
        json{
            {"target", to_string(decision.target)},
            {"reason", decision.reason},
            {"matched_rules", decision.matched_rules},
            {"estimated_tokens", decision.estimated_tokens},
            {"preferred_engine_id", decision.preferred_engine_id},
            {"resolved_backend_id", decision.resolved_backend_id},
            {"resolved_model_alias", decision.resolved_model_alias},
        });
}

void RequestTraceRecorder::record_stream_started(const InferenceRequest & request) {
    push_trace_event(
        traces_,
        request,
        "stream_started",
        "output_stream",
        "assistant-stream");
}

void RequestTraceRecorder::record_stream_delta(
    const InferenceRequest & request,
    const StreamedChatDelta & delta) {
    if (!delta.content.empty()) {
        push_trace_event(
            traces_,
            request,
            "stream_delta",
            "output_text",
            "assistant-output",
            0,
            "assistant",
            {},
            delta.content);
    }

    if (!delta.tool_calls_json.empty()) {
        json payload;
        try {
            payload = json::parse(delta.tool_calls_json);
        } catch (const std::exception &) {
            payload = json{{"tool_calls_json", delta.tool_calls_json}};
        }
        push_trace_event(
            traces_,
            request,
            "stream_delta",
            "tool_call_delta",
            "assistant-tool-calls",
            0,
            "assistant",
            {},
            {},
            {},
            payload);
    }
}

void RequestTraceRecorder::record_response(
    const InferenceRequest & request,
    const InferenceResponse & response) {
    if (!response.message.content.empty()) {
        push_trace_event(
            traces_,
            request,
            "output_item",
            "assistant_message",
            "assistant-output",
            0,
            response.message.role.empty() ? "assistant" : response.message.role,
            {},
            {},
            response.message.content);
    }

    for (const auto & tool_call : response.message.tool_calls) {
        push_trace_event(
            traces_,
            request,
            "output_item",
            "tool_call",
            tool_call.id,
            0,
            response.message.role.empty() ? "assistant" : response.message.role,
            tool_call.function.name,
            {},
            {},
            json{
                {"id", tool_call.id},
                {"type", tool_call.type},
                {"function",
                 {
                     {"name", tool_call.function.name},
                     {"arguments", tool_call.function.arguments_json},
                 }},
            });
    }

    const std::string phase = response.ok ? "request_complete" : "request_error";
    json payload = {
        {"ok", response.ok},
        {"finish_reason", response.finish_reason},
        {"requested_target", to_string(response.requested_target)},
        {"initial_target", to_string(response.initial_target)},
        {"final_target", to_string(response.final_target)},
        {"route_reason", response.route_reason},
        {"fallback_used", response.fallback_used},
        {"fallback_reason", response.fallback_reason},
        {"error_kind", to_string(response.error_kind)},
        {"error", response.error},
        {"active_model_path", response.active_model_path},
        {"matched_rules", response.matched_rules},
        {"initial_identity",
         {
             {"engine_id", response.initial_identity.engine_id},
             {"backend_id", response.initial_identity.backend_id},
             {"model_id", response.initial_identity.model_id},
             {"model_alias", response.initial_identity.model_alias},
             {"display_name", response.initial_identity.display_name},
             {"execution_mode", response.initial_identity.execution_mode},
         }},
        {"final_identity",
         {
             {"engine_id", response.final_identity.engine_id},
             {"backend_id", response.final_identity.backend_id},
             {"model_id", response.final_identity.model_id},
             {"model_alias", response.final_identity.model_alias},
             {"display_name", response.final_identity.display_name},
             {"execution_mode", response.final_identity.execution_mode},
         }},
        {"timing", timing_trace_json(response.total_timing)},
    };

    if (response.local_timing.has_value()) {
        payload["local_timing"] = timing_trace_json(*response.local_timing);
    }
    if (response.cloud_timing.has_value()) {
        payload["cloud_timing"] = timing_trace_json(*response.cloud_timing);
    }

    push_trace_event(traces_, request, phase, "request", "request", -1, {}, {}, {}, {}, payload);
}

}  // namespace seceda::edge
