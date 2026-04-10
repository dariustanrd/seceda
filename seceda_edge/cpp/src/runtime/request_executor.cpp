#include "runtime/request_executor.hpp"

#include <chrono>

namespace seceda::edge {

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
    response.requested_target = request.route_override;

    if (request.text.empty()) {
        response.error_kind = InferenceErrorKind::kInvalidRequest;
        response.error = "Request text must not be empty";
        response.initial_target = RouteTarget::kLocal;
        response.final_target = RouteTarget::kLocal;
        response.total_timing.total_latency_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - request_start)
                                                     .count();
        metrics_.request_finished(response);
        return response;
    }

    RouteDecision decision;
    if (request.route_override == RouteTarget::kLocal) {
        decision.target = RouteTarget::kLocal;
        decision.reason = "forced_local";
    } else if (request.route_override == RouteTarget::kCloud) {
        decision.target = RouteTarget::kCloud;
        decision.reason = "forced_cloud";
    } else {
        decision = router_.decide(request);
    }

    response.initial_target = decision.target;
    response.final_target = decision.target;
    response.route_reason = decision.reason;
    response.matched_rules = decision.matched_rules;

    if (decision.target == RouteTarget::kLocal) {
        response = execute_local(
            request,
            std::move(response),
            request.route_override == RouteTarget::kAuto);
    } else {
        response = execute_cloud(
            request,
            std::move(response),
            request.route_override == RouteTarget::kAuto);
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
    response.active_model_path = local_runtime_.info().model_path;

    if (!local_runtime_.is_ready()) {
        if (allow_cloud_fallback && cloud_client_.is_configured()) {
            response.fallback_used = true;
            response.fallback_reason = "local_unavailable";
            return execute_cloud(request, std::move(response), false);
        }

        response.error_kind = InferenceErrorKind::kLocalUnavailable;
        response.error = "Local runtime is unavailable";
        return response;
    }

    const auto local_result = local_runtime_.generate(request);
    response.local_timing = local_result.timing;
    response.active_model_path = local_result.active_model_path;

    if (local_result.ok) {
        response.ok = true;
        response.text = local_result.text;
        response.final_target = RouteTarget::kLocal;
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
    return response;
}

InferenceResponse RequestExecutor::execute_cloud(
    const InferenceRequest & request,
    InferenceResponse response,
    bool allow_local_best_effort) {
    if (!cloud_client_.is_configured()) {
        if (allow_local_best_effort && local_runtime_.is_ready()) {
            response.fallback_used = true;
            response.fallback_reason = "cloud_unavailable_best_effort_local";
            return execute_local(request, std::move(response), false);
        }

        response.error_kind = InferenceErrorKind::kCloudUnavailable;
        response.error = "Cloud fallback is unavailable";
        return response;
    }

    const auto cloud_result = cloud_client_.complete(request);
    response.cloud_timing = cloud_result.timing;

    if (cloud_result.ok) {
        response.ok = true;
        response.text = cloud_result.text;
        response.final_target = RouteTarget::kCloud;
        return response;
    }

    if (allow_local_best_effort && local_runtime_.is_ready()) {
        response.fallback_used = true;
        response.fallback_reason =
            cloud_result.error.empty() ? "cloud_failure_best_effort_local" : cloud_result.error;
        return execute_local(request, std::move(response), false);
    }

    response.error_kind = InferenceErrorKind::kCloudFailure;
    response.error = cloud_result.error.empty() ? "Cloud inference failed" : cloud_result.error;
    return response;
}

}  // namespace seceda::edge
