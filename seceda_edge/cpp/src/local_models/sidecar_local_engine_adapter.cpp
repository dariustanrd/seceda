#include "local_models/sidecar_local_engine_adapter.hpp"

#include <utility>

namespace seceda::edge {
namespace {

std::vector<std::string> ensure_sidecar_capabilities(std::vector<std::string> capabilities) {
    const std::vector<std::string> defaults = {
        "chat.completions",
        "text",
        "stream",
        "tools",
        "response_format",
    };

    for (const auto & capability : defaults) {
        bool seen = false;
        for (const auto & existing : capabilities) {
            if (existing == capability) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            capabilities.push_back(capability);
        }
    }

    return capabilities;
}

}  // namespace

bool SidecarLocalEngineAdapter::load(
    const LocalModelConfig & config,
    const std::string &,
    std::string & error) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;

    if (config.sidecar_base_url.empty()) {
        last_error_ = "Local sidecar base URL is empty";
        error = last_error_;
        ready_ = false;
        return false;
    }

    client_ = CloudClient(sidecar_cloud_config(config_));
    if (!client_.is_configured()) {
        last_error_ = "Local sidecar adapter is not configured";
        error = last_error_;
        ready_ = false;
        return false;
    }

    last_error_.clear();
    error.clear();
    ready_ = true;
    return true;
}

bool SidecarLocalEngineAdapter::reload(
    const LocalModelConfig & config,
    const std::string & warmup_prompt,
    std::string & error) {
    return load(config, warmup_prompt, error);
}

bool SidecarLocalEngineAdapter::is_ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ready_;
}

LocalModelInfo SidecarLocalEngineAdapter::info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return to_local_info(config_, ready_, last_error_);
}

LocalCompletionResult SidecarLocalEngineAdapter::generate(const InferenceRequest & request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_) {
        LocalCompletionResult result;
        result.error = last_error_.empty() ? "Local sidecar adapter is not ready" : last_error_;
        result.identity.route_target = RouteTarget::kLocal;
        result.identity.engine_id = config_.engine_id;
        result.identity.backend_id = config_.backend_id;
        result.identity.model_id = config_.model_id;
        result.identity.model_alias = config_.model_alias;
        result.identity.display_name = config_.display_name;
        result.identity.execution_mode = config_.execution_mode;
        result.identity.capabilities = sidecar_capabilities(config_);
        return result;
    }

    const auto cloud_result = client_.complete(request);
    auto result = to_local_result(config_, cloud_result);
    if (!result.ok) {
        last_error_ = result.error;
    } else {
        last_error_.clear();
    }
    return result;
}

LocalCompletionResult SidecarLocalEngineAdapter::generate_stream(
    const InferenceRequest & request,
    const StreamDeltaCallback & on_delta) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_) {
        LocalCompletionResult result;
        result.error = last_error_.empty() ? "Local sidecar adapter is not ready" : last_error_;
        result.identity.route_target = RouteTarget::kLocal;
        result.identity.engine_id = config_.engine_id;
        result.identity.backend_id = config_.backend_id;
        result.identity.model_id = config_.model_id;
        result.identity.model_alias = config_.model_alias;
        result.identity.display_name = config_.display_name;
        result.identity.execution_mode = config_.execution_mode;
        result.identity.capabilities = sidecar_capabilities(config_);
        return result;
    }

    const auto cloud_result = client_.complete_stream(request, on_delta);
    auto result = to_local_result(config_, cloud_result);
    if (!result.ok) {
        last_error_ = result.error;
    } else {
        last_error_.clear();
    }
    return result;
}

CloudConfig SidecarLocalEngineAdapter::sidecar_cloud_config(const LocalModelConfig & config) {
    CloudConfig cloud;
    cloud.backend_id = config.backend_id;
    cloud.model = config.model_id;
    cloud.model_alias = config.model_alias;
    cloud.display_name = config.display_name;
    cloud.execution_mode = config.execution_mode;
    cloud.capabilities = sidecar_capabilities(config);
    cloud.base_url = config.sidecar_base_url;
    cloud.timeout_seconds = config.sidecar_timeout_seconds;
    cloud.connect_timeout_seconds = config.sidecar_connect_timeout_seconds;
    cloud.retry_attempts = 0;
    cloud.retry_backoff_ms = 0;
    cloud.verify_tls = config.sidecar_verify_tls;
    return cloud;
}

std::vector<std::string> SidecarLocalEngineAdapter::sidecar_capabilities(
    const LocalModelConfig & config) {
    return ensure_sidecar_capabilities(config.capabilities);
}

LocalCompletionResult SidecarLocalEngineAdapter::to_local_result(
    const LocalModelConfig & config,
    const CloudCompletionResult & cloud_result) {
    LocalCompletionResult result;
    result.ok = cloud_result.ok;
    result.text = cloud_result.text;
    result.message = cloud_result.message;
    result.finish_reason = cloud_result.finish_reason;
    result.error = cloud_result.error;
    result.timing = cloud_result.timing;
    result.identity.route_target = RouteTarget::kLocal;
    result.identity.engine_id = config.engine_id;
    result.identity.backend_id = config.backend_id;
    result.identity.model_id = config.model_id;
    result.identity.model_alias = config.model_alias;
    result.identity.display_name = config.display_name;
    result.identity.execution_mode = config.execution_mode;
    result.identity.capabilities = sidecar_capabilities(config);
    return result;
}

LocalModelInfo SidecarLocalEngineAdapter::to_local_info(
    const LocalModelConfig & config,
    bool ready,
    const std::string & last_error) {
    LocalModelInfo info;
    info.ready = ready;
    info.engine_id = config.engine_id;
    info.backend_id = config.backend_id;
    info.model_id = config.model_id;
    info.model_alias = config.model_alias;
    info.display_name = config.display_name;
    info.execution_mode = config.execution_mode;
    info.capabilities = sidecar_capabilities(config);
    info.endpoint_base_url = config.sidecar_base_url;
    info.description = "Externally managed localhost OpenAI-compatible sidecar";
    info.last_error = last_error;
    return info;
}

}  // namespace seceda::edge
