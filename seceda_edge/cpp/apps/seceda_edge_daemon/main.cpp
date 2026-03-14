#include "cloud_bridge/cloud_client.hpp"
#include "local_models/llama_runtime.hpp"
#include "router/heuristic_router.hpp"
#include "runtime/edge_daemon.hpp"
#include "runtime/runtime_config.hpp"

#include <cpp-httplib/httplib.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <iostream>

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

json local_model_info_to_json(const LocalModelInfo & info) {
    return {
        {"ready", info.ready},
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
        {"base_url", info.base_url},
        {"model", info.model},
        {"timeout_seconds", info.timeout_seconds},
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
    return {
        {"state", to_string(info.state)},
        {"host", info.host},
        {"port", info.port},
        {"default_system_prompt", info.default_system_prompt},
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
    };
}

json response_to_json(const InferenceResponse & response) {
    json payload = {
        {"ok", response.ok},
        {"error_kind", to_string(response.error_kind)},
        {"error", response.error},
        {"text", response.text},
        {"requested_target", to_string(response.requested_target)},
        {"initial_target", to_string(response.initial_target)},
        {"final_target", to_string(response.final_target)},
        {"fallback_used", response.fallback_used},
        {"fallback_reason", response.fallback_reason},
        {"route_reason", response.route_reason},
        {"matched_rules", response.matched_rules},
        {"active_model_path", response.active_model_path},
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

    request.text = parsed["text"].get<std::string>();
    if (parsed.contains("system_prompt") && !parsed["system_prompt"].is_string()) {
        error = "system_prompt must be a string when provided";
        return false;
    }
    request.system_prompt = parsed.contains("system_prompt")
        ? parsed["system_prompt"].get<std::string>()
        : config.default_system_prompt;
    request.options = config.default_generation;

    if (parsed.contains("route_override")) {
        if (!parsed["route_override"].is_string() ||
            !parse_route_target(parsed["route_override"].get<std::string>(), request.route_override)) {
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
        if (!read_int(options, "max_tokens", request.options.max_tokens) ||
            !read_float(options, "temperature", request.options.temperature) ||
            !read_float(options, "top_p", request.options.top_p) ||
            !read_int(options, "top_k", request.options.top_k) ||
            !read_float(options, "min_p", request.options.min_p) ||
            !read_uint(options, "seed", request.options.seed)) {
            error = "One or more generation options had an invalid type";
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
