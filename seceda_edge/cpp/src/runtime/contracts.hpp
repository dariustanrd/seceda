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
    if (value == "cloud") {
        target = RouteTarget::kCloud;
        return true;
    }

    return false;
}

enum class InferenceErrorKind {
    kNone,
    kInvalidRequest,
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

struct GenerationOptions {
    int max_tokens = 128;
    float temperature = 0.7f;
    float top_p = 0.9f;
    int top_k = 40;
    float min_p = 0.05f;
    std::uint32_t seed = 0xFFFFFFFFu;
    bool use_chat_template = true;
};

struct InferenceRequest {
    std::string text;
    std::string system_prompt;
    RouteTarget route_override = RouteTarget::kAuto;
    GenerationOptions options;
};

struct TimingInfo {
    double total_latency_ms = 0.0;
    double ttft_ms = 0.0;
    bool has_ttft = false;
    int prompt_tokens = 0;
    int generated_tokens = 0;
};

struct RouteDecision {
    RouteTarget target = RouteTarget::kLocal;
    std::string reason;
    std::vector<std::string> matched_rules;
    int estimated_tokens = 0;
};

struct InferenceResponse {
    bool ok = false;
    InferenceErrorKind error_kind = InferenceErrorKind::kNone;
    std::string text;
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
    std::string active_model_path;
};

struct LocalModelConfig {
    std::string model_path;
    int context_size = 2048;
    int batch_size = 2048;
    int n_gpu_layers = 99;
    int n_threads = -1;
    int n_threads_batch = -1;
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
    std::string base_url;
    std::string model = "seceda-cloud-default";
    std::string api_key;
    int timeout_seconds = 120;
    bool verify_tls = true;
};

struct DaemonConfig {
    std::string host = "0.0.0.0";
    int port = 8080;
    std::string default_system_prompt;
    std::string warmup_prompt = "Hello from Seceda.";
    GenerationOptions default_generation;
    LocalModelConfig local;
    RouterConfig router;
    CloudConfig cloud;
    std::size_t event_log_capacity = 2048;
};

struct LocalModelInfo {
    bool ready = false;
    std::string model_path;
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
    std::string base_url;
    std::string model;
    int timeout_seconds = 0;
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
    RouterConfig router_config;
    LocalModelInfo local_model;
    CloudClientInfo cloud_client;
    std::string last_error;
};

struct InferenceEvent {
    std::uint64_t id = 0;
    std::string timestamp_utc;
    RouteTarget initial_target = RouteTarget::kLocal;
    RouteTarget final_target = RouteTarget::kLocal;
    bool ok = false;
    bool fallback_used = false;
    InferenceErrorKind error_kind = InferenceErrorKind::kNone;
    std::string route_reason;
    std::string fallback_reason;
    double total_latency_ms = 0.0;
    double local_latency_ms = 0.0;
    double cloud_latency_ms = 0.0;
};

struct EventBatch {
    std::uint64_t since_id = 0;
    std::uint64_t latest_id = 0;
    std::vector<InferenceEvent> events;
};

}  // namespace seceda::edge
