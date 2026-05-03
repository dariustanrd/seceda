#include "cloud_bridge/cloud_client.hpp"
#include "openai_compat/openai_compat.hpp"
#include "runtime/edge_daemon.hpp"

#include <cpp-httplib/httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using json = nlohmann::json;
using namespace seceda::edge;
namespace oa = seceda::edge::openai_compat;

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

bool require_not_contains(
    const std::string & haystack,
    const std::string & needle,
    const char * message) {
    return require(haystack.find(needle) == std::string::npos, message);
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

struct StreamStep {
    int delay_ms = 0;
    std::string content;
};

class StaticRouter : public IRouter {
public:
    RouteDecision decide(const InferenceRequest &) const override { return decision_; }
    RouterConfig config() const override { return {}; }

    RouteDecision decision_;
};

class ScriptedLocalRuntime : public ILocalModelRuntime {
public:
    bool load(const LocalModelConfig & config, const std::string &, std::string & error) override {
        config_ = config;
        if (!next_load_ok_) {
            ready_ = false;
            error = "load failed";
            return false;
        }
        ready_ = true;
        if (config_.model_path.empty()) {
            config_.model_path = "scripted.gguf";
        }
        return true;
    }

    bool reload(const LocalModelConfig & config, const std::string &, std::string & error) override {
        return load(config, {}, error);
    }

    bool is_ready() const override { return ready_; }

    LocalModelInfo info() const override {
        LocalModelInfo info;
        info.ready = ready_;
        info.engine_id = config_.engine_id.empty() ? "local/test" : config_.engine_id;
        info.backend_id = config_.backend_id.empty() ? "local" : config_.backend_id;
        info.model_id = config_.model_id.empty() ? "local-test" : config_.model_id;
        info.model_alias = config_.model_alias.empty() ? "local/default" : config_.model_alias;
        info.display_name = config_.display_name.empty() ? "Scripted local runtime" : config_.display_name;
        info.execution_mode = config_.execution_mode.empty() ? "in_process" : config_.execution_mode;
        info.model_path = config_.model_path;
        return info;
    }

    LocalCompletionResult generate(const InferenceRequest & request) override {
        return generate_stream(request, [](const StreamedChatDelta &) { return true; });
    }

    LocalCompletionResult generate_stream(
        const InferenceRequest &,
        const StreamDeltaCallback & on_delta) override {
        ++stream_calls_;

        LocalCompletionResult result;
        result.identity.route_target = RouteTarget::kLocal;
        result.identity.engine_id = config_.engine_id.empty() ? "local/test" : config_.engine_id;
        result.identity.backend_id = config_.backend_id.empty() ? "local" : config_.backend_id;
        result.identity.model_id = config_.model_id.empty() ? "local-test" : config_.model_id;
        result.identity.model_alias = config_.model_alias.empty() ? "local/default" : config_.model_alias;
        result.identity.display_name =
            config_.display_name.empty() ? "Scripted local runtime" : config_.display_name;
        result.identity.execution_mode = config_.execution_mode.empty() ? "in_process" : config_.execution_mode;
        result.active_model_path = config_.model_path;
        result.message.role = "assistant";
        result.timing.prompt_tokens = 1;

        if (!ready_) {
            result.error = "Local runtime is unavailable";
            return result;
        }

        const auto request_start = std::chrono::steady_clock::now();
        if (fail_before_first_delta_) {
            result.error = error_message_;
            result.timing.total_latency_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - request_start)
                                                 .count();
            return result;
        }

        for (const auto & step : steps_) {
            if (step.delay_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(step.delay_ms));
            }

            if (step.content.empty()) {
                continue;
            }

            if (!result.timing.has_ttft) {
                result.timing.has_ttft = true;
                result.timing.ttft_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - request_start)
                                            .count();
            }

            StreamedChatDelta delta;
            delta.content = step.content;
            if (!on_delta(delta)) {
                stream_cancelled_ = true;
                result.error = "Streaming cancelled by client";
                result.timing.total_latency_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - request_start)
                                                     .count();
                return result;
            }

            result.text += step.content;
            result.timing.generated_tokens += 1;
        }

        result.message.content = result.text;
        result.timing.total_latency_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - request_start)
                                                 .count();

        if (fail_after_stream_) {
            result.error = error_message_;
            return result;
        }

        result.ok = true;
        result.finish_reason = "stop";
        return result;
    }

    bool next_load_ok_ = true;
    bool ready_ = false;
    bool fail_before_first_delta_ = false;
    bool fail_after_stream_ = false;
    std::string error_message_ = "local failure";
    std::vector<StreamStep> steps_;
    std::atomic<bool> stream_cancelled_{false};
    std::atomic<int> stream_calls_{0};

private:
    LocalModelConfig config_;
};

class RecordingCloudClient : public ICloudClient {
public:
    bool is_configured() const override { return configured_; }

    CloudClientInfo info() const override {
        CloudClientInfo info;
        info.configured = configured_;
        info.backend_id = "remote/test";
        info.model = "remote-model";
        info.model_alias = "remote/default";
        info.display_name = "Recording cloud client";
        info.execution_mode = "remote_service";
        return info;
    }

    CloudCompletionResult complete(const InferenceRequest & request) override {
        return complete_stream(request, [](const StreamedChatDelta &) { return true; });
    }

    CloudCompletionResult complete_stream(
        const InferenceRequest &,
        const StreamDeltaCallback &) override {
        ++complete_stream_calls_;
        CloudCompletionResult result;
        result.identity.route_target = RouteTarget::kCloud;
        result.identity.backend_id = "remote/test";
        result.identity.model_id = "remote-model";
        result.identity.model_alias = "remote/default";
        result.identity.display_name = "Recording cloud client";
        result.identity.execution_mode = "remote_service";
        result.message.role = "assistant";
        result.text = "cloud answer";
        result.message.content = result.text;
        result.finish_reason = "stop";
        result.ok = configured_;
        if (!configured_) {
            result.error = "Cloud fallback is not configured";
        }
        return result;
    }

    bool configured_ = false;
    std::atomic<int> complete_stream_calls_{0};
};

struct StreamCapture {
    bool request_ok = false;
    bool saw_first_chunk = false;
    int status = 0;
    std::string body;
    std::chrono::steady_clock::duration first_chunk_after{};
    std::chrono::steady_clock::duration total_duration{};
};

DaemonConfig minimal_config() {
    DaemonConfig config;
    config.public_model_alias = "seceda/default";
    config.exposed_models = {
        {"seceda/default", "Seceda default route", "seceda"},
    };
    return config;
}

json build_stream_request(bool include_usage = true) {
    json request = {
        {"model", "seceda/default"},
        {"stream", true},
        {"messages", json::array({{{"role", "user"}, {"content", "hello"}}})},
    };
    if (include_usage) {
        request["stream_options"] = {{"include_usage", true}};
    }
    return request;
}

std::string sse_data(const json & payload) {
    return "data: " + payload.dump() + "\n\n";
}

json upstream_role_chunk() {
    return {
        {"choices", json::array({{{"delta", {{"role", "assistant"}}}}})},
    };
}

json upstream_content_chunk(
    const std::string & content,
    const char * finish_reason = nullptr,
    int prompt_tokens = 0,
    int completion_tokens = 0) {
    json choice = {{"delta", {{"content", content}}}};
    if (finish_reason != nullptr) {
        choice["finish_reason"] = finish_reason;
    }

    json payload = {
        {"choices", json::array({choice})},
    };
    if (prompt_tokens > 0 || completion_tokens > 0) {
        payload["usage"] = {
            {"prompt_tokens", prompt_tokens},
            {"completion_tokens", completion_tokens},
        };
    }
    return payload;
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

json event_to_json(const InferenceEvent & event) {
    json payload = {
        {"id", event.id},
        {"request_id", event.request_id},
        {"timestamp_utc", event.timestamp_utc},
        {"requested_target", to_string(event.requested_target)},
        {"initial_target", to_string(event.initial_target)},
        {"final_target", to_string(event.final_target)},
        {"ok", event.ok},
        {"fallback_used", event.fallback_used},
        {"error_kind", to_string(event.error_kind)},
        {"error", event.error},
        {"finish_reason", event.finish_reason},
        {"route_reason", event.route_reason},
        {"fallback_reason", event.fallback_reason},
        {"matched_rules", event.matched_rules},
        {"initial_engine_id", event.initial_engine_id},
        {"initial_backend_id", event.initial_backend_id},
        {"initial_model_id", event.initial_model_id},
        {"initial_model_alias", event.initial_model_alias},
        {"initial_execution_mode", event.initial_execution_mode},
        {"engine_id", event.engine_id},
        {"backend_id", event.backend_id},
        {"model_id", event.model_id},
        {"model_alias", event.model_alias},
        {"display_name", event.display_name},
        {"execution_mode", event.execution_mode},
        {"active_model_path", event.active_model_path},
        {"timing", timing_to_json(event.timing)},
    };
    if (event.local_timing.has_value()) {
        payload["local_timing"] = timing_to_json(*event.local_timing);
    }
    if (event.cloud_timing.has_value()) {
        payload["cloud_timing"] = timing_to_json(*event.cloud_timing);
    }
    return payload;
}

json trace_event_to_json(const PromptTraceEvent & event) {
    json payload = {
        {"id", event.id},
        {"sequence_number", event.sequence_number},
        {"request_id", event.request_id},
        {"transport", event.transport},
        {"timestamp_utc", event.timestamp_utc},
        {"phase", event.phase},
        {"item_type", event.item_type},
        {"item_id", event.item_id},
        {"content_index", event.content_index >= 0 ? json(event.content_index) : json(nullptr)},
        {"role", event.role},
        {"tool_name", event.tool_name},
        {"delta_text", event.delta_text},
        {"text", event.text},
    };
    if (!event.payload_json.empty()) {
        try {
            payload["payload"] = json::parse(event.payload_json);
        } catch (const std::exception &) {
            payload["payload_json"] = event.payload_json;
        }
    }
    return payload;
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

void install_openai_chat_route(
    TestServer & server,
    EdgeDaemon & daemon,
    const DaemonConfig & config) {
    server.server().Get("/metrics/events", [&](const httplib::Request & request, httplib::Response & response) {
        std::string error;
        std::uint64_t since_id = 0;
        std::uint64_t limit = 1000;
        const std::string request_id =
            request.has_param("request_id") ? request.get_param_value("request_id") : std::string{};
        if (!parse_query_uint64(request, "since_id", 0, since_id, error) ||
            !parse_query_uint64(request, "limit", 1000, limit, error)) {
            response.status = 400;
            response.set_content(json{{"ok", false}, {"error", error}}.dump(2), "application/json");
            return;
        }

        const auto batch = daemon.events(since_id, static_cast<std::size_t>(limit), request_id);
        json payload = {
            {"since_id", batch.since_id},
            {"latest_id", batch.latest_id},
            {"returned", batch.events.size()},
            {"events", json::array()},
        };
        if (!request_id.empty()) {
            payload["request_id"] = request_id;
        }
        for (const auto & event : batch.events) {
            payload["events"].push_back(event_to_json(event));
        }

        response.set_content(payload.dump(2), "application/json");
    });

    server.server().Get("/trace/events", [&](const httplib::Request & request, httplib::Response & response) {
        std::string error;
        std::uint64_t since_id = 0;
        std::uint64_t limit = 2000;
        const std::string request_id =
            request.has_param("request_id") ? request.get_param_value("request_id") : std::string{};
        if (!parse_query_uint64(request, "since_id", 0, since_id, error) ||
            !parse_query_uint64(request, "limit", 2000, limit, error)) {
            response.status = 400;
            response.set_content(json{{"ok", false}, {"error", error}}.dump(2), "application/json");
            return;
        }

        const auto batch = daemon.trace_events(since_id, static_cast<std::size_t>(limit), request_id);
        json payload = {
            {"since_id", batch.since_id},
            {"latest_id", batch.latest_id},
            {"returned", batch.events.size()},
            {"events", json::array()},
        };
        if (!request_id.empty()) {
            payload["request_id"] = request_id;
        }
        for (const auto & event : batch.events) {
            payload["events"].push_back(trace_event_to_json(event));
        }

        response.set_content(payload.dump(2), "application/json");
    });

    server.server().Post(
        "/v1/chat/completions",
        [&](const httplib::Request & request, httplib::Response & response) {
            InferenceRequest inference_request;
            std::string error;
            if (!oa::parse_chat_completion_request(request.body, config, inference_request, error)) {
                oa::set_openai_error(response, 400, error, "invalid_request_error");
                return;
            }

            const std::string completion_id = oa::ensure_chat_completion_id(inference_request);
            if (inference_request.options.stream) {
                oa::set_streaming_chat_completion_response(
                    response,
                    daemon,
                    std::move(inference_request),
                    request.is_connection_closed);
                return;
            }

            const auto inference_response = daemon.handle_inference(inference_request);
            if (!inference_response.ok) {
                oa::set_openai_error(
                    response,
                    oa::http_status_for_inference(inference_response),
                    inference_response.error.empty() ? "Inference request failed"
                                                     : inference_response.error,
                    oa::openai_error_type(inference_response));
                return;
            }

            response.set_content(
                oa::chat_completion_response(
                    inference_request,
                    inference_response,
                    completion_id,
                    oa::unix_timestamp_now())
                    .dump(),
                "application/json");
        });
}

bool post_stream_request(
    const std::string & base_url,
    const json & request_body,
    StreamCapture & capture,
    bool cancel_after_first_chunk = false) {
    httplib::Client client(base_url);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(10, 0);

    const auto start = std::chrono::steady_clock::now();
    const auto result = client.Post(
        "/v1/chat/completions",
        httplib::Headers{},
        request_body.dump(),
        "application/json",
        [&](const char * data, size_t data_length) {
            if (!capture.saw_first_chunk) {
                capture.saw_first_chunk = true;
                capture.first_chunk_after = std::chrono::steady_clock::now() - start;
            }
            capture.body.append(data, data_length);
            return !cancel_after_first_chunk;
        });
    capture.total_duration = std::chrono::steady_clock::now() - start;
    capture.request_ok = static_cast<bool>(result);
    if (result) {
        capture.status = result->status;
    }
    return capture.request_ok;
}

bool require_stream_starts_early(const StreamCapture & capture, const char * message) {
    if (!require(capture.saw_first_chunk, "stream should deliver at least one chunk")) {
        return false;
    }

    const auto first_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(capture.first_chunk_after).count();
    const auto total_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(capture.total_duration).count();
    return require(first_ms + 60 < total_ms, message);
}

std::string extract_stream_request_id(const std::string & body) {
    static constexpr const char kNeedle[] = "\"id\":\"";
    const std::size_t start = body.find(kNeedle);
    if (start == std::string::npos) {
        return {};
    }
    const std::size_t value_start = start + sizeof(kNeedle) - 1;
    const std::size_t value_end = body.find('"', value_start);
    if (value_end == std::string::npos) {
        return {};
    }
    return body.substr(value_start, value_end - value_start);
}

bool test_local_incremental_streaming() {
    auto local = std::make_shared<ScriptedLocalRuntime>();
    local->steps_ = {{0, "hel"}, {150, "lo"}};

    auto cloud = std::make_shared<RecordingCloudClient>();
    auto router = std::make_shared<StaticRouter>();
    router->decision_.target = RouteTarget::kLocal;
    router->decision_.reason = "local_default";

    DaemonConfig config = minimal_config();
    config.local.model_path = "local.gguf";

    EdgeDaemon daemon(config, local, cloud, router);
    if (!require(daemon.initialize(), "local streaming daemon should initialize")) {
        return false;
    }

    TestServer edge_server;
    install_openai_chat_route(edge_server, daemon, config);
    if (!require(edge_server.start(), "edge server should start")) {
        return false;
    }

    StreamCapture capture;
    if (!require(
            post_stream_request(edge_server.base_url(), build_stream_request(), capture),
            "local stream request should succeed")) {
        return false;
    }
    if (!require(capture.status == 200, "local stream should return HTTP 200")) {
        return false;
    }
    if (!require_stream_starts_early(capture, "local streaming should reach the client before completion")) {
        return false;
    }
    if (!require_contains(capture.body, "\"content\":\"hel\"", "local stream should include first delta")) {
        return false;
    }
    if (!require_contains(capture.body, "\"content\":\"lo\"", "local stream should include second delta")) {
        return false;
    }
    if (!require_contains(capture.body, "\"prompt_tokens\":1", "local stream should include usage chunk")) {
        return false;
    }
    if (!require_contains(capture.body, "data: [DONE]", "local stream should terminate with DONE")) {
        return false;
    }

    return true;
}

bool test_stream_completion_id_correlates_to_metrics_events() {
    auto local = std::make_shared<ScriptedLocalRuntime>();
    local->steps_ = {{0, "hel"}, {25, "lo"}};

    auto cloud = std::make_shared<RecordingCloudClient>();
    auto router = std::make_shared<StaticRouter>();
    router->decision_.target = RouteTarget::kLocal;
    router->decision_.reason = "local_default";
    router->decision_.matched_rules = {"local_default"};

    DaemonConfig config = minimal_config();
    config.local.model_path = "local.gguf";

    EdgeDaemon daemon(config, local, cloud, router);
    if (!require(daemon.initialize(), "stream correlation daemon should initialize")) {
        return false;
    }

    TestServer edge_server;
    install_openai_chat_route(edge_server, daemon, config);
    if (!require(edge_server.start(), "edge server should start")) {
        return false;
    }

    StreamCapture capture;
    if (!require(
            post_stream_request(edge_server.base_url(), build_stream_request(), capture),
            "stream correlation request should succeed")) {
        return false;
    }
    if (!require(capture.status == 200, "stream correlation should return HTTP 200")) {
        return false;
    }

    const std::string request_id = extract_stream_request_id(capture.body);
    if (!require(!request_id.empty(), "stream chunks should expose a request id")) {
        return false;
    }

    httplib::Client client(edge_server.base_url());
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(5, 0);
    const auto events = client.Get(("/metrics/events?request_id=" + request_id).c_str());
    if (!require(events && events->status == 200, "stream metrics events should return HTTP 200")) {
        return false;
    }

    json payload;
    try {
        payload = json::parse(events->body);
    } catch (...) {
        return require(false, "stream metrics events should return JSON");
    }

    if (!require(payload["returned"] == 1, "stream request id should match one event")) {
        return false;
    }
    if (!require(payload["events"].is_array() && payload["events"].size() == 1, "one stream event")) {
        return false;
    }

    const auto & event = payload["events"][0];
    if (!require(event["request_id"] == request_id, "stream event request id should match")) {
        return false;
    }
    if (!require(event["final_target"] == "local", "stream event should stay local")) {
        return false;
    }
    if (!require(event["active_model_path"] == "local.gguf", "stream event should record active model path")) {
        return false;
    }
    if (!require(event["timing"]["prompt_tokens"] == 1, "stream event should record prompt tokens")) {
        return false;
    }
    if (!require(event["timing"]["generated_tokens"] == 2, "stream event should record generated tokens")) {
        return false;
    }
    if (!require(event["local_timing"]["total_tokens"] == 3, "stream event should expose local timing totals")) {
        return false;
    }

    const auto traces = client.Get(("/trace/events?request_id=" + request_id).c_str());
    if (!require(traces && traces->status == 200, "stream trace events should return HTTP 200")) {
        return false;
    }

    json trace_payload;
    try {
        trace_payload = json::parse(traces->body);
    } catch (...) {
        return require(false, "stream trace events should return JSON");
    }

    if (!require(trace_payload["events"].is_array() && trace_payload["events"].size() >= 7, "stream trace rows")) {
        return false;
    }
    if (!require(trace_payload["events"][0]["phase"] == "request_normalized", "stream trace normalized")) {
        return false;
    }
    if (!require(trace_payload["events"][2]["phase"] == "route_selected", "stream trace route")) {
        return false;
    }
    if (!require(trace_payload["events"][3]["phase"] == "stream_started", "stream trace start")) {
        return false;
    }
    if (!require(trace_payload["events"][4]["delta_text"] == "hel", "stream trace first delta")) {
        return false;
    }
    if (!require(trace_payload["events"][5]["delta_text"] == "lo", "stream trace second delta")) {
        return false;
    }
    if (!require(
            trace_payload["events"].back()["phase"] == "request_complete",
            "stream trace completion")) {
        return false;
    }

    return true;
}

bool test_cloud_incremental_streaming() {
    TestServer upstream;
    if (!require(upstream.start(), "upstream server should start")) {
        return false;
    }

    std::mutex upstream_mutex;
    json upstream_request_body;
    upstream.server().Post(
        "/v1/chat/completions",
        [&](const httplib::Request & request, httplib::Response & response) {
            {
                std::lock_guard<std::mutex> lock(upstream_mutex);
                upstream_request_body = json::parse(request.body);
            }

            response.set_chunked_content_provider(
                "text/event-stream",
                [](size_t offset, httplib::DataSink & sink) {
                    if (offset > 0) {
                        sink.done();
                        return true;
                    }

                    const std::string role = sse_data(upstream_role_chunk());
                    if (!sink.write(role.data(), role.size())) {
                        return false;
                    }

                    const std::string first = sse_data(upstream_content_chunk("hel"));
                    if (!sink.write(first.data(), first.size())) {
                        return false;
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(150));

                    const std::string second =
                        sse_data(upstream_content_chunk("lo", "stop", 3, 2));
                    if (!sink.write(second.data(), second.size())) {
                        return false;
                    }

                    static constexpr const char kDone[] = "data: [DONE]\n\n";
                    if (!sink.write(kDone, sizeof(kDone) - 1)) {
                        return false;
                    }

                    sink.done();
                    return true;
                });
        });

    auto local = std::make_shared<ScriptedLocalRuntime>();
    auto router = std::make_shared<StaticRouter>();
    router->decision_.target = RouteTarget::kCloud;
    router->decision_.reason = "forced_cloud";

    DaemonConfig config = minimal_config();
    config.cloud.base_url = upstream.base_url();
    config.cloud.model = "remote-model";
    config.cloud.timeout_seconds = 5;
    config.cloud.connect_timeout_seconds = 2;
    config.cloud.retry_attempts = 0;
    config.cloud.retry_backoff_ms = 0;

    auto cloud = std::make_shared<CloudClient>(config.cloud);
    EdgeDaemon daemon(config, local, cloud, router);
    if (!require(daemon.initialize(), "cloud-only daemon should initialize")) {
        return false;
    }

    TestServer edge_server;
    install_openai_chat_route(edge_server, daemon, config);
    if (!require(edge_server.start(), "edge server should start")) {
        return false;
    }

    StreamCapture capture;
    if (!require(
            post_stream_request(edge_server.base_url(), build_stream_request(), capture),
            "cloud stream request should succeed")) {
        return false;
    }
    if (!require(capture.status == 200, "cloud stream should return HTTP 200")) {
        return false;
    }
    if (!require_stream_starts_early(capture, "cloud streaming should reach the client before completion")) {
        return false;
    }
    if (!require_contains(capture.body, "\"content\":\"hel\"", "cloud stream should include first delta")) {
        return false;
    }
    if (!require_contains(capture.body, "\"content\":\"lo\"", "cloud stream should include second delta")) {
        return false;
    }
    if (!require_contains(capture.body, "\"prompt_tokens\":3", "cloud usage should be forwarded")) {
        return false;
    }

    std::lock_guard<std::mutex> lock(upstream_mutex);
    if (!require(
            upstream_request_body.contains("stream") && upstream_request_body["stream"].get<bool>(),
            "streaming cloud request should keep upstream stream enabled")) {
        return false;
    }

    return true;
}

bool test_pre_first_byte_fallback_to_cloud_stream() {
    TestServer upstream;
    if (!require(upstream.start(), "fallback upstream server should start")) {
        return false;
    }

    std::atomic<int> upstream_calls = 0;
    upstream.server().Post(
        "/v1/chat/completions",
        [&](const httplib::Request &, httplib::Response & response) {
            ++upstream_calls;
            response.set_chunked_content_provider(
                "text/event-stream",
                [](size_t offset, httplib::DataSink & sink) {
                    if (offset > 0) {
                        sink.done();
                        return true;
                    }

                    const std::string role = sse_data(upstream_role_chunk());
                    if (!sink.write(role.data(), role.size())) {
                        return false;
                    }

                    const std::string content =
                        sse_data(upstream_content_chunk("cloud", "stop", 2, 1));
                    if (!sink.write(content.data(), content.size())) {
                        return false;
                    }

                    static constexpr const char kDone[] = "data: [DONE]\n\n";
                    if (!sink.write(kDone, sizeof(kDone) - 1)) {
                        return false;
                    }

                    sink.done();
                    return true;
                });
        });

    auto local = std::make_shared<ScriptedLocalRuntime>();
    local->fail_before_first_delta_ = true;
    local->error_message_ = "local early fail";

    auto router = std::make_shared<StaticRouter>();
    router->decision_.target = RouteTarget::kLocal;
    router->decision_.reason = "local_default";

    DaemonConfig config = minimal_config();
    config.local.model_path = "local.gguf";
    config.cloud.base_url = upstream.base_url();
    config.cloud.model = "remote-model";
    config.cloud.timeout_seconds = 5;
    config.cloud.connect_timeout_seconds = 2;
    config.cloud.retry_attempts = 0;
    config.cloud.retry_backoff_ms = 0;

    auto cloud = std::make_shared<CloudClient>(config.cloud);
    EdgeDaemon daemon(config, local, cloud, router);
    if (!require(daemon.initialize(), "fallback daemon should initialize")) {
        return false;
    }

    TestServer edge_server;
    install_openai_chat_route(edge_server, daemon, config);
    if (!require(edge_server.start(), "edge server should start")) {
        return false;
    }

    StreamCapture capture;
    if (!require(
            post_stream_request(edge_server.base_url(), build_stream_request(), capture),
            "fallback stream request should succeed")) {
        return false;
    }
    if (!require(capture.status == 200, "fallback stream should return HTTP 200")) {
        return false;
    }
    if (!require_contains(capture.body, "\"content\":\"cloud\"", "fallback should stream cloud output")) {
        return false;
    }
    if (!require_not_contains(
            capture.body,
            "local early fail",
            "pre-first-byte fallback should hide the local failure from the stream")) {
        return false;
    }
    if (!require(upstream_calls.load() == 1, "cloud fallback should be attempted once")) {
        return false;
    }

    return true;
}

bool test_post_first_byte_error_stays_on_selected_backend() {
    auto local = std::make_shared<ScriptedLocalRuntime>();
    local->steps_ = {{0, "partial"}};
    local->fail_after_stream_ = true;
    local->error_message_ = "local midstream fail";

    auto cloud = std::make_shared<RecordingCloudClient>();
    cloud->configured_ = true;

    auto router = std::make_shared<StaticRouter>();
    router->decision_.target = RouteTarget::kLocal;
    router->decision_.reason = "local_default";

    DaemonConfig config = minimal_config();
    config.local.model_path = "local.gguf";

    EdgeDaemon daemon(config, local, cloud, router);
    if (!require(daemon.initialize(), "midstream error daemon should initialize")) {
        return false;
    }

    TestServer edge_server;
    install_openai_chat_route(edge_server, daemon, config);
    if (!require(edge_server.start(), "edge server should start")) {
        return false;
    }

    StreamCapture capture;
    if (!require(
            post_stream_request(edge_server.base_url(), build_stream_request(false), capture),
            "midstream error request should still stream a response")) {
        return false;
    }
    if (!require(capture.status == 200, "midstream error stream should keep HTTP 200")) {
        return false;
    }
    if (!require_contains(
            capture.body,
            "\"content\":\"partial\"",
            "midstream error stream should include already-produced content")) {
        return false;
    }
    if (!require_contains(
            capture.body,
            "\"message\":\"local midstream fail\"",
            "midstream error should surface in-stream")) {
        return false;
    }
    if (!require_contains(capture.body, "data: [DONE]", "midstream error should still terminate the stream")) {
        return false;
    }
    if (!require(
            cloud->complete_stream_calls_.load() == 0,
            "post-first-byte error should not fall back to cloud")) {
        return false;
    }

    return true;
}

bool test_stream_disconnect_cancels_local_generation() {
    auto local = std::make_shared<ScriptedLocalRuntime>();
    local->steps_ = {{0, "one"}, {150, "two"}};

    auto cloud = std::make_shared<RecordingCloudClient>();
    auto router = std::make_shared<StaticRouter>();
    router->decision_.target = RouteTarget::kLocal;
    router->decision_.reason = "local_default";

    DaemonConfig config = minimal_config();
    config.local.model_path = "local.gguf";

    EdgeDaemon daemon(config, local, cloud, router);
    if (!require(daemon.initialize(), "disconnect daemon should initialize")) {
        return false;
    }

    TestServer edge_server;
    install_openai_chat_route(edge_server, daemon, config);
    if (!require(edge_server.start(), "edge server should start")) {
        return false;
    }

    StreamCapture capture;
    post_stream_request(edge_server.base_url(), build_stream_request(false), capture, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    if (!require(capture.saw_first_chunk, "disconnect test should receive an initial chunk")) {
        return false;
    }
    if (!require(
            local->stream_cancelled_.load(),
            "client disconnect should cancel local generation")) {
        return false;
    }

    return true;
}

}  // namespace

int main() {
    if (!test_local_incremental_streaming()) {
        return 1;
    }
    if (!test_stream_completion_id_correlates_to_metrics_events()) {
        return 1;
    }
    if (!test_cloud_incremental_streaming()) {
        return 1;
    }
    if (!test_pre_first_byte_fallback_to_cloud_stream()) {
        return 1;
    }
    if (!test_post_first_byte_error_stays_on_selected_backend()) {
        return 1;
    }
    if (!test_stream_disconnect_cancels_local_generation()) {
        return 1;
    }

    std::cout << "openai_streaming_integration_test: ok\n";
    return 0;
}
