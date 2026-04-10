#include "openai_compat/openai_compat.hpp"
#include "runtime/contracts.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

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

std::string read_fixture(const std::string & name) {
    const std::filesystem::path path =
        std::filesystem::path(__FILE__).parent_path() / "fixtures" / "openai" / name;
    std::ifstream in(path);
    if (!in) {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

DaemonConfig default_config() {
    DaemonConfig c;
    c.public_model_alias = "seceda/default";
    c.exposed_models = {
        {"seceda/default", "Seceda default route", "seceda"},
    };
    c.default_generation.max_completion_tokens = 128;
    return c;
}

bool test_parse_basic_fixture() {
    const std::string body = read_fixture("chat_completion_basic.json");
    if (!require(!body.empty(), "fixture chat_completion_basic.json must load")) {
        return false;
    }

    InferenceRequest req;
    std::string err;
    const DaemonConfig cfg = default_config();
    if (!require(oa::parse_chat_completion_request(body, cfg, req, err), err.c_str())) {
        return false;
    }
    if (!require(req.model == "seceda/default", "model")) {
        return false;
    }
    if (!require(req.messages.size() == 1, "one message")) {
        return false;
    }
    if (!require(req.messages[0].content == "Hello", "user text")) {
        return false;
    }
    if (!require(req.options.max_completion_tokens == 64, "max_tokens alias")) {
        return false;
    }
    if (!require(req.options.temperature > 0.19f && req.options.temperature < 0.21f, "temperature")) {
        return false;
    }
    return true;
}

bool test_unknown_model() {
    const std::string body = read_fixture("chat_completion_unknown_model.json");
    InferenceRequest req;
    std::string err;
    if (!require(
            !oa::parse_chat_completion_request(body, default_config(), req, err),
            "unknown model should fail")) {
        return false;
    }
    if (!require(err.find("Unknown model") != std::string::npos, "error mentions unknown model")) {
        return false;
    }
    return true;
}

bool test_tools_passthrough_fixture() {
    const std::string body = read_fixture("chat_completion_tools_passthrough.json");
    InferenceRequest req;
    std::string err;
    if (!require(oa::parse_chat_completion_request(body, default_config(), req, err), err.c_str())) {
        return false;
    }
    if (!require(req.capabilities.has_tools, "has_tools")) {
        return false;
    }
    if (!require(req.capabilities.requests_tool_choice, "requests_tool_choice")) {
        return false;
    }
    if (!require(req.capabilities.requests_structured_output, "requests_structured_output")) {
        return false;
    }
    if (!require(req.capabilities.requires_remote_backend, "requires_remote_backend")) {
        return false;
    }
    if (!require(!req.advanced.tools_json.empty(), "tools_json preserved")) {
        return false;
    }
    return true;
}

bool test_model_alias_sets_explicit_local_selection() {
    DaemonConfig cfg = default_config();
    cfg.local.model_alias = "local/default";
    cfg.local.engine_id = "local/llama.cpp";
    cfg.local.backend_id = "local";

    const json body = {
        {"model", "local/default"},
        {"messages", json::array({{{"role", "user"}, {"content", "Hello"}}})},
    };

    InferenceRequest req;
    std::string err;
    if (!require(oa::parse_chat_completion_request(body.dump(), cfg, req, err), err.c_str())) {
        return false;
    }
    if (!require(req.seceda.route_override == RouteTarget::kLocal, "local alias forces local route")) {
        return false;
    }
    if (!require(req.seceda.preferred_engine_id == "local/llama.cpp", "local alias preferred engine")) {
        return false;
    }
    return true;
}

bool test_model_alias_sets_explicit_remote_selection() {
    DaemonConfig cfg = default_config();
    cfg.cloud.model_alias = "remote/default";
    cfg.cloud.backend_id = "remote/modal-default";

    const json body = {
        {"model", "remote/default"},
        {"messages", json::array({{{"role", "user"}, {"content", "Hello"}}})},
    };

    InferenceRequest req;
    std::string err;
    if (!require(oa::parse_chat_completion_request(body.dump(), cfg, req, err), err.c_str())) {
        return false;
    }
    if (!require(req.seceda.route_override == RouteTarget::kCloud, "remote alias forces cloud route")) {
        return false;
    }
    if (!require(
            req.seceda.preferred_backend_id == "remote/modal-default",
            "remote alias preferred backend")) {
        return false;
    }
    return true;
}

bool test_models_list_payload() {
    DaemonConfig cfg = default_config();
    cfg.exposed_models.push_back({"alias/other", "Other", "seceda"});
    const json payload = oa::models_list_payload(cfg);
    if (!require(payload["object"] == "list", "object=list")) {
        return false;
    }
    if (!require(payload["data"].is_array(), "data array")) {
        return false;
    }
    bool saw_default = false;
    for (const auto & row : payload["data"]) {
        if (row["id"] == "seceda/default") {
            saw_default = true;
            if (!require(row["object"] == "model", "row object")) {
                return false;
            }
        }
    }
    if (!require(saw_default, "includes seceda/default")) {
        return false;
    }
    return true;
}

bool test_named_remote_backend_alias_uses_catalog_metadata() {
    DaemonConfig cfg = default_config();
    cfg.remote_backends.push_back(
        CloudConfig{
            "remote/alt",
            "remote-alt-model",
            "remote/alt",
            "Remote Alt",
            "remote_service",
            {"chat.completions", "text", "stream", "tools", "response_format"},
            "http://127.0.0.1:9090",
            {},
            30,
            5,
            0,
            0,
            false,
            true,
        });
    cfg.exposed_models.push_back(
        {
            "remote/alt",
            "Remote Alt",
            "seceda",
            RouteTarget::kCloud,
            {},
            "remote/alt",
            "remote-alt-model",
            "remote/alt",
            "remote_service",
            {"chat.completions", "text", "stream"},
        });

    const json body = {
        {"model", "remote/alt"},
        {"messages", json::array({{{"role", "user"}, {"content", "Hello"}}})},
    };

    InferenceRequest req;
    std::string err;
    if (!require(oa::parse_chat_completion_request(body.dump(), cfg, req, err), err.c_str())) {
        return false;
    }
    if (!require(req.seceda.route_override == RouteTarget::kCloud, "catalog metadata should force cloud")) {
        return false;
    }
    if (!require(
            req.seceda.preferred_backend_id == "remote/alt",
            "catalog metadata should select named backend id")) {
        return false;
    }
    if (!require(
            req.seceda.preferred_model_alias == "remote/alt",
            "catalog metadata should preserve model alias")) {
        return false;
    }

    const json models = oa::models_list_payload(cfg);
    bool saw_metadata = false;
    for (const auto & row : models["data"]) {
        if (row["id"] == "remote/alt" &&
            row.contains("metadata") &&
            row["metadata"]["backend_id"] == "remote/alt") {
            saw_metadata = true;
            break;
        }
    }
    if (!require(saw_metadata, "named remote alias should expose metadata in /v1/models")) {
        return false;
    }

    return true;
}

bool test_openai_error_mapping() {
    InferenceResponse bad;
    bad.ok = false;
    bad.error_kind = InferenceErrorKind::kInvalidRequest;
    bad.error = "bad";

    if (!require(oa::http_status_for_inference(bad) == 400, "400 for invalid_request")) {
        return false;
    }
    if (!require(oa::openai_error_type(bad) == "invalid_request_error", "invalid_request_error type")) {
        return false;
    }

    InferenceResponse cloud_down;
    cloud_down.ok = false;
    cloud_down.error_kind = InferenceErrorKind::kCloudUnavailable;
    if (!require(oa::http_status_for_inference(cloud_down) == 503, "503 for cloud unavailable")) {
        return false;
    }

    const json err = oa::openai_error_payload("msg", "server_error");
    if (!require(err.contains("error"), "error envelope")) {
        return false;
    }
    if (!require(err["error"]["message"] == "msg", "message field")) {
        return false;
    }
    if (!require(err["error"]["type"] == "server_error", "type field")) {
        return false;
    }
    return true;
}

bool test_read_completion_token_limit_accepts_completion_aliases() {
    json options = {
        {"max_completion_tokens", 99},
    };
    int out = 0;
    std::string err;
    if (!require(oa::read_completion_token_limit(options, out, err), err.c_str())) {
        return false;
    }
    if (!require(out == 99, "max_completion_tokens")) {
        return false;
    }
    return true;
}

bool test_ensure_chat_completion_id_reuses_existing_value() {
    InferenceRequest request;
    const std::string generated = oa::ensure_chat_completion_id(request);
    if (!require(!generated.empty(), "generated request id should not be empty")) {
        return false;
    }
    if (!require(request.seceda.request_id == generated, "generated request id should be stored")) {
        return false;
    }
    if (!require(
            oa::ensure_chat_completion_id(request) == generated,
            "existing request id should be reused")) {
        return false;
    }
    return true;
}

}  // namespace

int main() {
    if (!test_parse_basic_fixture() || !test_unknown_model() || !test_tools_passthrough_fixture() ||
        !test_model_alias_sets_explicit_local_selection() ||
        !test_model_alias_sets_explicit_remote_selection() || !test_models_list_payload() ||
        !test_named_remote_backend_alias_uses_catalog_metadata() ||
        !test_openai_error_mapping() ||
        !test_read_completion_token_limit_accepts_completion_aliases() ||
        !test_ensure_chat_completion_id_reuses_existing_value()) {
        return 1;
    }
    std::cout << "openai_compat_test: ok\n";
    return 0;
}
