#include "local_models/local_engine_registry.hpp"

#include "local_models/local_engine_resolve.hpp"

namespace seceda::edge {

LocalEngineRegistry::LocalEngineRegistry()
    : cactus_stub_(
          "Cactus SDK bridge adapter is not implemented yet; reserved for a future Cactus "
          "integration."),
      runanywhere_stub_(
          "RunAnywhere SDK bridge adapter is not implemented yet; reserved for a future "
          "RunAnywhere integration.") {}

LocalEngineRegistry::~LocalEngineRegistry() = default;

ILocalModelRuntime * LocalEngineRegistry::resolve_delegate(
    const LocalModelConfig & config,
    std::string & error) {
    const ResolvedLocalEngine resolved = resolve_local_engine(config, error);
    if (!error.empty() || resolved == ResolvedLocalEngine::kUnknown) {
        delegate_ = nullptr;
        return nullptr;
    }

    switch (resolved) {
        case ResolvedLocalEngine::kLlamaInProcess:
            delegate_ = &llama_;
            return delegate_;
        case ResolvedLocalEngine::kCactusSdkBridge:
            delegate_ = &cactus_stub_;
            return delegate_;
        case ResolvedLocalEngine::kRunAnywhereSdkBridge:
            delegate_ = &runanywhere_stub_;
            return delegate_;
        case ResolvedLocalEngine::kSidecarServer:
            delegate_ = &sidecar_;
            return delegate_;
        case ResolvedLocalEngine::kUnknown:
            delegate_ = nullptr;
            return nullptr;
    }

    delegate_ = nullptr;
    return nullptr;
}

bool LocalEngineRegistry::load(
    const LocalModelConfig & config,
    const std::string & warmup_prompt,
    std::string & error) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_config_ = config;
    last_error_.clear();

    std::string resolve_error;
    ILocalModelRuntime * const delegate = resolve_delegate(config, resolve_error);
    if (delegate == nullptr) {
        last_error_ = resolve_error.empty() ? "Unable to resolve local engine" : resolve_error;
        error = last_error_;
        return false;
    }

    const bool ok = delegate->load(config, warmup_prompt, error);
    if (!ok) {
        last_error_ = error;
    } else {
        last_error_.clear();
    }
    return ok;
}

bool LocalEngineRegistry::reload(
    const LocalModelConfig & config,
    const std::string & warmup_prompt,
    std::string & error) {
    return load(config, warmup_prompt, error);
}

bool LocalEngineRegistry::is_ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return delegate_ != nullptr && delegate_->is_ready();
}

LocalModelInfo LocalEngineRegistry::info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (delegate_ != nullptr) {
        return delegate_->info();
    }

    LocalModelInfo info;
    info.engine_id = last_config_.engine_id;
    info.backend_id = last_config_.backend_id;
    info.model_id = last_config_.model_id;
    info.model_alias = last_config_.model_alias;
    info.display_name = last_config_.display_name;
    info.execution_mode = last_config_.execution_mode;
    info.capabilities = last_config_.capabilities;
    info.model_path = last_config_.model_path;
    info.last_error = last_error_;
    return info;
}

LocalCompletionResult LocalEngineRegistry::generate(const InferenceRequest & request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (delegate_ == nullptr) {
        LocalCompletionResult result;
        result.error =
            last_error_.empty() ? "Local engine is not initialized" : last_error_;
        result.identity.route_target = RouteTarget::kLocal;
        result.identity.engine_id = last_config_.engine_id;
        result.identity.execution_mode = last_config_.execution_mode;
        return result;
    }

    return delegate_->generate(request);
}

LocalCompletionResult LocalEngineRegistry::generate_stream(
    const InferenceRequest & request,
    const StreamDeltaCallback & on_delta) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (delegate_ == nullptr) {
        LocalCompletionResult result;
        result.error =
            last_error_.empty() ? "Local engine is not initialized" : last_error_;
        result.identity.route_target = RouteTarget::kLocal;
        result.identity.engine_id = last_config_.engine_id;
        result.identity.execution_mode = last_config_.execution_mode;
        return result;
    }

    return delegate_->generate_stream(request, on_delta);
}

}  // namespace seceda::edge
