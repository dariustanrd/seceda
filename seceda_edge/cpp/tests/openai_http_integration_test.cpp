#include "openai_compat/openai_compat.hpp"
#include "runtime/contracts.hpp"
#include "runtime/edge_daemon.hpp"

#include <cpp-httplib/httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace seceda::edge;
namespace oa = seceda::edge::openai_compat;
using json = nlohmann::json;

bool require(bool ok, const char * msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << std::endl;
        return false;
    }
    return true;
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

class FakeLocalRuntime : public ILocalModelRuntime {
public:
    bool load(const LocalModelConfig & config, const std::string &, std::string &) override {
        ready_ = true;
        model_path_ = config.model_path.empty() ? "demo.gguf" : config.model_path;
        return true;
    }

    bool reload(const LocalModelConfig & config, const std::string &, std::string &) override {
        model_path_ = config.model_path.empty() ? "demo.gguf" : config.model_path;
        ready_ = true;
        return true;
    }

    bool is_ready() const override { return ready_; }

    LocalModelInfo info() const override {
        LocalModelInfo info;
        info.ready = ready_;
        info.engine_id = "local/test";
        info.backend_id = "local";
        info.model_id = "local-test";
        info.model_alias = "local/default";
        info.display_name = "Fake local runtime";
        info.execution_mode = "in_process";
        info.model_path = model_path_;
        return info;
    }

    LocalCompletionResult generate(const InferenceRequest &) override {
        LocalCompletionResult result;
        result.ok = true;
        result.text = "hello from local";
        result.message.role = "assistant";
        result.message.content = result.text;
        result.finish_reason = "stop";
        result.timing.total_latency_ms = 12.0;
        result.timing.has_ttft = true;
        result.timing.ttft_ms = 3.0;
        result.timing.prompt_tokens = 4;
        result.timing.generated_tokens = 2;
        result.identity.route_target = RouteTarget::kLocal;
        result.identity.engine_id = "local/test";
        result.identity.backend_id = "local";
        result.identity.model_id = "local-test";
        result.identity.model_alias = "local/default";
        result.identity.display_name = "Fake local runtime";
        result.identity.execution_mode = "in_process";
        result.active_model_path = model_path_;
        return result;
    }

private:
    bool ready_ = false;
    std::string model_path_ = "demo.gguf";
};

class FakeCloudClient : public ICloudClient {
public:
    bool is_configured() const override { return false; }
    CloudClientInfo info() const override { return {}; }
    CloudCompletionResult complete(const InferenceRequest &) override { return {}; }
};

class StaticRouter : public IRouter {
public:
    RouteDecision decide(const InferenceRequest &) const override { return decision_; }
    RouterConfig config() const override { return {}; }

    RouteDecision decision_;
};

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

void install_supported_routes(
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

    server.server().Post("/v1/chat/completions", [&](const httplib::Request & req, httplib::Response & res) {
        InferenceRequest inference_request;
        std::string err;
        if (!oa::parse_chat_completion_request(req.body, config, inference_request, err)) {
            oa::set_openai_error(res, 400, err, "invalid_request_error");
            return;
        }

        const std::string completion_id = oa::ensure_chat_completion_id(inference_request);
        const auto inference_response = daemon.handle_inference(inference_request);
        if (!inference_response.ok) {
            oa::set_openai_error(
                res,
                oa::http_status_for_inference(inference_response),
                inference_response.error.empty() ? "Inference request failed" : inference_response.error,
                oa::openai_error_type(inference_response));
            return;
        }

        res.set_content(
            oa::chat_completion_response(
                inference_request,
                inference_response,
                completion_id,
                oa::unix_timestamp_now())
                .dump(),
            "application/json");
    });
}

DaemonConfig minimal_config() {
    DaemonConfig c;
    c.public_model_alias = "seceda/default";
    c.exposed_models = {
        {"seceda/default", "Seceda default route", "seceda"},
    };
    return c;
}

bool test_get_models_openai_shape() {
    TestServer ts;
    const DaemonConfig cfg = minimal_config();
    ts.server().Get("/v1/models", [&](const httplib::Request &, httplib::Response & res) {
        res.set_content(oa::models_list_payload(cfg).dump(), "application/json");
    });

    if (!require(ts.start(), "server start")) {
        return false;
    }

    httplib::Client cli(ts.base_url());
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(5, 0);
    const auto r = cli.Get("/v1/models");
    if (!require(r && r->status == 200, "GET /v1/models 200")) {
        return false;
    }
    json body;
    try {
        body = json::parse(r->body);
    } catch (...) {
        return require(false, "response JSON");
    }
    if (!require(body["object"] == "list", "models object=list")) {
        return false;
    }
    if (!require(body["data"].is_array() && !body["data"].empty(), "models data non-empty")) {
        return false;
    }
    if (!require(body["data"][0]["id"].is_string(), "first model id")) {
        return false;
    }
    return true;
}

bool test_post_chat_completions_validation_error_is_openai_json() {
    TestServer ts;
    const DaemonConfig cfg = minimal_config();
    ts.server().Post("/v1/chat/completions", [&](const httplib::Request & req, httplib::Response & res) {
        InferenceRequest out;
        std::string err;
        if (!oa::parse_chat_completion_request(req.body, cfg, out, err)) {
            oa::set_openai_error(res, 400, err, "invalid_request_error");
            return;
        }
        res.status = 500;
        res.set_content("unexpected", "text/plain");
    });

    if (!require(ts.start(), "server start")) {
        return false;
    }

    httplib::Client cli(ts.base_url());
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(5, 0);

    json bad = {
        {"model", "seceda/default"},
    };
    const auto r = cli.Post("/v1/chat/completions", bad.dump(), "application/json");
    if (!require(r && r->status == 400, "validation 400")) {
        return false;
    }
    json err_body;
    try {
        err_body = json::parse(r->body);
    } catch (...) {
        return require(false, "error body JSON");
    }
    if (!require(err_body.contains("error"), "OpenAI error envelope")) {
        return false;
    }
    if (!require(err_body["error"]["type"] == "invalid_request_error", "invalid_request_error")) {
        return false;
    }
    return true;
}

bool test_post_chat_completions_success_shape() {
    TestServer ts;
    const DaemonConfig cfg = minimal_config();
    ts.server().Post("/v1/chat/completions", [&](const httplib::Request & req, httplib::Response & res) {
        InferenceRequest ireq;
        std::string err;
        if (!oa::parse_chat_completion_request(req.body, cfg, ireq, err)) {
            oa::set_openai_error(res, 400, err, "invalid_request_error");
            return;
        }

        InferenceResponse ires;
        ires.ok = true;
        ires.message.role = "assistant";
        ires.message.content = "hi";
        ires.finish_reason = "stop";
        ires.final_target = RouteTarget::kLocal;
        ires.total_timing.prompt_tokens = 1;
        ires.total_timing.generated_tokens = 2;

        const std::string id = oa::make_chat_completion_id();
        const auto created = oa::unix_timestamp_now();
        res.set_content(
            oa::chat_completion_response(ireq, ires, id, created).dump(),
            "application/json");
    });

    if (!require(ts.start(), "server start")) {
        return false;
    }

    httplib::Client cli(ts.base_url());
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(5, 0);

    json good = {
        {"model", "seceda/default"},
        {"messages", json::array({{{"role", "user"}, {"content", "yo"}}})},
    };
    const auto r = cli.Post("/v1/chat/completions", good.dump(), "application/json");
    if (!require(r && r->status == 200, "success 200")) {
        return false;
    }
    json body;
    try {
        body = json::parse(r->body);
    } catch (...) {
        return require(false, "success JSON");
    }
    if (!require(body["object"] == "chat.completion", "chat.completion")) {
        return false;
    }
    if (!require(body["choices"].is_array() && body["choices"].size() == 1, "one choice")) {
        return false;
    }
    if (!require(body["choices"][0]["message"]["content"] == "hi", "assistant content")) {
        return false;
    }
    if (!require(body["usage"]["total_tokens"].get<int>() == 3, "usage total_tokens")) {
        return false;
    }
    return true;
}

bool test_chat_completion_id_correlates_to_metrics_events_and_inference_is_removed() {
    auto local = std::make_shared<FakeLocalRuntime>();
    auto cloud = std::make_shared<FakeCloudClient>();
    auto router = std::make_shared<StaticRouter>();
    router->decision_.target = RouteTarget::kLocal;
    router->decision_.reason = "local_default";
    router->decision_.matched_rules = {"local_default"};

    DaemonConfig config = minimal_config();
    config.local.model_path = "demo.gguf";

    EdgeDaemon daemon(config, local, cloud, router);
    if (!require(daemon.initialize(), "daemon should initialize")) {
        return false;
    }

    TestServer ts;
    install_supported_routes(ts, daemon, config);
    if (!require(ts.start(), "server start")) {
        return false;
    }

    httplib::Client cli(ts.base_url());
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(5, 0);

    json good = {
        {"model", "seceda/default"},
        {"messages", json::array({{{"role", "user"}, {"content", "yo"}}})},
    };
    const auto completion = cli.Post("/v1/chat/completions", good.dump(), "application/json");
    if (!require(completion && completion->status == 200, "completion 200")) {
        return false;
    }

    json completion_body;
    try {
        completion_body = json::parse(completion->body);
    } catch (...) {
        return require(false, "completion JSON");
    }

    if (!require(completion_body["id"].is_string(), "completion id")) {
        return false;
    }
    const std::string request_id = completion_body["id"].get<std::string>();

    const auto events = cli.Get(("/metrics/events?request_id=" + request_id).c_str());
    if (!require(events && events->status == 200, "metrics events 200")) {
        return false;
    }

    json events_body;
    try {
        events_body = json::parse(events->body);
    } catch (...) {
        return require(false, "metrics events JSON");
    }

    if (!require(events_body["returned"] == 1, "request id filter should match one event")) {
        return false;
    }
    if (!require(events_body["events"].is_array() && events_body["events"].size() == 1, "one event row")) {
        return false;
    }

    const auto & event = events_body["events"][0];
    if (!require(event["request_id"] == request_id, "event request id should match completion id")) {
        return false;
    }
    if (!require(event["requested_target"] == "auto", "requested target should be recorded")) {
        return false;
    }
    if (!require(event["final_target"] == "local", "final target should be local")) {
        return false;
    }
    if (!require(event["finish_reason"] == "stop", "finish reason should be recorded")) {
        return false;
    }
    if (!require(event["active_model_path"] == "demo.gguf", "active model path should be recorded")) {
        return false;
    }
    if (!require(event["timing"]["prompt_tokens"] == 4, "prompt tokens should be recorded")) {
        return false;
    }
    if (!require(event["timing"]["generated_tokens"] == 2, "generated tokens should be recorded")) {
        return false;
    }
    if (!require(event["timing"]["total_tokens"] == 6, "total tokens should be derived")) {
        return false;
    }
    if (!require(event["local_timing"]["ttft_ms"] == 3.0, "local timing should include ttft")) {
        return false;
    }

    const auto traces = cli.Get(("/trace/events?request_id=" + request_id).c_str());
    if (!require(traces && traces->status == 200, "trace events 200")) {
        return false;
    }

    json traces_body;
    try {
        traces_body = json::parse(traces->body);
    } catch (...) {
        return require(false, "trace events JSON");
    }

    if (!require(traces_body["events"].is_array() && traces_body["events"].size() >= 4, "trace rows")) {
        return false;
    }
    if (!require(traces_body["events"][0]["phase"] == "request_normalized", "trace request normalized")) {
        return false;
    }
    if (!require(traces_body["events"][1]["phase"] == "input_item", "trace input item")) {
        return false;
    }
    if (!require(traces_body["events"][1]["text"] == "yo", "trace input text")) {
        return false;
    }
    if (!require(traces_body["events"][2]["phase"] == "route_selected", "trace route selected")) {
        return false;
    }
    if (!require(traces_body["events"][3]["phase"] == "output_item", "trace output item")) {
        return false;
    }
    if (!require(traces_body["events"][3]["text"] == "hello from local", "trace output text")) {
        return false;
    }
    if (!require(
            traces_body["events"].back()["phase"] == "request_complete",
            "trace completion phase")) {
        return false;
    }
    if (!require(
            traces_body["events"][0]["transport"] == "chat_completions",
            "trace transport")) {
        return false;
    }

    const auto inference = cli.Post("/inference", "{}", "application/json");
    if (!require(inference && inference->status == 404, "/inference should no longer be registered")) {
        return false;
    }

    return true;
}

}  // namespace

int main() {
    if (!test_get_models_openai_shape() || !test_post_chat_completions_validation_error_is_openai_json() ||
        !test_post_chat_completions_success_shape() ||
        !test_chat_completion_id_correlates_to_metrics_events_and_inference_is_removed()) {
        return 1;
    }
    std::cout << "openai_http_integration_test: ok\n";
    return 0;
}
