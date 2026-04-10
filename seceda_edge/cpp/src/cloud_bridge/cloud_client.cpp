#include "cloud_bridge/cloud_client.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace seceda::edge {
namespace {

using json = nlohmann::json;

struct CurlStreamState {
    CloudCompletionResult * result = nullptr;
    const StreamDeltaCallback * on_delta = nullptr;
    std::string raw_body;
    std::string event_buffer;
    std::string stream_error;
    std::string parser_error;
    bool saw_stream_event = false;
    bool saw_done = false;
    bool stream_cancelled = false;
    std::chrono::steady_clock::time_point request_start;
};

bool curl_ready() {
    static std::once_flag init_once;
    static bool initialized = false;

    std::call_once(init_once, []() {
        initialized = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    });

    return initialized;
}

std::string completion_url(std::string base_url) {
    while (!base_url.empty() && base_url.back() == '/') {
        base_url.pop_back();
    }

    constexpr const char * kCompletionSuffix = "/chat/completions";
    constexpr const char * kV1Suffix = "/v1";

    if (base_url.size() >= std::char_traits<char>::length(kCompletionSuffix) &&
        base_url.compare(
            base_url.size() - std::char_traits<char>::length(kCompletionSuffix),
            std::char_traits<char>::length(kCompletionSuffix),
            kCompletionSuffix) == 0) {
        return base_url;
    }

    if (base_url.size() >= std::char_traits<char>::length(kV1Suffix) &&
        base_url.compare(
            base_url.size() - std::char_traits<char>::length(kV1Suffix),
            std::char_traits<char>::length(kV1Suffix),
            kV1Suffix) == 0) {
        return base_url + kCompletionSuffix;
    }

    return base_url + "/v1/chat/completions";
}

bool take_next_sse_event(std::string & buffer, std::string & raw_event) {
    const std::size_t lf_pos = buffer.find("\n\n");
    const std::size_t crlf_pos = buffer.find("\r\n\r\n");

    std::size_t event_end = std::string::npos;
    std::size_t delimiter_length = 0;

    if (lf_pos != std::string::npos && crlf_pos != std::string::npos) {
        if (lf_pos <= crlf_pos) {
            event_end = lf_pos;
            delimiter_length = 2;
        } else {
            event_end = crlf_pos;
            delimiter_length = 4;
        }
    } else if (lf_pos != std::string::npos) {
        event_end = lf_pos;
        delimiter_length = 2;
    } else if (crlf_pos != std::string::npos) {
        event_end = crlf_pos;
        delimiter_length = 4;
    } else {
        return false;
    }

    raw_event = buffer.substr(0, event_end);
    buffer.erase(0, event_end + delimiter_length);
    return true;
}

bool parse_sse_event(
    const std::string & raw_event,
    std::string & field_name,
    std::string & payload) {
    field_name.clear();
    payload.clear();

    std::istringstream stream(raw_event);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        const auto colon_pos = line.find(':');
        if (colon_pos == std::string::npos) {
            continue;
        }

        std::string key = line.substr(0, colon_pos);
        std::string value = line.substr(colon_pos + 1);
        if (!value.empty() && value.front() == ' ') {
            value.erase(0, 1);
        }

        if (key != "data" && key != "error") {
            continue;
        }

        if (field_name.empty()) {
            field_name = std::move(key);
        }
        if (!payload.empty()) {
            payload.push_back('\n');
        }
        payload += value;
    }

    return !field_name.empty() && !payload.empty();
}

int extract_completion_tokens(const json & payload) {
    if (!payload.contains("usage") || !payload["usage"].is_object() ||
        !payload["usage"].contains("completion_tokens") ||
        !payload["usage"]["completion_tokens"].is_number_integer()) {
        return 0;
    }

    return payload["usage"]["completion_tokens"].get<int>();
}

int extract_prompt_tokens(const json & payload) {
    if (!payload.contains("usage") || !payload["usage"].is_object() ||
        !payload["usage"].contains("prompt_tokens") ||
        !payload["usage"]["prompt_tokens"].is_number_integer()) {
        return 0;
    }

    return payload["usage"]["prompt_tokens"].get<int>();
}

void apply_usage_tokens(const json & payload, TimingInfo & timing) {
    if (!payload.contains("usage") || !payload["usage"].is_object()) {
        return;
    }

    if (payload["usage"].contains("prompt_tokens") &&
        payload["usage"]["prompt_tokens"].is_number_integer()) {
        timing.prompt_tokens = extract_prompt_tokens(payload);
    }
    if (payload["usage"].contains("completion_tokens") &&
        payload["usage"]["completion_tokens"].is_number_integer()) {
        timing.generated_tokens = extract_completion_tokens(payload);
    }
}

bool parse_tool_call(const json & payload, ToolCall & tool_call, std::string & error) {
    if (!payload.is_object()) {
        error = "Tool call payload must be an object";
        return false;
    }

    if (payload.contains("id") && payload["id"].is_string()) {
        tool_call.id = payload["id"].get<std::string>();
    }
    if (payload.contains("type") && payload["type"].is_string()) {
        tool_call.type = payload["type"].get<std::string>();
    }

    if (payload.contains("function")) {
        if (!payload["function"].is_object()) {
            error = "Tool call function payload must be an object";
            return false;
        }

        const auto & function = payload["function"];
        if (function.contains("name")) {
            if (!function["name"].is_string()) {
                error = "Tool call function.name must be a string";
                return false;
            }
            tool_call.function.name = function["name"].get<std::string>();
        }
        if (function.contains("arguments")) {
            if (function["arguments"].is_string()) {
                tool_call.function.arguments_json = function["arguments"].get<std::string>();
            } else {
                tool_call.function.arguments_json = function["arguments"].dump();
            }
        }
    }

    return true;
}

bool apply_stream_delta(
    const json & delta,
    CloudCompletionResult & result,
    std::string & error) {
    if (!delta.is_object()) {
        error = "Cloud delta payload must be an object";
        return false;
    }

    if (delta.contains("role") && delta["role"].is_string()) {
        result.message.role = delta["role"].get<std::string>();
    }

    if (delta.contains("content") && delta["content"].is_string()) {
        const std::string content = delta["content"].get<std::string>();
        result.message.content += content;
        result.text += content;
    }

    if (delta.contains("tool_calls")) {
        if (delta["tool_calls"].is_null()) {
            return true;
        }

        json tool_calls = delta["tool_calls"];
        if (tool_calls.is_object()) {
            tool_calls = json::array({tool_calls});
        }
        if (!tool_calls.is_array()) {
            error = "delta.tool_calls must be an array";
            return false;
        }

        for (const auto & item : tool_calls) {
            if (!item.is_object()) {
                error = "delta.tool_calls entries must be objects";
                return false;
            }

            std::size_t index = result.message.tool_calls.size();
            if (item.contains("index")) {
                if (!item["index"].is_number_unsigned()) {
                    error = "delta.tool_calls.index must be an unsigned integer";
                    return false;
                }
                index = item["index"].get<std::size_t>();
            }

            while (result.message.tool_calls.size() <= index) {
                result.message.tool_calls.push_back(ToolCall{});
            }

            ToolCall update = result.message.tool_calls[index];
            if (!parse_tool_call(item, update, error)) {
                return false;
            }

            if (item.contains("function") &&
                item["function"].is_object() &&
                item["function"].contains("arguments") &&
                item["function"]["arguments"].is_string()) {
                const std::string delta_arguments =
                    item["function"]["arguments"].get<std::string>();
                if (!result.message.tool_calls[index].function.arguments_json.empty() &&
                    result.message.tool_calls[index].function.arguments_json != delta_arguments) {
                    update.function.arguments_json =
                        result.message.tool_calls[index].function.arguments_json + delta_arguments;
                }
            }

            result.message.tool_calls[index] = std::move(update);
        }
    }

    return true;
}

bool emit_stream_delta_payload(
    const json & delta,
    const StreamDeltaCallback & on_delta,
    std::string & error) {
    if (!delta.is_object()) {
        error = "Cloud delta payload must be an object";
        return false;
    }

    StreamedChatDelta emitted_delta;
    if (delta.contains("content")) {
        if (delta["content"].is_string()) {
            emitted_delta.content = delta["content"].get<std::string>();
        } else if (!delta["content"].is_null()) {
            error = "Cloud delta content must be a string or null";
            return false;
        }
    }

    if (delta.contains("tool_calls")) {
        if (delta["tool_calls"].is_null()) {
            if (emitted_delta.content.empty()) {
                return true;
            }
            if (!on_delta(emitted_delta)) {
                error = "Streaming cancelled by client";
                return false;
            }
            return true;
        }
        if (!delta["tool_calls"].is_array() && !delta["tool_calls"].is_object()) {
            error = "Cloud delta tool_calls must be an array or object";
            return false;
        }
        emitted_delta.tool_calls_json = delta["tool_calls"].dump();
    }

    if (emitted_delta.content.empty() && emitted_delta.tool_calls_json.empty()) {
        return true;
    }

    if (!on_delta(emitted_delta)) {
        error = "Streaming cancelled by client";
        return false;
    }

    return true;
}

bool apply_full_message(
    const json & message,
    CloudCompletionResult & result,
    std::string & error) {
    if (!message.is_object()) {
        error = "Cloud message payload must be an object";
        return false;
    }

    if (message.contains("role") && message["role"].is_string()) {
        result.message.role = message["role"].get<std::string>();
    }

    if (message.contains("content")) {
        if (message["content"].is_string()) {
            result.message.content = message["content"].get<std::string>();
            result.text = result.message.content;
        } else if (!message["content"].is_null()) {
            error = "Cloud message content must be a string or null";
            return false;
        }
    }

    if (message.contains("refusal") && message["refusal"].is_string()) {
        result.message.refusal = message["refusal"].get<std::string>();
    }

    if (message.contains("tool_calls")) {
        if (message["tool_calls"].is_null()) {
            return true;
        }

        json tool_calls = message["tool_calls"];
        if (tool_calls.is_object()) {
            tool_calls = json::array({tool_calls});
        }
        if (!tool_calls.is_array()) {
            error = "Cloud message tool_calls must be an array";
            return false;
        }

        result.message.tool_calls.clear();
        for (const auto & item : tool_calls) {
            ToolCall tool_call;
            if (!parse_tool_call(item, tool_call, error)) {
                return false;
            }
            result.message.tool_calls.push_back(std::move(tool_call));
        }
    }

    return true;
}

bool apply_completion_payload(
    const json & payload,
    CloudCompletionResult & result,
    bool streaming_payload,
    bool & finished,
    std::string & error) {
    finished = false;
    if (streaming_payload &&
        payload.contains("choices") &&
        payload["choices"].is_array() &&
        payload["choices"].empty()) {
        return true;
    }

    if (!payload.contains("choices") || !payload["choices"].is_array() ||
        payload["choices"].empty()) {
        error = "Cloud response must contain a non-empty choices array";
        return false;
    }

    const auto & choice = payload["choices"][0];
    if (!choice.is_object()) {
        error = "Cloud choice payload must be an object";
        return false;
    }

    if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
        if (!choice["finish_reason"].is_string()) {
            error = "Cloud finish_reason must be a string";
            return false;
        }
        result.finish_reason = choice["finish_reason"].get<std::string>();
        finished = true;
    }

    if (streaming_payload) {
        if (choice.contains("delta")) {
            return apply_stream_delta(choice["delta"], result, error);
        }
        return true;
    }

    if (choice.contains("message")) {
        return apply_full_message(choice["message"], result, error);
    }

    error = "Cloud response did not include a message payload";
    return false;
}

json build_message_payload(const ChatMessage & message) {
    json payload = {
        {"role", message.role},
        {"content", message.content},
    };
    if (!message.name.empty()) {
        payload["name"] = message.name;
    }
    if (!message.tool_call_id.empty()) {
        payload["tool_call_id"] = message.tool_call_id;
    }
    if (!message.tool_calls_json.empty()) {
        payload["tool_calls"] = json::parse(message.tool_calls_json);
    }
    return payload;
}

bool wants_rich_openai_response(const InferenceRequest & request) {
    return request.capabilities.has_tools || request.capabilities.requests_tool_choice;
}

std::string generate_modal_session_id() {
    static constexpr char kHex[] = "0123456789abcdef";

    std::array<unsigned char, 16> bytes{};
    thread_local std::mt19937_64 generator(std::random_device{}());
    std::uniform_int_distribution<int> distribution(0, 255);
    for (auto & byte : bytes) {
        byte = static_cast<unsigned char>(distribution(generator));
    }

    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0Fu) | 0x40u);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3Fu) | 0x80u);

    std::string session_id;
    session_id.reserve(36);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            session_id.push_back('-');
        }

        session_id.push_back(kHex[(bytes[i] >> 4u) & 0x0Fu]);
        session_id.push_back(kHex[bytes[i] & 0x0Fu]);
    }

    return session_id;
}

bool should_forward_seed(const InferenceRequest & request) {
    return request.options.seed != 0xFFFFFFFFu;
}

bool is_retryable_http_status(long status_code) {
    return status_code == 502 || status_code == 503 || status_code == 504;
}

bool is_retryable_curl_code(CURLcode curl_code) {
    switch (curl_code) {
        case CURLE_COULDNT_CONNECT:
        case CURLE_OPERATION_TIMEDOUT:
        case CURLE_RECV_ERROR:
        case CURLE_SEND_ERROR:
        case CURLE_GOT_NOTHING:
            return true;
        default:
            return false;
    }
}

size_t curl_write_callback(char * data, size_t size, size_t count, void * user_data) {
    const std::size_t total = size * count;
    auto & state = *static_cast<CurlStreamState *>(user_data);
    state.raw_body.append(data, total);
    state.event_buffer.append(data, total);

    std::string raw_event;
    while (take_next_sse_event(state.event_buffer, raw_event)) {
        std::string field;
        std::string payload;
        if (!parse_sse_event(raw_event, field, payload)) {
            continue;
        }
        if (payload == "[DONE]") {
            state.saw_done = true;
            continue;
        }
        if (field == "error") {
            state.stream_error = payload;
            continue;
        }

        state.saw_stream_event = true;
        try {
            const json parsed_payload = json::parse(payload);
            bool finished = false;
            if (state.on_delta != nullptr &&
                parsed_payload.contains("choices") &&
                parsed_payload["choices"].is_array() &&
                !parsed_payload["choices"].empty() &&
                parsed_payload["choices"][0].is_object() &&
                parsed_payload["choices"][0].contains("delta")) {
                if (!emit_stream_delta_payload(
                        parsed_payload["choices"][0]["delta"],
                        *state.on_delta,
                        state.parser_error)) {
                    state.stream_cancelled = state.parser_error == "Streaming cancelled by client";
                    return 0;
                }
            }
            if (!apply_completion_payload(
                    parsed_payload,
                    *state.result,
                    true,
                    finished,
                    state.parser_error)) {
                return 0;
            }

            if (!state.result->timing.has_ttft &&
                (!state.result->text.empty() || !state.result->message.tool_calls.empty())) {
                if (!state.result->timing.has_ttft) {
                    state.result->timing.has_ttft = true;
                    state.result->timing.ttft_ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - state.request_start)
                                                        .count();
                }
            }
            apply_usage_tokens(parsed_payload, state.result->timing);
            if (finished) {
                state.saw_done = true;
            }
        } catch (const std::exception & exception) {
            state.parser_error =
                std::string("Failed to parse cloud SSE payload: ") + exception.what();
            return 0;
        }
    }

    return total;
}

}  // namespace

CloudClient::CloudClient(CloudConfig config, std::vector<CloudConfig> named_backends)
    : config_(std::move(config)),
      named_backends_(std::move(named_backends)) {}

bool CloudClient::is_configured() const {
    return default_backend() != nullptr && !default_backend()->base_url.empty();
}

CloudClientInfo CloudClient::info() const {
    const CloudConfig * const backend = default_backend();
    CloudClientInfo info;
    info.configured = backend != nullptr && !backend->base_url.empty();
    if (backend == nullptr) {
        return info;
    }

    info.backend_id = backend->backend_id;
    info.base_url = backend->base_url;
    info.model = backend->model;
    info.model_alias = backend->model_alias;
    info.display_name = backend->display_name;
    info.execution_mode = backend->execution_mode;
    info.capabilities = backend->capabilities;
    info.timeout_seconds = backend->timeout_seconds;
    info.connect_timeout_seconds = backend->connect_timeout_seconds;
    info.retry_attempts = backend->retry_attempts;
    info.retry_backoff_ms = backend->retry_backoff_ms;
    info.send_modal_session_id = backend->send_modal_session_id;
    info.verify_tls = backend->verify_tls;
    return info;
}

const CloudConfig * CloudClient::default_backend() const {
    if (!config_.base_url.empty()) {
        return &config_;
    }
    for (const auto & backend : named_backends_) {
        if (!backend.base_url.empty()) {
            return &backend;
        }
    }
    return nullptr;
}

const CloudConfig * CloudClient::select_backend(const InferenceRequest & request) const {
    const auto select_match = [&](const auto & predicate) -> const CloudConfig * {
        if (predicate(config_)) {
            return &config_;
        }
        for (const auto & backend : named_backends_) {
            if (predicate(backend)) {
                return &backend;
            }
        }
        return nullptr;
    };

    if (!request.seceda.preferred_backend_id.empty()) {
        if (const CloudConfig * const matched =
                select_match([&](const CloudConfig & backend) {
                    return backend.backend_id == request.seceda.preferred_backend_id &&
                        !backend.base_url.empty();
                })) {
            return matched;
        }
    }

    if (!request.seceda.preferred_model_alias.empty()) {
        if (const CloudConfig * const matched =
                select_match([&](const CloudConfig & backend) {
                    return backend.model_alias == request.seceda.preferred_model_alias &&
                        !backend.base_url.empty();
                })) {
            return matched;
        }
    }

    return default_backend();
}

CloudCompletionResult CloudClient::complete(const InferenceRequest & request) {
    return complete_impl(request, nullptr);
}

CloudCompletionResult CloudClient::complete_stream(
    const InferenceRequest & request,
    const StreamDeltaCallback & on_delta) {
    return complete_impl(request, &on_delta);
}

CloudCompletionResult CloudClient::complete_impl(
    const InferenceRequest & request,
    const StreamDeltaCallback * on_delta) {
    CloudCompletionResult result;
    const CloudConfig * const backend = select_backend(request);
    if (backend == nullptr || backend->base_url.empty()) {
        result.error = "Cloud fallback is not configured";
        return result;
    }

    if (!curl_ready()) {
        result.error = "Failed to initialize libcurl";
        return result;
    }

    result.message.role = "assistant";
    result.identity.route_target = RouteTarget::kCloud;
    result.identity.backend_id = backend->backend_id;
    result.identity.model_id = backend->model;
    result.identity.model_alias = backend->model_alias;
    result.identity.display_name = backend->display_name;
    result.identity.execution_mode = backend->execution_mode;
    result.identity.capabilities = backend->capabilities;

    const bool upstream_stream = request.options.stream || !wants_rich_openai_response(request);
    json body = {
        {"model", backend->model},
        {"messages", json::array()},
        {"max_tokens", request.options.max_completion_tokens},
        {"temperature", request.options.temperature},
        {"top_p", request.options.top_p},
        {"stream", upstream_stream},
    };

    if (request.options.top_k > 0) {
        body["top_k"] = request.options.top_k;
    }
    if (request.options.min_p > 0.0f) {
        body["min_p"] = request.options.min_p;
    }
    if (should_forward_seed(request)) {
        body["seed"] = request.options.seed;
    }

    for (const auto & message : request.messages) {
        body["messages"].push_back(build_message_payload(message));
    }

    if (!request.advanced.user.empty()) {
        body["user"] = request.advanced.user;
    }
    if (!request.advanced.stop_sequences.empty()) {
        body["stop"] = request.advanced.stop_sequences;
    }
    try {
        if (!request.advanced.tools_json.empty()) {
            body["tools"] = json::parse(request.advanced.tools_json);
        }
        if (!request.advanced.tool_choice_json.empty()) {
            body["tool_choice"] = json::parse(request.advanced.tool_choice_json);
        }
        if (!request.advanced.response_format_json.empty()) {
            body["response_format"] = json::parse(request.advanced.response_format_json);
        }
    } catch (const std::exception & exception) {
        result.error = std::string("Failed to serialize cloud request JSON: ") + exception.what();
        return result;
    }
    if (upstream_stream && request.options.include_usage_in_stream) {
        body["stream_options"] = {{"include_usage", true}};
    }

    const std::string body_string = body.dump();
    const std::string request_url = completion_url(backend->base_url);
    const auto request_start = std::chrono::steady_clock::now();
    const auto request_deadline = request_start + std::chrono::seconds(backend->timeout_seconds);
    const int max_attempts = std::max(1, backend->retry_attempts + 1);
    const std::string modal_session_id =
        backend->send_modal_session_id ? generate_modal_session_id() : std::string{};

    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        const auto attempt_start = std::chrono::steady_clock::now();
        const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      request_deadline - attempt_start)
                                      .count();
        if (remaining_ms <= 0) {
            if (result.error.empty()) {
                result.error = "Cloud request timed out";
            }
            result.timing.total_latency_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - request_start)
                                                 .count();
            return result;
        }

        CurlStreamState stream_state;
        stream_state.result = &result;
        stream_state.on_delta = on_delta;
        stream_state.raw_body.clear();
        stream_state.event_buffer.clear();
        stream_state.stream_error.clear();
        stream_state.parser_error.clear();
        stream_state.saw_stream_event = false;
        stream_state.saw_done = false;
        stream_state.stream_cancelled = false;
        stream_state.request_start = request_start;

        result = CloudCompletionResult{};
        result.message.role = "assistant";
        result.identity.route_target = RouteTarget::kCloud;
        result.identity.backend_id = backend->backend_id;
        result.identity.model_id = backend->model;
        result.identity.model_alias = backend->model_alias;
        result.identity.display_name = backend->display_name;
        result.identity.execution_mode = backend->execution_mode;
        result.identity.capabilities = backend->capabilities;

        CURL * curl = curl_easy_init();
        if (curl == nullptr) {
            result.error = "curl_easy_init failed";
            return result;
        }

        struct curl_slist * headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, "Accept: text/event-stream");
        const std::string auth_header = "Authorization: Bearer " + backend->api_key;
        if (!backend->api_key.empty()) {
            headers = curl_slist_append(headers, auth_header.c_str());
        }

        std::string modal_session_header;
        if (!modal_session_id.empty()) {
            modal_session_header = "Modal-Session-ID: " + modal_session_id;
            headers = curl_slist_append(headers, modal_session_header.c_str());
        }

        char error_buffer[CURL_ERROR_SIZE] = {};
        const long timeout_ms = static_cast<long>(remaining_ms);
        const long connect_timeout_ms = static_cast<long>(std::min<std::int64_t>(
            remaining_ms,
            static_cast<std::int64_t>(backend->connect_timeout_seconds) * 1000));

        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
        curl_easy_setopt(curl, CURLOPT_URL, request_url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_string.c_str());
        curl_easy_setopt(
            curl,
            CURLOPT_POSTFIELDSIZE_LARGE,
            static_cast<curl_off_t>(body_string.size()));
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, connect_timeout_ms);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, backend->verify_tls ? 1L : 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, backend->verify_tls ? 2L : 0L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &stream_state);

        const CURLcode curl_code = curl_easy_perform(curl);
        long response_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        result.timing.total_latency_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - request_start)
                                             .count();

        if (!stream_state.parser_error.empty()) {
            result.error = stream_state.parser_error;
            return result;
        }

        if (stream_state.stream_cancelled) {
            result.error = "Streaming cancelled by client";
            return result;
        }

        bool should_retry = false;
        if (curl_code != CURLE_OK) {
            result.error = std::string("Cloud request failed: ") +
                (!stream_state.stream_error.empty()
                        ? stream_state.stream_error
                        : (error_buffer[0] != '\0' ? error_buffer : curl_easy_strerror(curl_code)));
            should_retry = is_retryable_curl_code(curl_code);
        } else if (response_code != 200) {
            result.error = "Cloud server returned HTTP " + std::to_string(response_code) + ": " +
                stream_state.raw_body;
            should_retry = is_retryable_http_status(response_code);
        } else if (!stream_state.stream_error.empty()) {
            result.error = stream_state.stream_error;
        } else if (!stream_state.saw_stream_event) {
            try {
                const json payload = json::parse(stream_state.raw_body);
                bool finished = false;
                std::string parse_error;
                if (!apply_completion_payload(payload, result, false, finished, parse_error)) {
                    result.error = parse_error;
                    return result;
                }
                apply_usage_tokens(payload, result.timing);
                result.ok = true;
                return result;
            } catch (const std::exception & exception) {
                result.error =
                    std::string("Failed to parse cloud response JSON: ") + exception.what();
                return result;
            }
        } else if (!stream_state.saw_done) {
            result.error = "Cloud stream terminated before a completion marker was received";
        } else {
            result.ok = true;
            return result;
        }

        if (!should_retry || attempt + 1 >= max_attempts) {
            return result;
        }

        const auto sleep_remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            request_deadline - std::chrono::steady_clock::now())
                                            .count();
        if (sleep_remaining_ms <= 0) {
            return result;
        }

        const auto sleep_duration = std::chrono::milliseconds(std::min<std::int64_t>(
            sleep_remaining_ms,
            static_cast<std::int64_t>(backend->retry_backoff_ms)));
        if (sleep_duration.count() > 0) {
            std::this_thread::sleep_for(sleep_duration);
        }
    }

    return result;
}

}  // namespace seceda::edge
