#include "runtime/edge_daemon.hpp"
#include "local_models/local_execution_modes.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <utility>

namespace seceda::edge {
namespace {

using json = nlohmann::json;

bool has_local_runtime_configuration(const LocalModelConfig & config) {
    if (is_sidecar_execution_mode(config.execution_mode)) {
        return !config.sidecar_base_url.empty();
    }

    return !config.model_path.empty();
}

std::size_t trace_capacity_from_event_capacity(std::size_t event_capacity) {
    const std::size_t base = event_capacity == 0 ? 1 : event_capacity;
    return std::max<std::size_t>(base * 4, 4096);
}

void emit_normalized_request_trace(TraceRegistry & traces, const InferenceRequest & request) {
    if (request.seceda.request_id.empty()) {
        return;
    }

    PromptTraceEvent request_event;
    request_event.request_id = request.seceda.request_id;
    request_event.transport = request.seceda.transport;
    request_event.phase = "request_normalized";
    request_event.item_type = "request";
    request_event.item_id = "request";
    request_event.payload_json =
        json{
            {"model", request.model},
            {"route_override", to_string(request.seceda.route_override)},
            {"preferred_engine_id", request.seceda.preferred_engine_id},
            {"preferred_backend_id", request.seceda.preferred_backend_id},
            {"preferred_model_alias", request.seceda.preferred_model_alias},
            {"normalized",
             {
                 {"system_prompt", request.normalized.system_prompt},
                 {"latest_user_message", request.normalized.latest_user_message},
                 {"routing_prompt", request.normalized.routing_prompt},
             }},
            {"capabilities",
             {
                 {"has_tools", request.capabilities.has_tools},
                 {"requests_tool_choice", request.capabilities.requests_tool_choice},
                 {"requests_structured_output", request.capabilities.requests_structured_output},
                 {"requires_remote_backend", request.capabilities.requires_remote_backend},
             }},
        }
            .dump();
    traces.push_event(std::move(request_event));

    for (std::size_t index = 0; index < request.messages.size(); ++index) {
        const auto & message = request.messages[index];

        PromptTraceEvent message_event;
        message_event.request_id = request.seceda.request_id;
        message_event.transport = request.seceda.transport;
        message_event.phase = "input_item";
        message_event.item_type = "message";
        message_event.item_id = "input-" + std::to_string(index);
        message_event.content_index = message.content.empty() ? -1 : 0;
        message_event.role = message.role;
        message_event.text = message.content;

        json payload = {
            {"name", message.name},
            {"tool_call_id", message.tool_call_id},
        };
        if (!message.tool_calls_json.empty()) {
            try {
                payload["tool_calls"] = json::parse(message.tool_calls_json);
            } catch (const std::exception &) {
                payload["tool_calls_json"] = message.tool_calls_json;
            }
        }
        message_event.payload_json = payload.dump();
        traces.push_event(std::move(message_event));
    }
}

}  // namespace

EdgeDaemon::EdgeDaemon(
    DaemonConfig config,
    std::shared_ptr<ILocalModelRuntime> local_runtime,
    std::shared_ptr<ICloudClient> cloud_client,
    std::shared_ptr<IRouter> router)
    : config_(std::move(config)),
      local_runtime_(std::move(local_runtime)),
      cloud_client_(std::move(cloud_client)),
      router_(std::move(router)),
      metrics_(config_.event_log_capacity),
      traces_(trace_capacity_from_event_capacity(config_.event_log_capacity)),
      executor_(*local_runtime_, *cloud_client_, *router_, metrics_, traces_) {}

bool EdgeDaemon::initialize() {
    std::string error;
    bool local_ready = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = RuntimeState::kLoadingModel;
        last_error_.clear();
    }

    if (has_local_runtime_configuration(config_.local)) {
        local_ready = local_runtime_->load(config_.local, config_.warmup_prompt, error);
    } else {
        error = "No local model configured";
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_ = local_ready ? "" : error;
        update_state_locked(local_ready);
    }

    return local_ready || cloud_client_->is_configured();
}

InferenceResponse EdgeDaemon::handle_inference(InferenceRequest request) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (request.model.empty()) {
            request.model = config_.public_model_alias;
        }
        prepend_system_message(request, config_.default_system_prompt);
    }

    emit_normalized_request_trace(traces_, request);
    auto response = executor_.execute(request);
    if (!response.ok) {
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_ = response.error;
    }
    return response;
}

InferenceResponse EdgeDaemon::handle_inference_stream(
    InferenceRequest request,
    const StreamDeltaCallback & on_delta) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (request.model.empty()) {
            request.model = config_.public_model_alias;
        }
        prepend_system_message(request, config_.default_system_prompt);
    }

    emit_normalized_request_trace(traces_, request);
    auto response = executor_.execute_streaming(request, on_delta);
    if (!response.ok) {
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_ = response.error;
    }
    return response;
}

ModelReloadResult EdgeDaemon::reload_model(
    const std::string & model_path,
    const std::string & warmup_prompt) {
    ModelReloadResult result;
    if (model_path.empty()) {
        result.error = "Model path must not be empty";
        return result;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = RuntimeState::kReloadingModel;
        last_error_.clear();
    }

    LocalModelConfig next_config;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        next_config = config_.local;
    }
    next_config.model_path = model_path;

    std::string error;
    const bool reload_ok = local_runtime_->reload(
        next_config,
        warmup_prompt.empty() ? config_.warmup_prompt : warmup_prompt,
        error);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (reload_ok) {
            config_.local = next_config;
            last_error_.clear();
        } else {
            last_error_ = error;
        }
        update_state_locked(local_runtime_->is_ready());
        result.active_model_path = local_runtime_->info().model_path;
    }

    result.ok = reload_ok;
    result.error = error;
    return result;
}

HealthSnapshot EdgeDaemon::health() const {
    std::lock_guard<std::mutex> lock(mutex_);

    HealthSnapshot snapshot;
    snapshot.state = state_;
    snapshot.local_ready = local_runtime_->is_ready();
    snapshot.cloud_configured = cloud_client_->is_configured();
    snapshot.active_model_path = local_runtime_->info().model_path;
    snapshot.last_error = last_error_;
    return snapshot;
}

InfoSnapshot EdgeDaemon::info() const {
    std::lock_guard<std::mutex> lock(mutex_);

    InfoSnapshot snapshot;
    snapshot.state = state_;
    snapshot.host = config_.host;
    snapshot.port = config_.port;
    snapshot.default_system_prompt = config_.default_system_prompt;
    snapshot.public_model_alias = config_.public_model_alias;
    snapshot.exposed_models = config_.exposed_models;
    snapshot.router_config = router_->config();
    snapshot.local_model = local_runtime_->info();
    snapshot.cloud_client = cloud_client_->info();
    snapshot.configured_local_engines = config_.local_engines;
    snapshot.configured_remote_backends = config_.remote_backends;
    snapshot.last_error = last_error_;
    return snapshot;
}

std::string EdgeDaemon::metrics_text() const {
    return metrics_.render_prometheus();
}

EventBatch EdgeDaemon::events(std::uint64_t since_id, std::size_t limit, std::string request_id) const {
    return metrics_.get_events(since_id, limit, std::move(request_id));
}

PromptTraceBatch EdgeDaemon::trace_events(
    std::uint64_t since_id,
    std::size_t limit,
    std::string request_id) const {
    return traces_.get_events(since_id, limit, std::move(request_id));
}

std::uint64_t EdgeDaemon::subscribe_traces(
    std::string request_id,
    TraceSubscriberCallback callback) {
    return traces_.subscribe(std::move(request_id), std::move(callback));
}

void EdgeDaemon::unsubscribe_traces(std::uint64_t token) {
    traces_.unsubscribe(token);
}

void EdgeDaemon::update_state_locked(bool local_ready) {
    if (local_ready) {
        state_ = RuntimeState::kReady;
        return;
    }

    if (cloud_client_->is_configured()) {
        state_ = RuntimeState::kDegraded;
        return;
    }

    state_ = RuntimeState::kFailed;
}

}  // namespace seceda::edge
