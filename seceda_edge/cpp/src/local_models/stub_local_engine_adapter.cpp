#include "local_models/stub_local_engine_adapter.hpp"

namespace seceda::edge {

StubLocalEngineAdapter::StubLocalEngineAdapter(std::string not_implemented_detail)
    : detail_(std::move(not_implemented_detail)) {}

void StubLocalEngineAdapter::store_config_identity(const LocalModelConfig & config) {
    last_config_ = config;
}

bool StubLocalEngineAdapter::load(
    const LocalModelConfig & config,
    const std::string &,
    std::string & error) {
    std::lock_guard<std::mutex> lock(mutex_);
    store_config_identity(config);
    last_error_ = detail_;
    error = detail_;
    return false;
}

bool StubLocalEngineAdapter::reload(
    const LocalModelConfig & config,
    const std::string &,
    std::string & error) {
    return load(config, {}, error);
}

bool StubLocalEngineAdapter::is_ready() const {
    return false;
}

LocalModelInfo StubLocalEngineAdapter::info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    LocalModelInfo out;
    out.engine_id = last_config_.engine_id;
    out.backend_id = last_config_.backend_id;
    out.model_id = last_config_.model_id;
    out.model_alias = last_config_.model_alias;
    out.display_name = last_config_.display_name;
    out.execution_mode = last_config_.execution_mode;
    out.capabilities = last_config_.capabilities;
    out.model_path = last_config_.model_path;
    out.last_error = last_error_;
    return out;
}

LocalCompletionResult StubLocalEngineAdapter::generate(const InferenceRequest &) {
    LocalCompletionResult result;
    std::lock_guard<std::mutex> lock(mutex_);
    result.error = last_error_.empty() ? detail_ : last_error_;
    result.identity.route_target = RouteTarget::kLocal;
    result.identity.engine_id = last_config_.engine_id;
    result.identity.backend_id = last_config_.backend_id;
    result.identity.model_id = last_config_.model_id;
    result.identity.model_alias = last_config_.model_alias;
    result.identity.display_name = last_config_.display_name;
    result.identity.execution_mode = last_config_.execution_mode;
    result.identity.capabilities = last_config_.capabilities;
    return result;
}

}  // namespace seceda::edge
