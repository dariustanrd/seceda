#include "runtime/request_executor.hpp"

#include <chrono>
#include <nlohmann/json.hpp>
#include <optional>

namespace seceda::edge {
namespace {

using json = nlohmann::json;

ExecutionTargetIdentity local_identity_from_info(const LocalModelInfo & info) {
    ExecutionTargetIdentity identity;
    identity.route_target = RouteTarget::kLocal;
    identity.engine_id = info.engine_id;
    identity.backend_id = info.backend_id;
    identity.model_id = info.model_id;
    identity.model_alias = info.model_alias;
    identity.display_name = info.display_name;
    identity.execution_mode = info.execution_mode;
    identity.capabilities = info.capabilities;
    return identity;
}

ExecutionTargetIdentity cloud_identity_from_info(const CloudClientInfo & info) {
    ExecutionTargetIdentity identity;
    identity.route_target = RouteTarget::kCloud;
    identity.backend_id = info.backend_id;
    identity.model_id = info.model;
    identity.model_alias = info.model_alias;
    identity.display_name = info.display_name;
    identity.execution_mode = info.execution_mode;
    identity.capabilities = info.capabilities;
    return identity;
}

void hydrate_assistant_message(InferenceResponse & response) {
    if (response.message.role.empty()) {
        response.message.role = "assistant";
    }
    if (response.message.content.empty() && !response.text.empty()) {
        response.message.content = response.text;
    }
    if (response.text.empty() && !response.message.content.empty()) {
        response.text = response.message.content;
    }
}

bool capabilities_include(
    const std::vector<std::string> & capabilities,
    const std::string & required_capability) {
    for (const auto & capability : capabilities) {
        if (capability == required_capability) {
            return true;
        }
    }
    return false;
}

bool local_runtime_supports_request_features(
    const InferenceRequest & request,
    const LocalModelInfo & info) {
    if (request.capabilities.has_tools || request.capabilities.requests_tool_choice) {
        if (!capabilities_include(info.capabilities, "tools")) {
            return false;
        }
    }
    if (request.capabilities.requests_structured_output &&
        !capabilities_include(info.capabilities, "response_format")) {
        return false;
    }
    return true;
}

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

void emit_route_trace(
    TraceRegistry & traces,
    const InferenceRequest & request,
    const RouteDecision & decision) {
    push_trace_event(
        traces,
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

void emit_stream_delta_trace(
    TraceRegistry & traces,
    const InferenceRequest & request,
    const StreamedChatDelta & delta) {
    if (!delta.content.empty()) {
        push_trace_event(
            traces,
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
            traces,
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

void emit_response_trace_events(
    TraceRegistry & traces,
    const InferenceRequest & request,
    const InferenceResponse & response) {
    if (!response.message.content.empty()) {
        push_trace_event(
            traces,
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
            traces,
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

    push_trace_event(traces, request, phase, "request", "request", -1, {}, {}, {}, {}, payload);
}

}  // namespace

RequestExecutor::RequestExecutor(
    ILocalModelRuntime & local_runtime,
    ICloudClient & cloud_client,
    IRouter & router,
    MetricsRegistry & metrics,
    TraceRegistry & traces)
    : local_runtime_(local_runtime),
      cloud_client_(cloud_client),
      router_(router),
      metrics_(metrics),
      traces_(traces) {}

InferenceResponse RequestExecutor::execute(const InferenceRequest & request) {
    metrics_.request_started();
    const auto request_start = std::chrono::steady_clock::now();

    InferenceResponse response;
    response.request_id = request.seceda.request_id;
    response.requested_target = request.seceda.route_override;

    if (request.messages.empty() || request.normalized.latest_user_message.empty()) {
        response.error_kind = InferenceErrorKind::kInvalidRequest;
        response.error = "Request messages must include at least one non-empty user message";
        response.initial_target = RouteTarget::kLocal;
        response.final_target = RouteTarget::kLocal;
        response.total_timing.total_latency_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - request_start)
                                                     .count();
        emit_response_trace_events(traces_, request, response);
        metrics_.request_finished(response);
        return response;
    }

    RouteDecision decision;
    if (request.seceda.route_override == RouteTarget::kLocal) {
        decision.target = RouteTarget::kLocal;
        decision.reason = "forced_local";
    } else if (request.seceda.route_override == RouteTarget::kCloud) {
        decision.target = RouteTarget::kCloud;
        decision.reason = "forced_cloud";
    } else {
        decision = router_.decide(request);
    }

    response.initial_target = decision.target;
    response.final_target = decision.target;
    response.route_reason = decision.reason;
    response.matched_rules = decision.matched_rules;
    response.initial_identity.route_target = decision.target;
    response.initial_identity.engine_id = decision.preferred_engine_id;
    response.initial_identity.backend_id = decision.resolved_backend_id;
    response.initial_identity.model_alias = decision.resolved_model_alias;
    emit_route_trace(traces_, request, decision);

    if (decision.target == RouteTarget::kLocal) {
        response = execute_local(
            request,
            std::move(response),
            request.seceda.route_override == RouteTarget::kAuto);
    } else {
        response = execute_cloud(
            request,
            std::move(response),
            request.seceda.route_override == RouteTarget::kAuto);
    }

    response.total_timing.total_latency_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - request_start)
                                                 .count();
    emit_response_trace_events(traces_, request, response);
    metrics_.request_finished(response);
    return response;
}

InferenceResponse RequestExecutor::execute_streaming(
    const InferenceRequest & request,
    const StreamDeltaCallback & on_delta) {
    metrics_.request_started();
    const auto request_start = std::chrono::steady_clock::now();

    InferenceResponse response;
    response.request_id = request.seceda.request_id;
    response.requested_target = request.seceda.route_override;

    if (request.messages.empty() || request.normalized.latest_user_message.empty()) {
        response.error_kind = InferenceErrorKind::kInvalidRequest;
        response.error = "Request messages must include at least one non-empty user message";
        response.initial_target = RouteTarget::kLocal;
        response.final_target = RouteTarget::kLocal;
        response.total_timing.total_latency_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - request_start)
                                                     .count();
        emit_response_trace_events(traces_, request, response);
        metrics_.request_finished(response);
        return response;
    }

    RouteDecision decision;
    if (request.seceda.route_override == RouteTarget::kLocal) {
        decision.target = RouteTarget::kLocal;
        decision.reason = "forced_local";
    } else if (request.seceda.route_override == RouteTarget::kCloud) {
        decision.target = RouteTarget::kCloud;
        decision.reason = "forced_cloud";
    } else {
        decision = router_.decide(request);
    }

    response.initial_target = decision.target;
    response.final_target = decision.target;
    response.route_reason = decision.reason;
    response.matched_rules = decision.matched_rules;
    response.initial_identity.route_target = decision.target;
    response.initial_identity.engine_id = decision.preferred_engine_id;
    response.initial_identity.backend_id = decision.resolved_backend_id;
    response.initial_identity.model_alias = decision.resolved_model_alias;
    emit_route_trace(traces_, request, decision);

    bool stream_started = false;
    const auto tracked_on_delta = [&](const StreamedChatDelta & delta) {
        if (!stream_started && (!delta.content.empty() || !delta.tool_calls_json.empty())) {
            push_trace_event(
                traces_,
                request,
                "stream_started",
                "output_stream",
                "assistant-stream");
        }
        if (!delta.content.empty() || !delta.tool_calls_json.empty()) {
            stream_started = true;
        }
        emit_stream_delta_trace(traces_, request, delta);
        return on_delta(delta);
    };

    if (decision.target == RouteTarget::kLocal) {
        response = execute_local_streaming(
            request,
            std::move(response),
            request.seceda.route_override == RouteTarget::kAuto,
            tracked_on_delta,
            stream_started);
    } else {
        response = execute_cloud_streaming(
            request,
            std::move(response),
            request.seceda.route_override == RouteTarget::kAuto,
            tracked_on_delta,
            stream_started);
    }

    response.total_timing.total_latency_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - request_start)
                                                 .count();
    emit_response_trace_events(traces_, request, response);
    metrics_.request_finished(response);
    return response;
}

InferenceResponse RequestExecutor::execute_local(
    const InferenceRequest & request,
    InferenceResponse response,
    bool allow_cloud_fallback) {
    const auto local_info = local_runtime_.info();
    response.active_model_path = local_info.model_path;
    if (response.initial_identity.engine_id.empty()) {
        response.initial_identity = local_identity_from_info(local_info);
    }

    if (request.capabilities.requires_remote_backend &&
        !local_runtime_supports_request_features(request, local_info)) {
        if (allow_cloud_fallback && cloud_client_.is_configured()) {
            response.fallback_used = true;
            response.fallback_reason = "remote_capability_required";
            return execute_cloud(request, std::move(response), false);
        }

        response.error_kind = InferenceErrorKind::kUnsupportedFeature;
        response.error = "Requested OpenAI features require a configured remote backend";
        response.final_identity = local_identity_from_info(local_info);
        return response;
    }

    if (!local_runtime_.is_ready()) {
        if (allow_cloud_fallback && cloud_client_.is_configured()) {
            response.fallback_used = true;
            response.fallback_reason = "local_unavailable";
            return execute_cloud(request, std::move(response), false);
        }

        response.error_kind = InferenceErrorKind::kLocalUnavailable;
        response.error = "Local runtime is unavailable";
        response.final_identity = local_identity_from_info(local_info);
        return response;
    }

    const auto local_result = local_runtime_.generate(request);
    response.local_timing = local_result.timing;
    response.total_timing = local_result.timing;
    response.active_model_path = local_result.active_model_path;

    if (local_result.ok) {
        response.ok = true;
        response.text = local_result.text;
        response.message = local_result.message;
        response.finish_reason = local_result.finish_reason;
        response.final_target = RouteTarget::kLocal;
        response.final_identity =
            local_result.identity.engine_id.empty() ? local_identity_from_info(local_info)
                                                    : local_result.identity;
        hydrate_assistant_message(response);
        return response;
    }

    if (allow_cloud_fallback && cloud_client_.is_configured()) {
        response.fallback_used = true;
        response.fallback_reason =
            local_result.error.empty() ? "local_failure" : local_result.error;
        response.error.clear();
        response.error_kind = InferenceErrorKind::kNone;
        return execute_cloud(request, std::move(response), false);
    }

    response.error_kind = InferenceErrorKind::kLocalFailure;
    response.error = local_result.error.empty() ? "Local inference failed" : local_result.error;
    response.final_identity = local_identity_from_info(local_info);
    return response;
}

InferenceResponse RequestExecutor::execute_cloud(
    const InferenceRequest & request,
    InferenceResponse response,
    bool allow_local_best_effort) {
    const auto cloud_info = cloud_client_.info();
    if (response.initial_identity.backend_id.empty()) {
        response.initial_identity = cloud_identity_from_info(cloud_info);
    }

    if (!cloud_client_.is_configured()) {
        if (allow_local_best_effort &&
            local_runtime_.is_ready() &&
            !request.capabilities.requires_remote_backend) {
            response.fallback_used = true;
            response.fallback_reason = "cloud_unavailable_best_effort_local";
            return execute_local(request, std::move(response), false);
        }

        response.error_kind = InferenceErrorKind::kCloudUnavailable;
        response.error = request.capabilities.requires_remote_backend
            ? "Remote backend required by request is unavailable"
            : "Cloud fallback is unavailable";
        response.final_identity = cloud_identity_from_info(cloud_info);
        return response;
    }

    const auto cloud_result = cloud_client_.complete(request);
    response.cloud_timing = cloud_result.timing;
    response.total_timing = cloud_result.timing;

    if (cloud_result.ok) {
        response.ok = true;
        response.text = cloud_result.text;
        response.message = cloud_result.message;
        response.finish_reason = cloud_result.finish_reason;
        response.final_target = RouteTarget::kCloud;
        response.final_identity =
            cloud_result.identity.backend_id.empty() ? cloud_identity_from_info(cloud_info)
                                                     : cloud_result.identity;
        hydrate_assistant_message(response);
        return response;
    }

    if (allow_local_best_effort &&
        local_runtime_.is_ready() &&
        !request.capabilities.requires_remote_backend) {
        response.fallback_used = true;
        response.fallback_reason =
            cloud_result.error.empty() ? "cloud_failure_best_effort_local" : cloud_result.error;
        return execute_local(request, std::move(response), false);
    }

    response.error_kind = InferenceErrorKind::kCloudFailure;
    response.error = cloud_result.error.empty() ? "Cloud inference failed" : cloud_result.error;
    response.final_identity = cloud_identity_from_info(cloud_info);
    return response;
}

InferenceResponse RequestExecutor::execute_local_streaming(
    const InferenceRequest & request,
    InferenceResponse response,
    bool allow_cloud_fallback,
    const StreamDeltaCallback & on_delta,
    bool & stream_started) {
    const auto local_info = local_runtime_.info();
    response.active_model_path = local_info.model_path;
    if (response.initial_identity.engine_id.empty()) {
        response.initial_identity = local_identity_from_info(local_info);
    }

    if (request.capabilities.requires_remote_backend &&
        !local_runtime_supports_request_features(request, local_info)) {
        if (!stream_started && allow_cloud_fallback && cloud_client_.is_configured()) {
            response.fallback_used = true;
            response.fallback_reason = "remote_capability_required";
            return execute_cloud_streaming(request, std::move(response), false, on_delta, stream_started);
        }

        response.error_kind = InferenceErrorKind::kUnsupportedFeature;
        response.error = "Requested OpenAI features require a configured remote backend";
        response.final_identity = local_identity_from_info(local_info);
        return response;
    }

    if (!local_runtime_.is_ready()) {
        if (!stream_started && allow_cloud_fallback && cloud_client_.is_configured()) {
            response.fallback_used = true;
            response.fallback_reason = "local_unavailable";
            return execute_cloud_streaming(request, std::move(response), false, on_delta, stream_started);
        }

        response.error_kind = InferenceErrorKind::kLocalUnavailable;
        response.error = "Local runtime is unavailable";
        response.final_identity = local_identity_from_info(local_info);
        return response;
    }

    const auto local_result = local_runtime_.generate_stream(request, on_delta);
    response.local_timing = local_result.timing;
    response.total_timing = local_result.timing;
    response.active_model_path = local_result.active_model_path;

    if (local_result.ok) {
        response.ok = true;
        response.text = local_result.text;
        response.message = local_result.message;
        response.finish_reason = local_result.finish_reason;
        response.final_target = RouteTarget::kLocal;
        response.final_identity =
            local_result.identity.engine_id.empty() ? local_identity_from_info(local_info)
                                                    : local_result.identity;
        hydrate_assistant_message(response);
        return response;
    }

    if (!stream_started && allow_cloud_fallback && cloud_client_.is_configured()) {
        response.fallback_used = true;
        response.fallback_reason =
            local_result.error.empty() ? "local_failure" : local_result.error;
        response.error.clear();
        response.error_kind = InferenceErrorKind::kNone;
        return execute_cloud_streaming(request, std::move(response), false, on_delta, stream_started);
    }

    response.error_kind = InferenceErrorKind::kLocalFailure;
    response.error = local_result.error.empty() ? "Local inference failed" : local_result.error;
    response.final_identity =
        local_result.identity.engine_id.empty() ? local_identity_from_info(local_info)
                                                : local_result.identity;
    return response;
}

InferenceResponse RequestExecutor::execute_cloud_streaming(
    const InferenceRequest & request,
    InferenceResponse response,
    bool allow_local_best_effort,
    const StreamDeltaCallback & on_delta,
    bool & stream_started) {
    const auto cloud_info = cloud_client_.info();
    if (response.initial_identity.backend_id.empty()) {
        response.initial_identity = cloud_identity_from_info(cloud_info);
    }

    if (!cloud_client_.is_configured()) {
        if (!stream_started &&
            allow_local_best_effort &&
            local_runtime_.is_ready() &&
            !request.capabilities.requires_remote_backend) {
            response.fallback_used = true;
            response.fallback_reason = "cloud_unavailable_best_effort_local";
            return execute_local_streaming(request, std::move(response), false, on_delta, stream_started);
        }

        response.error_kind = InferenceErrorKind::kCloudUnavailable;
        response.error = request.capabilities.requires_remote_backend
            ? "Remote backend required by request is unavailable"
            : "Cloud fallback is unavailable";
        response.final_identity = cloud_identity_from_info(cloud_info);
        return response;
    }

    const auto cloud_result = cloud_client_.complete_stream(request, on_delta);
    response.cloud_timing = cloud_result.timing;
    response.total_timing = cloud_result.timing;

    if (cloud_result.ok) {
        response.ok = true;
        response.text = cloud_result.text;
        response.message = cloud_result.message;
        response.finish_reason = cloud_result.finish_reason;
        response.final_target = RouteTarget::kCloud;
        response.final_identity =
            cloud_result.identity.backend_id.empty() ? cloud_identity_from_info(cloud_info)
                                                     : cloud_result.identity;
        hydrate_assistant_message(response);
        return response;
    }

    if (!stream_started &&
        allow_local_best_effort &&
        local_runtime_.is_ready() &&
        !request.capabilities.requires_remote_backend) {
        response.fallback_used = true;
        response.fallback_reason =
            cloud_result.error.empty() ? "cloud_failure_best_effort_local" : cloud_result.error;
        return execute_local_streaming(request, std::move(response), false, on_delta, stream_started);
    }

    response.error_kind = InferenceErrorKind::kCloudFailure;
    response.error = cloud_result.error.empty() ? "Cloud inference failed" : cloud_result.error;
    response.final_identity =
        cloud_result.identity.backend_id.empty() ? cloud_identity_from_info(cloud_info)
                                                 : cloud_result.identity;
    return response;
}

}  // namespace seceda::edge
