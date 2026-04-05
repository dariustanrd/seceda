#include "openai_compat/openai_compat.hpp"
#include "json_utils/read_optional.hpp"

#include <nlohmann/json.hpp>

#include <optional>

namespace seceda::edge::openai_compat {

namespace ju = seceda::edge::json_utils;

namespace {

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

bool read_completion_token_limit_impl(
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

const ModelCatalogEntry * find_model_catalog_entry(
    const std::vector<ModelCatalogEntry> & models,
    const std::string & model_id) {
    for (const auto & model : models) {
        if (model.id == model_id) {
            return &model;
        }
    }
    return nullptr;
}

void apply_model_selection_hints(
    const DaemonConfig & config,
    InferenceRequest & request) {
    request.seceda.preferred_model_alias = request.model;

    const auto catalog = configured_model_catalog(config);
    const ModelCatalogEntry * const selected = find_model_catalog_entry(catalog, request.model);
    if (selected == nullptr) {
        return;
    }

    if (selected->route_target != RouteTarget::kAuto) {
        request.seceda.route_override = selected->route_target;
    }
    if (!selected->engine_id.empty()) {
        request.seceda.preferred_engine_id = selected->engine_id;
    }
    if (!selected->backend_id.empty()) {
        request.seceda.preferred_backend_id = selected->backend_id;
    }
    if (!selected->model_alias.empty()) {
        request.seceda.preferred_model_alias = selected->model_alias;
    }
}

bool parse_chat_completion_request_impl(
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

    if (!read_completion_token_limit_impl(parsed, request.options.max_completion_tokens, error) ||
        !ju::read_optional_float(parsed, "temperature", request.options.temperature) ||
        !ju::read_optional_float(parsed, "top_p", request.options.top_p) ||
        !ju::read_optional_integer(parsed, "top_k", request.options.top_k) ||
        !ju::read_optional_float(parsed, "min_p", request.options.min_p) ||
        !ju::read_optional_integer(parsed, "seed", request.options.seed)) {
        if (error.empty()) {
            error = "One or more generation controls had an invalid type";
        }
        return false;
    }

    if (!parse_openai_request_features(parsed, request, error)) {
        return false;
    }

    apply_model_selection_hints(config, request);
    refresh_request_views(request);
    return true;
}

}  // namespace

bool read_completion_token_limit(
    const json & payload,
    int & out,
    std::string & error) {
    return read_completion_token_limit_impl(payload, out, error);
}

bool parse_chat_completion_request(
    const std::string & body,
    const DaemonConfig & config,
    InferenceRequest & request,
    std::string & error) {
    return parse_chat_completion_request_impl(body, config, request, error);
}

}  // namespace seceda::edge::openai_compat
