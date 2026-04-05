#include "runtime/request_executor.hpp"

#include <chrono>

namespace seceda::edge {
namespace {

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

}  // namespace

RequestExecutor::RequestExecutor(
    ILocalModelRuntime & local_runtime,
    ICloudClient & cloud_client,
    IRouter & router,
    MetricsRegistry & metrics)
    : local_runtime_(local_runtime),
      cloud_client_(cloud_client),
      router_(router),
      metrics_(metrics) {}

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

    bool stream_started = false;
    const auto tracked_on_delta = [&](const StreamedChatDelta & delta) {
        if (!delta.content.empty() || !delta.tool_calls_json.empty()) {
            stream_started = true;
        }
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
