#include "openai_compat/openai_compat.hpp"
#include "runtime/contracts.hpp"

#include <cpp-httplib/httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

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

}  // namespace

int main() {
    if (!test_get_models_openai_shape() || !test_post_chat_completions_validation_error_is_openai_json() ||
        !test_post_chat_completions_success_shape()) {
        return 1;
    }
    std::cout << "openai_http_integration_test: ok\n";
    return 0;
}
