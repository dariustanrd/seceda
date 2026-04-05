#include "local_models/sidecar_local_engine_adapter.hpp"

#include <cpp-httplib/httplib.h>
#include <nlohmann/json.hpp>

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

}  // namespace

int main() {
    TestServer server;
    if (!require(server.start(), "test server should start")) {
        return 1;
    }

    std::mutex mutex;
    std::vector<json> request_bodies;
    server.server().Post(
        "/v1/chat/completions",
        [&](const httplib::Request & request, httplib::Response & response) {
            std::lock_guard<std::mutex> lock(mutex);
            request_bodies.push_back(json::parse(request.body));

            const json body = json::parse(request.body);
            if (body.contains("stream") && body["stream"].get<bool>()) {
                response.set_content(
                    "data: {\"choices\":[{\"delta\":{\"content\":\"side\"}}]}\n\n"
                    "data: {\"choices\":[{\"delta\":{\"content\":\"car\"},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":2,\"completion_tokens\":2}}\n\n"
                    "data: [DONE]\n\n",
                    "text/event-stream");
                return;
            }

            response.set_content(
                R"({"choices":[{"message":{"role":"assistant","content":"sidecar ok"}}],"usage":{"prompt_tokens":4,"completion_tokens":2}})",
                "application/json");
        });

    SidecarLocalEngineAdapter adapter;
    LocalModelConfig config;
    config.engine_id = "local/sidecar";
    config.backend_id = "local/sidecar";
    config.model_id = "sidecar-model";
    config.model_alias = "local/sidecar";
    config.display_name = "Sidecar test";
    config.execution_mode = "sidecar_server";
    config.sidecar_base_url = server.base_url();

    std::string error;
    if (!require(adapter.load(config, {}, error), error.c_str())) {
        return 1;
    }
    if (!require(adapter.is_ready(), "adapter should be ready after load")) {
        return 1;
    }
    if (!require(
            adapter.info().endpoint_base_url == server.base_url(),
            "adapter info should expose sidecar base url")) {
        return 1;
    }

    InferenceRequest tool_request;
    tool_request.messages.push_back({"user", "call lookup", {}, {}, {}});
    tool_request.advanced.tools_json =
        R"([{"type":"function","function":{"name":"lookup","description":"Lookup","parameters":{"type":"object"}}}])";
    tool_request.advanced.tool_choice_json = R"({"type":"function","function":{"name":"lookup"}})";
    tool_request.capabilities.has_tools = true;
    tool_request.capabilities.requests_tool_choice = true;
    refresh_request_views(tool_request);

    const auto tool_result = adapter.generate(tool_request);
    if (!require(tool_result.ok, "sidecar adapter should support tool-shaped requests")) {
        return 1;
    }
    if (!require(tool_result.text == "sidecar ok", "sidecar adapter should parse JSON completions")) {
        return 1;
    }
    if (!require(tool_result.identity.route_target == RouteTarget::kLocal, "sidecar route target should stay local")) {
        return 1;
    }

    InferenceRequest stream_request;
    stream_request.messages.push_back({"user", "stream", {}, {}, {}});
    stream_request.options.stream = true;
    refresh_request_views(stream_request);

    std::string streamed_text;
    const auto stream_result = adapter.generate_stream(
        stream_request,
        [&](const StreamedChatDelta & delta) {
            streamed_text += delta.content;
            return true;
        });
    if (!require(stream_result.ok, "sidecar adapter should support streaming completions")) {
        return 1;
    }
    if (!require(streamed_text == "sidecar", "stream callback should receive sidecar deltas")) {
        return 1;
    }
    if (!require(stream_result.text == "sidecar", "stream result should aggregate streamed text")) {
        return 1;
    }

    std::lock_guard<std::mutex> lock(mutex);
    if (!require(request_bodies.size() == 2, "two sidecar requests should be captured")) {
        return 1;
    }
    if (!require(
            request_bodies[0].contains("tools"),
            "tool request should forward tools to the sidecar")) {
        return 1;
    }
    if (!require(
            request_bodies[1].contains("stream") && request_bodies[1]["stream"].get<bool>(),
            "stream request should keep upstream streaming enabled")) {
        return 1;
    }

    return 0;
}
