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
    std::string raw_body;
    std::string event_buffer;
    std::string stream_error;
    std::string parser_error;
    bool saw_stream_event = false;
    bool saw_done = false;
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

std::string extract_chat_content(const json & payload, bool & finished) {
    finished = false;
    if (!payload.contains("choices") || !payload["choices"].is_array() ||
        payload["choices"].empty()) {
        return {};
    }

    const auto & choice = payload["choices"][0];
    if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
        finished = true;
    }

    if (choice.contains("delta") && choice["delta"].is_object()) {
        const auto & delta = choice["delta"];
        if (delta.contains("content") && delta["content"].is_string()) {
            return delta["content"].get<std::string>();
        }
    }

    if (choice.contains("message") && choice["message"].is_object()) {
        const auto & message = choice["message"];
        if (message.contains("content") && message["content"].is_string()) {
            finished = true;
            return message["content"].get<std::string>();
        }
    }

    return {};
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
            const std::string content = extract_chat_content(parsed_payload, finished);
            if (!content.empty()) {
                if (!state.result->timing.has_ttft) {
                    state.result->timing.has_ttft = true;
                    state.result->timing.ttft_ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - state.request_start)
                                                        .count();
                }
                state.result->text += content;
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

CloudClient::CloudClient(CloudConfig config) : config_(std::move(config)) {}

bool CloudClient::is_configured() const {
    return !config_.base_url.empty();
}

CloudClientInfo CloudClient::info() const {
    CloudClientInfo info;
    info.configured = is_configured();
    info.base_url = config_.base_url;
    info.model = config_.model;
    info.timeout_seconds = config_.timeout_seconds;
    info.connect_timeout_seconds = config_.connect_timeout_seconds;
    info.retry_attempts = config_.retry_attempts;
    info.retry_backoff_ms = config_.retry_backoff_ms;
    info.send_modal_session_id = config_.send_modal_session_id;
    info.verify_tls = config_.verify_tls;
    return info;
}

CloudCompletionResult CloudClient::complete(const InferenceRequest & request) {
    CloudCompletionResult result;
    if (!is_configured()) {
        result.error = "Cloud fallback is not configured";
        return result;
    }

    if (!curl_ready()) {
        result.error = "Failed to initialize libcurl";
        return result;
    }

    json body = {
        {"model", config_.model},
        {"messages", json::array()},
        {"max_tokens", request.options.max_tokens},
        {"temperature", request.options.temperature},
        {"top_p", request.options.top_p},
        {"stream", true},
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

    if (!request.system_prompt.empty()) {
        body["messages"].push_back(
            {{"role", "system"}, {"content", request.system_prompt}});
    }
    body["messages"].push_back({{"role", "user"}, {"content", request.text}});
    const std::string body_string = body.dump();
    const std::string request_url = completion_url(config_.base_url);
    const auto request_start = std::chrono::steady_clock::now();
    const auto request_deadline = request_start + std::chrono::seconds(config_.timeout_seconds);
    const int max_attempts = std::max(1, config_.retry_attempts + 1);
    const std::string modal_session_id =
        config_.send_modal_session_id ? generate_modal_session_id() : std::string{};

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
        stream_state.raw_body.clear();
        stream_state.event_buffer.clear();
        stream_state.stream_error.clear();
        stream_state.parser_error.clear();
        stream_state.saw_stream_event = false;
        stream_state.saw_done = false;
        stream_state.request_start = request_start;

        result = CloudCompletionResult{};

        CURL * curl = curl_easy_init();
        if (curl == nullptr) {
            result.error = "curl_easy_init failed";
            return result;
        }

        struct curl_slist * headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, "Accept: text/event-stream");
        const std::string auth_header = "Authorization: Bearer " + config_.api_key;
        if (!config_.api_key.empty()) {
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
            static_cast<std::int64_t>(config_.connect_timeout_seconds) * 1000));

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
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, config_.verify_tls ? 1L : 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, config_.verify_tls ? 2L : 0L);
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
                result.text = extract_chat_content(payload, finished);
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
            static_cast<std::int64_t>(config_.retry_backoff_ms)));
        if (sleep_duration.count() > 0) {
            std::this_thread::sleep_for(sleep_duration);
        }
    }

    return result;
}

}  // namespace seceda::edge
