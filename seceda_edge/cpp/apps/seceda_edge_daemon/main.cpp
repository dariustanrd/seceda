#include "cloud_bridge/cloud_client.hpp"
#include "local_models/llama_runtime.hpp"
#include "router/heuristic_router.hpp"
#include "runtime/edge_daemon.hpp"
#include "runtime/runtime_config.hpp"

#include <cpp-httplib/httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

using json = nlohmann::json;
using namespace seceda::edge;

json router_config_to_json(const RouterConfig & config) {
    return {
        {"max_prompt_chars", config.max_prompt_chars},
        {"max_estimated_tokens", config.max_estimated_tokens},
        {"structured_keywords", config.structured_keywords},
        {"cloud_keywords", config.cloud_keywords},
        {"freshness_keywords", config.freshness_keywords},
    };
}

json model_catalog_entry_to_json(const ModelCatalogEntry & entry) {
    return {
        {"id", entry.id},
        {"object", "model"},
        {"created", 0},
        {"owned_by", entry.owned_by},
        {"display_name", entry.display_name},
    };
}

json local_model_info_to_json(const LocalModelInfo & info) {
    return {
        {"ready", info.ready},
        {"engine_id", info.engine_id},
        {"backend_id", info.backend_id},
        {"model_id", info.model_id},
        {"model_alias", info.model_alias},
        {"display_name", info.display_name},
        {"execution_mode", info.execution_mode},
        {"capabilities", info.capabilities},
        {"model_path", info.model_path},
        {"description", info.description},
        {"last_error", info.last_error},
        {"context_size", info.context_size},
        {"batch_size", info.batch_size},
        {"n_gpu_layers", info.n_gpu_layers},
        {"has_chat_template", info.has_chat_template},
        {"model_size_bytes", info.model_size_bytes},
        {"parameter_count", info.parameter_count},
        {"trained_context_size", info.trained_context_size},
        {"layer_count", info.layer_count},
    };
}

json cloud_client_info_to_json(const CloudClientInfo & info) {
    return {
        {"configured", info.configured},
        {"backend_id", info.backend_id},
        {"model", info.model},
        {"model_alias", info.model_alias},
        {"display_name", info.display_name},
        {"execution_mode", info.execution_mode},
        {"capabilities", info.capabilities},
        {"base_url", info.base_url},
        {"timeout_seconds", info.timeout_seconds},
        {"connect_timeout_seconds", info.connect_timeout_seconds},
        {"retry_attempts", info.retry_attempts},
        {"retry_backoff_ms", info.retry_backoff_ms},
        {"send_modal_session_id", info.send_modal_session_id},
        {"verify_tls", info.verify_tls},
    };
}

json health_to_json(const HealthSnapshot & health) {
    return {
        {"state", to_string(health.state)},
        {"local_ready", health.local_ready},
        {"cloud_configured", health.cloud_configured},
        {"active_model_path", health.active_model_path},
        {"last_error", health.last_error},
    };
}

json info_to_json(const InfoSnapshot & info) {
    json exposed_models = json::array();
    for (const auto & model : info.exposed_models) {
        exposed_models.push_back(model_catalog_entry_to_json(model));
    }

    return {
        {"state", to_string(info.state)},
        {"host", info.host},
        {"port", info.port},
        {"default_system_prompt", info.default_system_prompt},
        {"public_model_alias", info.public_model_alias},
        {"exposed_models", std::move(exposed_models)},
        {"router", router_config_to_json(info.router_config)},
        {"local_model", local_model_info_to_json(info.local_model)},
        {"cloud_client", cloud_client_info_to_json(info.cloud_client)},
        {"last_error", info.last_error},
    };
}

json timing_to_json(const TimingInfo & timing) {
    return {
        {"total_latency_ms", timing.total_latency_ms},
        {"has_ttft", timing.has_ttft},
        {"ttft_ms", timing.ttft_ms},
        {"prompt_tokens", timing.prompt_tokens},
        {"generated_tokens", timing.generated_tokens},
        {"total_tokens", total_token_count(timing)},
    };
}

json execution_target_to_json(const ExecutionTargetIdentity & identity) {
    return {
        {"target", to_string(identity.route_target)},
        {"engine_id", identity.engine_id},
        {"backend_id", identity.backend_id},
        {"model_id", identity.model_id},
        {"model_alias", identity.model_alias},
        {"display_name", identity.display_name},
        {"execution_mode", identity.execution_mode},
        {"capabilities", identity.capabilities},
    };
}

json tool_call_to_json(const ToolCall & tool_call) {
    return {
        {"id", tool_call.id},
        {"type", tool_call.type},
        {"function",
         {
             {"name", tool_call.function.name},
             {"arguments", tool_call.function.arguments_json},
         }},
    };
}

json assistant_message_to_json(const AssistantMessage & message) {
    json payload = {
        {"role", message.role.empty() ? "assistant" : message.role},
        {"content", message.content.empty() && !message.tool_calls.empty() ? json(nullptr)
                                                                            : json(message.content)},
    };

    if (!message.refusal.empty()) {
        payload["refusal"] = message.refusal;
    }

    if (!message.tool_calls.empty()) {
        payload["tool_calls"] = json::array();
        for (const auto & tool_call : message.tool_calls) {
            payload["tool_calls"].push_back(tool_call_to_json(tool_call));
        }
    }

    return payload;
}

TimingInfo usage_timing_for_response(const InferenceResponse & response) {
    if (response.final_target == RouteTarget::kLocal && response.local_timing.has_value()) {
        return *response.local_timing;
    }
    if (response.final_target == RouteTarget::kCloud && response.cloud_timing.has_value()) {
        return *response.cloud_timing;
    }
    if (response.local_timing.has_value()) {
        return *response.local_timing;
    }
    if (response.cloud_timing.has_value()) {
        return *response.cloud_timing;
    }
    return response.total_timing;
}

json usage_to_json(const TimingInfo & timing) {
    return {
        {"prompt_tokens", timing.prompt_tokens},
        {"completion_tokens", timing.generated_tokens},
        {"total_tokens", total_token_count(timing)},
    };
}

json response_to_json(const InferenceResponse & response) {
    json payload = {
        {"ok", response.ok},
        {"error_kind", to_string(response.error_kind)},
        {"error", response.error},
        {"text", response.text},
        {"message", assistant_message_to_json(response.message)},
        {"finish_reason", response.finish_reason},
        {"requested_target", to_string(response.requested_target)},
        {"initial_target", to_string(response.initial_target)},
        {"final_target", to_string(response.final_target)},
        {"fallback_used", response.fallback_used},
        {"fallback_reason", response.fallback_reason},
        {"route_reason", response.route_reason},
        {"matched_rules", response.matched_rules},
        {"active_model_path", response.active_model_path},
        {"initial_identity", execution_target_to_json(response.initial_identity)},
        {"final_identity", execution_target_to_json(response.final_identity)},
        {"timing", timing_to_json(response.total_timing)},
    };

    if (response.local_timing.has_value()) {
        payload["local_timing"] = timing_to_json(*response.local_timing);
    }
    if (response.cloud_timing.has_value()) {
        payload["cloud_timing"] = timing_to_json(*response.cloud_timing);
    }

    return payload;
}

json event_to_json(const InferenceEvent & event) {
    return {
        {"id", event.id},
        {"timestamp_utc", event.timestamp_utc},
        {"initial_target", to_string(event.initial_target)},
        {"final_target", to_string(event.final_target)},
        {"ok", event.ok},
        {"fallback_used", event.fallback_used},
        {"error_kind", to_string(event.error_kind)},
        {"route_reason", event.route_reason},
        {"fallback_reason", event.fallback_reason},
        {"engine_id", event.engine_id},
        {"backend_id", event.backend_id},
        {"model_id", event.model_id},
        {"model_alias", event.model_alias},
        {"total_latency_ms", event.total_latency_ms},
        {"local_latency_ms", event.local_latency_ms},
        {"cloud_latency_ms", event.cloud_latency_ms},
    };
}

bool read_int(const json & object, const char * key, int & out) {
    if (!object.contains(key)) {
        return true;
    }
    if (!object[key].is_number_integer()) {
        return false;
    }
    out = object[key].get<int>();
    return true;
}

bool read_uint(const json & object, const char * key, std::uint32_t & out) {
    if (!object.contains(key)) {
        return true;
    }
    if (!object[key].is_number_integer()) {
        return false;
    }
    out = object[key].get<std::uint32_t>();
    return true;
}

bool read_float(const json & object, const char * key, float & out) {
    if (!object.contains(key)) {
        return true;
    }
    if (!object[key].is_number()) {
        return false;
    }
    out = object[key].get<float>();
    return true;
}

std::vector<ModelCatalogEntry> configured_model_catalog(const DaemonConfig & config) {
    std::vector<ModelCatalogEntry> models = config.exposed_models;

    auto add_unique = [&](const std::string & id, const std::string & display_name) {
        if (id.empty()) {
            return;
        }

        for (const auto & model : models) {
            if (model.id == id) {
                return;
            }
        }

        models.push_back({id, display_name.empty() ? id : display_name, "seceda"});
    };

    add_unique(config.public_model_alias, "Seceda default route");
    if (!config.local.model_alias.empty()) {
        add_unique(config.local.model_alias, config.local.display_name);
    }
    if (!config.cloud.model_alias.empty()) {
        add_unique(config.cloud.model_alias, config.cloud.display_name);
    }

    return models;
}

bool is_known_model(const std::vector<ModelCatalogEntry> & models, const std::string & model_id) {
    for (const auto & model : models) {
        if (model.id == model_id) {
            return true;
        }
    }
    return false;
}

bool read_text_message_content(
    const json & content,
    std::string & out,
    std::string & error) {
    out.clear();
    if (content.is_null()) {
        return true;
    }
    if (content.is_string()) {
        out = content.get<std::string>();
        return true;
    }
    if (!content.is_array()) {
        error = "message.content must be a string, null, or an array of text parts";
        return false;
    }

    for (const auto & part : content) {
        if (!part.is_object()) {
            error = "message.content parts must be objects";
            return false;
        }
        if (!part.contains("type") || !part["type"].is_string()) {
            error = "message.content parts must contain a string type";
            return false;
        }

        const std::string type = part["type"].get<std::string>();
        if (type != "text" && type != "input_text") {
            error = "Only text content parts are supported in phase 1";
            return false;
        }
        if (!part.contains("text") || !part["text"].is_string()) {
            error = "Text content parts must contain a string text field";
            return false;
        }

        out += part["text"].get<std::string>();
    }

    return true;
}

bool parse_chat_message(
    const json & payload,
    ChatMessage & message,
    std::string & error) {
    if (!payload.is_object()) {
        error = "Each message must be a JSON object";
        return false;
    }
    if (!payload.contains("role") || !payload["role"].is_string()) {
        error = "Each message must contain a string role";
        return false;
    }

    message.role = payload["role"].get<std::string>();
    if (message.role == "function") {
        message.role = "tool";
    }

    if (message.role != "system" &&
        message.role != "developer" &&
        message.role != "user" &&
        message.role != "assistant" &&
        message.role != "tool") {
        error = "Unsupported message role: " + message.role;
        return false;
    }

    if (payload.contains("name")) {
        if (!payload["name"].is_string()) {
            error = "message.name must be a string when provided";
            return false;
        }
        message.name = payload["name"].get<std::string>();
    }

    if (payload.contains("tool_call_id")) {
        if (!payload["tool_call_id"].is_string()) {
            error = "message.tool_call_id must be a string when provided";
            return false;
        }
        message.tool_call_id = payload["tool_call_id"].get<std::string>();
    }

    if (payload.contains("tool_calls")) {
        if (!payload["tool_calls"].is_array()) {
            error = "message.tool_calls must be an array when provided";
            return false;
        }
        message.tool_calls_json = payload["tool_calls"].dump();
    }

    if (!payload.contains("content")) {
        error = "Each message must contain a content field";
        return false;
    }

    return read_text_message_content(payload["content"], message.content, error);
}

bool read_stop_sequences(
    const json & payload,
    std::vector<std::string> & out,
    std::string & error) {
    out.clear();
    if (payload.is_string()) {
        out.push_back(payload.get<std::string>());
        return true;
    }
    if (!payload.is_array()) {
        error = "stop must be a string or an array of strings";
        return false;
    }

    for (const auto & item : payload) {
        if (!item.is_string()) {
            error = "stop entries must be strings";
            return false;
        }
        out.push_back(item.get<std::string>());
    }

    return true;
}

bool read_completion_token_limit(
    const json & payload,
    int & out,
    std::string & error) {
    std::optional<int> max_tokens;
    std::optional<int> max_completion_tokens;

    if (payload.contains("max_tokens")) {
        if (!payload["max_tokens"].is_number_integer()) {
            error = "max_tokens must be an integer when provided";
            return false;
        }
        max_tokens = payload["max_tokens"].get<int>();
    }

    if (payload.contains("max_completion_tokens")) {
        if (!payload["max_completion_tokens"].is_number_integer()) {
            error = "max_completion_tokens must be an integer when provided";
            return false;
        }
        max_completion_tokens = payload["max_completion_tokens"].get<int>();
    }

    if (max_tokens.has_value() &&
        max_completion_tokens.has_value() &&
        *max_tokens != *max_completion_tokens) {
        error = "max_tokens and max_completion_tokens must match when both are provided";
        return false;
    }

    if (max_completion_tokens.has_value()) {
        out = *max_completion_tokens;
    } else if (max_tokens.has_value()) {
        out = *max_tokens;
    }

    return true;
}

bool read_stream_options(
    const json & payload,
    bool & include_usage,
    std::string & error) {
    if (!payload.is_object()) {
        error = "stream_options must be an object when provided";
        return false;
    }

    if (payload.contains("include_usage")) {
        if (!payload["include_usage"].is_boolean()) {
            error = "stream_options.include_usage must be a boolean";
            return false;
        }
        include_usage = payload["include_usage"].get<bool>();
    }

    return true;
}

bool parse_openai_request_features(
    const json & payload,
    InferenceRequest & request,
    std::string & error) {
    if (payload.contains("user")) {
        if (!payload["user"].is_string()) {
            error = "user must be a string when provided";
            return false;
        }
        request.advanced.user = payload["user"].get<std::string>();
    }

    if (payload.contains("stop")) {
        if (!read_stop_sequences(payload["stop"], request.advanced.stop_sequences, error)) {
            return false;
        }
    }

    if (payload.contains("tools")) {
        if (!payload["tools"].is_array()) {
            error = "tools must be an array when provided";
            return false;
        }
        request.advanced.tools_json = payload["tools"].dump();
        request.capabilities.has_tools = !payload["tools"].empty();
    }

    if (payload.contains("tool_choice") && !payload["tool_choice"].is_null()) {
        if (!payload["tool_choice"].is_string() && !payload["tool_choice"].is_object()) {
            error = "tool_choice must be a string or object when provided";
            return false;
        }
        request.advanced.tool_choice_json = payload["tool_choice"].dump();
        request.capabilities.requests_tool_choice =
            !(payload["tool_choice"].is_string() &&
              payload["tool_choice"].get<std::string>() == "none");
    }

    if (payload.contains("response_format") && !payload["response_format"].is_null()) {
        if (!payload["response_format"].is_string() && !payload["response_format"].is_object()) {
            error = "response_format must be a string or object when provided";
            return false;
        }
        request.advanced.response_format_json = payload["response_format"].dump();
        if (payload["response_format"].is_string()) {
            request.capabilities.requests_structured_output =
                payload["response_format"].get<std::string>() != "text";
        } else {
            const auto & response_format = payload["response_format"];
            request.capabilities.requests_structured_output =
                !response_format.contains("type") ||
                !response_format["type"].is_string() ||
                response_format["type"].get<std::string>() != "text";
        }
    }

    return true;
}

bool parse_chat_completion_request(
    const std::string & body,
    const DaemonConfig & config,
    InferenceRequest & request,
    std::string & error) {
    json parsed;
    try {
        parsed = json::parse(body);
    } catch (const std::exception & exception) {
        error = std::string("Invalid JSON: ") + exception.what();
        return false;
    }

    if (!parsed.is_object()) {
        error = "Request body must be a JSON object";
        return false;
    }

    request = {};
    request.options = config.default_generation;

    const auto model_catalog = configured_model_catalog(config);
    if (parsed.contains("model") && !parsed["model"].is_null()) {
        if (!parsed["model"].is_string()) {
            error = "model must be a string when provided";
            return false;
        }
        request.model = parsed["model"].get<std::string>();
    } else if (!model_catalog.empty()) {
        request.model = model_catalog.front().id;
    } else {
        request.model = config.public_model_alias;
    }

    if (!is_known_model(model_catalog, request.model)) {
        error = "Unknown model '" + request.model + "'";
        return false;
    }

    if (!parsed.contains("messages") || !parsed["messages"].is_array()) {
        error = "messages must be a JSON array";
        return false;
    }

    for (const auto & message_payload : parsed["messages"]) {
        ChatMessage message;
        if (!parse_chat_message(message_payload, message, error)) {
            return false;
        }
        request.messages.push_back(std::move(message));
    }

    if (parsed.contains("stream")) {
        if (!parsed["stream"].is_boolean()) {
            error = "stream must be a boolean when provided";
            return false;
        }
        request.options.stream = parsed["stream"].get<bool>();
    }

    if (parsed.contains("stream_options")) {
        if (!read_stream_options(
                parsed["stream_options"],
                request.options.include_usage_in_stream,
                error)) {
            return false;
        }
    }

    if (!read_completion_token_limit(parsed, request.options.max_completion_tokens, error) ||
        !read_float(parsed, "temperature", request.options.temperature) ||
        !read_float(parsed, "top_p", request.options.top_p) ||
        !read_int(parsed, "top_k", request.options.top_k) ||
        !read_float(parsed, "min_p", request.options.min_p) ||
        !read_uint(parsed, "seed", request.options.seed)) {
        if (error.empty()) {
            error = "One or more generation controls had an invalid type";
        }
        return false;
    }

    if (!parse_openai_request_features(parsed, request, error)) {
        return false;
    }

    request.seceda.preferred_model_alias = request.model;
    refresh_request_views(request);
    return true;
}

bool parse_inference_request(
    const std::string & body,
    const DaemonConfig & config,
    InferenceRequest & request,
    std::string & error) {
    json parsed;
    try {
        parsed = json::parse(body);
    } catch (const std::exception & exception) {
        error = std::string("Invalid JSON: ") + exception.what();
        return false;
    }

    if (!parsed.contains("text") || !parsed["text"].is_string()) {
        error = "Request body must contain a string field named 'text'";
        return false;
    }

    request = {};
    request.model = config.public_model_alias;
    request.seceda.preferred_model_alias = request.model;
    request.options = config.default_generation;

    if (parsed.contains("system_prompt")) {
        if (!parsed["system_prompt"].is_string()) {
            error = "system_prompt must be a string when provided";
            return false;
        }
        request.messages.push_back({"system", parsed["system_prompt"].get<std::string>(), {}, {}, {}});
    }

    request.messages.push_back({"user", parsed["text"].get<std::string>(), {}, {}, {}});

    if (parsed.contains("route_override")) {
        if (!parsed["route_override"].is_string() ||
            !parse_route_target(
                parsed["route_override"].get<std::string>(),
                request.seceda.route_override)) {
            error = "route_override must be one of: auto, local, cloud";
            return false;
        }
    }

    if (parsed.contains("options")) {
        if (!parsed["options"].is_object()) {
            error = "options must be a JSON object";
            return false;
        }

        const auto & options = parsed["options"];
        if (!read_completion_token_limit(options, request.options.max_completion_tokens, error) ||
            !read_float(options, "temperature", request.options.temperature) ||
            !read_float(options, "top_p", request.options.top_p) ||
            !read_int(options, "top_k", request.options.top_k) ||
            !read_float(options, "min_p", request.options.min_p) ||
            !read_uint(options, "seed", request.options.seed)) {
            if (error.empty()) {
                error = "One or more generation options had an invalid type";
            }
            return false;
        }

        if (options.contains("use_chat_template")) {
            if (!options["use_chat_template"].is_boolean()) {
                error = "options.use_chat_template must be a boolean";
                return false;
            }
            request.options.use_chat_template = options["use_chat_template"].get<bool>();
        }
    }

    refresh_request_views(request);
    return true;
}

bool parse_query_uint64(
    const httplib::Request & request,
    const char * key,
    std::uint64_t default_value,
    std::uint64_t & out,
    std::string & error) {
    out = default_value;
    if (!request.has_param(key)) {
        return true;
    }

    try {
        out = static_cast<std::uint64_t>(std::stoull(request.get_param_value(key)));
        return true;
    } catch (const std::exception &) {
        error = std::string("Query parameter '") + key + "' must be an unsigned integer";
        return false;
    }
}

int http_status_for_state(RuntimeState state) {
    return state == RuntimeState::kReady || state == RuntimeState::kDegraded ? 200 : 503;
}

int http_status_for_response(const InferenceResponse & response) {
    if (response.ok) {
        return 200;
    }

    switch (response.error_kind) {
        case InferenceErrorKind::kInvalidRequest:
        case InferenceErrorKind::kUnsupportedFeature:
            return 400;
        case InferenceErrorKind::kLocalUnavailable:
        case InferenceErrorKind::kCloudUnavailable:
            return 503;
        case InferenceErrorKind::kLocalFailure:
        case InferenceErrorKind::kCloudFailure:
            return 502;
        case InferenceErrorKind::kNone:
            return 500;
    }

    return 500;
}

std::string openai_error_type(const InferenceResponse & response) {
    switch (response.error_kind) {
        case InferenceErrorKind::kInvalidRequest:
        case InferenceErrorKind::kUnsupportedFeature:
            return "invalid_request_error";
        case InferenceErrorKind::kLocalUnavailable:
        case InferenceErrorKind::kCloudUnavailable:
        case InferenceErrorKind::kLocalFailure:
        case InferenceErrorKind::kCloudFailure:
        case InferenceErrorKind::kNone:
            return "server_error";
    }

    return "server_error";
}

json openai_error_payload(
    const std::string & message,
    const std::string & type,
    const std::string & param = {},
    const std::string & code = {}) {
    json error = {
        {"message", message},
        {"type", type},
        {"param", param.empty() ? json(nullptr) : json(param)},
        {"code", code.empty() ? json(nullptr) : json(code)},
    };
    return {{"error", std::move(error)}};
}

void set_openai_error(
    httplib::Response & response,
    int status,
    const std::string & message,
    const std::string & type,
    const std::string & param = {},
    const std::string & code = {}) {
    response.status = status;
    response.set_content(
        openai_error_payload(message, type, param, code).dump(),
        "application/json");
}

std::int64_t unix_timestamp_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string make_chat_completion_id() {
    static std::atomic<std::uint64_t> next_id{1};
    return "chatcmpl-seceda-" + std::to_string(unix_timestamp_now()) + "-" +
        std::to_string(next_id.fetch_add(1));
}

json openai_chat_completion_response(
    const InferenceRequest & request,
    const InferenceResponse & response,
    const std::string & completion_id,
    std::int64_t created_at) {
    json choices = json::array();
    choices.push_back(
        {
            {"index", 0},
            {"message", assistant_message_to_json(response.message)},
            {"finish_reason", response.finish_reason.empty() ? "stop" : response.finish_reason},
        });

    return {
        {"id", completion_id},
        {"object", "chat.completion"},
        {"created", created_at},
        {"model", request.model},
        {"choices", std::move(choices)},
        {"usage", usage_to_json(usage_timing_for_response(response))},
    };
}

std::string sse_event(const json & payload) {
    return "data: " + payload.dump() + "\n\n";
}

std::string openai_chat_completion_sse(
    const InferenceRequest & request,
    const InferenceResponse & response,
    const std::string & completion_id,
    std::int64_t created_at) {
    const auto chunk_base = [&](const json & delta, const json & finish_reason) {
        json choices = json::array();
        choices.push_back(
            {
                {"index", 0},
                {"delta", delta},
                {"finish_reason", finish_reason},
            });

        return json{
            {"id", completion_id},
            {"object", "chat.completion.chunk"},
            {"created", created_at},
            {"model", request.model},
            {"choices", std::move(choices)},
        };
    };

    std::string stream;
    stream += sse_event(chunk_base(json{{"role", "assistant"}}, nullptr));

    if (!response.message.content.empty()) {
        stream += sse_event(chunk_base(json{{"content", response.message.content}}, nullptr));
    }

    if (!response.message.tool_calls.empty()) {
        json tool_calls = json::array();
        for (const auto & tool_call : response.message.tool_calls) {
            tool_calls.push_back(tool_call_to_json(tool_call));
        }
        stream += sse_event(chunk_base(json{{"tool_calls", std::move(tool_calls)}}, nullptr));
    }

    stream += sse_event(
        chunk_base(
            json::object(),
            response.finish_reason.empty() ? json("stop") : json(response.finish_reason)));

    if (request.options.include_usage_in_stream) {
        stream += sse_event(
            json{
                {"id", completion_id},
                {"object", "chat.completion.chunk"},
                {"created", created_at},
                {"model", request.model},
                {"choices", json::array()},
                {"usage", usage_to_json(usage_timing_for_response(response))},
            });
    }

    stream += "data: [DONE]\n\n";
    return stream;
}

}  // namespace

int main(int argc, char ** argv) {
    DaemonConfig config;
    std::string parse_error;
    bool show_help = false;
    if (!RuntimeConfigParser::parse(argc, argv, config, parse_error, show_help)) {
        std::cerr << parse_error << "\n\n" << RuntimeConfigParser::help_text(argv[0]);
        return 1;
    }
    if (show_help) {
        std::cout << RuntimeConfigParser::help_text(argv[0]);
        return 0;
    }

    auto local_runtime = std::make_shared<LlamaRuntime>();
    auto cloud_client = std::make_shared<CloudClient>(config.cloud);
    auto router = std::make_shared<HeuristicRouter>(config.router);

    EdgeDaemon daemon(config, local_runtime, cloud_client, router);
    daemon.initialize();

    httplib::Server server;
    server.set_payload_max_length(1024 * 1024);
    server.set_read_timeout(120);
    server.set_write_timeout(120);

    server.Get("/health", [&](const httplib::Request &, httplib::Response & response) {
        const auto health = daemon.health();
        response.status = http_status_for_state(health.state);
        response.set_content(health_to_json(health).dump(2), "application/json");
    });

    server.Get("/info", [&](const httplib::Request &, httplib::Response & response) {
        response.set_content(info_to_json(daemon.info()).dump(2), "application/json");
    });

    server.Get("/metrics", [&](const httplib::Request &, httplib::Response & response) {
        response.set_content(
            daemon.metrics_text(),
            "text/plain; version=0.0.4; charset=utf-8");
    });

    server.Get("/metrics/events", [&](const httplib::Request & request, httplib::Response & response) {
        std::string error;
        std::uint64_t since_id = 0;
        std::uint64_t limit = 1000;
        if (!parse_query_uint64(request, "since_id", 0, since_id, error) ||
            !parse_query_uint64(request, "limit", 1000, limit, error)) {
            response.status = 400;
            response.set_content(json{{"ok", false}, {"error", error}}.dump(2), "application/json");
            return;
        }

        const auto batch = daemon.events(since_id, static_cast<std::size_t>(limit));
        json payload = {
            {"since_id", batch.since_id},
            {"latest_id", batch.latest_id},
            {"returned", batch.events.size()},
            {"events", json::array()},
        };
        for (const auto & event : batch.events) {
            payload["events"].push_back(event_to_json(event));
        }

        response.set_content(payload.dump(2), "application/json");
    });

    server.Get("/v1/models", [&](const httplib::Request &, httplib::Response & response) {
        json payload = {
            {"object", "list"},
            {"data", json::array()},
        };
        for (const auto & model : configured_model_catalog(config)) {
            payload["data"].push_back(model_catalog_entry_to_json(model));
        }

        response.set_content(payload.dump(), "application/json");
    });

    server.Post("/v1/chat/completions", [&](const httplib::Request & request, httplib::Response & response) {
        InferenceRequest inference_request;
        std::string error;
        if (!parse_chat_completion_request(request.body, config, inference_request, error)) {
            set_openai_error(response, 400, error, "invalid_request_error");
            return;
        }

        const auto inference_response = daemon.handle_inference(inference_request);
        if (!inference_response.ok) {
            set_openai_error(
                response,
                http_status_for_response(inference_response),
                inference_response.error.empty() ? "Inference request failed" : inference_response.error,
                openai_error_type(inference_response));
            return;
        }

        const std::string completion_id = make_chat_completion_id();
        const std::int64_t created_at = unix_timestamp_now();
        if (inference_request.options.stream) {
            response.status = 200;
            response.set_header("Cache-Control", "no-cache");
            response.set_content(
                openai_chat_completion_sse(
                    inference_request,
                    inference_response,
                    completion_id,
                    created_at),
                "text/event-stream");
            return;
        }

        response.set_content(
            openai_chat_completion_response(
                inference_request,
                inference_response,
                completion_id,
                created_at)
                .dump(),
            "application/json");
    });

    server.Post("/inference", [&](const httplib::Request & request, httplib::Response & response) {
        InferenceRequest inference_request;
        std::string error;
        if (!parse_inference_request(request.body, config, inference_request, error)) {
            response.status = 400;
            response.set_content(json{{"ok", false}, {"error", error}}.dump(2), "application/json");
            return;
        }

        const auto inference_response = daemon.handle_inference(std::move(inference_request));
        response.status = http_status_for_response(inference_response);
        response.set_content(response_to_json(inference_response).dump(2), "application/json");
    });

    server.Post("/admin/model", [&](const httplib::Request & request, httplib::Response & response) {
        json payload;
        try {
            payload = json::parse(request.body);
        } catch (const std::exception & exception) {
            response.status = 400;
            response.set_content(
                json{{"ok", false}, {"error", std::string("Invalid JSON: ") + exception.what()}}.dump(2),
                "application/json");
            return;
        }

        if (!payload.contains("model_path") || !payload["model_path"].is_string()) {
            response.status = 400;
            response.set_content(
                json{{"ok", false}, {"error", "model_path must be a string"}}.dump(2),
                "application/json");
            return;
        }

        const std::string warmup_prompt =
            payload.contains("warmup_prompt") && payload["warmup_prompt"].is_string()
            ? payload["warmup_prompt"].get<std::string>()
            : "";
        const auto reload =
            daemon.reload_model(payload["model_path"].get<std::string>(), warmup_prompt);

        response.status = reload.ok ? 200 : 500;
        response.set_content(
            json{
                {"ok", reload.ok},
                {"active_model_path", reload.active_model_path},
                {"error", reload.error},
            }
                .dump(2),
            "application/json");
    });

    server.set_error_handler([](const httplib::Request &, httplib::Response & response) {
        if (response.status == 404) {
            response.set_content(
                json{{"ok", false}, {"error", "Not found"}}.dump(2),
                "application/json");
        }
    });

    std::cout << "Seceda edge daemon listening on " << config.host << ":" << config.port << std::endl;
    if (!server.listen(config.host, config.port)) {
        std::cerr << "Failed to start HTTP server on " << config.host << ":" << config.port
                  << std::endl;
        return 1;
    }

    return 0;
}
