#include "cloud_bridge/cloud_client.hpp"

#include <cpp-httplib/httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using json = nlohmann::json;
using namespace seceda::edge;

bool require(bool condition, const char * message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        return false;
    }
    return true;
}

bool require_contains(
    const std::string & haystack,
    const std::string & needle,
    const char * message) {
    return require(haystack.find(needle) != std::string::npos, message);
}

class TestServer {
public:
    bool start() {
        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ <= 0) {
            return false;
        }

        thread_ = std::thread([this]() { server_.listen_after_bind(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        return true;
    }

    ~TestServer() {
        server_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    httplib::Server & server() { return server_; }

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

private:
    httplib::Server server_;
    int port_ = 0;
    std::thread thread_;
};

bool test_streaming_completion_generates_distinct_session_ids() {
    TestServer test_server;
    if (!require(test_server.start(), "test server should start")) {
        return false;
    }

    std::mutex mutex;
    std::vector<std::string> session_ids;
    std::vector<json> request_bodies;

    test_server.server().Post(
        "/v1/chat/completions",
        [&](const httplib::Request & request, httplib::Response & response) {
            std::lock_guard<std::mutex> lock(mutex);
            session_ids.push_back(request.get_header_value("Modal-Session-ID"));
            request_bodies.push_back(json::parse(request.body));

            response.set_content(
                "data: {\"choices\":[{\"delta\":{\"content\":\"hello\"}}]}\n\n"
                "data: {\"choices\":[{\"delta\":{\"content\":\" world\"},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":2}}\n\n"
                "data: [DONE]\n\n",
                "text/event-stream");
        });

    CloudConfig config;
    config.base_url = test_server.base_url();
    config.model = "seceda-cloud-default";
    config.timeout_seconds = 5;
    config.connect_timeout_seconds = 2;
    config.retry_attempts = 0;
    config.retry_backoff_ms = 0;
    config.send_modal_session_id = true;

    CloudClient client(config);

    InferenceRequest first_request;
    first_request.messages.push_back({"system", "be helpful", {}, {}, {}});
    first_request.messages.push_back({"user", "first", {}, {}, {}});
    refresh_request_views(first_request);

    const auto first_result = client.complete(first_request);
    if (!require(first_result.ok, "streaming request should succeed")) {
        return false;
    }
    if (!require(first_result.text == "hello world", "streaming chunks should concatenate")) {
        return false;
    }
    if (!require(first_result.timing.has_ttft, "streaming request should record ttft")) {
        return false;
    }
    if (!require(first_result.timing.prompt_tokens == 3, "streaming request should parse prompt tokens")) {
        return false;
    }
    if (!require(
            first_result.timing.generated_tokens == 2,
            "streaming request should parse completion tokens")) {
        return false;
    }

    InferenceRequest second_request = first_request;
    second_request.messages.back().content = "second";
    refresh_request_views(second_request);

    const auto second_result = client.complete(second_request);
    if (!require(second_result.ok, "second streaming request should succeed")) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex);
    if (!require(session_ids.size() == 2, "two streaming requests should be captured")) {
        return false;
    }
    if (!require(!session_ids[0].empty(), "first session id should be present")) {
        return false;
    }
    if (!require(!session_ids[1].empty(), "second session id should be present")) {
        return false;
    }
    if (!require(session_ids[0] != session_ids[1], "single-turn requests should get distinct session ids")) {
        return false;
    }
    if (!require(request_bodies.size() == 2, "streaming request bodies should be captured")) {
        return false;
    }
    if (!require(request_bodies[0]["stream"].get<bool>(), "stream flag should stay enabled")) {
        return false;
    }
    if (!require(
            request_bodies[0]["messages"].size() == 2,
            "system and user messages should both be forwarded")) {
        return false;
    }
    if (!require(
            request_bodies[0]["messages"][0]["role"].get<std::string>() == "system",
            "system prompt should be forwarded as a system message")) {
        return false;
    }
    if (!require(
            request_bodies[0]["messages"][1]["content"].get<std::string>() == "first",
            "user text should be forwarded")) {
        return false;
    }
    if (!require(
            !request_bodies[0].contains("seed"),
            "default sentinel seed should not be forwarded")) {
        return false;
    }

    return true;
}

bool test_json_fallback_response_and_conditional_forwarding() {
    TestServer test_server;
    if (!require(test_server.start(), "test server should start")) {
        return false;
    }

    std::mutex mutex;
    std::string captured_session_id;
    json captured_body;

    test_server.server().Post(
        "/v1/chat/completions",
        [&](const httplib::Request & request, httplib::Response & response) {
            std::lock_guard<std::mutex> lock(mutex);
            captured_session_id = request.get_header_value("Modal-Session-ID");
            captured_body = json::parse(request.body);

            response.set_content(
                R"({"choices":[{"message":{"content":"json fallback"}}],"usage":{"prompt_tokens":7,"completion_tokens":5}})",
                "application/json");
        });

    CloudConfig config;
    config.base_url = test_server.base_url();
    config.timeout_seconds = 5;
    config.connect_timeout_seconds = 2;
    config.retry_attempts = 0;
    config.retry_backoff_ms = 0;
    config.send_modal_session_id = false;

    CloudClient client(config);

    InferenceRequest request;
    request.messages.push_back({"user", "json", {}, {}, {}});
    refresh_request_views(request);
    request.options.top_k = 0;
    request.options.min_p = 0.0f;
    request.options.seed = 1234;

    const auto result = client.complete(request);
    if (!require(result.ok, "json fallback response should succeed")) {
        return false;
    }
    if (!require(result.text == "json fallback", "json fallback content should be parsed")) {
        return false;
    }
    if (!require(result.timing.prompt_tokens == 7, "json fallback should parse prompt tokens")) {
        return false;
    }
    if (!require(result.timing.generated_tokens == 5, "json fallback should parse completion tokens")) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex);
    if (!require(
            captured_session_id.empty(),
            "Modal session header should be omitted when disabled")) {
        return false;
    }
    if (!require(captured_body.contains("seed"), "explicit seed should be forwarded")) {
        return false;
    }
    if (!require(captured_body["seed"].get<std::uint32_t>() == 1234, "seed should round-trip")) {
        return false;
    }
    if (!require(!captured_body.contains("top_k"), "non-positive top_k should be omitted")) {
        return false;
    }
    if (!require(!captured_body.contains("min_p"), "non-positive min_p should be omitted")) {
        return false;
    }

    return true;
}

bool test_retry_on_503_reuses_same_session_id() {
    TestServer test_server;
    if (!require(test_server.start(), "test server should start")) {
        return false;
    }

    std::atomic<int> request_count = 0;
    std::mutex mutex;
    std::vector<std::string> session_ids;

    test_server.server().Post(
        "/v1/chat/completions",
        [&](const httplib::Request & request, httplib::Response & response) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                session_ids.push_back(request.get_header_value("Modal-Session-ID"));
            }

            const int current_request = ++request_count;
            if (current_request == 1) {
                response.status = 503;
                response.set_content("warming up", "text/plain");
                return;
            }

            response.set_content(
                "data: {\"choices\":[{\"delta\":{\"content\":\"ready\"},\"finish_reason\":\"stop\"}]}\n\n"
                "data: [DONE]\n\n",
                "text/event-stream");
        });

    CloudConfig config;
    config.base_url = test_server.base_url();
    config.timeout_seconds = 5;
    config.connect_timeout_seconds = 2;
    config.retry_attempts = 1;
    config.retry_backoff_ms = 0;
    config.send_modal_session_id = true;

    CloudClient client(config);

    InferenceRequest request;
    request.messages.push_back({"user", "retry", {}, {}, {}});
    refresh_request_views(request);

    const auto result = client.complete(request);
    if (!require(result.ok, "retryable 503 should recover")) {
        return false;
    }
    if (!require(result.text == "ready", "retry should return the successful body")) {
        return false;
    }
    if (!require(request_count.load() == 2, "retryable 503 should trigger exactly one retry")) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex);
    if (!require(session_ids.size() == 2, "both attempts should be captured")) {
        return false;
    }
    if (!require(!session_ids[0].empty(), "retry path should include a session id")) {
        return false;
    }
    if (!require(
            session_ids[0] == session_ids[1],
            "retries for one logical request should reuse the same session id")) {
        return false;
    }

    return true;
}

bool test_partial_stream_returns_error() {
    TestServer test_server;
    if (!require(test_server.start(), "test server should start")) {
        return false;
    }

    test_server.server().Post(
        "/v1/chat/completions",
        [&](const httplib::Request &, httplib::Response & response) {
            response.set_content(
                "data: {\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}\n\n",
                "text/event-stream");
        });

    CloudConfig config;
    config.base_url = test_server.base_url();
    config.timeout_seconds = 5;
    config.connect_timeout_seconds = 2;
    config.retry_attempts = 0;
    config.retry_backoff_ms = 0;
    config.send_modal_session_id = true;

    CloudClient client(config);

    InferenceRequest request;
    request.messages.push_back({"user", "partial", {}, {}, {}});
    refresh_request_views(request);

    const auto result = client.complete(request);
    if (!require(!result.ok, "partial stream should fail")) {
        return false;
    }
    if (!require_contains(
            result.error,
            "completion marker",
            "partial stream error should mention missing completion marker")) {
        return false;
    }

    return true;
}

bool test_named_backend_selection_by_model_alias() {
    TestServer default_server;
    TestServer alt_server;
    if (!require(default_server.start(), "default server should start")) {
        return false;
    }
    if (!require(alt_server.start(), "alt server should start")) {
        return false;
    }

    std::atomic<int> default_hits = 0;
    std::atomic<int> alt_hits = 0;

    default_server.server().Post(
        "/v1/chat/completions",
        [&](const httplib::Request &, httplib::Response & response) {
            ++default_hits;
            response.set_content(
                R"({"choices":[{"message":{"content":"default"}}],"usage":{"prompt_tokens":1,"completion_tokens":1}})",
                "application/json");
        });

    alt_server.server().Post(
        "/v1/chat/completions",
        [&](const httplib::Request &, httplib::Response & response) {
            ++alt_hits;
            response.set_content(
                R"({"choices":[{"message":{"content":"alt"}}],"usage":{"prompt_tokens":2,"completion_tokens":1}})",
                "application/json");
        });

    CloudConfig default_config;
    default_config.backend_id = "remote/default";
    default_config.model = "default-model";
    default_config.model_alias = "remote/default";
    default_config.base_url = default_server.base_url();
    default_config.timeout_seconds = 5;
    default_config.connect_timeout_seconds = 2;
    default_config.retry_attempts = 0;
    default_config.retry_backoff_ms = 0;

    CloudConfig alt_config = default_config;
    alt_config.backend_id = "remote/alt";
    alt_config.model = "alt-model";
    alt_config.model_alias = "remote/alt";
    alt_config.base_url = alt_server.base_url();

    CloudClient client(default_config, {alt_config});

    InferenceRequest request;
    request.messages.push_back({"user", "route to alt", {}, {}, {}});
    request.seceda.preferred_backend_id = "remote/alt";
    request.seceda.preferred_model_alias = "remote/alt";
    refresh_request_views(request);

    const auto result = client.complete(request);
    if (!require(result.ok, "named backend request should succeed")) {
        return false;
    }
    if (!require(result.text == "alt", "named backend should route to alternate backend")) {
        return false;
    }
    if (!require(result.identity.backend_id == "remote/alt", "result identity should reflect named backend")) {
        return false;
    }
    if (!require(default_hits.load() == 0, "default backend should not be used")) {
        return false;
    }
    if (!require(alt_hits.load() == 1, "alternate backend should be used exactly once")) {
        return false;
    }

    return true;
}

}  // namespace

int main() {
    if (!test_streaming_completion_generates_distinct_session_ids()) {
        return 1;
    }
    if (!test_json_fallback_response_and_conditional_forwarding()) {
        return 1;
    }
    if (!test_retry_on_503_reuses_same_session_id()) {
        return 1;
    }
    if (!test_partial_stream_returns_error()) {
        return 1;
    }
    if (!test_named_backend_selection_by_model_alias()) {
        return 1;
    }

    return 0;
}
