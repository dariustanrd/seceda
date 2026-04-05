#include "runtime/edge_daemon.hpp"

#include <utility>

namespace seceda::edge {
namespace {

bool is_sidecar_execution_mode(const std::string & raw_mode) {
    return raw_mode == "sidecar_server" || raw_mode == "sidecar-server" || raw_mode == "sidecar";
}

bool has_local_runtime_configuration(const LocalModelConfig & config) {
    if (is_sidecar_execution_mode(config.execution_mode)) {
        return !config.sidecar_base_url.empty();
    }

    return !config.model_path.empty();
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
      executor_(*local_runtime_, *cloud_client_, *router_, metrics_) {}

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

EventBatch EdgeDaemon::events(std::uint64_t since_id, std::size_t limit) const {
    return metrics_.get_events(since_id, limit);
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
