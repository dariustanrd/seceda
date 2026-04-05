#include "cloud_bridge/cloud_client.hpp"
#include "local_models/local_engine_registry.hpp"
#include "router/heuristic_router.hpp"
#include "openai_compat/openai_compat.hpp"
#include "runtime/edge_daemon.hpp"
#include "runtime/runtime_config.hpp"

#include <cpp-httplib/httplib.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using json = nlohmann::json;
using namespace seceda::edge;
namespace oa = seceda::edge::openai_compat;

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
        {"engine_id", info.engine_id},
        {"backend_id", info.backend_id},
        {"model_id", info.model_id},
        {"model_alias", info.model_alias},
        {"display_name", info.display_name},
        {"execution_mode", info.execution_mode},
        {"capabilities", info.capabilities},
        {"model_path", info.model_path},
        {"endpoint_base_url", info.endpoint_base_url},
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

json local_model_config_to_json(const LocalModelConfig & config) {
    return {
        {"engine_id", config.engine_id},
        {"backend_id", config.backend_id},
        {"model_id", config.model_id},
        {"model_alias", config.model_alias},
        {"display_name", config.display_name},
        {"execution_mode", config.execution_mode},
        {"capabilities", config.capabilities},
        {"model_path", config.model_path},
        {"sidecar_base_url", config.sidecar_base_url},
        {"sidecar_timeout_seconds", config.sidecar_timeout_seconds},
        {"sidecar_connect_timeout_seconds", config.sidecar_connect_timeout_seconds},
        {"sidecar_verify_tls", config.sidecar_verify_tls},
    };
}

json cloud_client_info_to_json(const CloudClientInfo & info) {
    return {
        {"configured", info.configured},
        {"backend_id", info.backend_id},
        {"model", info.model},
        {"model_alias", info.model_alias},
        {"display_name", info.display_name},
        {"execution_mode", info.execution_mode},
        {"capabilities", info.capabilities},
        {"base_url", info.base_url},
        {"timeout_seconds", info.timeout_seconds},
        {"connect_timeout_seconds", info.connect_timeout_seconds},
        {"retry_attempts", info.retry_attempts},
        {"retry_backoff_ms", info.retry_backoff_ms},
        {"send_modal_session_id", info.send_modal_session_id},
        {"verify_tls", info.verify_tls},
    };
}

json cloud_config_to_json(const CloudConfig & config) {
    return {
        {"backend_id", config.backend_id},
        {"model", config.model},
        {"model_alias", config.model_alias},
        {"display_name", config.display_name},
        {"execution_mode", config.execution_mode},
        {"capabilities", config.capabilities},
        {"base_url", config.base_url},
        {"timeout_seconds", config.timeout_seconds},
        {"connect_timeout_seconds", config.connect_timeout_seconds},
        {"retry_attempts", config.retry_attempts},
        {"retry_backoff_ms", config.retry_backoff_ms},
        {"send_modal_session_id", config.send_modal_session_id},
        {"verify_tls", config.verify_tls},
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
    json exposed_models = json::array();
    for (const auto & model : info.exposed_models) {
        exposed_models.push_back(oa::model_catalog_entry_json(model));
    }

    json configured_local_engines = json::array();
    for (const auto & engine : info.configured_local_engines) {
        configured_local_engines.push_back(local_model_config_to_json(engine));
    }

    json configured_remote_backends = json::array();
    for (const auto & backend : info.configured_remote_backends) {
        configured_remote_backends.push_back(cloud_config_to_json(backend));
    }

    return {
        {"state", to_string(info.state)},
        {"host", info.host},
        {"port", info.port},
        {"default_system_prompt", info.default_system_prompt},
        {"public_model_alias", info.public_model_alias},
        {"exposed_models", std::move(exposed_models)},
        {"configured_local_engines", std::move(configured_local_engines)},
        {"configured_remote_backends", std::move(configured_remote_backends)},
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
        {"total_latency_ms", event.timing.total_latency_ms},
        {"local_latency_ms", event.local_timing.has_value() ? event.local_timing->total_latency_ms : 0.0},
        {"cloud_latency_ms", event.cloud_timing.has_value() ? event.cloud_timing->total_latency_ms : 0.0},
    };

    if (event.local_timing.has_value()) {
        payload["local_timing"] = timing_to_json(*event.local_timing);
    }
    if (event.cloud_timing.has_value()) {
        payload["cloud_timing"] = timing_to_json(*event.cloud_timing);
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

int http_status_for_state(RuntimeState state) {
    return state == RuntimeState::kReady || state == RuntimeState::kDegraded ? 200 : 503;
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

    auto local_runtime = std::make_shared<LocalEngineRegistry>();
    auto cloud_client = std::make_shared<CloudClient>(config.cloud, config.remote_backends);
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

    server.Get("/v1/models", [&](const httplib::Request &, httplib::Response & response) {
        response.set_content(oa::models_list_payload(config).dump(), "application/json");
    });

    server.Post("/v1/chat/completions", [&](const httplib::Request & request, httplib::Response & response) {
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
                inference_response.error.empty() ? "Inference request failed" : inference_response.error,
                oa::openai_error_type(inference_response));
            return;
        }

        const std::int64_t created_at = oa::unix_timestamp_now();

        response.set_content(
            oa::chat_completion_response(
                inference_request,
                inference_response,
                completion_id,
                created_at)
                .dump(),
            "application/json");
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
