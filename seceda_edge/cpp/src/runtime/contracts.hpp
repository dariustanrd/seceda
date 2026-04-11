#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace seceda::edge {

enum class RuntimeState {
    kStarting,
    kLoadingModel,
    kReady,
    kReloadingModel,
    kDegraded,
    kFailed,
};

inline const char * to_string(RuntimeState state) {
    switch (state) {
        case RuntimeState::kStarting:
            return "starting";
        case RuntimeState::kLoadingModel:
            return "loading_model";
        case RuntimeState::kReady:
            return "ready";
        case RuntimeState::kReloadingModel:
            return "reloading_model";
        case RuntimeState::kDegraded:
            return "degraded";
        case RuntimeState::kFailed:
            return "failed";
    }

    return "unknown";
}

enum class RouteTarget {
    kAuto,
    kLocal,
    kCloud,
};

inline const char * to_string(RouteTarget target) {
    switch (target) {
        case RouteTarget::kAuto:
            return "auto";
        case RouteTarget::kLocal:
            return "local";
        case RouteTarget::kCloud:
            return "cloud";
    }

    return "unknown";
}

inline bool parse_route_target(const std::string & value, RouteTarget & target) {
    if (value == "auto") {
        target = RouteTarget::kAuto;
        return true;
    }
    if (value == "local") {
        target = RouteTarget::kLocal;
        return true;
    }
    if (value == "cloud" || value == "remote") {
        target = RouteTarget::kCloud;
        return true;
    }

    return false;
}

enum class InferenceErrorKind {
    kNone,
    kInvalidRequest,
    kUnsupportedFeature,
    kLocalUnavailable,
    kLocalFailure,
    kCloudUnavailable,
    kCloudFailure,
};

inline const char * to_string(InferenceErrorKind kind) {
    switch (kind) {
        case InferenceErrorKind::kNone:
            return "none";
        case InferenceErrorKind::kInvalidRequest:
            return "invalid_request";
        case InferenceErrorKind::kUnsupportedFeature:
            return "unsupported_feature";
        case InferenceErrorKind::kLocalUnavailable:
            return "local_unavailable";
        case InferenceErrorKind::kLocalFailure:
            return "local_failure";
        case InferenceErrorKind::kCloudUnavailable:
            return "cloud_unavailable";
        case InferenceErrorKind::kCloudFailure:
            return "cloud_failure";
    }

    return "unknown";
}

struct ExecutionTargetIdentity {
    RouteTarget route_target = RouteTarget::kLocal;
    std::string engine_id;
    std::string backend_id;
    std::string model_id;
    std::string model_alias;
    std::string display_name;
    std::string execution_mode;
    std::vector<std::string> capabilities;
};

struct ChatMessage {
    std::string role = "user";
    std::string content;
    std::string name;
    std::string tool_call_id;
    std::string tool_calls_json;
};

struct ToolFunctionCall {
    std::string name;
    std::string arguments_json;
};

struct ToolCall {
    std::string id;
    std::string type = "function";
    ToolFunctionCall function;
};

struct AssistantMessage {
    std::string role = "assistant";
    std::string content;
    std::vector<ToolCall> tool_calls;
    std::string refusal;
};

struct GenerationOptions {
    int max_completion_tokens = 128;
    float temperature = 0.7f;
    float top_p = 0.9f;
    int top_k = 40;
    float min_p = 0.05f;
    std::uint32_t seed = 0xFFFFFFFFu;
    bool use_chat_template = true;
    bool stream = false;
    bool include_usage_in_stream = false;
};

struct AdvancedRequestFields {
    std::string tools_json;
    std::string tool_choice_json;
    std::string response_format_json;
    std::vector<std::string> stop_sequences;
    std::string user;
};

struct RequestCapabilities {
    bool has_tools = false;
    bool requests_tool_choice = false;
    bool requests_structured_output = false;
    bool requires_remote_backend = false;
};

struct NormalizedRequestText {
    std::string system_prompt;
    std::string latest_user_message;
    std::string routing_prompt;
};

struct SecedaRequestContext {
    RouteTarget route_override = RouteTarget::kAuto;
    std::string preferred_engine_id;
    std::string preferred_backend_id;
    std::string preferred_model_alias;
    std::string request_id;
    std::string transport = "chat_completions";
};

struct InferenceRequest {
    std::string model;
    std::vector<ChatMessage> messages;
    SecedaRequestContext seceda;
    GenerationOptions options;
    AdvancedRequestFields advanced;
    RequestCapabilities capabilities;
    NormalizedRequestText normalized;
};

inline bool is_system_like_role(const std::string & role) {
    return role == "system" || role == "developer";
}

inline bool is_user_like_role(const std::string & role) {
    return role == "user";
}

inline bool has_system_like_message(const InferenceRequest & request) {
    for (const auto & message : request.messages) {
        if (is_system_like_role(message.role) && !message.content.empty()) {
            return true;
        }
    }
    return false;
}

inline void refresh_request_views(InferenceRequest & request) {
    request.normalized = {};
    request.capabilities.requires_remote_backend =
        request.capabilities.has_tools ||
        request.capabilities.requests_tool_choice ||
        request.capabilities.requests_structured_output;

    for (const auto & message : request.messages) {
        if (message.content.empty()) {
            continue;
        }

        if (is_system_like_role(message.role)) {
            if (!request.normalized.system_prompt.empty()) {
                request.normalized.system_prompt += "\n\n";
            }
            request.normalized.system_prompt += message.content;
        }

        if (is_user_like_role(message.role)) {
            request.normalized.latest_user_message = message.content;
        }

        if (!request.normalized.routing_prompt.empty()) {
            request.normalized.routing_prompt.push_back('\n');
        }

        request.normalized.routing_prompt += message.role.empty() ? "message" : message.role;
        if (!message.name.empty()) {
            request.normalized.routing_prompt += "(" + message.name + ")";
        }
        request.normalized.routing_prompt += ": ";
        request.normalized.routing_prompt += message.content;
    }

    if (request.normalized.latest_user_message.empty()) {
        for (auto it = request.messages.rbegin(); it != request.messages.rend(); ++it) {
            if (!it->content.empty()) {
                request.normalized.latest_user_message = it->content;
                break;
            }
        }
    }
}

inline void prepend_system_message(InferenceRequest & request, const std::string & system_prompt) {
    if (system_prompt.empty() || has_system_like_message(request)) {
        return;
    }

    request.messages.insert(request.messages.begin(), ChatMessage{"system", system_prompt, {}});
    refresh_request_views(request);
}

struct TimingInfo {
    double total_latency_ms = 0.0;
    double ttft_ms = 0.0;
    bool has_ttft = false;
    int prompt_tokens = 0;
    int generated_tokens = 0;
};

inline int total_token_count(const TimingInfo & timing) {
    return timing.prompt_tokens + timing.generated_tokens;
}

struct RouteDecision {
    RouteTarget target = RouteTarget::kLocal;
    std::string reason;
    std::vector<std::string> matched_rules;
    int estimated_tokens = 0;
    std::string preferred_engine_id;
    std::string resolved_backend_id;
    std::string resolved_model_alias;
};

struct InferenceResponse {
    bool ok = false;
    InferenceErrorKind error_kind = InferenceErrorKind::kNone;
    std::string request_id;
    std::string text;
    AssistantMessage message;
    std::string finish_reason = "stop";
    std::string error;
    RouteTarget requested_target = RouteTarget::kAuto;
    RouteTarget initial_target = RouteTarget::kLocal;
    RouteTarget final_target = RouteTarget::kLocal;
    bool fallback_used = false;
    std::string fallback_reason;
    std::string route_reason;
    std::vector<std::string> matched_rules;
    TimingInfo total_timing;
    std::optional<TimingInfo> local_timing;
    std::optional<TimingInfo> cloud_timing;
    ExecutionTargetIdentity initial_identity;
    ExecutionTargetIdentity final_identity;
    std::string active_model_path;
};

struct LocalModelConfig {
    std::string engine_id = "local/llama.cpp";
    std::string backend_id = "local";
    std::string model_id = "local-default";
    std::string model_alias = "local/default";
    std::string display_name = "Seceda local llama.cpp";
    std::string execution_mode = "in_process";
    std::vector<std::string> capabilities = {"chat.completions", "text", "stream"};
    std::string model_path;
    int context_size = 2048;
    int batch_size = 2048;
    int n_gpu_layers = 99;
    int n_threads = -1;
    int n_threads_batch = -1;
    std::string sidecar_base_url;
    int sidecar_timeout_seconds = 120;
    int sidecar_connect_timeout_seconds = 10;
    bool sidecar_verify_tls = true;
};

struct RouterConfig {
    std::size_t max_prompt_chars = 800;
    int max_estimated_tokens = 256;
    std::vector<std::string> structured_keywords = {
        "json",
        "schema",
        "sql",
        "typescript",
        "javascript",
        "python",
        "yaml",
        "xml",
        "csv",
        "formal proof",
    };
    std::vector<std::string> cloud_keywords = {
        "analyze",
        "compare",
        "plan",
        "strategy",
        "architecture",
        "reason",
        "step by step",
        "research",
    };
    std::vector<std::string> freshness_keywords = {
        "today",
        "latest",
        "current",
        "recent",
        "news",
        "weather",
        "price",
        "stock",
    };
};

struct CloudConfig {
    std::string backend_id = "remote/modal-default";
    std::string model = "seceda-cloud-default";
    std::string model_alias = "remote/default";
    std::string display_name = "Seceda remote backend";
    std::string execution_mode = "remote_service";
    std::vector<std::string> capabilities = {
        "chat.completions",
        "text",
        "stream",
        "tools",
        "response_format",
    };
    std::string base_url;
    std::string api_key;
    int timeout_seconds = 120;
    int connect_timeout_seconds = 10;
    int retry_attempts = 2;
    int retry_backoff_ms = 1000;
    bool send_modal_session_id = false;
    bool verify_tls = true;
};

struct ModelCatalogEntry {
    std::string id;
    std::string display_name;
    std::string owned_by = "seceda";
    RouteTarget route_target = RouteTarget::kAuto;
    std::string engine_id;
    std::string backend_id;
    std::string model_id;
    std::string model_alias;
    std::string execution_mode;
    std::vector<std::string> capabilities;
};

struct DaemonConfig {
    std::string host = "0.0.0.0";
    int port = 8080;
    std::string default_system_prompt;
    std::string warmup_prompt = "Hello from Seceda.";
    std::string public_model_alias = "seceda/default";
    GenerationOptions default_generation;
    LocalModelConfig local;
    std::vector<LocalModelConfig> local_engines;
    RouterConfig router;
    CloudConfig cloud;
    std::vector<CloudConfig> remote_backends;
    std::size_t event_log_capacity = 2048;
    std::vector<ModelCatalogEntry> exposed_models = {
        {"seceda/default", "Seceda default route", "seceda"},
    };
};

struct LocalModelInfo {
    bool ready = false;
    std::string engine_id;
    std::string backend_id;
    std::string model_id;
    std::string model_alias;
    std::string display_name;
    std::string execution_mode;
    std::vector<std::string> capabilities;
    std::string model_path;
    std::string endpoint_base_url;
    std::string description;
    std::string last_error;
    int context_size = 0;
    int batch_size = 0;
    int n_gpu_layers = 0;
    bool has_chat_template = false;
    std::uint64_t model_size_bytes = 0;
    std::uint64_t parameter_count = 0;
    int trained_context_size = 0;
    int layer_count = 0;
};

struct CloudClientInfo {
    bool configured = false;
    std::string backend_id;
    std::string model;
    std::string model_alias;
    std::string display_name;
    std::string execution_mode;
    std::vector<std::string> capabilities;
    std::string base_url;
    int timeout_seconds = 0;
    int connect_timeout_seconds = 0;
    int retry_attempts = 0;
    int retry_backoff_ms = 0;
    bool send_modal_session_id = false;
    bool verify_tls = true;
};

struct ModelReloadResult {
    bool ok = false;
    std::string active_model_path;
    std::string error;
};

struct HealthSnapshot {
    RuntimeState state = RuntimeState::kStarting;
    bool local_ready = false;
    bool cloud_configured = false;
    std::string active_model_path;
    std::string last_error;
};

struct InfoSnapshot {
    RuntimeState state = RuntimeState::kStarting;
    std::string host;
    int port = 0;
    std::string default_system_prompt;
    std::string public_model_alias;
    std::vector<ModelCatalogEntry> exposed_models;
    RouterConfig router_config;
    LocalModelInfo local_model;
    CloudClientInfo cloud_client;
    std::vector<LocalModelConfig> configured_local_engines;
    std::vector<CloudConfig> configured_remote_backends;
    std::string last_error;
};

struct InferenceEvent {
    std::uint64_t id = 0;
    std::string request_id;
    std::string timestamp_utc;
    RouteTarget requested_target = RouteTarget::kAuto;
    RouteTarget initial_target = RouteTarget::kLocal;
    RouteTarget final_target = RouteTarget::kLocal;
    bool ok = false;
    bool fallback_used = false;
    InferenceErrorKind error_kind = InferenceErrorKind::kNone;
    std::string error;
    std::string finish_reason = "stop";
    std::string route_reason;
    std::string fallback_reason;
    std::vector<std::string> matched_rules;
    std::string initial_engine_id;
    std::string initial_backend_id;
    std::string initial_model_id;
    std::string initial_model_alias;
    std::string initial_execution_mode;
    std::string engine_id;
    std::string backend_id;
    std::string model_id;
    std::string model_alias;
    std::string display_name;
    std::string execution_mode;
    std::string active_model_path;
    TimingInfo timing;
    std::optional<TimingInfo> local_timing;
    std::optional<TimingInfo> cloud_timing;
};

struct EventBatch {
    std::uint64_t since_id = 0;
    std::uint64_t latest_id = 0;
    std::vector<InferenceEvent> events;
};

struct PromptTraceEvent {
    std::uint64_t id = 0;
    std::uint64_t sequence_number = 0;
    std::string request_id;
    std::string transport = "chat_completions";
    std::string timestamp_utc;
    std::string phase;
    std::string item_type;
    std::string item_id;
    int content_index = -1;
    std::string role;
    std::string tool_name;
    std::string delta_text;
    std::string text;
    std::string payload_json;
};

struct PromptTraceBatch {
    std::uint64_t since_id = 0;
    std::uint64_t latest_id = 0;
    std::vector<PromptTraceEvent> events;
};

}  // namespace seceda::edge
